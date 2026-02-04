#!/usr/bin/env python3
"""
Combined MJPEG/H.264 Camera Streamer for Rinkhals

Supports three encoder modes:
1. gkcam (default) - Uses gkcam's built-in FLV stream, extracts JPEG via ffmpeg
2. rkmpi - Uses rkmpi_enc binary for direct USB camera capture (MJPEG capture)
3. rkmpi-yuyv - Uses rkmpi_enc with YUYV capture and hardware JPEG encoding

Provides:
- /stream - MJPEG multipart stream (for Mainsail/Fluidd)
- /snapshot - JPEG snapshot
- /flv - H.264 FLV stream (for Anycubic slicer) - only in rkmpi modes
- /control - Web interface for runtime control

Uses local binary API (port 18086) to:
- Query printer status (LAN mode)
- Enable LAN mode if autolanmode is enabled
"""

import argparse
import json
import os
import re
import signal
import socket
import ssl
import subprocess
import sys
import threading
import time
import struct
import urllib.parse
import urllib.request
import uuid
from collections import deque

# Pre-compiled regex patterns for performance
RE_CONTENT_LENGTH = re.compile(rb'Content-Length:\s*(\d+)', re.IGNORECASE)
RE_NAL_START_4 = b'\x00\x00\x00\x01'
RE_NAL_START_3 = b'\x00\x00\x01'


# ============================================================================
# MQTT Camera Control (port 9883 TLS)
# ============================================================================

# MQTT packet types
MQTT_CONNECT = 0x10
MQTT_CONNACK = 0x20
MQTT_PUBLISH = 0x30
MQTT_SUBSCRIBE = 0x82
MQTT_SUBACK = 0x90
MQTT_DISCONNECT = 0xE0

# Config paths
DEVICE_ACCOUNT_PATH = '/userdata/app/gk/config/device_account.json'
API_CONFIG_PATH = '/userdata/app/gk/config/api.cfg'

# Timelapse directory
TIMELAPSE_DIR = '/useremain/app/gk/Time-lapse-Video/'
TIMELAPSE_FPS = 10  # FPS used for duration calculation

# MQTT packet type for PUBACK
MQTT_PUBACK = 0x40


def load_mqtt_credentials():
    """Load MQTT credentials from device account file"""
    try:
        with open(DEVICE_ACCOUNT_PATH, 'r') as f:
            data = json.load(f)
            return {
                'deviceId': data.get('deviceId'),
                'username': data.get('username'),
                'password': data.get('password')
            }
    except Exception as e:
        print(f"Failed to load MQTT credentials: {e}", flush=True)
        return None


def load_model_id():
    """Load model ID from api.cfg"""
    try:
        with open(API_CONFIG_PATH, 'r') as f:
            data = json.load(f)
            return data.get('cloud', {}).get('modelId')
    except Exception as e:
        print(f"Failed to load model ID: {e}", flush=True)
        return None


# ============================================================================
# Display Orientation and Touch Coordinate Transformation
# ============================================================================

# Model IDs from api.cfg
MODEL_ID_K2P = "20021"
MODEL_ID_K3 = "20024"
MODEL_ID_KS1 = "20025"
MODEL_ID_K3M = "20026"
MODEL_ID_K3V2 = "20027"
MODEL_ID_KS1M = "20029"

# Display orientations
DISPLAY_ORIENT_NORMAL = 0
DISPLAY_ORIENT_FLIP_180 = 1     # KS1, KS1M
DISPLAY_ORIENT_ROTATE_90 = 2    # K3, K2P, K3V2
DISPLAY_ORIENT_ROTATE_270 = 3   # K3M

# Framebuffer native resolution
FB_WIDTH = 800
FB_HEIGHT = 480


def get_display_orientation(model_id: str) -> int:
    """Get display orientation based on printer model ID"""
    if model_id in (MODEL_ID_KS1, MODEL_ID_KS1M):
        return DISPLAY_ORIENT_FLIP_180
    elif model_id == MODEL_ID_K3M:
        return DISPLAY_ORIENT_ROTATE_270
    elif model_id in (MODEL_ID_K3, MODEL_ID_K2P, MODEL_ID_K3V2):
        return DISPLAY_ORIENT_ROTATE_90
    return DISPLAY_ORIENT_NORMAL


def get_display_dimensions(orientation: int) -> tuple:
    """Get display dimensions after rotation (width, height)"""
    if orientation in (DISPLAY_ORIENT_ROTATE_90, DISPLAY_ORIENT_ROTATE_270):
        return (FB_HEIGHT, FB_WIDTH)  # Swapped: 480x800
    return (FB_WIDTH, FB_HEIGHT)  # Normal: 800x480


def transform_touch_coordinates(x: int, y: int, orientation: int) -> tuple:
    """Transform web display coordinates to touch panel coordinates.

    The touch panel is aligned with the native framebuffer (800x480).
    The web display shows a rotated/flipped version of the framebuffer.
    We need to reverse the transformation to get touch panel coordinates.

    Args:
        x, y: Coordinates on the web display (after rotation/flip)
        orientation: Display orientation applied to the framebuffer

    Returns:
        (touch_x, touch_y): Coordinates for the touch panel
    """
    if orientation == DISPLAY_ORIENT_FLIP_180:
        # Web shows 180° flipped image (800x480)
        # Reverse: touch = (800-x, 480-y)
        return (FB_WIDTH - x, FB_HEIGHT - y)

    elif orientation == DISPLAY_ORIENT_ROTATE_90:
        # Web shows 90° CW rotated image (480x800)
        # Original fb(fx,fy) -> web(fy, 480-fx)
        # Reverse: touch_x = 480-y, touch_y = x
        # But web coords are 0-480 for x, 0-800 for y
        # touch_x (0-800) = y * 800/800 = y (scale from web_y 0-800)
        # touch_y (0-480) = (480 - x) where x is 0-480
        return (y, FB_HEIGHT - x)

    elif orientation == DISPLAY_ORIENT_ROTATE_270:
        # Web shows 270° CW (90° CCW) rotated image (480x800)
        # Original fb(fx,fy) -> web(480-fy, fx)
        # Reverse: touch_x = 800-y, touch_y = x
        return (FB_WIDTH - y, x)

    else:
        # No transformation needed
        return (x, y)


# Global display orientation (set during startup)
g_display_orientation = DISPLAY_ORIENT_NORMAL
g_display_width = FB_WIDTH
g_display_height = FB_HEIGHT


def init_display_orientation():
    """Initialize display orientation from model ID"""
    global g_display_orientation, g_display_width, g_display_height
    model_id = load_model_id()
    if model_id:
        g_display_orientation = get_display_orientation(model_id)
        g_display_width, g_display_height = get_display_dimensions(g_display_orientation)
        orient_names = {0: "NORMAL", 1: "FLIP_180", 2: "ROTATE_90", 3: "ROTATE_270"}
        print(f"Display orientation: {orient_names.get(g_display_orientation, 'UNKNOWN')} "
              f"(model={model_id}, display={g_display_width}x{g_display_height})", flush=True)
    else:
        print("Could not detect model ID, using default display orientation", flush=True)


def mqtt_encode_string(s):
    """Encode a string for MQTT protocol (length-prefixed UTF-8)"""
    encoded = s.encode('utf-8')
    return struct.pack('>H', len(encoded)) + encoded


def mqtt_encode_remaining_length(length):
    """Encode MQTT remaining length (variable-length encoding)"""
    result = bytearray()
    while True:
        byte = length % 128
        length //= 128
        if length > 0:
            byte |= 0x80
        result.append(byte)
        if length == 0:
            break
    return bytes(result)


def mqtt_decode_remaining_length(sock):
    """Decode MQTT remaining length from socket"""
    multiplier = 1
    value = 0
    while True:
        byte = sock.recv(1)
        if not byte:
            return 0
        b = byte[0]
        value += (b & 0x7F) * multiplier
        if (b & 0x80) == 0:
            break
        multiplier *= 128
    return value


def mqtt_build_connect(client_id, username, password):
    """Build MQTT CONNECT packet"""
    protocol_name = mqtt_encode_string("MQTT")
    protocol_level = bytes([0x04])  # MQTT 3.1.1
    connect_flags = bytes([0xC2])  # Username, Password, Clean Session
    keep_alive = struct.pack('>H', 60)
    var_header = protocol_name + protocol_level + connect_flags + keep_alive
    payload = mqtt_encode_string(client_id)
    payload += mqtt_encode_string(username)
    payload += mqtt_encode_string(password)
    remaining = var_header + payload
    fixed_header = bytes([MQTT_CONNECT]) + mqtt_encode_remaining_length(len(remaining))
    return fixed_header + remaining


def mqtt_build_subscribe(topic, packet_id=1):
    """Build MQTT SUBSCRIBE packet"""
    var_header = struct.pack('>H', packet_id)
    payload = mqtt_encode_string(topic) + bytes([0x00])  # QoS 0
    remaining = var_header + payload
    fixed_header = bytes([MQTT_SUBSCRIBE]) + mqtt_encode_remaining_length(len(remaining))
    return fixed_header + remaining


def mqtt_build_publish(topic, payload, qos=0, packet_id=1):
    """Build MQTT PUBLISH packet"""
    var_header = mqtt_encode_string(topic)
    if qos > 0:
        var_header += struct.pack('>H', packet_id)
    payload_bytes = payload.encode('utf-8') if isinstance(payload, str) else payload
    remaining = var_header + payload_bytes
    flags = (qos << 1) & 0x06
    fixed_header = bytes([MQTT_PUBLISH | flags]) + mqtt_encode_remaining_length(len(remaining))
    return fixed_header + remaining


def mqtt_send_led_control(on: bool):
    """Send LED control command via MQTT.

    Based on ACProxyCam's MqttCameraController.cs:793-800
    """
    creds = load_mqtt_credentials()
    if not creds or not creds.get('deviceId'):
        print("MQTT LED: No credentials available", flush=True)
        return False

    device_id = creds['deviceId']
    username = creds['username']
    password = creds['password']

    model_id = load_model_id()
    if not model_id:
        print("MQTT LED: Could not load model ID", flush=True)
        return False

    try:
        # Create TLS socket
        context = ssl.create_default_context()
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(10)
        ssl_sock = context.wrap_socket(sock)
        ssl_sock.connect(('127.0.0.1', 9883))

        # Send CONNECT
        client_id = f"h264led_{uuid.uuid4().hex[:8]}"
        ssl_sock.send(mqtt_build_connect(client_id, username, password))

        # Read CONNACK
        response = ssl_sock.recv(4)
        if len(response) < 4 or response[0] != MQTT_CONNACK or response[3] != 0:
            ssl_sock.close()
            return False

        # Send LED control command
        topic = f"anycubic/anycubicCloud/v1/web/printer/{model_id}/{device_id}/light"
        payload = json.dumps({
            "type": "light",
            "action": "control",
            "timestamp": int(time.time() * 1000),
            "msgid": str(uuid.uuid4()),
            "data": {
                "type": 2,
                "status": 1 if on else 0,
                "brightness": 100
            }
        })

        ssl_sock.send(mqtt_build_publish(topic, payload, qos=1, packet_id=1))

        # Wait for PUBACK
        ssl_sock.settimeout(3)
        try:
            ssl_sock.recv(4)
        except socket.timeout:
            pass

        # Disconnect
        ssl_sock.send(bytes([MQTT_DISCONNECT, 0x00]))
        ssl_sock.close()
        print(f"MQTT LED: {'On' if on else 'Off'} command sent", flush=True)
        return True

    except Exception as e:
        print(f"MQTT LED: Error - {e}", flush=True)
        return False


def mqtt_send_start_camera():
    """Send startCapture command via MQTT to start gkcam's camera stream"""
    creds = load_mqtt_credentials()
    if not creds or not creds.get('deviceId'):
        print("MQTT: No credentials available", flush=True)
        return False

    device_id = creds['deviceId']
    username = creds['username']
    password = creds['password']

    # Load model ID from api.cfg
    model_id = load_model_id()
    if not model_id:
        print("MQTT: Could not load model ID from api.cfg", flush=True)
        return False

    print(f"MQTT: Connecting to broker (model={model_id}, device={device_id[:8]}...)...", flush=True)

    try:
        # Create TLS socket
        context = ssl.create_default_context()
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(10)
        ssl_sock = context.wrap_socket(sock)
        ssl_sock.connect(('127.0.0.1', 9883))

        # Send CONNECT
        client_id = f"h264streamer_{uuid.uuid4().hex[:8]}"
        ssl_sock.send(mqtt_build_connect(client_id, username, password))

        # Read CONNACK
        response = ssl_sock.recv(4)
        if len(response) < 4 or response[0] != MQTT_CONNACK or response[3] != 0:
            print(f"MQTT: Connection failed: {response.hex()}", flush=True)
            ssl_sock.close()
            return False
        print("MQTT: Connected", flush=True)

        # Send startCapture with QoS 1 (AtLeastOnce)
        topic = f"anycubic/anycubicCloud/v1/web/printer/{model_id}/{device_id}/video"
        payload = json.dumps({
            "type": "video",
            "action": "startCapture",
            "timestamp": int(time.time() * 1000),
            "msgid": str(uuid.uuid4()),
            "data": None
        })

        print(f"MQTT: Sending startCapture to {model_id}/{device_id}", flush=True)
        ssl_sock.send(mqtt_build_publish(topic, payload, qos=1, packet_id=1))

        # Wait for PUBACK (QoS 1 acknowledgment)
        ssl_sock.settimeout(5)
        try:
            puback = ssl_sock.recv(4)
            if puback and (puback[0] & 0xF0) == MQTT_PUBACK:
                print("MQTT: startCapture acknowledged (PUBACK)", flush=True)
            else:
                print(f"MQTT: startCapture sent (no PUBACK: {puback.hex() if puback else 'empty'})", flush=True)
        except socket.timeout:
            print("MQTT: startCapture sent (PUBACK timeout)", flush=True)

        # Disconnect
        ssl_sock.send(bytes([MQTT_DISCONNECT, 0x00]))
        ssl_sock.close()
        return True

    except Exception as e:
        print(f"MQTT: Error - {e}", flush=True)
        return False


class MQTTVideoResponder:
    """MQTT subscriber that responds to video startCapture/stopCapture commands.

    When the slicer sends stopCapture followed by startCapture, we respond
    as if gkcam is running to prevent the slicer from disconnecting.
    """

    def __init__(self):
        self.running = False
        self.thread = None
        self.creds = None
        self.model_id = None
        self.device_id = None
        self.streaming_paused = False  # Set True on stopCapture, False on startCapture
        self.handled_msgids = set()  # Track handled msgids to avoid duplicates
        self.msgid_cleanup_time = 0  # Last cleanup time

    def start(self):
        """Start the MQTT responder thread"""
        self.creds = load_mqtt_credentials()
        if not self.creds or not self.creds.get('deviceId'):
            print("MQTT Responder: No credentials available", flush=True)
            return False

        self.model_id = load_model_id()
        if not self.model_id:
            print("MQTT Responder: Could not load model ID", flush=True)
            return False

        self.device_id = self.creds['deviceId']
        self.running = True
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        print(f"MQTT Responder: Started (model={self.model_id})", flush=True)
        return True

    def stop(self):
        """Stop the MQTT responder thread"""
        self.running = False
        if self.thread:
            self.thread.join(timeout=5)

    def _run(self):
        """Main responder loop - subscribe and handle video commands"""
        while self.running:
            try:
                self._connect_and_listen()
            except Exception as e:
                print(f"MQTT Responder: Error - {e}, reconnecting in 5s", flush=True)
                time.sleep(5)

    def _connect_and_listen(self):
        """Connect to MQTT and listen for video commands"""
        context = ssl.create_default_context()
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(10)
        ssl_sock = context.wrap_socket(sock)
        ssl_sock.connect(('127.0.0.1', 9883))

        # Send CONNECT
        client_id = f"h264resp_{uuid.uuid4().hex[:8]}"
        username = self.creds['username']
        password = self.creds['password']
        ssl_sock.send(mqtt_build_connect(client_id, username, password))

        # Read CONNACK
        response = ssl_sock.recv(4)
        if len(response) < 4 or response[0] != MQTT_CONNACK or response[3] != 0:
            print(f"MQTT Responder: Connection failed: {response.hex()}", flush=True)
            ssl_sock.close()
            time.sleep(5)
            return

        print("MQTT Responder: Connected to broker", flush=True)

        # Subscribe to video topic (both web and slicer paths)
        video_topic = f"anycubic/anycubicCloud/v1/web/printer/{self.model_id}/{self.device_id}/video"
        self._subscribe(ssl_sock, video_topic)

        # Also subscribe to slicer video topic pattern
        slicer_topic = f"anycubic/anycubicCloud/v1/slicer/printer/{self.model_id}/{self.device_id}/video"
        self._subscribe(ssl_sock, slicer_topic, packet_id=2)

        # Subscribe to report topic to detect spurious stopCapture from firmware
        report_topic = f"anycubic/anycubicCloud/v1/printer/public/{self.model_id}/{self.device_id}/video/report"
        self._subscribe(ssl_sock, report_topic, packet_id=3)

        print(f"MQTT Responder: Subscribed to video topics and reports", flush=True)

        # Listen loop with buffer for multi-packet handling
        # Also send periodic startCapture reports to counteract firmware's 9-second timeout
        ssl_sock.settimeout(1.0)
        buffer = b""
        last_report_time = time.time()
        report_interval = 2.0  # Send keepalive command every 2 seconds to counteract spurious stopCapture

        while self.running:
            try:
                data = ssl_sock.recv(4096)
                if data:
                    buffer += data
                    # Process all complete packets in buffer
                    while len(buffer) > 2:
                        processed, remaining = self._handle_packet(ssl_sock, buffer)
                        if processed == 0:
                            break  # Incomplete packet
                        buffer = remaining
            except socket.timeout:
                pass
            except Exception as e:
                print(f"MQTT Responder: Receive error - {e}", flush=True)
                break

            # DISABLED: Proactive keepalive commands were creating feedback loops
            # Now we only react to spurious stopCapture reports (handled in _handle_packet)
            # now = time.time()
            # if now - last_report_time >= report_interval:
            #     self._send_keepalive_command(ssl_sock)
            #     last_report_time = now

        ssl_sock.close()

    def _subscribe(self, ssl_sock, topic, packet_id=1):
        """Subscribe to an MQTT topic"""
        topic_bytes = topic.encode('utf-8')
        payload = bytearray([0x00, packet_id])  # packet id
        payload += bytearray([len(topic_bytes) >> 8, len(topic_bytes) & 0xFF])
        payload.extend(topic_bytes)
        payload.append(0x00)  # QoS 0

        subscribe = bytearray([MQTT_SUBSCRIBE])
        subscribe.extend(mqtt_encode_remaining_length(len(payload)))
        subscribe.extend(payload)
        ssl_sock.send(subscribe)

        # Read SUBACK
        try:
            ssl_sock.settimeout(5)
            suback = ssl_sock.recv(5)
        except:
            pass

    def _handle_packet(self, ssl_sock, data):
        """Handle incoming MQTT packet. Returns (bytes_processed, remaining_data)."""
        if not data or len(data) < 2:
            return (0, data)

        pkt_type = data[0] >> 4

        # Decode remaining length to find packet boundary
        i = 1
        mult, remaining_len = 1, 0
        while i < len(data) and data[i] & 0x80:
            remaining_len += (data[i] & 0x7F) * mult
            mult *= 128
            i += 1
        if i >= len(data):
            return (0, data)  # Incomplete length
        remaining_len += (data[i] & 0x7F) * mult
        header_end = i + 1

        pkt_len = header_end + remaining_len
        if len(data) < pkt_len:
            return (0, data)  # Incomplete packet

        # Skip non-PUBLISH packets
        if pkt_type != 3:
            return (pkt_len, data[pkt_len:])

        # Parse PUBLISH
        try:
            # Parse topic
            topic_len = (data[header_end] << 8) | data[header_end + 1]
            topic_start = header_end + 2
            topic = data[topic_start:topic_start + topic_len].decode('utf-8')

            # Parse payload
            payload_start = topic_start + topic_len
            payload = data[payload_start:pkt_len]

            # Check if it's a video command (not a report)
            if '/video' in topic and '/report' not in topic:
                try:
                    msg = json.loads(payload.decode('utf-8'))
                    action = msg.get('action')
                    msgid = msg.get('msgid')

                    if action in ('startCapture', 'stopCapture'):
                        # Deduplicate by msgid
                        if msgid and msgid in self.handled_msgids:
                            print(f"MQTT Responder: Skipping duplicate {action} (msgid={msgid[:8]}...)", flush=True)
                            return (pkt_len, data[pkt_len:])

                        # Track this msgid
                        if msgid:
                            now = time.time()
                            if now - self.msgid_cleanup_time > 60:
                                self.handled_msgids.clear()
                                self.msgid_cleanup_time = now
                            self.handled_msgids.add(msgid)

                        print(f"MQTT Responder: Received {action} on {topic.split('/')[-2]}/{topic.split('/')[-1]} (msgid={msgid[:8] if msgid else 'none'}...)", flush=True)
                        self._send_video_response(ssl_sock, action, msgid)
                except json.JSONDecodeError:
                    pass

            # Detect spurious stopCapture reports from firmware and counter them
            elif '/video/report' in topic:
                try:
                    msg = json.loads(payload.decode('utf-8'))
                    action = msg.get('action')
                    msgid = msg.get('msgid')

                    # If it's a stopCapture report that we didn't send, counter it immediately
                    if action == 'stopCapture' and msgid and msgid not in self.handled_msgids:
                        print(f"MQTT Responder: Detected spurious stopCapture report (msgid={msgid[:8]}...), countering!", flush=True)
                        self._send_counter_report(ssl_sock)
                except json.JSONDecodeError:
                    pass
        except Exception as e:
            print(f"MQTT Responder: Parse error - {e}", flush=True)

        return (pkt_len, data[pkt_len:])

    def _send_video_response(self, ssl_sock, action, msgid):
        """Send response to video command to keep slicer happy"""
        # Update streaming state based on action
        if action == 'stopCapture':
            self.streaming_paused = True
            print("MQTT Responder: Streaming PAUSED", flush=True)
        elif action == 'startCapture':
            self.streaming_paused = False
            print("MQTT Responder: Streaming RESUMED", flush=True)

        # NOTE: Don't send response acknowledgment - printer firmware already does that
        # Only send the video/report which gkcam normally sends

        # Send video report (match gkcam's response format exactly)
        report_topic = f"anycubic/anycubicCloud/v1/printer/public/{self.model_id}/{self.device_id}/video/report"
        # gkcam uses 'pushStopped' for stopCapture, 'initSuccess' for startCapture
        state = "pushStopped" if action == "stopCapture" else "initSuccess"
        report_payload = json.dumps({
            "type": "video",
            "action": action,
            "timestamp": int(time.time() * 1000),
            "msgid": str(uuid.uuid4()),
            "state": state,
            "code": 200,
            "msg": "",
            "data": None
        })
        ssl_sock.send(mqtt_build_publish(report_topic, report_payload, qos=0))
        print(f"MQTT Responder: Sent {action} report ({state})", flush=True)

    def _send_keepalive_command(self, ssl_sock):
        """Send periodic startCapture COMMAND to reset firmware's internal timeout.

        The firmware sends spurious stopCapture reports after ~9 seconds if it doesn't
        receive startCapture commands. By sending commands (not just reports), we reset
        the firmware's internal timer.
        """
        # Send to the video command topic (same as slicer uses)
        cmd_topic = f"anycubic/anycubicCloud/v1/web/printer/{self.model_id}/{self.device_id}/video"
        cmd_msgid = str(uuid.uuid4())
        cmd_payload = json.dumps({
            "type": "video",
            "action": "startCapture",
            "timestamp": int(time.time() * 1000),
            "msgid": cmd_msgid,
            "data": None
        })
        try:
            ssl_sock.send(mqtt_build_publish(cmd_topic, cmd_payload, qos=0))
            # Track this msgid so we don't respond to our own command
            self.handled_msgids.add(cmd_msgid)
            print(f"MQTT Responder: Sent keepalive command", flush=True)
        except Exception as e:
            print(f"MQTT Responder: Keepalive command error - {e}", flush=True)

    def _send_keepalive_report(self, ssl_sock):
        """Send periodic startCapture report (legacy, may not be needed)."""
        report_topic = f"anycubic/anycubicCloud/v1/printer/public/{self.model_id}/{self.device_id}/video/report"
        msgid = str(uuid.uuid4())
        report_payload = json.dumps({
            "type": "video",
            "action": "startCapture",
            "timestamp": int(time.time() * 1000),
            "msgid": msgid,
            "state": "initSuccess",
            "code": 200,
            "msg": "",
            "data": None
        })
        try:
            self.handled_msgids.add(msgid)  # Track so we don't counter our own report
            ssl_sock.send(mqtt_build_publish(report_topic, report_payload, qos=0))
            print(f"MQTT Responder: Sent keepalive report (initSuccess)", flush=True)
        except Exception as e:
            print(f"MQTT Responder: Keepalive send error - {e}", flush=True)

    def _send_counter_report(self, ssl_sock):
        """Send immediate startCapture report to counter spurious stopCapture."""
        report_topic = f"anycubic/anycubicCloud/v1/printer/public/{self.model_id}/{self.device_id}/video/report"
        msgid = str(uuid.uuid4())
        report_payload = json.dumps({
            "type": "video",
            "action": "startCapture",
            "timestamp": int(time.time() * 1000),
            "msgid": msgid,
            "state": "initSuccess",
            "code": 200,
            "msg": "",
            "data": None
        })
        try:
            self.handled_msgids.add(msgid)  # Track so we don't counter our own report
            ssl_sock.send(mqtt_build_publish(report_topic, report_payload, qos=0))
            print(f"MQTT Responder: Sent counter report (initSuccess)", flush=True)
        except Exception as e:
            print(f"MQTT Responder: Counter send error - {e}", flush=True)


def mqtt_decode_remaining_length(data, offset):
    """Decode MQTT remaining length from data buffer"""
    multiplier = 1
    value = 0
    i = offset
    while i < len(data):
        byte = data[i]
        value += (byte & 0x7F) * multiplier
        multiplier *= 128
        i += 1
        if (byte & 0x80) == 0:
            break
    return value, i


# ============================================================================
# Touch Panel Control
# ============================================================================

TOUCH_DEVICE = "/dev/input/event0"

# Linux input event types and codes
EV_SYN = 0x00
EV_KEY = 0x01
EV_ABS = 0x03
SYN_REPORT = 0x00
BTN_TOUCH = 0x14a
ABS_MT_SLOT = 0x2f
ABS_MT_TRACKING_ID = 0x39
ABS_MT_POSITION_X = 0x35
ABS_MT_POSITION_Y = 0x36
ABS_MT_TOUCH_MAJOR = 0x30
ABS_MT_PRESSURE = 0x3a

def inject_touch(x: int, y: int, duration_ms: int = 100) -> bool:
    """Inject a touch event at the specified coordinates.

    Args:
        x: X coordinate (0-800 typically)
        y: Y coordinate (0-480 typically)
        duration_ms: Touch duration in milliseconds

    Returns:
        True if successful, False otherwise
    """
    print(f"inject_touch: x={x}, y={y}, duration={duration_ms}", flush=True)
    try:
        fd = os.open(TOUCH_DEVICE, os.O_WRONLY)
        print(f"inject_touch: opened fd={fd}", flush=True)
    except OSError as e:
        print(f"Failed to open touch device: {e}", flush=True)
        return False

    try:
        def emit(event_type, code, value):
            # struct input_event: time (16 bytes on 32-bit), type (u16), code (u16), value (s32)
            # On 32-bit ARM: struct timeval is 8 bytes (tv_sec + tv_usec as 32-bit)
            tv_sec = int(time.time())
            tv_usec = int((time.time() % 1) * 1000000)
            event = struct.pack('IIHHi', tv_sec, tv_usec, event_type, code, value)
            os.write(fd, event)

        # Touch down - MT Protocol B
        emit(EV_ABS, ABS_MT_SLOT, 0)
        emit(EV_ABS, ABS_MT_TRACKING_ID, 1)
        emit(EV_ABS, ABS_MT_POSITION_X, x)
        emit(EV_ABS, ABS_MT_POSITION_Y, y)
        emit(EV_ABS, ABS_MT_TOUCH_MAJOR, 50)
        emit(EV_ABS, ABS_MT_PRESSURE, 100)
        emit(EV_KEY, BTN_TOUCH, 1)
        emit(EV_SYN, SYN_REPORT, 0)

        time.sleep(duration_ms / 1000.0)

        # Touch up
        emit(EV_ABS, ABS_MT_TRACKING_ID, -1)
        emit(EV_KEY, BTN_TOUCH, 0)
        emit(EV_SYN, SYN_REPORT, 0)

        print(f"inject_touch: completed successfully", flush=True)
        return True
    except Exception as e:
        print(f"inject_touch: failed: {e}", flush=True)
        return False
    finally:
        os.close(fd)


# ============================================================================
# Video RPC Responder (port 18086)
# ============================================================================

class VideoRPCResponder:
    """Fake video RPC responder that connects to the local binary API (port 18086).

    When gkcam is killed for rkmpi mode, gkapi/gklib periodically check if gkcam
    is responding via RPC. If these checks fail, gkapi sends spurious stopCapture
    MQTT reports that disconnect the slicer.

    This class pretends to be gkcam by connecting to port 18086 and responding to
    video_stream_request messages with appropriate Video/VideoStreamReply responses.
    """

    def __init__(self):
        self.running = False
        self.thread = None
        self.sock = None

    def start(self):
        """Start the RPC responder thread"""
        self.running = True
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        print("Video RPC Responder: Started", flush=True)
        return True

    def stop(self):
        """Stop the RPC responder thread"""
        self.running = False
        if self.sock:
            try:
                self.sock.close()
            except:
                pass
        if self.thread:
            self.thread.join(timeout=5)
        print("Video RPC Responder: Stopped", flush=True)

    def _run(self):
        """Main responder loop - connect and handle video RPC requests"""
        while self.running:
            try:
                self._connect_and_listen()
            except Exception as e:
                print(f"Video RPC Responder: Error - {e}, reconnecting in 3s", flush=True)
                time.sleep(3)

    def _connect_and_listen(self):
        """Connect to port 18086 and handle video RPC messages"""
        import select

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

        # Set tiny receive buffer - kernel drops old data, we only see recent
        # This prevents buffer growth that causes progressive CPU increase
        try:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        except Exception:
            pass

        self.sock.settimeout(30)

        try:
            self.sock.connect(('127.0.0.1', 18086))
            print("Video RPC Responder: Connected to port 18086", flush=True)
        except Exception as e:
            print(f"Video RPC Responder: Connect failed - {e}", flush=True)
            time.sleep(5)
            return

        # Pattern we're looking for
        needle = b'"video_stream_request"'

        while self.running:
            # Use select to wait for data efficiently
            try:
                readable, _, _ = select.select([self.sock], [], [], 0.5)
            except Exception:
                break

            if not readable:
                continue

            # Single recv - tiny buffer means we only get recent data
            try:
                data = self.sock.recv(4096)
                if not data:
                    print("Video RPC Responder: Connection closed", flush=True)
                    self.sock.close()
                    return
            except Exception as e:
                print(f"Video RPC Responder: Receive error - {e}", flush=True)
                break

            # Quick check for video request
            if needle not in data:
                continue

            # Extract message containing video_stream_request
            pos = data.find(needle)
            start = data.rfind(b'\x03', 0, pos)
            start = start + 1 if start != -1 else 0
            end = data.find(b'\x03', pos)
            if end == -1:
                continue

            try:
                msg = json.loads(data[start:end].decode('utf-8'))
                self._handle_message(msg)
            except json.JSONDecodeError:
                pass

        self.sock.close()

    def _handle_message(self, msg):
        """Handle an incoming RPC message"""
        method = msg.get('method', '')
        params = msg.get('params', {})

        # Look for video_stream_request in process_status_update
        if method == 'process_status_update':
            status = params.get('status', {})
            video_request = status.get('video_stream_request')

            if video_request:
                req_id = video_request.get('id', 0)
                req_method = video_request.get('method', '')
                # ONLY respond to start/stop - ignore routine polling
                if req_method in ('startLanCapture', 'stopLanCapture'):
                    print(f"Video RPC: {req_method}", flush=True)
                    self._send_video_reply(req_id, req_method)

    def _send_video_reply(self, req_id, req_method):
        """Send Video/VideoStreamReply response"""
        response = {
            "id": 0,
            "method": "Video/VideoStreamReply",
            "params": {
                "eventtime": 0,
                "status": {
                    "video_stream_reply": {
                        "id": req_id,
                        "method": req_method,
                        "result": {}
                    }
                }
            }
        }

        # Format as pretty JSON like gkcam does
        response_json = json.dumps(response, indent='\t')
        response_bytes = response_json.encode('utf-8') + b'\x03'

        try:
            self.sock.sendall(response_bytes)
        except Exception as e:
            print(f"Video RPC: Send error - {e}", flush=True)


# ============================================================================
# Local Binary API (port 18086)
# ============================================================================

def send_api_command(method, params=None, timeout=5):
    """Send command to local binary API on port 18086"""
    if params is None:
        params = {}

    msg = json.dumps({"id": 2016, "method": method, "params": params})
    # Add ETX (0x03) terminator
    msg_bytes = msg.encode('utf-8') + b'\x03'

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        sock.connect(('127.0.0.1', 18086))
        sock.sendall(msg_bytes)

        # Read response until ETX or timeout
        response = b''
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            response += chunk
            if b'\x03' in chunk:
                break

        sock.close()

        # Parse response (remove ETX)
        response = response.rstrip(b'\x03')
        if response:
            return json.loads(response.decode('utf-8', errors='ignore'))
        return None
    except Exception as e:
        print(f"API command {method} failed: {e}", flush=True)
        return None


def get_printer_status():
    """Get printer status - queries LAN mode"""
    return query_lan_mode_status()


def query_lan_mode_status():
    """Query LAN mode status using Printer/QueryLanPrintStatus"""
    result = send_api_command("Printer/QueryLanPrintStatus")
    print(f"LAN mode query response: {result}", flush=True)

    if result and 'result' in result:
        res = result['result']
        if isinstance(res, dict) and 'open' in res:
            return res['open'] == 1
    return False


def is_lan_mode_enabled():
    """Check if LAN mode is enabled"""
    return query_lan_mode_status()


def enable_lan_mode():
    """Enable LAN mode"""
    print("Enabling LAN mode...", flush=True)
    result = send_api_command("Printer/OpenLanPrint")
    if result:
        print(f"LAN mode enable response: {result}", flush=True)
    return result is not None


def disable_lan_mode():
    """Disable LAN mode"""
    print("Disabling LAN mode...", flush=True)
    result = send_api_command("Printer/CloseLanPrint")
    return result is not None


def start_camera_stream():
    """Start camera stream via binary API

    This tells gkcam to start capturing and encoding video.
    Without this, gkcam may be running but not streaming.
    """
    print("Starting camera stream...", flush=True)
    # Try VideoCapture/StartFluency - this starts the FLV stream
    result = send_api_command("VideoCapture/StartFluency")
    if result:
        print(f"Camera stream start response: {result}", flush=True)
        return True
    else:
        print("Camera stream start: no response (may already be running)", flush=True)
        return True  # Continue anyway, might already be streaming


def stop_camera_stream():
    """Stop camera stream - currently a no-op"""
    return True


# ============================================================================
# Camera Detection and Management
# ============================================================================

def find_camera_device(internal_usb_port=None):
    """Find USB camera device - prioritizes internal camera by USB port.

    Args:
        internal_usb_port: USB port for internal camera (e.g., "1.3").
                          If None, uses first available camera.

    The internal camera is typically at a fixed USB port (e.g., 1.3),
    while external cameras connect through a USB hub (e.g., 1.4.x).
    Using the USB path ensures we always get the internal camera,
    even when external cameras are connected.
    """
    by_path = "/dev/v4l/by-path"

    # First, try to find internal camera by USB port
    if internal_usb_port and os.path.exists(by_path):
        try:
            # Pattern: platform-xhci-hcd.0.auto-usb-0:1.3:1.0-video-index0
            port_pattern = f"usb-0:{internal_usb_port}:"
            for entry in sorted(os.listdir(by_path)):
                if port_pattern in entry and 'video-index0' in entry:
                    device = os.path.realpath(os.path.join(by_path, entry))
                    print(f"Found internal camera at USB port {internal_usb_port}: {entry} -> {device}", flush=True)
                    return device
            print(f"Internal camera not found at USB port {internal_usb_port}", flush=True)
        except Exception as e:
            print(f"Error scanning by-path for internal camera: {e}", flush=True)

    # Fallback: Check by-id symlinks (any camera)
    by_id_path = "/dev/v4l/by-id"
    if os.path.exists(by_id_path):
        try:
            for entry in os.listdir(by_id_path):
                # Look for video-index0 (main capture device, not metadata)
                if 'video-index0' in entry.lower():
                    device = os.path.realpath(os.path.join(by_id_path, entry))
                    print(f"Found camera (by-id): {entry} -> {device}", flush=True)
                    return device
        except Exception as e:
            print(f"Error scanning by-id: {e}", flush=True)

    # Last resort: Try common USB camera devices on RV1106 (video10-19 range)
    for i in list(range(10, 20)) + list(range(0, 10)):
        device = f"/dev/video{i}"
        if os.path.exists(device):
            # Quick check: try to open the device
            try:
                fd = os.open(device, os.O_RDONLY)
                os.close(fd)
                print(f"Found accessible video device: {device}", flush=True)
                return device
            except Exception:
                continue

    print("WARNING: No camera device found", flush=True)
    return None


def detect_camera_resolution(device):
    """Detect camera's best MJPEG resolution"""
    # Default resolution (common USB camera resolution)
    default_width, default_height = 1280, 720

    # Try v4l2-ctl if available
    try:
        result = subprocess.run(
            ['v4l2-ctl', '-d', device, '--list-formats-ext'],
            capture_output=True, text=True, timeout=5
        )

        if result.returncode == 0:
            # Parse MJPEG formats
            best_width, best_height = default_width, default_height
            in_mjpeg = False

            for line in result.stdout.split('\n'):
                if 'MJPEG' in line or 'Motion-JPEG' in line:
                    in_mjpeg = True
                elif in_mjpeg and 'Size:' in line:
                    match = re.search(r'(\d+)x(\d+)', line)
                    if match:
                        w, h = int(match.group(1)), int(match.group(2))
                        # Prefer 720p or higher
                        if w >= 1280 and h >= 720:
                            best_width, best_height = w, h
                            break
                elif in_mjpeg and line.strip() and not line.startswith('\t'):
                    in_mjpeg = False

            print(f"Camera resolution (v4l2-ctl): {best_width}x{best_height}", flush=True)
            return best_width, best_height
    except FileNotFoundError:
        print("v4l2-ctl not found, using default resolution", flush=True)
    except Exception as e:
        print(f"v4l2-ctl failed: {e}", flush=True)

    # Fallback: the encoder will negotiate with the camera
    print(f"Using default camera resolution: {default_width}x{default_height}", flush=True)
    return default_width, default_height


def detect_camera_max_fps(device, width=1280, height=720):
    """Detect camera's max MJPEG framerate at given resolution"""
    default_fps = 30  # Most USB cameras support at least 30fps MJPEG

    # Try v4l2-ctl if available
    try:
        result = subprocess.run(
            ['v4l2-ctl', '-d', device, '--list-formats-ext'],
            capture_output=True, text=True, timeout=5
        )

        if result.returncode == 0:
            max_fps = default_fps
            in_mjpeg = False
            found_resolution = False

            for line in result.stdout.split('\n'):
                if 'MJPEG' in line or 'Motion-JPEG' in line:
                    in_mjpeg = True
                    found_resolution = False
                elif in_mjpeg and 'Size:' in line:
                    match = re.search(r'(\d+)x(\d+)', line)
                    if match:
                        w, h = int(match.group(1)), int(match.group(2))
                        found_resolution = (w == width and h == height)
                elif in_mjpeg and found_resolution and 'Interval' in line:
                    # Parse framerate from interval line
                    # Format: "Interval: Discrete 0.033s (30.000 fps)"
                    match = re.search(r'\((\d+(?:\.\d+)?)\s*fps\)', line)
                    if match:
                        fps = float(match.group(1))
                        max_fps = max(max_fps, int(fps))
                elif in_mjpeg and line.strip() and not line.startswith('\t') and 'Interval' not in line:
                    in_mjpeg = False
                    found_resolution = False

            print(f"Camera max FPS at {width}x{height} (v4l2-ctl): {max_fps}", flush=True)
            return max_fps
    except FileNotFoundError:
        print("v4l2-ctl not found, using default max FPS", flush=True)
    except Exception as e:
        print(f"v4l2-ctl failed for FPS detection: {e}", flush=True)

    # Fallback
    print(f"Using default max FPS: {default_fps}", flush=True)
    return default_fps


def kill_gkcam():
    """Kill gkcam process"""
    print("Killing gkcam...", flush=True)
    try:
        subprocess.run(
            ["sh", "-c", ". /useremain/rinkhals/.current/tools.sh && kill_by_name gkcam"],
            timeout=5
        )
        time.sleep(2)
        return True
    except Exception as e:
        print(f"Error killing gkcam: {e}", flush=True)
        return False


# ============================================================================
# IP Address Detection
# ============================================================================

def get_ip_address():
    """Get the printer's IP address"""
    interfaces = ['eth1', 'wlan0']

    try:
        result = subprocess.run(['ifconfig'], capture_output=True, text=True, timeout=5)
        output = result.stdout

        for iface in interfaces:
            pattern = rf'{iface}.*?(?=\n\w|\Z)'
            match = re.search(pattern, output, re.DOTALL)
            if match:
                iface_section = match.group(0)
                if 'UP' not in iface_section:
                    continue
                ip_match = re.search(r'inet addr:(\d+\.\d+\.\d+\.\d+)', iface_section)
                if ip_match:
                    ip = ip_match.group(1)
                    if ip != '127.0.0.1':
                        return ip
    except Exception as e:
        print(f"Error getting IP: {e}", flush=True)

    return None


# ============================================================================
# Moonraker Integration
# ============================================================================

def update_moonraker_webcam_config(ip_address, port=8080, target_fps=10):
    """Update moonraker webcam configuration"""
    if not ip_address:
        return False

    stream_url = f"http://{ip_address}:{port}/stream"
    snapshot_url = f"http://{ip_address}:{port}/snapshot"
    webcam_name = "USB Camera"

    try:
        webcam_data = {
            "name": webcam_name,
            "enabled": True,
            "service": "mjpegstreamer-adaptive",
            "target_fps": target_fps,
            "target_fps_idle": 1,
            "stream_url": stream_url,
            "snapshot_url": snapshot_url,
            "rotation": 0,
            "flip_horizontal": False,
            "flip_vertical": False
        }

        req = urllib.request.Request(
            "http://127.0.0.1:7125/server/webcams/item",
            data=json.dumps(webcam_data).encode('utf-8'),
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        with urllib.request.urlopen(req, timeout=5):
            print(f"Updated moonraker webcam: {stream_url}", flush=True)
    except Exception as e:
        print(f"Moonraker API update failed: {e}", flush=True)
        return False

    # Set as default webcam
    try:
        mainsail_data = {
            "namespace": "mainsail",
            "key": "view.webcam.currentCam",
            "value": {"dashboard": webcam_name, "page": webcam_name}
        }

        req = urllib.request.Request(
            "http://127.0.0.1:7125/server/database/item",
            data=json.dumps(mainsail_data).encode('utf-8'),
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        with urllib.request.urlopen(req, timeout=5):
            pass
    except Exception:
        pass

    return True


# ============================================================================
# CPU Usage Monitoring
# ============================================================================

def get_cpu_usage():
    """Get total CPU usage from /proc/stat (all fields including irq/softirq)"""
    try:
        with open('/proc/stat', 'r') as f:
            line = f.readline()
        parts = line.split()
        if parts[0] == 'cpu':
            # Format: cpu user nice system idle iowait irq softirq steal guest guest_nice
            values = list(map(int, parts[1:]))
            # Pad with zeros if fewer fields (older kernels)
            while len(values) < 10:
                values.append(0)
            user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice = values[:10]
            # Total includes all time (active + idle)
            total = user + nice + system + idle + iowait + irq + softirq + steal
            # Active excludes idle and iowait (guest is already in user)
            active = user + nice + system + irq + softirq + steal
            return active, total
    except Exception:
        pass
    return 0, 1


def parse_stat_file(content):
    """Parse /proc/*/stat file, handling comm field with spaces"""
    # Find closing paren to skip comm field (which may contain spaces)
    end_comm = content.rfind(')')
    if end_comm == -1:
        return None
    # Fields after comm: state ppid pgrp session tty_nr tpgid flags
    #                    minflt cminflt majflt cmajflt utime stime cutime cstime ...
    # Indices after split: 0=state, 11=utime, 12=stime
    fields = content[end_comm+2:].split()
    if len(fields) < 13:
        return None
    return fields


def get_process_cpu_usage(pid):
    """Get CPU usage for a specific process (sum of all threads)"""
    total_time = 0
    try:
        # Sum CPU time for all threads in the process
        task_dir = f'/proc/{pid}/task'
        if os.path.isdir(task_dir):
            for tid in os.listdir(task_dir):
                try:
                    with open(f'{task_dir}/{tid}/stat', 'r') as f:
                        content = f.read()
                    fields = parse_stat_file(content)
                    if fields:
                        utime = int(fields[11])  # utime is 14th field, index 11 after comm
                        stime = int(fields[12])  # stime is 15th field, index 12 after comm
                        total_time += utime + stime
                except Exception:
                    continue
        else:
            # Fallback to main process stat
            with open(f'/proc/{pid}/stat', 'r') as f:
                content = f.read()
            fields = parse_stat_file(content)
            if fields:
                total_time = int(fields[11]) + int(fields[12])
    except Exception:
        pass
    return total_time


def find_gkcam_pids():
    """Find all PIDs of gkcam processes using psutil"""
    pids = []
    try:
        import psutil
        for proc in psutil.process_iter(['pid', 'name']):
            try:
                if proc.info['name'] == 'gkcam':
                    pids.append(proc.info['pid'])
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                continue
    except ImportError:
        # Fallback to /proc parsing if psutil not available
        try:
            for pid in os.listdir('/proc'):
                if not pid.isdigit():
                    continue
                try:
                    with open(f'/proc/{pid}/comm', 'r') as f:
                        comm = f.read().strip()
                    if comm == 'gkcam':
                        pids.append(int(pid))
                except Exception:
                    continue
        except Exception:
            pass
    return pids


class CPUMonitor:
    """Monitor CPU usage using psutil's cpu_percent() for accurate measurements.

    Uses psutil's built-in cpu_percent() method which properly handles thread
    aggregation and timing internally.
    """

    def __init__(self):
        try:
            import psutil
            self.psutil = psutil
            self.has_psutil = True
        except ImportError:
            self.psutil = None
            self.has_psutil = False
            print("Warning: psutil not available, CPU monitoring disabled", flush=True)

        self.total_cpu = 0.0
        self.encoder_cpu = 0.0
        self.streamer_cpu = 0.0
        self.proc_cpu = {}
        self.processes = {}   # pid -> psutil.Process object
        self.lock = threading.Lock()

    def _get_process_cpu(self, pid):
        """Get CPU percentage for a process using psutil's cpu_percent().

        Returns CPU % as percentage of one CPU (0-100 per core).
        First call for a new process returns 0.0 (psutil needs two samples).
        """
        try:
            if pid not in self.processes:
                proc = self.psutil.Process(pid)
                # First call to cpu_percent() initializes internal tracking
                # It returns 0.0 but sets up the baseline for next call
                proc.cpu_percent(interval=None)
                self.processes[pid] = proc
                return 0.0

            # Subsequent calls return actual CPU percentage since last call
            return self.processes[pid].cpu_percent(interval=None)

        except (self.psutil.NoSuchProcess, self.psutil.AccessDenied):
            if pid in self.processes:
                del self.processes[pid]
            return 0.0

    def update(self, encoder_pids=None, streamer_pid=None):
        """Update CPU readings using psutil cpu_percent()"""
        if not self.has_psutil:
            return

        if encoder_pids is None:
            encoder_pids = []

        with self.lock:
            # Get total system CPU
            self.total_cpu = self.psutil.cpu_percent(interval=None)

            # Track encoder PIDs (sum all encoder processes)
            encoder_cpu_total = 0.0
            for pid in encoder_pids:
                cpu = self._get_process_cpu(pid)
                self.proc_cpu[pid] = cpu
                encoder_cpu_total += cpu

            self.encoder_cpu = encoder_cpu_total

            # Track streamer PID (h264_server.py - includes all threads)
            if streamer_pid:
                cpu = self._get_process_cpu(streamer_pid)
                self.proc_cpu[streamer_pid] = cpu
                self.streamer_cpu = cpu
            else:
                self.streamer_cpu = 0.0

            # Clean up old PIDs no longer being tracked
            current_pids = set(encoder_pids)
            if streamer_pid:
                current_pids.add(streamer_pid)

            old_pids = set(self.processes.keys()) - current_pids
            for pid in old_pids:
                if pid in self.processes:
                    del self.processes[pid]
                if pid in self.proc_cpu:
                    del self.proc_cpu[pid]

    def get_stats(self):
        """Get current CPU stats"""
        with self.lock:
            return {
                'total': round(self.total_cpu, 1),
                'encoder_cpu': round(self.encoder_cpu, 1),
                'streamer_cpu': round(self.streamer_cpu, 1),
                'processes': {str(k): round(v, 1) for k, v in self.proc_cpu.items()}
            }


# ============================================================================
# Moonraker WebSocket Client for Timelapse
# ============================================================================

class MoonrakerClient:
    """WebSocket client for Moonraker to monitor print status and trigger timelapse.

    Connects to Moonraker's WebSocket API and subscribes to print_stats to detect:
    - Print start (state: printing)
    - Layer changes (print_stats.info.current_layer)
    - Print complete (state: complete)
    - Print cancel (state: cancelled/error)

    Uses a simple WebSocket implementation with standard library to avoid
    dependencies that may not be available on the printer.
    """

    def __init__(self, host='127.0.0.1', port=7125):
        self.host = host
        self.port = port
        self.sock = None
        self.connected = False
        self.running = False
        self.thread = None
        self.lock = threading.Lock()

        # Print state tracking
        self.print_state = 'standby'  # standby, printing, paused, complete, cancelled, error
        self.current_layer = 0
        self.total_layers = 0
        self.filename = None
        self.print_duration = 0

        # JSON-RPC request ID counter
        self.request_id = 1

        # Callbacks
        self.on_print_start = None      # callback(filename)
        self.on_layer_change = None     # callback(layer, total_layers)
        self.on_print_complete = None   # callback(filename)
        self.on_print_cancel = None     # callback(filename, reason)
        self.on_connect = None          # callback()
        self.on_disconnect = None       # callback(reason)

        # Reconnect settings
        self.reconnect_delay = 5  # seconds between reconnect attempts
        self.max_reconnect_attempts = 0  # 0 = unlimited

    def _ws_handshake(self):
        """Perform WebSocket handshake"""
        import hashlib
        import base64

        # Generate random key
        key = base64.b64encode(os.urandom(16)).decode('utf-8')

        # Send HTTP upgrade request
        request = (
            f"GET /websocket HTTP/1.1\r\n"
            f"Host: {self.host}:{self.port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n"
            f"\r\n"
        )
        self.sock.sendall(request.encode('utf-8'))

        # Read response
        response = b''
        while b'\r\n\r\n' not in response:
            chunk = self.sock.recv(1024)
            if not chunk:
                raise ConnectionError("Connection closed during handshake")
            response += chunk

        # Verify response
        response_str = response.decode('utf-8', errors='ignore')
        if '101' not in response_str:
            raise ConnectionError(f"WebSocket handshake failed: {response_str[:100]}")

        return True

    def _ws_send(self, data):
        """Send a WebSocket text frame"""
        payload = data.encode('utf-8')
        length = len(payload)

        # Build frame header
        frame = bytearray()
        frame.append(0x81)  # Text frame, FIN=1

        # Mask bit must be set for client-to-server frames
        if length < 126:
            frame.append(0x80 | length)
        elif length < 65536:
            frame.append(0x80 | 126)
            frame.extend(struct.pack('>H', length))
        else:
            frame.append(0x80 | 127)
            frame.extend(struct.pack('>Q', length))

        # Generate masking key and mask payload
        mask = os.urandom(4)
        frame.extend(mask)

        masked = bytearray(length)
        for i in range(length):
            masked[i] = payload[i] ^ mask[i % 4]
        frame.extend(masked)

        self.sock.sendall(bytes(frame))

    def _ws_recv(self, timeout=None):
        """Receive a WebSocket frame, returns (opcode, payload) or (None, None) on timeout"""
        if timeout:
            self.sock.settimeout(timeout)
        else:
            self.sock.settimeout(None)

        try:
            # Read frame header
            header = self.sock.recv(2)
            if len(header) < 2:
                return (None, None)

            opcode = header[0] & 0x0F
            masked = (header[1] & 0x80) != 0
            length = header[1] & 0x7F

            # Extended length
            if length == 126:
                ext = self.sock.recv(2)
                length = struct.unpack('>H', ext)[0]
            elif length == 127:
                ext = self.sock.recv(8)
                length = struct.unpack('>Q', ext)[0]

            # Masking key (server frames typically not masked, but handle it)
            mask = None
            if masked:
                mask = self.sock.recv(4)

            # Read payload
            payload = b''
            while len(payload) < length:
                chunk = self.sock.recv(min(4096, length - len(payload)))
                if not chunk:
                    break
                payload += chunk

            # Unmask if needed
            if mask:
                unmasked = bytearray(len(payload))
                for i in range(len(payload)):
                    unmasked[i] = payload[i] ^ mask[i % 4]
                payload = bytes(unmasked)

            return (opcode, payload)

        except socket.timeout:
            return (None, None)

    def _send_jsonrpc(self, method, params=None):
        """Send a JSON-RPC 2.0 request"""
        with self.lock:
            req_id = self.request_id
            self.request_id += 1

        request = {
            "jsonrpc": "2.0",
            "method": method,
            "id": req_id
        }
        if params is not None:
            request["params"] = params

        self._ws_send(json.dumps(request))
        return req_id

    def _subscribe_print_stats(self):
        """Subscribe to print_stats object updates"""
        # Subscribe to print_stats with all fields
        self._send_jsonrpc("printer.objects.subscribe", {
            "objects": {
                "print_stats": None  # None = all fields
            }
        })

    def _handle_status_update(self, data):
        """Handle notify_status_update notifications from Moonraker"""
        if 'print_stats' not in data:
            return

        stats = data['print_stats']
        old_state = self.print_state
        old_layer = self.current_layer

        # Update state
        if 'state' in stats:
            self.print_state = stats['state']

        if 'filename' in stats:
            self.filename = stats['filename']

        if 'print_duration' in stats:
            self.print_duration = stats['print_duration']

        # Layer info is nested in 'info' dict
        if 'info' in stats and stats['info']:
            info = stats['info']
            if 'current_layer' in info:
                self.current_layer = info['current_layer'] or 0
            if 'total_layer' in info:
                self.total_layers = info['total_layer'] or 0

        # Trigger callbacks based on state changes
        if old_state != 'printing' and self.print_state == 'printing':
            # Print started
            if self.on_print_start:
                try:
                    self.on_print_start(self.filename)
                except Exception as e:
                    print(f"MoonrakerClient: on_print_start callback error: {e}", flush=True)

        elif self.print_state == 'printing' and self.current_layer != old_layer:
            # Layer changed
            if self.on_layer_change:
                try:
                    self.on_layer_change(self.current_layer, self.total_layers)
                except Exception as e:
                    print(f"MoonrakerClient: on_layer_change callback error: {e}", flush=True)

        elif old_state == 'printing' and self.print_state == 'complete':
            # Print completed
            if self.on_print_complete:
                try:
                    self.on_print_complete(self.filename)
                except Exception as e:
                    print(f"MoonrakerClient: on_print_complete callback error: {e}", flush=True)

        elif old_state == 'printing' and self.print_state in ('cancelled', 'error'):
            # Print cancelled or errored
            if self.on_print_cancel:
                try:
                    self.on_print_cancel(self.filename, self.print_state)
                except Exception as e:
                    print(f"MoonrakerClient: on_print_cancel callback error: {e}", flush=True)

    def _run_loop(self):
        """Main WebSocket receive loop"""
        reconnect_attempts = 0

        while self.running:
            try:
                # Connect
                self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.sock.settimeout(10)
                self.sock.connect((self.host, self.port))

                # WebSocket handshake
                self._ws_handshake()
                self.connected = True
                reconnect_attempts = 0
                print(f"MoonrakerClient: Connected to ws://{self.host}:{self.port}/websocket", flush=True)

                if self.on_connect:
                    try:
                        self.on_connect()
                    except Exception:
                        pass

                # Subscribe to print_stats
                self._subscribe_print_stats()

                # Receive loop
                while self.running and self.connected:
                    opcode, payload = self._ws_recv(timeout=30)

                    if opcode is None:
                        # Timeout - send ping
                        continue

                    if opcode == 0x08:
                        # Close frame
                        print("MoonrakerClient: Server closed connection", flush=True)
                        break

                    if opcode == 0x09:
                        # Ping - send pong
                        self._ws_send_pong(payload)
                        continue

                    if opcode == 0x01:
                        # Text frame
                        try:
                            msg = json.loads(payload.decode('utf-8'))

                            # Handle JSON-RPC notifications
                            if msg.get('method') == 'notify_status_update':
                                params = msg.get('params', [])
                                if params:
                                    self._handle_status_update(params[0])

                            # Handle subscription response with initial state
                            elif 'result' in msg and 'status' in msg.get('result', {}):
                                status = msg['result']['status']
                                self._handle_status_update(status)

                        except json.JSONDecodeError:
                            pass

            except Exception as e:
                print(f"MoonrakerClient: Connection error: {e}", flush=True)

            finally:
                self.connected = False
                if self.sock:
                    try:
                        self.sock.close()
                    except Exception:
                        pass
                    self.sock = None

                if self.on_disconnect:
                    try:
                        self.on_disconnect(str(e) if 'e' in dir() else "Unknown")
                    except Exception:
                        pass

            # Reconnect if still running
            if self.running:
                reconnect_attempts += 1
                if self.max_reconnect_attempts > 0 and reconnect_attempts >= self.max_reconnect_attempts:
                    print(f"MoonrakerClient: Max reconnect attempts reached, stopping", flush=True)
                    break

                print(f"MoonrakerClient: Reconnecting in {self.reconnect_delay}s (attempt {reconnect_attempts})...", flush=True)
                time.sleep(self.reconnect_delay)

    def _ws_send_pong(self, payload):
        """Send a WebSocket pong frame"""
        length = len(payload)
        frame = bytearray()
        frame.append(0x8A)  # Pong frame, FIN=1

        if length < 126:
            frame.append(0x80 | length)
        else:
            frame.append(0x80 | 126)
            frame.extend(struct.pack('>H', length))

        mask = os.urandom(4)
        frame.extend(mask)

        masked = bytearray(length)
        for i in range(length):
            masked[i] = payload[i] ^ mask[i % 4]
        frame.extend(masked)

        self.sock.sendall(bytes(frame))

    def start(self):
        """Start the WebSocket client in a background thread"""
        if self.running:
            return

        self.running = True
        self.thread = threading.Thread(target=self._run_loop, daemon=True)
        self.thread.start()

    def stop(self):
        """Stop the WebSocket client"""
        self.running = False
        self.connected = False

        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass

        if self.thread:
            self.thread.join(timeout=2)
            self.thread = None

    def is_connected(self):
        """Check if connected to Moonraker"""
        return self.connected

    def get_print_state(self):
        """Get current print state info"""
        return {
            'state': self.print_state,
            'filename': self.filename,
            'current_layer': self.current_layer,
            'total_layers': self.total_layers,
            'print_duration': self.print_duration
        }


# ============================================================================
# FLV Muxer for H.264 stream
# ============================================================================

class FLVMuxer:
    """Mux H.264 NAL units into FLV format"""

    def __init__(self, width, height, fps, initial_sps=None, initial_pps=None):
        self.width = width
        self.height = height
        self.fps = fps
        self.timestamp = 0
        self.frame_duration = 1000 // fps  # ms per frame
        self.has_sps_pps = False
        self.sps = initial_sps
        self.pps = initial_pps

    def create_header(self):
        """Create FLV file header"""
        header = bytearray()
        # FLV signature
        header.extend(b'FLV')
        # Version
        header.append(1)
        # Flags: has video (0x01)
        header.append(0x01)
        # Header length (9 bytes)
        header.extend(struct.pack('>I', 9))
        # PreviousTagSize0
        header.extend(struct.pack('>I', 0))
        return bytes(header)

    def create_metadata(self):
        """Create FLV onMetaData script tag (AMF encoded)"""
        # AMF0 encoding of onMetaData event
        data = bytearray()

        # String type (0x02) + "onMetaData"
        data.append(0x02)
        data.extend(struct.pack('>H', 10))  # string length
        data.extend(b'onMetaData')

        # ECMA array type (0x08)
        data.append(0x08)
        data.extend(struct.pack('>I', 8))  # approx number of elements

        # width (number)
        data.extend(struct.pack('>H', 5))
        data.extend(b'width')
        data.append(0x00)  # number type
        data.extend(struct.pack('>d', float(self.width)))

        # height (number)
        data.extend(struct.pack('>H', 6))
        data.extend(b'height')
        data.append(0x00)
        data.extend(struct.pack('>d', float(self.height)))

        # framerate (number)
        data.extend(struct.pack('>H', 9))
        data.extend(b'framerate')
        data.append(0x00)
        data.extend(struct.pack('>d', float(self.fps)))

        # videocodecid (number) - 7 = AVC/H.264
        data.extend(struct.pack('>H', 12))
        data.extend(b'videocodecid')
        data.append(0x00)
        data.extend(struct.pack('>d', 7.0))

        # duration (number) - 0 for live stream
        data.extend(struct.pack('>H', 8))
        data.extend(b'duration')
        data.append(0x00)
        data.extend(struct.pack('>d', 0.0))

        # encoder (string)
        data.extend(struct.pack('>H', 7))
        data.extend(b'encoder')
        data.append(0x02)  # string type
        encoder_str = b'h264-streamer'
        data.extend(struct.pack('>H', len(encoder_str)))
        data.extend(encoder_str)

        # End of object (0x00 0x00 0x09)
        data.extend(b'\x00\x00\x09')

        return self.create_tag(18, data, 0)  # Script data tag type = 18

    def create_tag(self, tag_type, data, timestamp):
        """Create an FLV tag"""
        tag = bytearray()
        # Tag type
        tag.append(tag_type)
        # Data size (3 bytes)
        data_size = len(data)
        tag.append((data_size >> 16) & 0xFF)
        tag.append((data_size >> 8) & 0xFF)
        tag.append(data_size & 0xFF)
        # Timestamp (3 bytes lower + 1 byte upper)
        tag.append((timestamp >> 16) & 0xFF)
        tag.append((timestamp >> 8) & 0xFF)
        tag.append(timestamp & 0xFF)
        tag.append((timestamp >> 24) & 0xFF)
        # Stream ID (always 0)
        tag.extend(b'\x00\x00\x00')
        # Data
        tag.extend(data)
        # PreviousTagSize
        tag.extend(struct.pack('>I', len(tag)))
        return bytes(tag)

    def parse_nal_units(self, h264_data):
        """Parse H.264 Annex B data into NAL units (optimized)"""
        nals = []
        data = memoryview(h264_data) if isinstance(h264_data, bytes) else h264_data
        data_len = len(data)
        i = 0

        while i < data_len - 4:
            # Find start code using bytes find (faster than slicing)
            start_code_len = 0
            if data[i:i+4] == RE_NAL_START_4:
                start_code_len = 4
            elif data[i:i+3] == RE_NAL_START_3:
                start_code_len = 3

            if start_code_len == 0:
                i += 1
                continue

            start = i + start_code_len

            # Find next start code using find() - much faster than loop
            # Convert memoryview slice to bytes for find
            remaining = bytes(data[start:])
            idx4 = remaining.find(RE_NAL_START_4)
            idx3 = remaining.find(RE_NAL_START_3)

            # Get the earlier of the two
            if idx4 == -1:
                end_offset = idx3
            elif idx3 == -1:
                end_offset = idx4
            else:
                end_offset = min(idx4, idx3)

            if end_offset == -1:
                # No more start codes, take rest of data
                nal = bytes(data[start:])
            else:
                nal = bytes(data[start:start + end_offset])

            if nal:
                nals.append(nal)

            if end_offset == -1:
                break
            i = start + end_offset

        return nals

    def create_avc_decoder_config(self, sps, pps):
        """Create AVC decoder configuration record"""
        config = bytearray()
        config.append(0x01)  # configurationVersion
        config.append(sps[1])  # AVCProfileIndication
        config.append(sps[2])  # profile_compatibility
        config.append(sps[3])  # AVCLevelIndication
        config.append(0xFF)  # 6 bits reserved + 2 bits NAL length size - 1 (3 = 4 bytes)
        config.append(0xE1)  # 3 bits reserved + 5 bits SPS count (1)
        # SPS length and data
        config.extend(struct.pack('>H', len(sps)))
        config.extend(sps)
        # PPS count
        config.append(0x01)
        # PPS length and data
        config.extend(struct.pack('>H', len(pps)))
        config.extend(pps)
        return bytes(config)

    def mux_frame(self, h264_data):
        """Mux H.264 data into FLV tags"""
        tags = []
        nals = self.parse_nal_units(h264_data)

        for nal in nals:
            if not nal:
                continue

            nal_type = nal[0] & 0x1F

            # SPS
            if nal_type == 7:
                self.sps = nal
            # PPS
            elif nal_type == 8:
                self.pps = nal

        # Send AVC decoder config if we have SPS/PPS
        if self.sps and self.pps and not self.has_sps_pps:
            config = self.create_avc_decoder_config(self.sps, self.pps)
            video_data = bytearray()
            video_data.append(0x17)  # keyframe + AVC
            video_data.append(0x00)  # AVC sequence header
            video_data.extend(b'\x00\x00\x00')  # composition time
            video_data.extend(config)
            tags.append(self.create_tag(9, video_data, 0))
            self.has_sps_pps = True

        # Video NALUs
        video_nals = [nal for nal in nals if nal and (nal[0] & 0x1F) not in (7, 8)]
        if video_nals:
            is_keyframe = any((nal[0] & 0x1F) == 5 for nal in video_nals)

            video_data = bytearray()
            video_data.append(0x17 if is_keyframe else 0x27)  # frame type + AVC
            video_data.append(0x01)  # AVC NALU
            video_data.extend(b'\x00\x00\x00')  # composition time

            for nal in video_nals:
                video_data.extend(struct.pack('>I', len(nal)))
                video_data.extend(nal)

            tags.append(self.create_tag(9, video_data, self.timestamp))
            self.timestamp += self.frame_duration

        return b''.join(tags)


# ============================================================================
# Gkcam FLV Reader (for gkcam encoder mode)
# ============================================================================

# H.264 start code
H264_START_CODE = b'\x00\x00\x00\x01'

# Path to ffmpeg binary and library path
FFMPEG_PATH = '/ac_lib/lib/third_bin/ffmpeg'
FFMPEG_LD_PATH = '/ac_lib/lib/third_lib:/userdata/app/gk'


def check_gkcam_flv(timeout=5):
    """Check if gkcam's /flv endpoint is responding with FLV data"""
    try:
        # First, quick HTTP check - just see if we can connect and read FLV header
        req = urllib.request.Request("http://127.0.0.1:18088/flv")
        with urllib.request.urlopen(req, timeout=timeout) as response:
            # Read first 16 bytes - should contain FLV header
            data = response.read(16)
            if data and len(data) >= 3 and data[:3] == b'FLV':
                return True
            else:
                print(f"  gkcam: unexpected response ({len(data) if data else 0} bytes)", flush=True)
                return False
    except urllib.error.URLError as e:
        print(f"  gkcam: {e.reason}", flush=True)
        return False
    except socket.timeout:
        print(f"  gkcam: timeout", flush=True)
        return False
    except Exception as e:
        print(f"  gkcam: {e}", flush=True)
        return False


def ensure_gkcam_running():
    """Ensure gkcam is running and responding"""
    print("Checking gkcam FLV stream...", flush=True)
    if check_gkcam_flv():
        print("gkcam already responding", flush=True)
        return True

    print("gkcam not responding, attempting to start...", flush=True)

    try:
        # Start gkcam process
        subprocess.Popen(
            ["sh", "-c",
             "cd /userdata/app/gk && "
             "LD_LIBRARY_PATH=/userdata/app/gk:$LD_LIBRARY_PATH "
             "./gkcam >> /tmp/rinkhals/gkcam.log 2>&1"],
            start_new_session=True
        )

        # Give gkcam a moment to initialize
        time.sleep(3)

        # Send startCapture via MQTT to start the camera stream
        mqtt_send_start_camera()

        # Wait for gkcam to start serving the FLV stream
        for i in range(30):
            time.sleep(1)
            if check_gkcam_flv(timeout=3):
                print(f"gkcam started successfully after {i+4}s", flush=True)
                return True
            if (i + 1) % 5 == 0:
                print(f"Waiting for gkcam stream... ({i+4}s)", flush=True)

        print("gkcam failed to start after 33s", flush=True)
        return False

    except Exception as e:
        print(f"Error starting gkcam: {e}", flush=True)
        return False


# Cached environment for ffmpeg (avoid re-creating dict on each decode)
_ffmpeg_env = None

def _get_ffmpeg_env():
    """Get cached ffmpeg environment with required library paths"""
    global _ffmpeg_env
    if _ffmpeg_env is None:
        _ffmpeg_env = os.environ.copy()
        _ffmpeg_env['LD_LIBRARY_PATH'] = '/ac_lib/lib/third_lib:/userdata/app/gk:' + _ffmpeg_env.get('LD_LIBRARY_PATH', '')
    return _ffmpeg_env


def decode_h264_to_jpeg(h264_data):
    """Decode H.264 data to JPEG using ffmpeg (single frame).

    Spawns a new ffmpeg process for each decode operation. While this has
    process spawn overhead (~100-200ms), it's reliable and handles any
    valid H.264 frame without buffering issues.
    """
    if not h264_data:
        return None

    try:
        cmd = [
            FFMPEG_PATH,
            '-f', 'h264',
            '-i', 'pipe:0',
            '-frames:v', '1',
            '-f', 'mjpeg',
            '-q:v', '9',
            'pipe:1'
        ]

        process = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=_get_ffmpeg_env()
        )

        jpeg_data, _ = process.communicate(input=h264_data, timeout=3)

        if jpeg_data and len(jpeg_data) > 100:
            return jpeg_data

    except subprocess.TimeoutExpired:
        try:
            process.kill()
            process.wait()
        except Exception:
            pass
    except Exception as e:
        print(f"ffmpeg decode error: {e}", flush=True)

    return None


class GkcamStreamingTranscoder:
    """Transcodes gkcam FLV stream to MJPEG using ffmpeg.

    Uses ffmpeg in streaming mode to maintain decoder state, allowing
    proper decoding of P-frames (inter-frames) which depend on previous frames.
    """

    JPEG_START = b'\xff\xd8'
    JPEG_END = b'\xff\xd9'

    def __init__(self, url="http://127.0.0.1:18088/flv"):
        self.url = url
        self.process = None
        self.running = False
        self.current_jpeg = None
        self.jpeg_time = 0
        self.jpeg_lock = threading.Lock()
        self.frame_event = threading.Event()
        self.fps_counter = FPSCounter()

    def start(self):
        """Start the ffmpeg transcoder process"""
        if self.running:
            return True

        self.running = True
        threading.Thread(target=self._run_transcoder, daemon=True).start()
        return True

    def stop(self):
        """Stop the transcoder"""
        self.running = False
        if self.process:
            try:
                self.process.terminate()
                self.process.wait(timeout=2)
            except Exception:
                try:
                    self.process.kill()
                except Exception:
                    pass
            self.process = None

    def _run_transcoder(self):
        """Main transcoder loop - restarts ffmpeg on failures"""
        while self.running:
            try:
                self._transcode_stream()
            except Exception as e:
                if self.running:
                    print(f"Transcoder error: {e}, restarting in 2s...", flush=True)
                    time.sleep(2)

    def _transcode_stream(self):
        """Run ffmpeg to transcode FLV to MJPEG"""
        env = _get_ffmpeg_env()

        cmd = [
            FFMPEG_PATH,
            '-y',
            '-fflags', '+nobuffer+flush_packets',
            '-flags', 'low_delay',
            '-probesize', '32768',
            '-analyzeduration', '100000',
            '-threads', '1',
            '-loglevel', 'error',
            '-i', self.url,
            '-vf', 'fps=3',  # Limit FPS to reduce CPU
            '-f', 'mjpeg',
            '-q:v', '9',
            'pipe:1'
        ]

        self.process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=env,
            bufsize=0
        )

        print(f"Started streaming transcoder from {self.url}", flush=True)
        buffer = bytearray()

        while self.running and self.process.poll() is None:
            try:
                chunk = self.process.stdout.read(8192)
                if not chunk:
                    break

                buffer.extend(chunk)

                # Parse complete JPEG frames
                while True:
                    start_idx = buffer.find(self.JPEG_START)
                    if start_idx == -1:
                        if len(buffer) > 1:
                            del buffer[:-1]
                        break

                    if start_idx > 0:
                        del buffer[:start_idx]

                    end_idx = buffer.find(self.JPEG_END, 2)
                    if end_idx == -1:
                        break

                    # Extract complete JPEG
                    jpeg_data = bytes(buffer[:end_idx + 2])
                    del buffer[:end_idx + 2]

                    # Update current frame
                    with self.jpeg_lock:
                        self.current_jpeg = jpeg_data
                        self.jpeg_time = time.time()
                    self.fps_counter.tick()
                    self.frame_event.set()

            except Exception as e:
                if self.running:
                    print(f"Transcoder read error: {e}", flush=True)
                break

        self.process = None
        print("Streaming transcoder stopped", flush=True)

    def get_jpeg(self, max_age=0.5):
        """Get current JPEG frame"""
        with self.jpeg_lock:
            if self.current_jpeg and (time.time() - self.jpeg_time) < max_age:
                return self.current_jpeg
        return None

    def get_frame(self):
        """Get current JPEG frame (alias for compatibility)"""
        return self.get_jpeg(max_age=1.0)


class GkcamFLVReader:
    """Reads and parses FLV stream from gkcam, extracts H.264 NAL units.

    Used for keyframe-only mode where we only need to decode IDR frames.
    For all-frames mode, use GkcamStreamingTranscoder instead.
    """

    def __init__(self, url="http://127.0.0.1:18088/flv", decode_all_frames=False):
        self.url = url
        self.decode_all_frames = decode_all_frames
        self.running = False
        self.sps = None
        self.pps = None
        self.last_keyframe = None
        self.last_keyframe_time = 0
        self.last_frame = None  # For keyframe-only mode (not used in all-frames)
        self.last_frame_time = 0
        self.connected = False
        self.reconnect_delay = 2
        self._current_response = None
        self._reconnect_flag = False

        # Frame event for streaming
        self.frame_event = threading.Event()
        self.frame_lock = threading.Lock()

        # JPEG cache
        self.jpeg_cache = None
        self.jpeg_cache_time = 0
        self.jpeg_lock = threading.Lock()

        # FPS counter for H.264 frames
        self.fps_counter = FPSCounter()

    def start(self):
        self.running = True
        threading.Thread(target=self._read_loop, daemon=True).start()

    def stop(self):
        self.running = False
        self._close_connection()

    def reconnect(self):
        """Force reconnection to FLV stream"""
        print("Forcing FLV reconnection...", flush=True)
        self._reconnect_flag = True
        self._close_connection()

    def _close_connection(self):
        """Close current connection"""
        if self._current_response:
            try:
                self._current_response.close()
            except:
                pass
            self._current_response = None
        self.connected = False

    def _read_loop(self):
        """Main loop to read FLV stream"""
        while self.running:
            self._reconnect_flag = False
            try:
                self._connect_and_read()
            except Exception as e:
                if not self._reconnect_flag:
                    print(f"FLV reader error: {e}", flush=True)
                self.connected = False

            if self.running and not self._reconnect_flag:
                print(f"Reconnecting in {self.reconnect_delay}s...", flush=True)
                time.sleep(self.reconnect_delay)

    def _connect_and_read(self):
        """Connect to FLV stream and parse it"""
        print(f"Connecting to {self.url}", flush=True)

        req = urllib.request.Request(self.url)
        self._current_response = urllib.request.urlopen(req, timeout=10)
        response = self._current_response
        self.connected = True
        print("Connected to gkcam FLV stream", flush=True)

        try:
            # Read FLV header (9 bytes)
            header = response.read(9)
            if header[:3] != b'FLV':
                raise Exception("Invalid FLV header")

            # Read PreviousTagSize0 (4 bytes)
            response.read(4)

            # Read FLV tags
            while self.running and not self._reconnect_flag:
                tag = self._read_flv_tag(response)
                if tag is None:
                    break
        finally:
            self._close_connection()

    def _read_flv_tag(self, stream):
        """Read and parse a single FLV tag"""
        # Tag header (11 bytes)
        header = stream.read(11)
        if len(header) < 11:
            return None

        tag_type = header[0]
        data_size = (header[1] << 16) | (header[2] << 8) | header[3]

        # Read tag data
        data = stream.read(data_size)
        if len(data) < data_size:
            return None

        # Read PreviousTagSize (4 bytes)
        stream.read(4)

        # Process video tags (type 9)
        if tag_type == 9 and len(data) >= 5:
            self._process_video_tag(data)

        return True

    def _process_video_tag(self, data):
        """Process FLV video tag, extract H.264 NAL units"""
        frame_type = (data[0] >> 4) & 0x0F  # 1=keyframe, 2=inter
        codec_id = data[0] & 0x0F  # 7=AVC

        if codec_id != 7:  # Not H.264/AVC
            return

        avc_packet_type = data[1]  # 0=sequence header, 1=NALU, 2=end

        if avc_packet_type == 0:
            # AVC sequence header (contains SPS/PPS)
            self._parse_avc_decoder_config(data[5:])
        elif avc_packet_type == 1:
            # AVC NALU
            self._parse_avc_nalus(data[5:], frame_type == 1)

    def _parse_avc_decoder_config(self, data):
        """Parse AVC decoder configuration record (contains SPS/PPS)"""
        if len(data) < 7:
            return

        offset = 5

        # SPS
        num_sps = data[offset] & 0x1F
        offset += 1

        for _ in range(num_sps):
            if offset + 2 > len(data):
                return
            sps_len = (data[offset] << 8) | data[offset + 1]
            offset += 2
            if offset + sps_len > len(data):
                return
            self.sps = H264_START_CODE + data[offset:offset + sps_len]
            offset += sps_len
            print(f"Got SPS ({len(self.sps)} bytes)", flush=True)

        # PPS
        if offset >= len(data):
            return
        num_pps = data[offset]
        offset += 1

        for _ in range(num_pps):
            if offset + 2 > len(data):
                return
            pps_len = (data[offset] << 8) | data[offset + 1]
            offset += 2
            if offset + pps_len > len(data):
                return
            self.pps = H264_START_CODE + data[offset:offset + pps_len]
            offset += pps_len
            print(f"Got PPS ({len(self.pps)} bytes)", flush=True)

    def _parse_avc_nalus(self, data, is_keyframe):
        """Parse AVC NALUs from FLV packet (length-prefixed format)"""
        offset = 0
        nalus = []

        while offset + 4 <= len(data):
            # Read NAL length (4 bytes, big-endian)
            nal_len = (data[offset] << 24) | (data[offset + 1] << 16) | \
                      (data[offset + 2] << 8) | data[offset + 3]
            offset += 4

            if offset + nal_len > len(data):
                break

            nal_data = data[offset:offset + nal_len]
            offset += nal_len

            # Convert to Annex B format (start code + NAL)
            nal_unit = H264_START_CODE + nal_data
            nalus.append(nal_unit)

        if not nalus:
            return

        # Build complete frame with SPS/PPS for keyframes
        frame_data = bytearray()
        if is_keyframe:
            if self.sps:
                frame_data.extend(self.sps)
            if self.pps:
                frame_data.extend(self.pps)

        for nal in nalus:
            frame_data.extend(nal)

        frame_bytes = bytes(frame_data)

        # Count all H.264 frames for FPS display
        self.fps_counter.tick()

        # Store keyframe for snapshot generation
        if is_keyframe:
            with self.frame_lock:
                self.last_keyframe = frame_bytes
                self.last_keyframe_time = time.time()

                # In keyframes-only mode, this is also the current frame
                if not self.decode_all_frames:
                    self.last_frame = frame_bytes
                    self.last_frame_time = time.time()
                    self.frame_event.set()

        # In all-frames mode, store every frame
        if self.decode_all_frames:
            with self.frame_lock:
                self.last_frame = frame_bytes
                self.last_frame_time = time.time()
            self.frame_event.set()

    def get_keyframe(self):
        """Get the last keyframe for snapshot generation"""
        with self.frame_lock:
            return self.last_keyframe

    def get_frame(self):
        """Get the last frame (keyframe or inter-frame depending on mode)"""
        with self.frame_lock:
            return self.last_frame

    def get_jpeg(self, max_age=0.5):
        """Get JPEG snapshot, using cache if recent"""
        with self.jpeg_lock:
            now = time.time()
            if self.jpeg_cache and (now - self.jpeg_cache_time) < max_age:
                return self.jpeg_cache

            # Generate new JPEG from keyframe
            keyframe = self.get_keyframe()
            if keyframe:
                jpeg = decode_h264_to_jpeg(keyframe)
                if jpeg:
                    self.jpeg_cache = jpeg
                    self.jpeg_cache_time = now
                    return jpeg

        return None


# ============================================================================
# HTTP Server
# ============================================================================

class FPSCounter:
    """Track frames per second"""

    def __init__(self, window_size=30):
        self.window_size = window_size
        self.timestamps = deque(maxlen=window_size)
        self.lock = threading.Lock()

    def tick(self):
        """Record a frame"""
        with self.lock:
            self.timestamps.append(time.time())

    def get_fps(self):
        """Calculate current FPS"""
        with self.lock:
            if len(self.timestamps) < 2:
                return 0.0
            elapsed = self.timestamps[-1] - self.timestamps[0]
            if elapsed <= 0:
                return 0.0
            return (len(self.timestamps) - 1) / elapsed


class StreamerApp:
    """Main streamer application"""

    MJPEG_BOUNDARY = "mjpegstream"
    FLV_PORT = 18088  # Fixed port for FLV stream (for Anycubic slicer)
    STREAMING_PORT = 8080  # Port where rkmpi_enc serves streams
    CONTROL_PORT = 8081  # Port for control interface when rkmpi_enc handles streaming

    def __init__(self, args):
        self.args = args
        self.running = False
        self.encoder_process = None
        self.encoder_pid = None
        self.gkcam_pid = None  # Tracked for CPU monitoring in gkcam mode

        # Operating mode: 'go-klipper' or 'vanilla-klipper'
        # Sanitize to remove any invisible Unicode characters
        raw_mode = getattr(args, 'mode', 'go-klipper')
        sanitized_mode = ''.join(c for c in str(raw_mode) if c.isascii() and c.isprintable())
        self.mode = sanitized_mode if sanitized_mode in ('go-klipper', 'vanilla-klipper') else 'go-klipper'

        # Encoder mode: 'gkcam', 'rkmpi', or 'rkmpi-yuyv'
        raw_encoder = getattr(args, 'encoder_type', 'rkmpi-yuyv')
        sanitized_encoder = ''.join(c for c in str(raw_encoder) if c.isascii() and c.isprintable())
        self.encoder_type = sanitized_encoder if sanitized_encoder in ('gkcam', 'rkmpi', 'rkmpi-yuyv') else 'rkmpi-yuyv'

        # Server ports - consistent layout for all modes:
        # - streaming_port (8080): /stream, /snapshot (rkmpi_enc in rkmpi mode, Python in gkcam mode)
        # - control_port (8081): /control, /api/* (always Python)
        self.streaming_port = getattr(args, 'streaming_port', self.STREAMING_PORT)
        self.control_port = getattr(args, 'control_port', self.CONTROL_PORT)
        self.gkcam_all_frames = getattr(args, 'gkcam_all_frames', False)
        self.jpeg_quality = getattr(args, 'jpeg_quality', 85)  # HW JPEG quality (1-99)
        self.h264_resolution = getattr(args, 'h264_resolution', '1280x720')  # H.264 encode resolution (rkmpi only)

        # Gkcam mode: FLV reader for keyframe extraction, transcoder for all-frames
        self.gkcam_reader = None  # Reads H.264 NALs from FLV (keyframe-only mode)
        self.gkcam_transcoder = None  # Streaming transcoder for all-frames mode

        # Runtime state
        self.autolanmode = args.autolanmode
        self.logging = getattr(args, 'logging', False)  # Debug logging enabled
        self.h264_enabled = getattr(args, 'h264_enabled', True)
        self.skip_ratio = getattr(args, 'skip_ratio', 4)  # Default 25% (100/4)
        self.saved_skip_ratio = self.skip_ratio  # Manual setting to restore when auto_skip disabled
        self.auto_skip = getattr(args, 'auto_skip', True)  # Default enabled
        self.target_cpu = getattr(args, 'target_cpu', 25)  # Default 25%
        self.bitrate = getattr(args, 'bitrate', 512)
        self.mjpeg_fps_target = getattr(args, 'mjpeg_fps', 10)  # Target MJPEG framerate
        self.max_camera_fps = 30  # Declared camera capability (from v4l2-ctl)
        self.detected_camera_fps = 0  # Actual measured camera delivery rate

        # Display capture settings (disabled by default to save CPU)
        self.display_enabled = getattr(args, 'display_enabled', False)
        self.display_fps = getattr(args, 'display_fps', 5)

        # Internal camera USB port (for reliable detection when external cameras connected)
        # Default: 1.3 (internal camera port on Anycubic printers)
        # External cameras typically connect through hub at port 1.4.x
        self.internal_usb_port = getattr(args, 'internal_usb_port', '1.3')

        # Advanced timelapse settings (Moonraker integration)
        self.timelapse_enabled = getattr(args, 'timelapse_enabled', False)
        self.timelapse_mode = getattr(args, 'timelapse_mode', 'layer')  # 'layer' or 'hyperlapse'
        self.timelapse_hyperlapse_interval = getattr(args, 'timelapse_hyperlapse_interval', 30)
        self.timelapse_storage = getattr(args, 'timelapse_storage', 'internal')  # 'internal' or 'usb'
        self.timelapse_usb_path = getattr(args, 'timelapse_usb_path', '/mnt/udisk/timelapse')
        self.moonraker_host = getattr(args, 'moonraker_host', '127.0.0.1')
        self.moonraker_port = getattr(args, 'moonraker_port', 7125)
        self.timelapse_output_fps = getattr(args, 'timelapse_output_fps', 30)
        self.timelapse_variable_fps = getattr(args, 'timelapse_variable_fps', False)
        self.timelapse_target_length = getattr(args, 'timelapse_target_length', 10)
        self.timelapse_variable_fps_min = getattr(args, 'timelapse_variable_fps_min', 5)
        self.timelapse_variable_fps_max = getattr(args, 'timelapse_variable_fps_max', 60)
        self.timelapse_crf = getattr(args, 'timelapse_crf', 23)
        self.timelapse_duplicate_last_frame = getattr(args, 'timelapse_duplicate_last_frame', 0)
        self.timelapse_stream_delay = getattr(args, 'timelapse_stream_delay', 0.05)
        self.timelapse_flip_x = getattr(args, 'timelapse_flip_x', False)
        self.timelapse_flip_y = getattr(args, 'timelapse_flip_y', False)

        # Moonraker client (for timelapse integration)
        self.moonraker_client = None
        self.timelapse_active = False  # True when timelapse recording is in progress
        self.timelapse_frames = 0  # Frame counter for current timelapse
        self.timelapse_filename = None  # Current print filename
        self.hyperlapse_timer = None  # Timer for hyperlapse mode

        # MJPEG frame buffer (for rkmpi mode)
        self.current_frame = None
        self.frame_lock = threading.Lock()
        self.frame_event = threading.Event()

        # H.264 buffer (for rkmpi mode)
        self.h264_buffer = deque(maxlen=100)
        self.h264_lock = threading.Lock()

        # Global SPS/PPS cache (for FLV clients that connect late)
        self.cached_sps = None
        self.cached_pps = None

        # FLV muxer (initialized after camera detection, rkmpi mode only)
        self.flv_muxer = None

        # FLV client tracking
        self.flv_client_count = 0
        self.flv_client_lock = threading.Lock()

        # Responder subprocesses (spawned when FLV clients connect, killed when they disconnect)
        # Run in separate processes to avoid GIL contention that causes CPU spikes
        self.rpc_subprocess = None
        self.mqtt_subprocess = None

        # FPS counters
        self.mjpeg_fps = FPSCounter()
        self.h264_fps = FPSCounter()

        # Encoder stats (read from control file in server mode)
        self.encoder_mjpeg_fps = 0.0
        self.encoder_h264_fps = 0.0
        self.encoder_mjpeg_clients = 0
        self.encoder_flv_clients = 0

        # CPU monitor (only runs when control page is active)
        self.cpu_monitor = CPUMonitor()
        self.last_stats_request = 0  # Timestamp of last /api/stats request
        self.stats_timeout = 10  # Stop monitoring if no requests for 10 seconds

        # Camera info (rkmpi mode only)
        self.camera_device = None
        self.camera_width = 1280
        self.camera_height = 720

        # Camera V4L2 controls (loaded from config, can be modified via API)
        self.cam_brightness = getattr(args, 'cam_brightness', 0)
        self.cam_contrast = getattr(args, 'cam_contrast', 32)
        self.cam_saturation = getattr(args, 'cam_saturation', 85)
        self.cam_hue = getattr(args, 'cam_hue', 0)
        self.cam_gamma = getattr(args, 'cam_gamma', 100)
        self.cam_sharpness = getattr(args, 'cam_sharpness', 3)
        self.cam_gain = getattr(args, 'cam_gain', 1)
        self.cam_backlight = getattr(args, 'cam_backlight', 0)
        self.cam_wb_auto = getattr(args, 'cam_wb_auto', 1)
        self.cam_wb_temp = getattr(args, 'cam_wb_temp', 4000)
        self.cam_exposure_auto = getattr(args, 'cam_exposure_auto', 3)
        self.cam_exposure = getattr(args, 'cam_exposure', 156)
        self.cam_exposure_priority = getattr(args, 'cam_exposure_priority', 0)
        self.cam_power_line = getattr(args, 'cam_power_line', 1)
        self.cam_controls_applied = False  # Track if initial controls have been applied

        # Control file (rkmpi mode only) - bidirectional settings/stats
        self.ctrl_file = "/tmp/h264_ctrl"
        # Command file - one-shot commands (timelapse, etc.)
        self.cmd_file = "/tmp/h264_cmd"

        # IP address
        self.current_ip = None

        # Session ID for restart detection (timestamp-based)
        self.session_id = int(time.time() * 1000)

    def is_rkmpi_mode(self):
        """Check if using rkmpi encoder (either rkmpi or rkmpi-yuyv)"""
        return self.encoder_type in ('rkmpi', 'rkmpi-yuyv')

    def _flv_keepalive_thread(self):
        """Send MQTT keepalive (startCapture) every 20 seconds while FLV clients are connected.

        This prevents Anycubic slicers from throttling/disconnecting the camera stream.
        The startCapture command tells gkcam to keep streaming at full rate.
        """
        last_keepalive = 0
        while self.running:
            time.sleep(1)  # Check every second

            with self.flv_client_lock:
                has_clients = self.flv_client_count > 0

            if has_clients:
                now = time.time()
                if now - last_keepalive >= self.keepalive_interval:
                    print(f"FLV keepalive: {self.flv_client_count} client(s), sending startCapture", flush=True)
                    try:
                        mqtt_send_start_camera()
                    except Exception as e:
                        print(f"FLV keepalive error: {e}", flush=True)
                    last_keepalive = now

    def write_ctrl_file(self):
        """Write settings to command file for encoder.

        All Python→encoder communication goes to cmd_file (/tmp/h264_cmd).
        ctrl_file (/tmp/h264_ctrl) is for encoder→Python stats only.
        """
        try:
            with open(self.cmd_file, 'a') as f:
                # For rkmpi-yuyv, H.264 is always enabled (no CPU impact, all HW)
                if self.encoder_type == 'rkmpi-yuyv':
                    f.write("h264=1\n")
                else:
                    f.write(f"h264={'1' if self.h264_enabled else '0'}\n")
                f.write(f"skip={self.skip_ratio}\n")
                f.write(f"auto_skip={'1' if self.auto_skip else '0'}\n")
                f.write(f"target_cpu={self.target_cpu}\n")
                # Display capture settings
                f.write(f"display_enabled={'1' if self.display_enabled else '0'}\n")
                f.write(f"display_fps={self.display_fps}\n")
                # Camera controls
                f.write(f"cam_brightness={self.cam_brightness}\n")
                f.write(f"cam_contrast={self.cam_contrast}\n")
                f.write(f"cam_saturation={self.cam_saturation}\n")
                f.write(f"cam_hue={self.cam_hue}\n")
                f.write(f"cam_gamma={self.cam_gamma}\n")
                f.write(f"cam_sharpness={self.cam_sharpness}\n")
                f.write(f"cam_gain={self.cam_gain}\n")
                f.write(f"cam_backlight={self.cam_backlight}\n")
                f.write(f"cam_wb_auto={self.cam_wb_auto}\n")
                f.write(f"cam_wb_temp={self.cam_wb_temp}\n")
                f.write(f"cam_exposure_auto={self.cam_exposure_auto}\n")
                f.write(f"cam_exposure={self.cam_exposure}\n")
                f.write(f"cam_exposure_priority={self.cam_exposure_priority}\n")
                f.write(f"cam_power_line={self.cam_power_line}\n")
        except Exception as e:
            print(f"Error writing command file: {e}", flush=True)

    def read_ctrl_file(self):
        """Read control file to get encoder's current stats (skip_ratio, FPS, clients)"""
        try:
            with open(self.ctrl_file, 'r') as f:
                for line in f:
                    line = line.strip()
                    if line.startswith('skip='):
                        val = int(line.split('=')[1])
                        if val >= 1 and val != self.skip_ratio:
                            self.skip_ratio = val
                    elif line.startswith('mjpeg_fps='):
                        try:
                            self.encoder_mjpeg_fps = float(line.split('=')[1])
                        except ValueError:
                            pass
                    elif line.startswith('h264_fps='):
                        try:
                            self.encoder_h264_fps = float(line.split('=')[1])
                        except ValueError:
                            pass
                    elif line.startswith('mjpeg_clients='):
                        try:
                            self.encoder_mjpeg_clients = int(line.split('=')[1])
                        except ValueError:
                            pass
                    elif line.startswith('flv_clients='):
                        try:
                            self.encoder_flv_clients = int(line.split('=')[1])
                        except ValueError:
                            pass
                    elif line.startswith('camera_max_fps='):
                        try:
                            detected_fps = int(line.split('=')[1])
                            # Store actual measured camera rate (for display only)
                            # Don't update max_camera_fps - that stays at declared capability
                            if detected_fps > 0:
                                self.detected_camera_fps = detected_fps
                        except ValueError:
                            pass
                    # Camera controls (read from encoder)
                    elif line.startswith('cam_brightness='):
                        try: self.cam_brightness = int(line.split('=')[1])
                        except ValueError: pass
                    elif line.startswith('cam_contrast='):
                        try: self.cam_contrast = int(line.split('=')[1])
                        except ValueError: pass
                    elif line.startswith('cam_saturation='):
                        try: self.cam_saturation = int(line.split('=')[1])
                        except ValueError: pass
                    elif line.startswith('cam_hue='):
                        try: self.cam_hue = int(line.split('=')[1])
                        except ValueError: pass
                    elif line.startswith('cam_gamma='):
                        try: self.cam_gamma = int(line.split('=')[1])
                        except ValueError: pass
                    elif line.startswith('cam_sharpness='):
                        try: self.cam_sharpness = int(line.split('=')[1])
                        except ValueError: pass
                    elif line.startswith('cam_gain='):
                        try: self.cam_gain = int(line.split('=')[1])
                        except ValueError: pass
                    elif line.startswith('cam_backlight='):
                        try: self.cam_backlight = int(line.split('=')[1])
                        except ValueError: pass
                    elif line.startswith('cam_wb_auto='):
                        try: self.cam_wb_auto = int(line.split('=')[1])
                        except ValueError: pass
                    elif line.startswith('cam_wb_temp='):
                        try: self.cam_wb_temp = int(line.split('=')[1])
                        except ValueError: pass
                    elif line.startswith('cam_exposure_auto='):
                        try: self.cam_exposure_auto = int(line.split('=')[1])
                        except ValueError: pass
                    elif line.startswith('cam_exposure='):
                        try: self.cam_exposure = int(line.split('=')[1])
                        except ValueError: pass
                    elif line.startswith('cam_exposure_priority='):
                        try: self.cam_exposure_priority = int(line.split('=')[1])
                        except ValueError: pass
                    elif line.startswith('cam_power_line='):
                        try: self.cam_power_line = int(line.split('=')[1])
                        except ValueError: pass
        except Exception:
            pass

    def save_config(self):
        """Save settings to persistent config file"""
        config_path = "/useremain/home/rinkhals/apps/29-h264-streamer.config"
        try:
            # Read existing config
            config = {}
            try:
                with open(config_path, 'r') as f:
                    config = json.load(f)
            except Exception:
                pass

            # Update with current settings (using string values for bools)
            config['mode'] = self.mode
            config['encoder_type'] = self.encoder_type
            config['gkcam_all_frames'] = 'true' if self.gkcam_all_frames else 'false'
            config['autolanmode'] = 'true' if self.autolanmode else 'false'
            config['logging'] = 'true' if self.logging else 'false'
            config['h264_enabled'] = 'true' if self.h264_enabled else 'false'
            config['auto_skip'] = 'true' if self.auto_skip else 'false'
            config['target_cpu'] = str(self.target_cpu)
            config['skip_ratio'] = str(self.saved_skip_ratio)  # Save manual setting
            config['bitrate'] = str(self.bitrate)
            config['mjpeg_fps'] = str(self.mjpeg_fps_target)
            config['streaming_port'] = str(self.streaming_port)
            config['control_port'] = str(self.control_port)
            config['h264_resolution'] = self.h264_resolution
            config['display_enabled'] = 'true' if self.display_enabled else 'false'
            config['display_fps'] = str(self.display_fps)
            config['internal_usb_port'] = self.internal_usb_port

            # Camera controls
            config['cam_brightness'] = str(self.cam_brightness)
            config['cam_contrast'] = str(self.cam_contrast)
            config['cam_saturation'] = str(self.cam_saturation)
            config['cam_hue'] = str(self.cam_hue)
            config['cam_gamma'] = str(self.cam_gamma)
            config['cam_sharpness'] = str(self.cam_sharpness)
            config['cam_gain'] = str(self.cam_gain)
            config['cam_backlight'] = str(self.cam_backlight)
            config['cam_wb_auto'] = str(self.cam_wb_auto)
            config['cam_wb_temp'] = str(self.cam_wb_temp)
            config['cam_exposure_auto'] = str(self.cam_exposure_auto)
            config['cam_exposure'] = str(self.cam_exposure)
            config['cam_exposure_priority'] = str(self.cam_exposure_priority)
            config['cam_power_line'] = str(self.cam_power_line)

            # Advanced timelapse settings
            config['timelapse_enabled'] = 'true' if self.timelapse_enabled else 'false'
            config['timelapse_mode'] = self.timelapse_mode
            config['timelapse_hyperlapse_interval'] = str(self.timelapse_hyperlapse_interval)
            config['timelapse_storage'] = self.timelapse_storage
            config['timelapse_usb_path'] = self.timelapse_usb_path
            config['moonraker_host'] = self.moonraker_host
            config['moonraker_port'] = str(self.moonraker_port)
            config['timelapse_output_fps'] = str(self.timelapse_output_fps)
            config['timelapse_variable_fps'] = 'true' if self.timelapse_variable_fps else 'false'
            config['timelapse_target_length'] = str(self.timelapse_target_length)
            config['timelapse_variable_fps_min'] = str(self.timelapse_variable_fps_min)
            config['timelapse_variable_fps_max'] = str(self.timelapse_variable_fps_max)
            config['timelapse_crf'] = str(self.timelapse_crf)
            config['timelapse_duplicate_last_frame'] = str(self.timelapse_duplicate_last_frame)
            config['timelapse_stream_delay'] = str(self.timelapse_stream_delay)
            config['timelapse_flip_x'] = 'true' if self.timelapse_flip_x else 'false'
            config['timelapse_flip_y'] = 'true' if self.timelapse_flip_y else 'false'

            # Write back with explicit flush and sync
            with open(config_path, 'w') as f:
                json.dump(config, f, indent=2)
                f.flush()
                os.fsync(f.fileno())
            # System-wide sync to ensure all buffers are written to disk
            os.sync()
            print(f"Settings saved to {config_path}", flush=True)
        except Exception as e:
            print(f"Error saving config: {e}", flush=True)

    def start(self):
        """Start the streamer"""
        self.running = True

        print(f"Operating mode: {self.mode}", flush=True)
        print(f"Encoder mode: {self.encoder_type}", flush=True)

        # In vanilla-klipper mode, skip Anycubic-specific initialization
        if self.mode == 'vanilla-klipper':
            print("Vanilla-klipper mode: skipping LAN mode check and Anycubic services", flush=True)
        else:
            # Check LAN mode (go-klipper mode only)
            print("Checking printer status...", flush=True)
            lan_enabled = is_lan_mode_enabled()
            print(f"LAN mode detected: {'enabled' if lan_enabled else 'disabled/unknown'}", flush=True)

            # Try to enable LAN mode if autolanmode is on
            if self.autolanmode and not lan_enabled:
                print("Auto-enabling LAN mode...", flush=True)
                enable_lan_mode()
                time.sleep(3)
                lan_enabled = is_lan_mode_enabled()
                print(f"LAN mode after enable: {'enabled' if lan_enabled else 'disabled/unknown'}", flush=True)

        if self.encoder_type == 'gkcam':
            # GKCAM MODE: Use gkcam's FLV stream, don't kill gkcam
            if not self._start_gkcam_mode():
                return False
        else:
            # RKMPI MODE: Kill gkcam and use our encoder
            if not self._start_rkmpi_mode():
                return False

        # Detect IP and update moonraker
        self.current_ip = get_ip_address()
        if self.current_ip:
            print(f"Printer IP: {self.current_ip}", flush=True)
            update_moonraker_webcam_config(self.current_ip, self.streaming_port, self.mjpeg_fps_target)

        # Start control server (both modes)
        threading.Thread(target=self._run_control_server, daemon=True).start()

        # In gkcam mode, also start streaming server (rkmpi mode uses rkmpi_enc for streaming)
        if not self.is_rkmpi_mode():
            threading.Thread(target=self._run_streaming_server, daemon=True).start()

        # Start CPU monitor thread
        threading.Thread(target=self._cpu_monitor_loop, daemon=True).start()

        # Start IP monitor thread
        threading.Thread(target=self._ip_monitor_loop, daemon=True).start()

        # Start Moonraker client if timelapse is enabled (rkmpi mode only)
        if self.is_rkmpi_mode() and self.timelapse_enabled:
            self._start_moonraker_client()

        print(f"Streamer started:", flush=True)
        print(f"  Streaming:", flush=True)
        print(f"    Port {self.streaming_port}: /stream, /snapshot", flush=True)
        print(f"    Port {self.FLV_PORT}: /flv (H.264 FLV)", flush=True)
        if self.is_rkmpi_mode():
            if self.mode == 'vanilla-klipper':
                print(f"    MQTT/RPC: disabled (vanilla-klipper)", flush=True)
            else:
                print(f"    MQTT/RPC: built-in (rkmpi_enc)", flush=True)
        else:
            print(f"    (gkcam native FLV)", flush=True)
        print(f"  Control:", flush=True)
        print(f"    Port {self.control_port}: /control, /api/*", flush=True)

        return True

    def _start_gkcam_mode(self):
        """Start in gkcam mode - use gkcam's FLV stream"""
        print("Starting in GKCAM mode...", flush=True)

        # Ensure gkcam is running
        if not ensure_gkcam_running():
            print("ERROR: gkcam not responding!", flush=True)
            return False

        # Wait for stream to stabilize
        time.sleep(2)

        # Start gkcam FLV reader (for keyframe extraction and H.264 FPS counting)
        self.gkcam_reader = GkcamFLVReader(
            url="http://127.0.0.1:18088/flv",
            decode_all_frames=False  # Reader only does keyframes now
        )
        self.gkcam_reader.start()

        # Wait for connection
        for i in range(10):
            time.sleep(1)
            if self.gkcam_reader.connected:
                print("Connected to gkcam FLV stream", flush=True)
                break
        else:
            print("WARNING: gkcam FLV connection taking longer than expected", flush=True)

        # In all-frames mode, start the streaming transcoder
        if self.gkcam_all_frames:
            print("Starting streaming transcoder for all-frames mode...", flush=True)
            self.gkcam_transcoder = GkcamStreamingTranscoder(
                url="http://127.0.0.1:18088/flv"
            )
            self.gkcam_transcoder.start()
            # Give transcoder time to start
            time.sleep(2)
            print("Streaming transcoder active", flush=True)

        return True

    def _start_rkmpi_mode(self):
        """Start in rkmpi mode - use our encoder"""
        print("Starting in RKMPI mode...", flush=True)

        if self.mode == 'vanilla-klipper':
            # Vanilla-klipper mode: no Anycubic services running
            print("Vanilla-klipper mode: skipping gkcam/API interactions", flush=True)
        else:
            # Go-klipper mode: need to manage gkcam and use Anycubic APIs
            # Kill gkcam to free port 18088 and the camera
            # We'll restart it later (without camera/port) so gkapi can still talk to it via RPC
            print("Stopping gkcam (if running)...", flush=True)
            kill_gkcam()
            time.sleep(1)

            # Stop any existing camera stream
            print("Stopping existing camera stream...", flush=True)
            stop_camera_stream()
            time.sleep(1)

            # Start camera stream via API (needed to activate camera on some printers)
            print("Starting camera stream via API...", flush=True)
            start_camera_stream()
            time.sleep(2)

        # Find camera (prioritize internal camera by USB port)
        self.camera_device = find_camera_device(self.internal_usb_port)
        if not self.camera_device:
            print("ERROR: No camera found!", flush=True)
            return False

        # Detect resolution
        self.camera_width, self.camera_height = detect_camera_resolution(self.camera_device)

        # Detect max camera FPS at this resolution
        self.max_camera_fps = detect_camera_max_fps(self.camera_device, self.camera_width, self.camera_height)

        # Validate mjpeg_fps_target against camera capabilities
        if self.mjpeg_fps_target > self.max_camera_fps:
            print(f"MJPEG FPS target ({self.mjpeg_fps_target}) exceeds camera max ({self.max_camera_fps}), clamping", flush=True)
            self.mjpeg_fps_target = self.max_camera_fps
        elif self.mjpeg_fps_target < 2:
            self.mjpeg_fps_target = 2
        print(f"MJPEG target FPS: {self.mjpeg_fps_target} (max: {self.max_camera_fps})", flush=True)

        # Initialize FLV muxer
        self.flv_muxer = FLVMuxer(self.camera_width, self.camera_height, 10)

        # Write control file BEFORE starting encoder (so encoder reads correct settings)
        self.write_ctrl_file()

        # Start encoder process
        if not self.start_encoder():
            print("ERROR: Failed to start encoder!", flush=True)
            return False

        return True

    def start_encoder(self):
        """Start the rkmpi_enc encoder process in server mode.

        The encoder runs with built-in HTTP/MQTT/RPC servers:
        - HTTP :8080 - MJPEG /stream and /snapshot
        - HTTP :18088 - FLV /flv for H.264
        - MQTT :9883 - Video responder (TLS)
        - RPC :18086 - Video stream request handler
        """
        encoder_path = os.path.join(os.path.dirname(os.path.realpath(__file__)), 'rkmpi_enc')

        if not os.path.exists(encoder_path):
            print(f"Encoder not found: {encoder_path}", flush=True)
            return False

        # Make executable
        os.chmod(encoder_path, 0o755)

        # Build command for server mode
        # -S: Server mode (built-in HTTP/MQTT/RPC)
        # -N: No stdout streaming (servers handle it)
        #
        # For rkmpi-yuyv mode:
        # - Skip ratio is always 1 (no skipping) since YUYV is already bandwidth-limited
        #   to ~4-5fps and both H.264/JPEG use hardware encoding
        # - Auto-skip is disabled (CPU is not a concern with full HW encoding)
        if self.encoder_type == 'rkmpi-yuyv':
            effective_skip = 1  # Always 100% for YUYV
            effective_auto_skip = False
        else:
            effective_skip = self.skip_ratio
            effective_auto_skip = self.auto_skip

        # Determine camera/encode resolution based on mode and settings
        # For rkmpi-yuyv: h264_resolution sets camera capture resolution
        # For rkmpi: camera is always at native res, h264_resolution sets encode resolution
        if self.encoder_type == 'rkmpi-yuyv' and self.h264_resolution:
            # Parse resolution setting for YUYV camera capture
            try:
                cam_w, cam_h = map(int, self.h264_resolution.split('x'))
            except:
                cam_w, cam_h = self.camera_width, self.camera_height
        else:
            cam_w, cam_h = self.camera_width, self.camera_height

        cmd = [
            encoder_path,
            '-S',  # Server mode
            '-N',  # No stdout
            '-d', self.camera_device,
            '-w', str(cam_w),
            '-h', str(cam_h),
            '-f', str(self.mjpeg_fps_target),
            '-s', str(effective_skip),
            '-b', str(self.bitrate),
            '-t', str(self.target_cpu),
            '-q',  # VBR mode
            '-v',  # Verbose logging
            '--mode', self.mode,  # Operating mode (go-klipper or vanilla-klipper)
            '--streaming-port', str(self.streaming_port),  # MJPEG HTTP port
            '--control-port', str(self.control_port)  # Control panel HTTP port
        ]

        # YUYV mode: use hardware JPEG encoding
        if self.encoder_type == 'rkmpi-yuyv':
            cmd.extend(['-y', '-j', str(self.jpeg_quality)])
        else:
            # rkmpi mode: allow custom H.264 resolution (for lower CPU during decode)
            if self.h264_resolution and self.h264_resolution != '1280x720':
                cmd.extend(['--h264-resolution', self.h264_resolution])

        if effective_auto_skip:
            cmd.append('-a')

        if not self.h264_enabled:
            cmd.append('-n')

        # Enable display capture (printer LCD framebuffer)
        # Always start the thread, but it idles until enabled via control file
        cmd.append('--display')
        cmd.extend(['--display-fps', str(self.display_fps)])

        print(f"Starting encoder (server mode): {' '.join(cmd)}", flush=True)

        try:
            # In server mode, encoder handles all streaming internally
            # We don't need to read from stdout/stderr for data
            self.encoder_process = subprocess.Popen(
                cmd,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                bufsize=0
            )
            self.encoder_pid = self.encoder_process.pid
            print(f"Encoder started in server mode (PID {self.encoder_pid})", flush=True)
            print(f"  Streaming on port {self.streaming_port} (/stream, /snapshot)", flush=True)
            print(f"  FLV on port {self.FLV_PORT} (/flv)", flush=True)
            if self.mode == 'vanilla-klipper':
                print(f"  MQTT/RPC responders: disabled (vanilla-klipper mode)", flush=True)
            else:
                print(f"  MQTT/RPC responders active", flush=True)

            # Start stderr reader for logging only (not for frame data)
            threading.Thread(target=self._encoder_stderr_reader, daemon=True).start()

            # Apply initial camera controls from saved config (after encoder starts)
            # Give encoder a moment to initialize, then write all controls
            threading.Thread(target=self._apply_initial_camera_controls, daemon=True).start()

            return True
        except Exception as e:
            print(f"Failed to start encoder: {e}", flush=True)
            return False

    def _mjpeg_reader(self):
        """Read MJPEG frames from encoder stdout (optimized with bytearray)"""
        boundary = f"--{self.MJPEG_BOUNDARY}".encode()
        buffer = bytearray()

        while self.running and self.encoder_process:
            try:
                chunk = self.encoder_process.stdout.read(8192)
                if not chunk:
                    break

                buffer.extend(chunk)

                # Parse multipart frames
                while True:
                    idx = buffer.find(boundary)
                    if idx == -1:
                        break

                    # Find headers end
                    header_end = buffer.find(b"\r\n\r\n", idx)
                    if header_end == -1:
                        break

                    # Parse Content-Length using pre-compiled regex
                    header_bytes = bytes(buffer[idx:header_end])
                    match = RE_CONTENT_LENGTH.search(header_bytes)
                    if not match:
                        del buffer[:header_end+4]
                        continue

                    content_length = int(match.group(1))
                    data_start = header_end + 4
                    data_end = data_start + content_length

                    if len(buffer) < data_end:
                        break

                    # Extract frame as bytes
                    frame = bytes(buffer[data_start:data_end])

                    # Store frame and count
                    with self.frame_lock:
                        self.current_frame = frame
                    self.frame_event.set()
                    self.mjpeg_fps.tick()

                    # Remove processed data from buffer
                    del buffer[:data_end]

            except Exception as e:
                if self.running:
                    print(f"MJPEG reader error: {e}", flush=True)
                break

    def _h264_reader(self, fifo_path):
        """Read H.264 data from FIFO (optimized with bytearray)"""
        while self.running:
            try:
                # Open FIFO (blocks until writer connects)
                with open(fifo_path, 'rb') as fifo:
                    buffer = bytearray()
                    while self.running:
                        chunk = fifo.read(4096)
                        if not chunk:
                            break

                        buffer.extend(chunk)

                        # Look for NAL unit boundaries
                        while len(buffer) > 4:
                            # Find start codes (search from position 1 to avoid matching at start)
                            idx4 = buffer.find(RE_NAL_START_4, 1)
                            idx3 = buffer.find(RE_NAL_START_3, 1)

                            # Get earliest match
                            if idx4 == -1:
                                idx = idx3
                            elif idx3 == -1:
                                idx = idx4
                            else:
                                idx = min(idx4, idx3)

                            if idx == -1 or idx > 65536:
                                # No more start codes or too much data
                                if len(buffer) > 65536:
                                    # Process what we have
                                    nal_bytes = bytes(buffer[:65536])
                                    with self.h264_lock:
                                        self.h264_buffer.append(nal_bytes)
                                    del buffer[:65536]
                                break

                            # Extract NAL unit
                            nal_bytes = bytes(buffer[:idx])
                            if nal_bytes:
                                with self.h264_lock:
                                    self.h264_buffer.append(nal_bytes)
                                # Parse NAL type and cache SPS/PPS
                                if len(nal_bytes) > 4:
                                    # Find NAL type after start code
                                    nal_start = 4 if nal_bytes[:4] == RE_NAL_START_4 else 3
                                    if nal_start < len(nal_bytes):
                                        nal_type = nal_bytes[nal_start] & 0x1F
                                        # Cache SPS/PPS for late-joining FLV clients
                                        if nal_type == 7:  # SPS
                                            self.cached_sps = nal_bytes[nal_start:]
                                        elif nal_type == 8:  # PPS
                                            self.cached_pps = nal_bytes[nal_start:]
                                        elif nal_type in (1, 5):  # Coded slice
                                            self.h264_fps.tick()
                            del buffer[:idx]
            except Exception as e:
                if self.running:
                    print(f"H.264 reader error: {e}", flush=True)
                time.sleep(1)

    def _encoder_stderr_reader(self):
        """Read and log encoder stderr"""
        while self.running and self.encoder_process:
            try:
                line = self.encoder_process.stderr.readline()
                if not line:
                    break
                print(f"[encoder] {line.decode('utf-8', errors='ignore').rstrip()}", flush=True)
            except Exception:
                break

    def _cpu_monitor_loop(self):
        """Monitor CPU usage and read encoder's skip_ratio when auto-skip enabled"""
        while self.running:
            now = time.time()
            stats_active = (now - self.last_stats_request) < self.stats_timeout

            # Only run CPU monitoring if:
            # 1. Control page is active (stats being requested), OR
            # 2. Auto-skip is enabled (needs CPU for skip ratio adjustment in rkmpi mode)
            needs_cpu_monitoring = stats_active or (self.auto_skip and self.is_rkmpi_mode())

            if needs_cpu_monitoring:
                encoder_pids = []

                # Track encoder PIDs (rkmpi_enc in rkmpi mode, all gkcam in gkcam mode)
                if self.is_rkmpi_mode() and self.encoder_pid:
                    encoder_pids.append(self.encoder_pid)
                elif not self.is_rkmpi_mode():
                    # Find all gkcam PIDs (main + any threads/children)
                    encoder_pids = find_gkcam_pids()

                # Streamer is h264_server.py (all Python threads are in same PID)
                streamer_pid = os.getpid()

                self.cpu_monitor.update(encoder_pids=encoder_pids, streamer_pid=streamer_pid)

            # Read stats from encoder (FPS, clients, skip_ratio) in rkmpi mode
            # Always read in rkmpi mode for FPS/client stats, not just when auto_skip enabled
            if self.is_rkmpi_mode():
                self.read_ctrl_file()

            time.sleep(1)

    def _ip_monitor_loop(self):
        """Monitor for IP changes"""
        while self.running:
            time.sleep(30)
            try:
                new_ip = get_ip_address()
                if new_ip and new_ip != self.current_ip:
                    print(f"IP changed: {self.current_ip} -> {new_ip}", flush=True)
                    self.current_ip = new_ip
                    update_moonraker_webcam_config(new_ip, self.streaming_port, self.mjpeg_fps_target)
            except Exception as e:
                print(f"IP monitor error: {e}", flush=True)

    def _run_control_server(self):
        """Run control HTTP server on control_port for /control and /api endpoints"""
        server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_socket.bind(('0.0.0.0', self.control_port))
        server_socket.listen(10)
        server_socket.settimeout(1.0)

        while self.running:
            try:
                client, addr = server_socket.accept()
                threading.Thread(target=self._handle_client, args=(client,), daemon=True).start()
            except socket.timeout:
                continue
            except Exception as e:
                if self.running:
                    print(f"Control server error: {e}", flush=True)

        server_socket.close()

    def _run_streaming_server(self):
        """Run streaming HTTP server on streaming_port for /stream and /snapshot (gkcam mode only)"""
        server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_socket.bind(('0.0.0.0', self.streaming_port))
        server_socket.listen(10)
        server_socket.settimeout(1.0)

        while self.running:
            try:
                client, addr = server_socket.accept()
                threading.Thread(target=self._handle_streaming_client, args=(client,), daemon=True).start()
            except socket.timeout:
                continue
            except Exception as e:
                if self.running:
                    print(f"Streaming server error: {e}", flush=True)

        server_socket.close()

    def _handle_streaming_client(self, client):
        """Handle streaming client - serves /stream and /snapshot on streaming_port (gkcam mode)"""
        try:
            client.settimeout(30.0)
            request = client.recv(4096).decode('utf-8', errors='ignore')

            lines = request.split('\r\n')
            if not lines:
                return

            parts = lines[0].split(' ')
            if len(parts) < 2:
                return

            path = parts[1].split('?')[0]

            if path == '/':
                self._serve_homepage(client)
            elif path == '/stream':
                client.settimeout(None)
                self._serve_mjpeg_stream(client)
            elif path in ('/snapshot', '/snap', '/image'):
                self._serve_snapshot(client)
            else:
                self._serve_404(client)
        except Exception:
            pass
        finally:
            try:
                client.close()
            except Exception:
                pass

    def _run_flv_server(self):
        """Run FLV HTTP server on port 18088 (for Anycubic slicer)"""
        server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_socket.bind(('0.0.0.0', self.FLV_PORT))
        server_socket.listen(10)
        server_socket.settimeout(1.0)

        while self.running:
            try:
                client, addr = server_socket.accept()
                threading.Thread(target=self._handle_flv_client, args=(client,), daemon=True).start()
            except socket.timeout:
                continue
            except Exception as e:
                if self.running:
                    print(f"FLV server error: {e}", flush=True)

        server_socket.close()

    def _handle_flv_client(self, client):
        """Handle FLV client - only serves /flv endpoint"""
        try:
            # Enable TCP keepalive to prevent connection timeout
            client.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            # Set TCP_NODELAY for low-latency streaming
            client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

            client.settimeout(30.0)
            request = client.recv(4096).decode('utf-8', errors='ignore')

            lines = request.split('\r\n')
            if not lines:
                return

            parts = lines[0].split(' ')
            if len(parts) < 2:
                return

            path = parts[1].split('?')[0]

            # Only serve /flv on this port
            if path == '/flv':
                # Remove timeout for streaming - we want to stream indefinitely
                client.settimeout(None)
                self._serve_flv_stream(client)
            else:
                self._serve_404(client)
        except Exception:
            pass
        finally:
            try:
                client.close()
            except Exception:
                pass

    def _handle_client(self, client):
        """Handle HTTP client"""
        try:
            client.settimeout(30.0)
            request = client.recv(4096).decode('utf-8', errors='ignore')

            lines = request.split('\r\n')
            if not lines:
                return

            parts = lines[0].split(' ')
            if len(parts) < 2:
                return

            method = parts[0]
            full_path = parts[1]  # Full path with query string
            path = full_path.split('?')[0]  # Path without query string

            # Parse query params
            query = {}
            if '?' in full_path:
                qs = full_path.split('?')[1]
                for param in qs.split('&'):
                    if '=' in param:
                        k, v = param.split('=', 1)
                        query[k] = urllib.parse.unquote(v)

            # Route
            # Streaming endpoints redirect to streaming_port (rkmpi_enc or gkcam streaming server)
            # Control server only handles /control and /api/*
            if path == '/':
                self._serve_homepage(client)
            elif path == '/stream':
                self._redirect_to_streaming(client, '/stream')
            elif path in ('/snapshot', '/snap', '/image'):
                self._redirect_to_streaming(client, '/snapshot')
            elif path == '/flv':
                # FLV is always on FLV_PORT (18088)
                self._redirect_to_streaming(client, '/flv', port=self.FLV_PORT)
            elif path == '/control':
                if method == 'POST':
                    # Read POST body
                    body = ""
                    content_length = 0
                    for line in lines:
                        if line.lower().startswith('content-length:'):
                            content_length = int(line.split(':')[1].strip())
                    if content_length > 0:
                        body_start = request.find('\r\n\r\n')
                        if body_start != -1:
                            body = request[body_start+4:body_start+4+content_length]
                    self._handle_control_post(client, body)
                else:
                    self._serve_control_page(client)
            elif path == '/status':
                self._serve_status(client)
            elif path == '/api/stats':
                self._serve_api_stats(client)
            elif path == '/api/config':
                self._serve_api_config(client)
            elif path == '/api/restart':
                self._handle_restart(client)
            elif path == '/api/led/on':
                self._handle_led_control(client, True)
            elif path == '/api/led/off':
                self._handle_led_control(client, False)
            elif path == '/api/touch' and method == 'POST':
                # Parse POST body for touch coordinates
                content_length = 0
                for line in request.split('\r\n'):
                    if line.lower().startswith('content-length:'):
                        content_length = int(line.split(':')[1].strip())
                body = ''
                if content_length > 0:
                    body_start = request.find('\r\n\r\n')
                    if body_start != -1:
                        body = request[body_start+4:body_start+4+content_length]
                self._handle_touch(client, body)
            # Timelapse management
            elif path == '/timelapse':
                self._serve_timelapse_page(client)
            elif path == '/api/timelapse/list':
                storage = query.get('storage', 'internal')
                self._serve_timelapse_list(client, storage)
            elif path.startswith('/api/timelapse/thumb/'):
                # Extract name from path
                name = urllib.parse.unquote(path[21:])
                storage = query.get('storage', 'internal')
                self._serve_timelapse_thumb(client, name, storage)
            elif path.startswith('/api/timelapse/video/'):
                name = urllib.parse.unquote(path[21:])
                storage = query.get('storage', 'internal')
                self._serve_timelapse_video(client, name, request, storage)
            elif path.startswith('/api/timelapse/delete/') and method == 'DELETE':
                name = urllib.parse.unquote(path[22:])
                storage = query.get('storage', 'internal')
                self._handle_timelapse_delete(client, name, storage)
            elif path == '/api/timelapse/storage':
                self._serve_timelapse_storage(client)
            elif path == '/api/timelapse/browse':
                browse_path = query.get('path', '/mnt/udisk')
                self._serve_timelapse_browse(client, browse_path)
            elif path == '/api/timelapse/mkdir' and method == 'POST':
                content_length = 0
                for line in request.split('\r\n'):
                    if line.lower().startswith('content-length:'):
                        content_length = int(line.split(':')[1].strip())
                body = ''
                if content_length > 0:
                    body_start = request.find('\r\n\r\n') + 4
                    body = request[body_start:body_start + content_length]
                self._handle_timelapse_mkdir(client, body)
            elif path == '/api/timelapse/moonraker':
                self._serve_timelapse_moonraker_status(client)
            elif path == '/api/timelapse/settings' and method == 'POST':
                content_length = 0
                for line in request.split('\r\n'):
                    if line.lower().startswith('content-length:'):
                        content_length = int(line.split(':')[1].strip())
                body = ''
                if content_length > 0:
                    body_start = request.find('\r\n\r\n')
                    if body_start != -1:
                        body = request[body_start+4:body_start+4+content_length]
                self._handle_timelapse_settings(client, body)
            # Camera controls API
            elif path == '/api/camera/controls':
                self._serve_camera_controls(client)
            elif path == '/api/camera/reset' and method == 'POST':
                self._handle_camera_reset(client)
            elif path == '/api/camera/set' and method == 'POST':
                content_length = 0
                for line in request.split('\r\n'):
                    if line.lower().startswith('content-length:'):
                        content_length = int(line.split(':')[1].strip())
                body = ''
                if content_length > 0:
                    body_start = request.find('\r\n\r\n')
                    if body_start != -1:
                        body = request[body_start+4:body_start+4+content_length]
                self._handle_camera_set(client, body)
            else:
                self._serve_404(client)
        except Exception:
            pass
        finally:
            try:
                client.close()
            except Exception:
                pass

    def _serve_mjpeg_stream(self, client):
        """Serve MJPEG stream"""
        response = (
            "HTTP/1.1 200 OK\r\n"
            f"Content-Type: multipart/x-mixed-replace; boundary={self.MJPEG_BOUNDARY}\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n"
        )
        client.sendall(response.encode())

        if self.encoder_type == 'gkcam':
            # GKCAM MODE: decode H.264 frames to JPEG
            self._serve_mjpeg_stream_gkcam(client)
        else:
            # RKMPI MODE: direct MJPEG from encoder
            self._serve_mjpeg_stream_rkmpi(client)

    def _serve_mjpeg_stream_rkmpi(self, client):
        """Serve MJPEG stream from rkmpi encoder"""
        while self.running:
            self.frame_event.wait(timeout=1.0)
            self.frame_event.clear()

            with self.frame_lock:
                frame = self.current_frame

            if frame:
                try:
                    header = f"--{self.MJPEG_BOUNDARY}\r\nContent-Type: image/jpeg\r\nContent-Length: {len(frame)}\r\n\r\n"
                    client.sendall(header.encode() + frame + b"\r\n")
                    self.mjpeg_fps.tick()
                except Exception:
                    break

    def _serve_mjpeg_stream_gkcam(self, client):
        """Serve MJPEG stream from gkcam"""
        # Use streaming transcoder for all-frames mode (provides much better FPS)
        if self.gkcam_all_frames and self.gkcam_transcoder:
            self._serve_mjpeg_stream_gkcam_transcoder(client)
        else:
            self._serve_mjpeg_stream_gkcam_keyframes(client)

    def _serve_mjpeg_stream_gkcam_transcoder(self, client):
        """Serve MJPEG stream from streaming transcoder (all-frames mode)"""
        last_jpeg_time = 0

        while self.running:
            # Wait for new frame from transcoder
            self.gkcam_transcoder.frame_event.wait(timeout=1.0)
            self.gkcam_transcoder.frame_event.clear()

            # Get JPEG directly from transcoder
            jpeg = self.gkcam_transcoder.get_jpeg(max_age=1.0)
            if not jpeg:
                continue

            # Avoid sending same frame twice
            jpeg_time = self.gkcam_transcoder.jpeg_time
            if jpeg_time == last_jpeg_time:
                continue
            last_jpeg_time = jpeg_time

            try:
                header = f"--{self.MJPEG_BOUNDARY}\r\nContent-Type: image/jpeg\r\nContent-Length: {len(jpeg)}\r\n\r\n"
                client.sendall(header.encode() + jpeg + b"\r\n")
                self.mjpeg_fps.tick()
            except Exception:
                break

    def _serve_mjpeg_stream_gkcam_keyframes(self, client):
        """Serve MJPEG stream from gkcam keyframes only (via ffmpeg decode)"""
        if not self.gkcam_reader:
            return

        last_frame_time = 0

        while self.running:
            # Wait for new frame from gkcam reader
            self.gkcam_reader.frame_event.wait(timeout=1.0)
            self.gkcam_reader.frame_event.clear()

            # Get H.264 frame and decode to JPEG
            h264_frame = self.gkcam_reader.get_frame()
            if not h264_frame:
                continue

            # Avoid decoding same frame twice
            frame_time = self.gkcam_reader.last_frame_time
            if frame_time == last_frame_time:
                continue
            last_frame_time = frame_time

            # Decode H.264 to JPEG
            jpeg = decode_h264_to_jpeg(h264_frame)
            if not jpeg:
                continue

            try:
                header = f"--{self.MJPEG_BOUNDARY}\r\nContent-Type: image/jpeg\r\nContent-Length: {len(jpeg)}\r\n\r\n"
                client.sendall(header.encode() + jpeg + b"\r\n")
                self.mjpeg_fps.tick()
            except Exception:
                break

    def _serve_snapshot(self, client):
        """Serve single JPEG snapshot"""
        if self.encoder_type == 'gkcam':
            frame = self._get_snapshot_gkcam()
        else:
            frame = self._get_snapshot_rkmpi()

        if not frame:
            self._serve_503(client)
            return

        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: image/jpeg\r\n"
            f"Content-Length: {len(frame)}\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n"
        )
        client.sendall(response.encode() + frame)

    def _get_snapshot_rkmpi(self):
        """Get JPEG snapshot from rkmpi encoder"""
        self.frame_event.wait(timeout=2.0)
        with self.frame_lock:
            return self.current_frame

    def _get_snapshot_gkcam(self):
        """Get JPEG snapshot from gkcam"""
        # Prefer transcoder (has more recent frames in all-frames mode)
        if self.gkcam_transcoder:
            jpeg = self.gkcam_transcoder.get_jpeg(max_age=0.5)
            if jpeg:
                return jpeg
        # Fall back to keyframe decoder
        if self.gkcam_reader:
            return self.gkcam_reader.get_jpeg(max_age=0.5)
        return None

    def _serve_flv_stream(self, client):
        """Serve FLV stream"""
        # Track client and spawn responder subprocesses when first client connects
        with self.flv_client_lock:
            self.flv_client_count += 1
            print(f"FLV client connected (total: {self.flv_client_count})", flush=True)

            # Spawn responder subprocesses when first client connects
            if self.flv_client_count == 1:
                script_path = os.path.abspath(__file__)

                # Spawn RPC responder subprocess
                if self.rpc_subprocess is None:
                    try:
                        self.rpc_subprocess = subprocess.Popen(
                            ['python', script_path, '--rpc-responder'],
                            stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT
                        )
                        print(f"RPC subprocess spawned (PID {self.rpc_subprocess.pid})", flush=True)
                    except Exception as e:
                        print(f"Failed to spawn RPC subprocess: {e}", flush=True)

                # Spawn MQTT responder subprocess
                if self.mqtt_subprocess is None:
                    try:
                        self.mqtt_subprocess = subprocess.Popen(
                            ['python', script_path, '--mqtt-responder'],
                            stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT
                        )
                        print(f"MQTT subprocess spawned (PID {self.mqtt_subprocess.pid})", flush=True)
                    except Exception as e:
                        print(f"Failed to spawn MQTT subprocess: {e}", flush=True)

        try:
            # Match gkcam's HTTP headers exactly to prevent slicer disconnection
            # gkcam uses text/plain and a huge Content-Length for streaming
            response = (
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: 99999999999\r\n"
                "\r\n"
            )
            client.sendall(response.encode())

            # Create fresh muxer for this connection, initialized with cached SPS/PPS
            muxer = FLVMuxer(
                self.camera_width, self.camera_height, 10,
                initial_sps=self.cached_sps,
                initial_pps=self.cached_pps
            )

            # Send FLV header
            client.sendall(muxer.create_header())

            # Send FLV metadata (onMetaData) - helps player decode properly
            client.sendall(muxer.create_metadata())

            # Send AVC decoder config immediately if we have cached SPS/PPS
            # This helps players initialize faster
            if self.cached_sps and self.cached_pps:
                config = muxer.create_avc_decoder_config(self.cached_sps, self.cached_pps)
                video_data = bytearray()
                video_data.append(0x17)  # keyframe + AVC
                video_data.append(0x00)  # AVC sequence header
                video_data.extend(b'\x00\x00\x00')  # composition time
                video_data.extend(config)
                client.sendall(muxer.create_tag(9, video_data, 0))
                muxer.has_sps_pps = True

            # Track if we've sent a keyframe yet (needed for player to start decoding)
            sent_keyframe = False

            while self.running:
                # Get H.264 data
                h264_data = None
                with self.h264_lock:
                    if self.h264_buffer:
                        h264_data = self.h264_buffer.popleft()

                if h264_data:
                    try:
                        # Scan ALL NAL units in the frame to find keyframes and SPS/PPS
                        # The encoder often sends [SPS, PPS, IDR] as one frame
                        contains_keyframe = False
                        contains_sps_pps = False
                        i = 0
                        data_len = len(h264_data)
                        while i < data_len - 4:
                            # Check for start codes
                            if h264_data[i:i+4] == RE_NAL_START_4:
                                nal_start = i + 4
                            elif h264_data[i:i+3] == RE_NAL_START_3:
                                nal_start = i + 3
                            else:
                                i += 1
                                continue
                            if nal_start < data_len:
                                nal_type = h264_data[nal_start] & 0x1F
                                if nal_type == 5:  # IDR keyframe
                                    contains_keyframe = True
                                elif nal_type in (7, 8):  # SPS or PPS
                                    contains_sps_pps = True
                            i = nal_start

                        # Skip P-frames until we've sent a keyframe
                        # Always allow SPS/PPS and keyframes through
                        if not sent_keyframe and not contains_keyframe and not contains_sps_pps:
                            time.sleep(0.01)  # Prevent busy loop while waiting for keyframe
                            continue

                        flv_data = muxer.mux_frame(h264_data)
                        if flv_data:
                            client.sendall(flv_data)
                            if contains_keyframe:
                                sent_keyframe = True
                    except Exception:
                        break
                else:
                    time.sleep(0.05)
        finally:
            # Decrement client count when disconnected
            with self.flv_client_lock:
                self.flv_client_count -= 1
                print(f"FLV client disconnected (total: {self.flv_client_count})", flush=True)

                # Kill responder subprocesses when last client disconnects
                if self.flv_client_count == 0:
                    # Kill RPC subprocess
                    if self.rpc_subprocess is not None:
                        try:
                            self.rpc_subprocess.terminate()
                            self.rpc_subprocess.wait(timeout=2)
                            print(f"RPC subprocess terminated", flush=True)
                        except Exception as e:
                            print(f"Error terminating RPC subprocess: {e}", flush=True)
                            try:
                                self.rpc_subprocess.kill()
                            except:
                                pass
                        self.rpc_subprocess = None

                    # Kill MQTT subprocess
                    if self.mqtt_subprocess is not None:
                        try:
                            self.mqtt_subprocess.terminate()
                            self.mqtt_subprocess.wait(timeout=2)
                            print(f"MQTT subprocess terminated", flush=True)
                        except Exception as e:
                            print(f"Error terminating MQTT subprocess: {e}", flush=True)
                            try:
                                self.mqtt_subprocess.kill()
                            except:
                                pass
                        self.mqtt_subprocess = None

    def _serve_control_page(self, client):
        """Serve control page HTML"""
        cpu_stats = self.cpu_monitor.get_stats()

        # Get FPS and client counts from appropriate source
        if self.encoder_type == 'gkcam':
            # gkcam mode: use local counters
            if self.gkcam_transcoder:
                mjpeg_fps = round(self.gkcam_transcoder.fps_counter.get_fps(), 1)
            else:
                mjpeg_fps = round(self.mjpeg_fps.get_fps(), 1)
            if self.gkcam_reader:
                h264_fps_val = round(self.gkcam_reader.fps_counter.get_fps(), 1)
            else:
                h264_fps_val = 0.0
            mjpeg_clients = 0  # Not tracked in gkcam mode
            flv_clients = 0
        elif self.is_rkmpi_mode():
            # rkmpi server mode: use encoder stats from control file
            mjpeg_fps = round(self.encoder_mjpeg_fps, 1)
            h264_fps_val = round(self.encoder_h264_fps, 1)
            mjpeg_clients = self.encoder_mjpeg_clients
            flv_clients = self.encoder_flv_clients
        else:
            # Fallback to local counters
            mjpeg_fps = round(self.mjpeg_fps.get_fps(), 1)
            h264_fps_val = round(self.h264_fps.get_fps(), 1)
            mjpeg_clients = 0
            flv_clients = 0

        # For rkmpi modes, streaming is on a different port than control page
        # Pass the streaming port to JavaScript for building URLs
        streaming_port = self.streaming_port

        html = f'''<!DOCTYPE html>
<html>
<head>
    <title>H264 Streamer Control</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <script src="https://cdn.jsdelivr.net/npm/flv.js@1.6.2/dist/flv.min.js"></script>
    <style>
        body {{ font-family: sans-serif; margin: 20px; background: #1a1a1a; color: #fff; }}
        .container {{ max-width: 900px; margin: 0 auto; }}
        h1 {{ color: #4CAF50; }}
        .section {{ background: #2d2d2d; padding: 15px; margin: 10px 0; border-radius: 8px; }}
        .section h2 {{ margin-top: 0; color: #888; font-size: 14px; text-transform: uppercase; }}
        .row {{ display: flex; justify-content: space-between; align-items: center; margin: 10px 0; }}
        .label {{ color: #ccc; }}
        /* Settings layout - controls aligned right, centered content */
        #settings-form {{ padding: 0 12%; }}
        .setting {{ margin-bottom: 18px; }}
        .setting-row {{ display: flex; justify-content: space-between; align-items: center; gap: 30px; }}
        .setting-row .label {{ flex-shrink: 0; white-space: nowrap; }}
        .setting-row .control {{ display: flex; align-items: center; gap: 10px; justify-content: flex-end; }}
        .setting-row .slider-control {{ display: flex; align-items: center; gap: 10px; flex: 1; max-width: 320px; }}
        .setting-row .slider-control input[type="range"] {{ flex: 1; min-width: 200px; }}
        .setting-note {{ text-align: right; color: #888; font-size: 12px; margin-top: 6px; }}
        .value {{ color: #4CAF50; font-weight: bold; }}
        button {{ background: #4CAF50; color: white; border: none; padding: 10px 20px;
                  border-radius: 4px; cursor: pointer; font-size: 14px; margin: 2px; }}
        button:hover {{ background: #45a049; }}
        button.secondary {{ background: #555; }}
        button.secondary:hover {{ background: #666; }}
        button.danger {{ background: #f44336; }}
        button.danger:hover {{ background: #da190b; }}
        input[type="number"] {{ width: 60px; padding: 8px; border-radius: 4px; border: 1px solid #555;
                                background: #333; color: #fff; }}
        .toggle {{ position: relative; display: inline-block; width: 50px; height: 26px; }}
        .toggle input {{ opacity: 0; width: 0; height: 0; }}
        .slider {{ position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0;
                   background-color: #555; transition: .3s; border-radius: 26px; }}
        .slider:before {{ position: absolute; content: ""; height: 20px; width: 20px; left: 3px;
                         bottom: 3px; background-color: white; transition: .3s; border-radius: 50%; }}
        input:checked + .slider {{ background-color: #4CAF50; }}
        input:checked + .slider:before {{ transform: translateX(24px); }}
        .preview {{ margin: 10px 0; }}
        .preview img, .preview video {{ max-width: 100%; border-radius: 4px; background: #000; }}
        .links a {{ color: #4CAF50; margin-right: 15px; }}
        #cpu-stats {{ font-family: monospace; }}
        .tabs {{ display: flex; gap: 5px; margin-bottom: 10px; }}
        .tabs button {{ flex: 1; }}
        .tabs button.active {{ background: #4CAF50; }}
        .tab-content {{ display: none; }}
        .tab-content.active {{ display: block; }}
        .stats-grid {{ display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }}
        .stat-box {{ background: #222; padding: 10px; border-radius: 4px; text-align: center; }}
        .stat-value {{ font-size: 24px; color: #4CAF50; font-weight: bold; }}
        .stat-label {{ font-size: 12px; color: #888; }}
        #flv-player {{ width: 100%; max-height: 480px; }}
        .player-controls {{ margin-top: 10px; }}
        /* Loading overlay for restart */
        .loading-overlay {{
            display: none;
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            background: rgba(0, 0, 0, 0.85);
            z-index: 9999;
            justify-content: center;
            align-items: center;
            flex-direction: column;
        }}
        .loading-overlay.active {{ display: flex; }}
        .spinner {{
            width: 50px;
            height: 50px;
            border: 4px solid #333;
            border-top: 4px solid #4CAF50;
            border-radius: 50%;
            animation: spin 1s linear infinite;
        }}
        @keyframes spin {{
            0% {{ transform: rotate(0deg); }}
            100% {{ transform: rotate(360deg); }}
        }}
        .loading-text {{
            margin-top: 20px;
            color: #4CAF50;
            font-size: 18px;
        }}
        .loading-status {{
            margin-top: 10px;
            color: #888;
            font-size: 14px;
        }}
        /* Camera controls */
        .camera-controls-section h2 {{
            margin-bottom: 10px;
        }}
        .camera-controls-grid {{
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
            gap: 20px;
        }}
        .cam-ctrl-group {{
            background: #252525;
            padding: 15px;
            border-radius: 6px;
        }}
        .cam-ctrl-group h3 {{
            margin: 0 0 12px 0;
            font-size: 14px;
            color: #4CAF50;
            border-bottom: 1px solid #444;
            padding-bottom: 8px;
        }}
        .cam-ctrl {{
            display: flex;
            align-items: center;
            margin-bottom: 10px;
            gap: 10px;
        }}
        .cam-ctrl label {{
            flex: 0 0 100px;
            font-size: 12px;
            color: #aaa;
        }}
        .cam-ctrl input[type="range"] {{
            flex: 1;
            height: 6px;
            background: #444;
            border-radius: 3px;
            -webkit-appearance: none;
            appearance: none;
        }}
        .cam-ctrl input[type="range"]::-webkit-slider-thumb {{
            -webkit-appearance: none;
            appearance: none;
            width: 14px;
            height: 14px;
            background: #4CAF50;
            border-radius: 50%;
            cursor: pointer;
        }}
        .cam-ctrl input[type="range"]::-moz-range-thumb {{
            width: 14px;
            height: 14px;
            background: #4CAF50;
            border-radius: 50%;
            cursor: pointer;
            border: none;
        }}
        .cam-ctrl span {{
            flex: 0 0 50px;
            text-align: right;
            font-size: 12px;
            color: #888;
            font-family: monospace;
        }}
        .cam-ctrl select {{
            flex: 1;
            padding: 6px;
            border-radius: 4px;
            border: 1px solid #555;
            background: #333;
            color: #fff;
            font-size: 12px;
        }}
    </style>
</head>
<body>
    <!-- Loading overlay for restart -->
    <div id="loading-overlay" class="loading-overlay">
        <div class="spinner"></div>
        <div class="loading-text">Restarting H264 Streamer...</div>
        <div class="loading-status" id="loading-status">Saving settings...</div>
    </div>

    <div class="container">
        <h1>H264 Streamer Control</h1>

        <div class="section">
            <h2 style="display:inline;">Live Preview</h2>
            <span style="color:#f44;font-size:12px;margin-left:15px;background:#411;padding:4px 8px;border-radius:4px;">WARNING: Debug tool - increases CPU usage, do not leave open!</span>
            <div class="tabs">
                <button class="active" onclick="switchTab('snapshot')">Snapshot</button>
                <button onclick="switchTab('mjpeg')">MJPEG Stream</button>
                <button onclick="switchTab('flv')">H.264 Stream</button>
                <button onclick="switchTab('display')">Display</button>
            </div>
            <div class="preview">
                <div id="tab-snapshot" class="tab-content active">
                    <img src="/snapshot" id="preview-img" onclick="this.src='/snapshot?'+Date.now()">
                    <p style="color:#888;font-size:12px">Click image to refresh</p>
                </div>
                <div id="tab-mjpeg" class="tab-content">
                    <img id="mjpeg-stream" style="display:none">
                    <p style="color:#888;font-size:12px">Live MJPEG stream</p>
                </div>
                <div id="tab-flv" class="tab-content">
                    <video id="flv-player" muted autoplay></video>
                    <div class="player-controls">
                        <button onclick="startFlvPlayer()">Play</button>
                        <button class="secondary" onclick="stopFlvPlayer()">Stop</button>
                        <span id="flv-status" style="margin-left:10px;color:#888;"></span>
                    </div>
                </div>
                <div id="tab-display" class="tab-content">
                    <img id="display-stream" style="display:none">
                    <p style="color:#888;font-size:12px">Printer LCD framebuffer - Click to touch</p>
                </div>
            </div>
            <div style="margin-top:10px;display:flex;justify-content:space-between;align-items:center;">
                <div style="display:flex;gap:5px;">
                    <button onclick="window.open(streamBase + '/stream', '_blank')" class="secondary" style="padding:5px 12px;font-size:12px;">Open MJPEG</button>
                    <button onclick="window.open(streamBase + '/snapshot', '_blank')" class="secondary" style="padding:5px 12px;font-size:12px;">Open Snapshot</button>
                    <button onclick="openFlvFullscreen()" class="secondary" style="padding:5px 12px;font-size:12px;">Open FLV</button>
                    <button onclick="window.open(streamBase + '/display', '_blank')" class="secondary" style="padding:5px 12px;font-size:12px;">Open Display</button>
                    <button onclick="window.open('/timelapse', 'timelapse_' + location.hostname.replace(/\\./g, '_'), 'width=900,height=700')" class="secondary" style="padding:5px 12px;font-size:12px;">Time Lapse</button>
                </div>
                <div style="display:flex;gap:5px;align-items:center;">
                    <span style="color:#888;font-size:12px;margin-right:5px;">LED:</span>
                    <button onclick="controlLed(true)" style="padding:5px 12px;font-size:12px;">On</button>
                    <button onclick="controlLed(false)" class="secondary" style="padding:5px 12px;font-size:12px;">Off</button>
                </div>
            </div>
        </div>

        <div class="section camera-controls-section">
            <h2 onclick="toggleCameraControls()" style="cursor:pointer;user-select:none;">
                <span id="camera-controls-arrow">&#9654;</span> CAMERA CONTROLS
                <span style="font-size:12px;color:#888;font-weight:normal;margin-left:10px;">(click to expand)</span>
            </h2>
            <div id="camera-controls-panel" style="display:none;">
                <div class="camera-controls-grid">
                    <div class="cam-ctrl-group">
                        <h3>Image</h3>
                        <div class="cam-ctrl">
                            <label>Brightness</label>
                            <input type="range" id="cam-brightness" min="0" max="255" value="0" oninput="setCameraControl('brightness', this.value)">
                            <span id="cam-brightness-val">0</span>
                        </div>
                        <div class="cam-ctrl">
                            <label>Contrast</label>
                            <input type="range" id="cam-contrast" min="0" max="255" value="32" oninput="setCameraControl('contrast', this.value)">
                            <span id="cam-contrast-val">32</span>
                        </div>
                        <div class="cam-ctrl">
                            <label>Saturation</label>
                            <input type="range" id="cam-saturation" min="0" max="132" value="85" oninput="setCameraControl('saturation', this.value)">
                            <span id="cam-saturation-val">85</span>
                        </div>
                        <div class="cam-ctrl">
                            <label>Hue</label>
                            <input type="range" id="cam-hue" min="-180" max="180" value="0" oninput="setCameraControl('hue', this.value)">
                            <span id="cam-hue-val">0</span>
                        </div>
                        <div class="cam-ctrl">
                            <label>Gamma</label>
                            <input type="range" id="cam-gamma" min="90" max="150" value="100" oninput="setCameraControl('gamma', this.value)">
                            <span id="cam-gamma-val">100</span>
                        </div>
                        <div class="cam-ctrl">
                            <label>Sharpness</label>
                            <input type="range" id="cam-sharpness" min="0" max="30" value="3" oninput="setCameraControl('sharpness', this.value)">
                            <span id="cam-sharpness-val">3</span>
                        </div>
                    </div>
                    <div class="cam-ctrl-group">
                        <h3>Exposure</h3>
                        <div class="cam-ctrl">
                            <label>Auto Exposure</label>
                            <select id="cam-exposure-auto" onchange="setCameraControl('exposure_auto', this.value)">
                                <option value="1">Manual</option>
                                <option value="3" selected>Auto (Aperture Priority)</option>
                            </select>
                        </div>
                        <div class="cam-ctrl">
                            <label>Exposure</label>
                            <input type="range" id="cam-exposure" min="10" max="2500" value="156" oninput="setCameraControl('exposure', this.value)">
                            <span id="cam-exposure-val">156</span>
                        </div>
                        <div class="cam-ctrl">
                            <label>Exposure Priority</label>
                            <select id="cam-exposure-priority" onchange="setCameraControl('exposure_priority', this.value)">
                                <option value="0" selected>Constant FPS</option>
                                <option value="1">Variable FPS</option>
                            </select>
                        </div>
                        <div class="cam-ctrl">
                            <label>Gain</label>
                            <select id="cam-gain" onchange="setCameraControl('gain', this.value)">
                                <option value="0">Off</option>
                                <option value="1" selected>On</option>
                            </select>
                        </div>
                        <div class="cam-ctrl">
                            <label>Backlight Comp</label>
                            <input type="range" id="cam-backlight" min="0" max="7" value="0" oninput="setCameraControl('backlight', this.value)">
                            <span id="cam-backlight-val">0</span>
                        </div>
                    </div>
                    <div class="cam-ctrl-group">
                        <h3>White Balance</h3>
                        <div class="cam-ctrl">
                            <label>Auto WB</label>
                            <select id="cam-wb-auto" onchange="setCameraControl('wb_auto', this.value); updateWbTempState();">
                                <option value="0">Manual</option>
                                <option value="1" selected>Auto</option>
                            </select>
                        </div>
                        <div class="cam-ctrl">
                            <label>Temperature</label>
                            <input type="range" id="cam-wb-temp" min="2800" max="6500" value="4000" oninput="setCameraControl('wb_temp', this.value)">
                            <span id="cam-wb-temp-val">4000K</span>
                        </div>
                        <div class="cam-ctrl">
                            <label>Power Line</label>
                            <select id="cam-power-line" onchange="setCameraControl('power_line', this.value)">
                                <option value="0">Disabled</option>
                                <option value="1" selected>50 Hz</option>
                                <option value="2">60 Hz</option>
                            </select>
                        </div>
                    </div>
                </div>
                <div style="margin-top:15px;text-align:center;">
                    <button type="button" onclick="resetCameraDefaults()" class="secondary" style="padding:8px 20px;">Reset to Defaults</button>
                </div>
            </div>
        </div>

        <div class="section">
            <h2>Performance</h2>
            <div class="stats-grid">
                <div class="stat-box">
                    <div class="stat-value" id="stat-mjpeg-fps">{mjpeg_fps}</div>
                    <div class="stat-label">MJPEG FPS</div>
                </div>
                <div class="stat-box">
                    <div class="stat-value" id="stat-h264-fps">{h264_fps_val}</div>
                    <div class="stat-label">H.264 FPS</div>
                </div>
                <div class="stat-box">
                    <div class="stat-value" id="stat-clients">{mjpeg_clients + flv_clients}</div>
                    <div class="stat-label">Clients</div>
                </div>
                <div class="stat-box">
                    <div class="stat-value" id="stat-cpu-total">{cpu_stats['total']}%</div>
                    <div class="stat-label">Total CPU</div>
                </div>
                <div class="stat-box">
                    <div class="stat-value" id="stat-cpu-encoder">-</div>
                    <div class="stat-label">Encoder CPU</div>
                </div>
                <div class="stat-box">
                    <div class="stat-value" id="stat-cpu-streamer">-</div>
                    <div class="stat-label">Streamer CPU</div>
                </div>
            </div>
        </div>

        <div class="section">
            <h2>Settings</h2>
            <form id="settings-form">
                <div class="setting">
                    <div class="setting-row">
                        <span class="label">Encoder Type:</span>
                        <div class="control">
                            <select name="encoder_type" id="encoder_type" onchange="handleEncoderChange(this)" style="padding:8px;border-radius:4px;border:1px solid #555;background:#333;color:#fff;">
                                <option value="gkcam" {'selected' if self.encoder_type == 'gkcam' else ''}>gkcam (built-in)</option>
                                <option value="rkmpi" {'selected' if self.encoder_type == 'rkmpi' else ''}>rkmpi (HW MJPEG)</option>
                                <option value="rkmpi-yuyv" {'selected' if self.encoder_type == 'rkmpi-yuyv' else ''}>rkmpi-yuyv (HW H.264)</option>
                            </select>
                        </div>
                    </div>
                    <div class="setting-note">Requires restart</div>
                </div>
                <div class="setting gkcam-only" style="{'display:none' if self.is_rkmpi_mode() else ''}">
                    <div class="setting-row">
                        <span class="label">Decode All Frames:</span>
                        <div class="control">
                            <label class="toggle">
                                <input type="checkbox" name="gkcam_all_frames" {'checked' if self.gkcam_all_frames else ''}>
                                <span class="slider"></span>
                            </label>
                        </div>
                    </div>
                    <div class="setting-note">Off = keyframes only, lower CPU</div>
                </div>
                <div class="setting">
                    <div class="setting-row">
                        <span class="label">Auto Enable LAN Mode:</span>
                        <div class="control">
                            <label class="toggle">
                                <input type="checkbox" name="autolanmode" {'checked' if self.autolanmode else ''}>
                                <span class="slider"></span>
                            </label>
                        </div>
                    </div>
                </div>
                <div class="setting">
                    <div class="setting-row">
                        <span class="label">Debug Logging:</span>
                        <div class="control">
                            <label class="toggle">
                                <input type="checkbox" name="logging" {'checked' if self.logging else ''}>
                                <span class="slider"></span>
                            </label>
                        </div>
                    </div>
                    <div class="setting-note">Requires restart</div>
                </div>
                <div class="setting rkmpi-mjpeg-only" style="{'display:none' if self.encoder_type != 'rkmpi' else ''}">
                    <div class="setting-row">
                        <span class="label">H.264 Encoding:</span>
                        <div class="control">
                            <label class="toggle">
                                <input type="checkbox" name="h264_enabled" {'checked' if self.h264_enabled else ''}>
                                <span class="slider"></span>
                            </label>
                        </div>
                    </div>
                </div>
                <div class="setting rkmpi-mjpeg-only" style="{'display:none' if self.encoder_type != 'rkmpi' else ''}">
                    <div class="setting-row">
                        <span class="label">MJPEG Frame Rate:</span>
                        <div class="slider-control">
                            <input type="range" id="mjpeg_fps_slider" name="mjpeg_fps" min="2" max="{self.max_camera_fps}" value="{self.mjpeg_fps_target}"
                                   oninput="document.getElementById('mjpeg_fps_input').value=this.value;updateMjpegFpsLabel();">
                            <input type="number" id="mjpeg_fps_input" min="2" max="{self.max_camera_fps}" value="{self.mjpeg_fps_target}"
                                   style="width:50px;"
                                   oninput="document.getElementById('mjpeg_fps_slider').value=this.value;document.querySelector('[name=mjpeg_fps]').value=this.value;updateMjpegFpsLabel();">
                            <span>fps</span>
                        </div>
                    </div>
                    <div class="setting-note"><span id="mjpeg_fps_label"></span></div>
                </div>
                <div class="setting rkmpi-yuyv-only" style="{'display:none' if self.encoder_type != 'rkmpi-yuyv' else ''}">
                    <div class="setting-row">
                        <span class="label">Moonraker FPS:</span>
                        <div class="slider-control">
                            <input type="range" id="mjpeg_fps_slider_yuyv" name="mjpeg_fps" min="2" max="30" value="{self.mjpeg_fps_target}"
                                   oninput="document.getElementById('mjpeg_fps_input_yuyv').value=this.value;">
                            <input type="number" id="mjpeg_fps_input_yuyv" min="2" max="30" value="{self.mjpeg_fps_target}"
                                   style="width:50px;"
                                   oninput="document.getElementById('mjpeg_fps_slider_yuyv').value=this.value;document.querySelector('[name=mjpeg_fps]').value=this.value;">
                            <span>fps</span>
                        </div>
                    </div>
                    <div class="setting-note">Only for Moonraker USB camera setup</div>
                </div>
                <div class="setting rkmpi-mjpeg-only" style="{'display:none' if self.encoder_type != 'rkmpi' else ''}">
                    <div class="setting-row">
                        <span class="label">H.264 Frame Rate:</span>
                        <div class="slider-control">
                            <input type="range" id="fps_pct_slider" min="0" max="100" value="{max(1, min(100, round(100 / self.saved_skip_ratio)))}"
                                   {'disabled' if self.auto_skip else ''}
                                   oninput="document.getElementById('fps_pct_input').value=this.value;updateFpsLabel();savedSkipRatio=pctToSkipRatio(parseInt(this.value));">
                            <input type="number" id="fps_pct_input" min="0" max="100" value="{max(1, min(100, round(100 / self.saved_skip_ratio)))}"
                                   style="width:50px;" {'disabled' if self.auto_skip else ''}
                                   oninput="document.getElementById('fps_pct_slider').value=this.value;updateFpsLabel();savedSkipRatio=pctToSkipRatio(parseInt(this.value));">
                            <span>%</span>
                        </div>
                    </div>
                    <div class="setting-note"><span id="fps_label"></span></div>
                    <input type="hidden" name="skip_ratio" id="skip_ratio_hidden" value="{self.saved_skip_ratio}">
                </div>
                <div class="setting rkmpi-mjpeg-only" style="{'display:none' if self.encoder_type != 'rkmpi' else ''}">
                    <div class="setting-row">
                        <span class="label">Auto Skip (CPU-based):</span>
                        <div class="control">
                            <label class="toggle">
                                <input type="checkbox" name="auto_skip" {'checked' if self.auto_skip else ''} onchange="toggleAutoSkip(this.checked);">
                                <span class="slider"></span>
                            </label>
                        </div>
                    </div>
                </div>
                <div class="setting rkmpi-mjpeg-only" style="{'display:none' if self.encoder_type != 'rkmpi' else ''}">
                    <div class="setting-row">
                        <span class="label">Target CPU % (for auto-skip):</span>
                        <div class="control">
                            <input type="number" name="target_cpu" value="{self.target_cpu}" min="25" max="90" {'disabled' if not self.auto_skip else ''}>
                        </div>
                    </div>
                </div>
                <div class="setting rkmpi-only" style="{'display:none' if not self.is_rkmpi_mode() else ''}">
                    <div class="setting-row">
                        <span class="label">H.264 Bitrate (kbps):</span>
                        <div class="control">
                            <input type="number" name="bitrate" value="{self.bitrate}" min="100" max="4000" style="width:80px;">
                        </div>
                    </div>
                    <div class="setting-note">Requires restart</div>
                </div>
                <div class="setting rkmpi-only" style="{'display:none' if not self.is_rkmpi_mode() else ''}">
                    <div class="setting-row">
                        <span class="label">Camera Resolution:</span>
                        <div class="control">
                            <select name="h264_resolution" id="camera_resolution" style="padding:8px;border-radius:4px;border:1px solid #555;background:#333;color:#fff;">
                                <option value="1280x720" {'selected' if self.h264_resolution == '1280x720' else ''}>1280x720 (full)</option>
                                <option value="640x360" {'selected' if self.h264_resolution == '640x360' else ''}>640x360 (scaled)</option>
                            </select>
                        </div>
                    </div>
                    <div class="setting-note rkmpi-mjpeg-note" style="{'display:none' if self.encoder_type != 'rkmpi' else ''}">Lower resolution = less TurboJPEG decode CPU. Requires restart.</div>
                    <div class="setting-note rkmpi-yuyv-note" style="{'display:none' if self.encoder_type != 'rkmpi-yuyv' else ''}">Lower resolution = more FPS. Requires restart.</div>
                </div>
                <div class="setting rkmpi-only" style="{'display:none' if not self.is_rkmpi_mode() else ''}; border-top: 1px solid #444; padding-top: 15px; margin-top: 15px;">
                    <div class="setting-row">
                        <span class="label">Display Capture:</span>
                        <div class="control">
                            <label class="toggle">
                                <input type="checkbox" name="display_enabled" {'checked' if self.display_enabled else ''} onchange="updateDisplaySettings()">
                                <span class="slider"></span>
                            </label>
                        </div>
                    </div>
                    <div class="setting-note">Capture printer LCD screen (full hardware acceleration)</div>
                </div>
                <div class="setting rkmpi-only" style="{'display:none' if not self.is_rkmpi_mode() else ''}">
                    <div class="setting-row">
                        <span class="label">Display FPS:</span>
                        <div class="control">
                            <select name="display_fps" id="display_fps_select" style="padding:8px;border-radius:4px;border:1px solid #555;background:#333;color:#fff;" {'disabled' if not self.display_enabled else ''}>
                                <option value="1" {'selected' if self.display_fps == 1 else ''}>1 fps (lowest CPU)</option>
                                <option value="2" {'selected' if self.display_fps == 2 else ''}>2 fps</option>
                                <option value="5" {'selected' if self.display_fps == 5 else ''}>5 fps</option>
                                <option value="10" {'selected' if self.display_fps == 10 else ''}>10 fps (highest)</option>
                            </select>
                        </div>
                    </div>
                    <div class="setting-note">Higher FPS = more CPU usage</div>
                </div>
                <div class="setting" style="margin-top: 15px;">
                    <div class="setting-row">
                        <button type="submit">Apply Settings</button>
                        <span style="color:#f90;font-size:12px;">Note: Encoder type change requires app restart</span>
                    </div>
                </div>
            </form>
        </div>

        <div class="section rkmpi-only" style="{'display:none' if not self.is_rkmpi_mode() else ''}">
            <h2>Timelapse Settings</h2>
            <p style="color:#888;font-size:12px;margin-bottom:15px;">Independent timelapse recording via Moonraker integration. Records regardless of slicer settings.</p>
            <form id="timelapse-form">
                <div class="setting">
                    <div class="setting-row">
                        <span class="label">Enable Timelapse:</span>
                        <div class="control">
                            <label class="toggle">
                                <input type="checkbox" name="timelapse_enabled" id="timelapse_enabled" {'checked' if self.timelapse_enabled else ''} onchange="updateTimelapseSettings()">
                                <span class="slider"></span>
                            </label>
                        </div>
                    </div>
                    <div class="setting-note">Auto-record timelapse for all prints via Moonraker</div>
                </div>
                <div class="setting timelapse-setting" style="{'display:none' if not self.timelapse_enabled else ''}">
                    <div class="setting-row">
                        <span class="label">Mode:</span>
                        <div class="control">
                            <select name="timelapse_mode" id="timelapse_mode" onchange="updateTimelapseModeSettings()" style="padding:8px;border-radius:4px;border:1px solid #555;background:#333;color:#fff;">
                                <option value="layer" {'selected' if self.timelapse_mode == 'layer' else ''}>Layer-based</option>
                                <option value="hyperlapse" {'selected' if self.timelapse_mode == 'hyperlapse' else ''}>Hyperlapse (time-based)</option>
                            </select>
                        </div>
                    </div>
                </div>
                <div class="setting timelapse-setting hyperlapse-only" style="{'display:none' if not self.timelapse_enabled or self.timelapse_mode != 'hyperlapse' else ''}">
                    <div class="setting-row">
                        <span class="label">Hyperlapse Interval:</span>
                        <div class="control">
                            <input type="number" name="timelapse_hyperlapse_interval" value="{self.timelapse_hyperlapse_interval}" min="5" max="300" style="width:60px;">
                            <span style="margin-left:5px;">seconds</span>
                        </div>
                    </div>
                </div>
                <div class="setting timelapse-setting" style="{'display:none' if not self.timelapse_enabled else ''}; border-top: 1px solid #444; padding-top: 15px; margin-top: 15px;">
                    <div class="setting-row">
                        <span class="label">Storage Location:</span>
                        <div class="control">
                            <select name="timelapse_storage" id="timelapse_storage" onchange="updateStorageSettings()" style="padding:8px;border-radius:4px;border:1px solid #555;background:#333;color:#fff;">
                                <option value="internal" {'selected' if self.timelapse_storage == 'internal' else ''}>Internal Flash</option>
                                <option value="usb" {'selected' if self.timelapse_storage == 'usb' else ''}>USB Drive</option>
                            </select>
                        </div>
                    </div>
                </div>
                <div class="setting timelapse-setting" style="{'display:none' if not self.timelapse_enabled else ''}">
                    <div class="setting-row">
                        <span class="label">USB Status:</span>
                        <div class="control">
                            <span id="usb_status" style="color:#888;">Checking...</span>
                        </div>
                    </div>
                </div>
                <div class="setting timelapse-setting usb-path-setting" style="{'display:none' if not self.timelapse_enabled or self.timelapse_storage != 'usb' else ''}">
                    <div class="setting-row">
                        <span class="label">USB Path:</span>
                        <div class="control" style="display:flex;gap:5px;">
                            <input type="text" name="timelapse_usb_path" id="timelapse_usb_path" value="{self.timelapse_usb_path}" readonly style="flex:1;min-width:150px;padding:8px;border-radius:4px;border:1px solid #555;background:#222;color:#aaa;cursor:pointer;" onclick="openFolderPicker()" title="Click to browse">
                            <button type="button" onclick="openFolderPicker()" style="padding:8px 12px;cursor:pointer;" title="Browse folders">&#128193;</button>
                        </div>
                    </div>
                </div>
                <div class="setting timelapse-setting" style="{'display:none' if not self.timelapse_enabled else ''}; border-top: 1px solid #444; padding-top: 15px; margin-top: 15px;">
                    <div class="setting-row">
                        <span class="label">Moonraker Host:</span>
                        <div class="control">
                            <input type="text" name="moonraker_host" value="{self.moonraker_host}" style="width:120px;padding:8px;border-radius:4px;border:1px solid #555;background:#333;color:#fff;">
                        </div>
                    </div>
                </div>
                <div class="setting timelapse-setting" style="{'display:none' if not self.timelapse_enabled else ''}">
                    <div class="setting-row">
                        <span class="label">Moonraker Port:</span>
                        <div class="control">
                            <input type="number" name="moonraker_port" value="{self.moonraker_port}" min="1" max="65535" style="width:80px;">
                        </div>
                    </div>
                </div>
                <div class="setting timelapse-setting" style="{'display:none' if not self.timelapse_enabled else ''}">
                    <div class="setting-row">
                        <span class="label">Connection Status:</span>
                        <div class="control">
                            <span id="moonraker_status" style="color:#888;">Not connected</span>
                        </div>
                    </div>
                </div>
                <div class="setting timelapse-setting" style="{'display:none' if not self.timelapse_enabled else ''}; border-top: 1px solid #444; padding-top: 15px; margin-top: 15px;">
                    <div class="setting-row">
                        <span class="label">Output FPS:</span>
                        <div class="control">
                            <input type="number" name="timelapse_output_fps" value="{self.timelapse_output_fps}" min="1" max="120" style="width:60px;">
                        </div>
                    </div>
                    <div class="setting-note">Video playback framerate</div>
                </div>
                <div class="setting timelapse-setting" style="{'display:none' if not self.timelapse_enabled else ''}">
                    <div class="setting-row">
                        <span class="label">Variable FPS:</span>
                        <div class="control">
                            <label class="toggle">
                                <input type="checkbox" name="timelapse_variable_fps" id="timelapse_variable_fps" {'checked' if self.timelapse_variable_fps else ''} onchange="updateVariableFpsSettings()">
                                <span class="slider"></span>
                            </label>
                        </div>
                    </div>
                    <div class="setting-note">Auto-adjust FPS to reach target video length</div>
                </div>
                <div class="setting timelapse-setting variable-fps-setting" style="{'display:none' if not self.timelapse_enabled or not self.timelapse_variable_fps else ''}">
                    <div class="setting-row">
                        <span class="label">Target Length:</span>
                        <div class="control">
                            <input type="number" name="timelapse_target_length" value="{self.timelapse_target_length}" min="1" max="300" style="width:60px;">
                            <span style="margin-left:5px;">seconds</span>
                        </div>
                    </div>
                </div>
                <div class="setting timelapse-setting variable-fps-setting" style="{'display:none' if not self.timelapse_enabled or not self.timelapse_variable_fps else ''}">
                    <div class="setting-row">
                        <span class="label">Min/Max FPS:</span>
                        <div class="control">
                            <input type="number" name="timelapse_variable_fps_min" value="{self.timelapse_variable_fps_min}" min="1" max="60" style="width:50px;">
                            <span style="margin:0 5px;">/</span>
                            <input type="number" name="timelapse_variable_fps_max" value="{self.timelapse_variable_fps_max}" min="1" max="120" style="width:50px;">
                        </div>
                    </div>
                </div>
                <div class="setting timelapse-setting" style="{'display:none' if not self.timelapse_enabled else ''}">
                    <div class="setting-row">
                        <span class="label">Quality (CRF):</span>
                        <div class="control">
                            <input type="number" name="timelapse_crf" value="{self.timelapse_crf}" min="0" max="51" style="width:60px;">
                        </div>
                    </div>
                    <div class="setting-note">0=lossless, 23=default, 51=worst</div>
                </div>
                <div class="setting timelapse-setting" style="{'display:none' if not self.timelapse_enabled else ''}">
                    <div class="setting-row">
                        <span class="label">Duplicate Last Frame:</span>
                        <div class="control">
                            <input type="number" name="timelapse_duplicate_last_frame" value="{self.timelapse_duplicate_last_frame}" min="0" max="60" style="width:60px;">
                        </div>
                    </div>
                    <div class="setting-note">Repeat final frame N times</div>
                </div>
                <div class="setting timelapse-setting" style="{'display:none' if not self.timelapse_enabled else ''}; border-top: 1px solid #444; padding-top: 15px; margin-top: 15px;">
                    <div class="setting-row">
                        <span class="label">Stream Delay:</span>
                        <div class="control">
                            <input type="number" name="timelapse_stream_delay" value="{self.timelapse_stream_delay}" min="0" max="5" step="0.01" style="width:60px;">
                            <span style="margin-left:5px;">seconds</span>
                        </div>
                    </div>
                    <div class="setting-note">Delay before capture to compensate for stream latency</div>
                </div>
                <div class="setting timelapse-setting" style="{'display:none' if not self.timelapse_enabled else ''}">
                    <div class="setting-row">
                        <span class="label">Flip Horizontal:</span>
                        <div class="control">
                            <label class="toggle">
                                <input type="checkbox" name="timelapse_flip_x" {'checked' if self.timelapse_flip_x else ''}>
                                <span class="slider"></span>
                            </label>
                        </div>
                    </div>
                </div>
                <div class="setting timelapse-setting" style="{'display:none' if not self.timelapse_enabled else ''}">
                    <div class="setting-row">
                        <span class="label">Flip Vertical:</span>
                        <div class="control">
                            <label class="toggle">
                                <input type="checkbox" name="timelapse_flip_y" {'checked' if self.timelapse_flip_y else ''}>
                                <span class="slider"></span>
                            </label>
                        </div>
                    </div>
                </div>
                <div class="setting timelapse-setting" style="{'display:none' if not self.timelapse_enabled else ''}; margin-top: 15px;">
                    <div class="setting-row">
                        <button type="submit">Apply Timelapse Settings</button>
                    </div>
                </div>
            </form>
        </div>

        <div class="section">
            <h2>Status</h2>
            <div class="row">
                <span class="label">Encoder:</span>
                <span class="value" id="status-encoder">{self.encoder_type}</span>
            </div>
            <div class="row rkmpi-only" style="{'display:none' if self.encoder_type == 'gkcam' else ''}">
                <span class="label">Camera Device:</span>
                <span class="value">{self.camera_device or 'Not found'}</span>
            </div>
            <div class="row rkmpi-only" style="{'display:none' if self.encoder_type == 'gkcam' else ''}">
                <span class="label">Resolution:</span>
                <span class="value">{self.camera_width}x{self.camera_height}</span>
            </div>
            <div class="row gkcam-only" style="{'display:none' if self.is_rkmpi_mode() else ''}">
                <span class="label">Gkcam Status:</span>
                <span class="value" id="status-gkcam">{'Connected' if self.gkcam_reader and self.gkcam_reader.connected else 'Connecting...'}</span>
            </div>
            <div class="row">
                <span class="label">Printer IP:</span>
                <span class="value">{self.current_ip or 'Unknown'}</span>
            </div>
        </div>
    </div>

    <script>
        let flvPlayer = null;
        let flvPlayerBusy = false;
        let mjpegActive = false;
        let statsInterval = null;
        let currentEncoderType = '{self.encoder_type}';
        let currentMjpegFpsTarget = {self.mjpeg_fps_target};  // Track for restart detection
        let currentGkcamAllFrames = {'true' if self.gkcam_all_frames else 'false'};  // Track for restart detection
        let currentLogging = {'true' if self.logging else 'false'};  // Track for restart detection
        let currentBitrate = {self.bitrate};  // Track for restart detection
        let currentH264Resolution = '{self.h264_resolution}';  // Track for restart detection
        let currentSessionId = {self.session_id};  // For restart detection

        // Streaming server base URL (for rkmpi modes, streaming is on a different port)
        const streamingPort = {streaming_port};
        const streamBase = (streamingPort == location.port || !streamingPort) ? '' : 'http://' + location.hostname + ':' + streamingPort;

        // Initialize stream URLs on page load
        function initStreamUrls() {{
            if (!streamBase) return;  // Same port, no changes needed

            // Update preview image
            const previewImg = document.getElementById('preview-img');
            if (previewImg) {{
                previewImg.src = streamBase + '/snapshot';
                previewImg.onclick = function() {{ this.src = streamBase + '/snapshot?' + Date.now(); }};
            }}

            // Update links
            document.querySelectorAll('a[href="/stream"]').forEach(a => {{
                a.href = streamBase + '/stream';
            }});
            document.querySelectorAll('a[href="/snapshot"]').forEach(a => {{
                a.href = streamBase + '/snapshot';
            }});
            document.querySelectorAll('a[href="/display"]').forEach(a => {{
                a.href = streamBase + '/display';
            }});
        }}
        document.addEventListener('DOMContentLoaded', initStreamUrls);

        // Show/hide settings based on encoder type
        function updateEncoderVisibility() {{
            const encoderType = document.getElementById('encoder_type').value;
            const isRkmpi = encoderType === 'rkmpi' || encoderType === 'rkmpi-yuyv';
            document.querySelectorAll('.rkmpi-only').forEach(el => {{
                el.style.display = isRkmpi ? '' : 'none';
            }});
            // rkmpi-mjpeg-only: skip-rate controls only for rkmpi (MJPEG mode)
            // Not needed for rkmpi-yuyv since YUYV is already ~4-5fps with full HW encoding
            document.querySelectorAll('.rkmpi-mjpeg-only').forEach(el => {{
                el.style.display = encoderType === 'rkmpi' ? '' : 'none';
            }});
            document.querySelectorAll('.gkcam-only').forEach(el => {{
                el.style.display = encoderType === 'gkcam' ? '' : 'none';
            }});
            // Camera resolution notes - show appropriate note for encoder type
            document.querySelectorAll('.rkmpi-mjpeg-note').forEach(el => {{
                el.style.display = encoderType === 'rkmpi' ? '' : 'none';
            }});
            document.querySelectorAll('.rkmpi-yuyv-note').forEach(el => {{
                el.style.display = encoderType === 'rkmpi-yuyv' ? '' : 'none';
            }});
        }}

        // Handle encoder type change - requires restart
        function handleEncoderChange(select) {{
            const newType = select.value;
            if (newType === currentEncoderType) {{
                updateEncoderVisibility();
                return;
            }}

            // Confirm with user
            if (!confirm('Changing encoder type requires a restart.\\nThe page will reload automatically when ready.\\n\\nContinue?')) {{
                select.value = currentEncoderType;
                return;
            }}

            // Show loading overlay
            const overlay = document.getElementById('loading-overlay');
            const statusEl = document.getElementById('loading-status');
            overlay.classList.add('active');
            statusEl.textContent = 'Saving settings...';

            // Save all current form settings with the new encoder type
            // Use querySelector().value for inputs that may be disabled (disabled inputs excluded from FormData)
            const formData = new FormData(document.getElementById('settings-form'));
            const data = new URLSearchParams();
            data.append('encoder_type', newType);
            data.append('gkcam_all_frames', formData.has('gkcam_all_frames') ? '1' : '0');
            data.append('autolanmode', formData.has('autolanmode') ? '1' : '0');
            data.append('logging', formData.has('logging') ? '1' : '0');
            data.append('h264_enabled', formData.has('h264_enabled') ? '1' : '0');
            data.append('skip_ratio', document.querySelector('[name=skip_ratio]').value);
            data.append('auto_skip', formData.has('auto_skip') ? '1' : '0');
            data.append('target_cpu', document.querySelector('[name=target_cpu]').value);
            data.append('bitrate', document.querySelector('[name=bitrate]').value);
            data.append('mjpeg_fps', document.querySelector('[name=mjpeg_fps]').value);
            data.append('h264_resolution', document.querySelector('[name=h264_resolution]')?.value || '1280x720');
            // Display capture settings
            data.append('display_enabled', formData.has('display_enabled') ? 'on' : '');
            data.append('display_fps', document.querySelector('[name=display_fps]').value);

            // Save settings, then restart
            // Stop stats polling during restart to avoid connection errors
            if (statsInterval) {{
                clearInterval(statsInterval);
                statsInterval = null;
            }}

            fetch('/control', {{
                method: 'POST',
                body: data
            }}).then(() => {{
                statusEl.textContent = 'Requesting restart...';
                return fetch('/api/restart');
            }}).then(() => {{
                statusEl.textContent = 'Waiting for server to restart...';
                // Poll until server is back
                pollForRestart();
            }}).catch(err => {{
                statusEl.textContent = 'Error: ' + err.message;
                setTimeout(() => {{
                    overlay.classList.remove('active');
                    select.value = currentEncoderType;
                }}, 3000);
            }});
        }}

        // Poll server until it comes back online with a NEW session ID
        function pollForRestart() {{
            const statusEl = document.getElementById('loading-status');
            const oldSessionId = currentSessionId;
            let attempts = 0;
            const maxAttempts = 45;  // 45 seconds max (gkcam can take 30s)

            const poll = () => {{
                attempts++;
                statusEl.textContent = 'Waiting for new server... (' + attempts + 's)';

                fetch('/status', {{ cache: 'no-store' }})
                    .then(r => r.json())
                    .then(data => {{
                        // Check if this is a NEW server instance
                        if (data.session_id && data.session_id !== oldSessionId) {{
                            statusEl.textContent = 'Server restarted! Reloading...';
                            setTimeout(() => location.reload(), 500);
                        }} else {{
                            // Still the old server, keep polling
                            statusEl.textContent = 'Waiting for restart... (' + attempts + 's)';
                            if (attempts >= maxAttempts) {{
                                statusEl.textContent = 'Timeout - please refresh manually';
                                setTimeout(() => location.reload(), 2000);
                            }} else {{
                                setTimeout(poll, 1000);
                            }}
                        }}
                    }})
                    .catch(() => {{
                        // Server not responding (expected during restart)
                        if (attempts >= maxAttempts) {{
                            statusEl.textContent = 'Timeout - please refresh manually';
                            setTimeout(() => location.reload(), 2000);
                        }} else {{
                            setTimeout(poll, 1000);
                        }}
                    }});
            }};

            // Wait a moment for server to shut down, then start polling
            setTimeout(poll, 2000);
        }}

        // Tab switching
        function switchTab(tab) {{
            document.querySelectorAll('.tab-content').forEach(el => el.classList.remove('active'));
            document.querySelectorAll('.tabs button').forEach(el => el.classList.remove('active'));
            document.getElementById('tab-' + tab).classList.add('active');
            event.target.classList.add('active');

            // Handle stream states
            if (tab === 'snapshot') {{
                // Refresh snapshot when switching to tab
                const img = document.getElementById('preview-img');
                img.src = streamBase + '/snapshot?' + Date.now();
            }} else if (tab === 'mjpeg') {{
                startMjpegStream();
            }} else {{
                stopMjpegStream();
            }}
            if (tab === 'display') {{
                startDisplayStream();
            }} else {{
                stopDisplayStream();
            }}
            // Don't auto-start FLV player on tab switch - user clicks Play
            if (tab !== 'flv') {{
                stopFlvPlayer();
            }}
        }}

        // MJPEG stream
        function startMjpegStream() {{
            const img = document.getElementById('mjpeg-stream');
            img.src = streamBase + '/stream';
            img.style.display = 'block';
            mjpegActive = true;
        }}
        function stopMjpegStream() {{
            const img = document.getElementById('mjpeg-stream');
            img.src = '';
            img.style.display = 'none';
            mjpegActive = false;
        }}

        // Display stream (printer LCD framebuffer)
        let displayActive = false;
        const DISPLAY_WIDTH = {g_display_width};
        const DISPLAY_HEIGHT = {g_display_height};

        function startDisplayStream() {{
            const img = document.getElementById('display-stream');
            img.src = streamBase + '/display';
            img.style.display = 'block';
            img.style.cursor = 'crosshair';
            displayActive = true;
        }}
        function stopDisplayStream() {{
            const img = document.getElementById('display-stream');
            img.src = '';
            img.style.display = 'none';
            displayActive = false;
        }}

        // Touch control for display stream
        function handleDisplayClick(event) {{
            const img = event.target;
            const rect = img.getBoundingClientRect();

            // Calculate click position relative to image (0-1)
            const relX = (event.clientX - rect.left) / rect.width;
            const relY = (event.clientY - rect.top) / rect.height;

            // Scale to display coordinates
            const x = Math.round(relX * DISPLAY_WIDTH);
            const y = Math.round(relY * DISPLAY_HEIGHT);

            // Send touch command
            fetch('/api/touch', {{
                method: 'POST',
                headers: {{'Content-Type': 'application/json'}},
                body: JSON.stringify({{x: x, y: y, duration: 100}})
            }})
            .then(r => r.json())
            .then(data => {{
                console.log('Touch:', data);
            }})
            .catch(e => console.error('Touch error:', e));
        }}

        // Initialize display click handler
        document.addEventListener('DOMContentLoaded', function() {{
            const displayImg = document.getElementById('display-stream');
            if (displayImg) {{
                displayImg.addEventListener('click', handleDisplayClick);
            }}
        }});

        // LED control
        function controlLed(on) {{
            fetch('/api/led/' + (on ? 'on' : 'off'))
                .then(r => r.json())
                .then(data => {{
                    console.log('LED:', data);
                }})
                .catch(e => console.error('LED error:', e));
        }}

        // Camera controls
        let cameraControlsExpanded = false;
        let cameraControlsLoaded = false;

        function toggleCameraControls() {{
            const panel = document.getElementById('camera-controls-panel');
            const arrow = document.getElementById('camera-controls-arrow');
            cameraControlsExpanded = !cameraControlsExpanded;
            panel.style.display = cameraControlsExpanded ? 'block' : 'none';
            arrow.innerHTML = cameraControlsExpanded ? '&#9660;' : '&#9654;';

            if (cameraControlsExpanded && !cameraControlsLoaded) {{
                loadCameraControls();
            }}
        }}

        function loadCameraControls() {{
            fetch('/api/camera/controls')
                .then(r => r.json())
                .then(data => {{
                    // Update all sliders and selects with current values
                    Object.keys(data).forEach(key => {{
                        const ctrl = data[key];
                        const el = document.getElementById('cam-' + key.replace(/_/g, '-'));
                        const valEl = document.getElementById('cam-' + key.replace(/_/g, '-') + '-val');

                        if (el) {{
                            el.value = ctrl.value;
                            if (valEl) {{
                                if (key === 'wb_temp') {{
                                    valEl.textContent = ctrl.value + 'K';
                                }} else {{
                                    valEl.textContent = ctrl.value;
                                }}
                            }}
                        }}
                    }});
                    updateWbTempState();
                    cameraControlsLoaded = true;
                }})
                .catch(e => console.error('Failed to load camera controls:', e));
        }}

        function setCameraControl(control, value) {{
            // Update display value immediately
            const valEl = document.getElementById('cam-' + control.replace(/_/g, '-') + '-val');
            if (valEl) {{
                if (control === 'wb_temp') {{
                    valEl.textContent = value + 'K';
                }} else {{
                    valEl.textContent = value;
                }}
            }}

            // Send to server
            fetch('/api/camera/set', {{
                method: 'POST',
                headers: {{'Content-Type': 'application/json'}},
                body: JSON.stringify({{control: control, value: parseInt(value)}})
            }})
            .then(r => r.json())
            .then(data => {{
                if (data.status !== 'ok') {{
                    console.error('Camera control error:', data);
                }}
            }})
            .catch(e => console.error('Camera control error:', e));
        }}

        function resetCameraDefaults() {{
            if (!confirm('Reset all camera settings to defaults?')) return;

            fetch('/api/camera/reset', {{method: 'POST'}})
                .then(r => r.json())
                .then(data => {{
                    if (data.status === 'ok') {{
                        cameraControlsLoaded = false;
                        loadCameraControls();
                    }}
                }})
                .catch(e => console.error('Reset error:', e));
        }}

        function updateWbTempState() {{
            const wbAuto = document.getElementById('cam-wb-auto');
            const wbTemp = document.getElementById('cam-wb-temp');
            if (wbAuto && wbTemp) {{
                wbTemp.disabled = wbAuto.value === '1';
                wbTemp.style.opacity = wbAuto.value === '1' ? '0.5' : '1';
            }}
        }}

        // Open FLV in fullscreen window
        function openFlvFullscreen() {{
            const w = screen.availWidth;
            const h = screen.availHeight;
            const flvUrl = 'http://' + location.hostname + ':{self.FLV_PORT}/flv';
            const html = `<!DOCTYPE html>
<html><head><title>H.264 Stream</title>
<script src="https://cdn.jsdelivr.net/npm/flv.js@1.6.2/dist/flv.min.js"><\\/script>
<style>body{{margin:0;background:#000;overflow:hidden}}video{{width:100vw;height:100vh;object-fit:contain}}</style>
</head><body>
<video id="v" muted autoplay></video>
<script>
if(flvjs.isSupported()){{
  var p=flvjs.createPlayer({{type:'flv',isLive:true,url:'${{flvUrl}}'}},{{enableWorker:false,enableStashBuffer:false}});
  p.attachMediaElement(document.getElementById('v'));
  p.load();p.play();
}}
<\\/script></body></html>`;
            const win = window.open('', '_blank', 'width='+w+',height='+h);
            win.document.write(html.replace(/\\$\\{{flvUrl\\}}/g, flvUrl));
            win.document.close();
        }}

        // FLV player using flv.js
        function startFlvPlayer() {{
            if (flvPlayerBusy) return;
            const statusEl = document.getElementById('flv-status');

            // Stop existing player first
            if (flvPlayer) {{
                flvPlayerBusy = true;
                try {{
                    flvPlayer.pause();
                    flvPlayer.unload();
                    flvPlayer.detachMediaElement();
                    flvPlayer.destroy();
                }} catch(e) {{}}
                flvPlayer = null;
                // Wait a bit before creating new player
                setTimeout(createFlvPlayer, 200);
            }} else {{
                createFlvPlayer();
            }}
        }}

        function createFlvPlayer() {{
            flvPlayerBusy = false;
            const statusEl = document.getElementById('flv-status');
            const videoElement = document.getElementById('flv-player');

            if (!flvjs.isSupported()) {{
                statusEl.textContent = 'FLV.js not supported';
                return;
            }}

            statusEl.textContent = 'Connecting...';

            flvPlayer = flvjs.createPlayer({{
                type: 'flv',
                isLive: true,
                url: 'http://' + location.hostname + ':{self.FLV_PORT}/flv'
            }}, {{
                enableWorker: false,
                enableStashBuffer: false,
                stashInitialSize: 128,
                lazyLoad: false
            }});

            flvPlayer.on(flvjs.Events.ERROR, (e, t) => {{
                statusEl.textContent = 'Error: ' + t;
            }});

            flvPlayer.on(flvjs.Events.LOADING_COMPLETE, () => {{
                statusEl.textContent = 'Stream ended';
            }});

            flvPlayer.attachMediaElement(videoElement);
            flvPlayer.load();

            // Handle play promise properly
            const playPromise = videoElement.play();
            if (playPromise !== undefined) {{
                playPromise.then(() => {{
                    statusEl.textContent = 'Playing';
                }}).catch(err => {{
                    statusEl.textContent = 'Click Play';
                }});
            }}
        }}

        function stopFlvPlayer() {{
            if (flvPlayerBusy) return;
            const statusEl = document.getElementById('flv-status');
            const videoElement = document.getElementById('flv-player');

            if (flvPlayer) {{
                flvPlayerBusy = true;
                try {{
                    videoElement.pause();
                    flvPlayer.unload();
                    flvPlayer.detachMediaElement();
                    flvPlayer.destroy();
                }} catch(e) {{}}
                flvPlayer = null;
                flvPlayerBusy = false;
            }}
            statusEl.textContent = 'Stopped';
        }}

        // FPS percentage helpers
        let currentMjpegFps = 20;  // Will be updated from stats
        let currentMaxCameraFps = {self.max_camera_fps};  // Declared camera capability (slider max)
        let savedSkipRatio = {self.saved_skip_ratio};  // Manual setting to restore

        function pctToSkipRatio(pct) {{
            // 100% = skip_ratio 1 (all frames)
            // 0% = skip_ratio = mjpegFps (1 fps output)
            if (pct <= 0) return Math.max(1, Math.round(currentMjpegFps));
            if (pct >= 100) return 1;
            return Math.max(1, Math.round(100 / pct));
        }}

        function skipRatioToPct(ratio) {{
            if (ratio <= 1) return 100;
            return Math.max(1, Math.min(100, Math.round(100 / ratio)));
        }}

        function updateFpsLabel() {{
            const pct = parseInt(document.getElementById('fps_pct_input').value) || 0;
            const skipRatio = pctToSkipRatio(pct);
            const estFps = (currentMjpegFps / skipRatio).toFixed(1);
            document.getElementById('fps_label').textContent = '~' + estFps + ' fps';
            document.getElementById('skip_ratio_hidden').value = skipRatio;
        }}

        function updateMjpegFpsLabel() {{
            const fps = parseInt(document.getElementById('mjpeg_fps_input').value) || 10;
            const label = document.getElementById('mjpeg_fps_label');
            if (fps !== {self.mjpeg_fps_target}) {{
                label.textContent = '(Requires restart)';
                label.style.color = '#f90';
            }} else {{
                label.textContent = '(Camera source rate)';
                label.style.color = '#888';
            }}
        }}

        function toggleAutoSkip(enabled) {{
            document.getElementById('fps_pct_slider').disabled = enabled;
            document.getElementById('fps_pct_input').disabled = enabled;
            document.querySelector('[name=target_cpu]').disabled = !enabled;

            // When disabling auto-skip, restore the saved manual setting
            if (!enabled) {{
                const pct = skipRatioToPct(savedSkipRatio);
                document.getElementById('fps_pct_slider').value = pct;
                document.getElementById('fps_pct_input').value = pct;
                document.getElementById('skip_ratio_hidden').value = savedSkipRatio;
                updateFpsLabel();
            }}
        }}

        function updateDisplaySettings() {{
            const enabled = document.querySelector('[name=display_enabled]').checked;
            document.getElementById('display_fps_select').disabled = !enabled;
        }}

        // Timelapse settings UI functions
        function updateTimelapseSettings() {{
            const enabled = document.getElementById('timelapse_enabled').checked;
            document.querySelectorAll('.timelapse-setting').forEach(el => {{
                el.style.display = enabled ? '' : 'none';
            }});
            if (enabled) {{
                updateTimelapseModeSettings();
                updateVariableFpsSettings();
                checkUsbStatus();
                checkMoonrakerStatus();
            }}
        }}

        function updateTimelapseModeSettings() {{
            const mode = document.getElementById('timelapse_mode').value;
            document.querySelectorAll('.hyperlapse-only').forEach(el => {{
                el.style.display = mode === 'hyperlapse' ? '' : 'none';
            }});
        }}

        function updateVariableFpsSettings() {{
            const enabled = document.getElementById('timelapse_variable_fps')?.checked || false;
            document.querySelectorAll('.variable-fps-setting').forEach(el => {{
                el.style.display = enabled ? '' : 'none';
            }});
        }}

        function checkUsbStatus() {{
            fetch('/api/timelapse/storage')
                .then(r => r.json())
                .then(data => {{
                    const statusEl = document.getElementById('usb_status');
                    if (data.usb_mounted) {{
                        statusEl.textContent = '● Mounted';
                        statusEl.style.color = '#4CAF50';
                    }} else {{
                        statusEl.textContent = '○ Not detected';
                        statusEl.style.color = '#888';
                    }}
                }})
                .catch(() => {{
                    const statusEl = document.getElementById('usb_status');
                    statusEl.textContent = '? Unknown';
                    statusEl.style.color = '#f90';
                }});
        }}

        function checkMoonrakerStatus() {{
            fetch('/api/timelapse/moonraker')
                .then(r => r.json())
                .then(data => {{
                    const statusEl = document.getElementById('moonraker_status');
                    if (data.connected) {{
                        statusEl.textContent = '● Connected';
                        statusEl.style.color = '#4CAF50';
                        if (data.print_state && data.print_state !== 'standby') {{
                            statusEl.textContent += ' (' + data.print_state + ')';
                        }}
                    }} else {{
                        statusEl.textContent = '○ Disconnected';
                        statusEl.style.color = '#888';
                    }}
                }})
                .catch(() => {{
                    const statusEl = document.getElementById('moonraker_status');
                    statusEl.textContent = '? Unknown';
                    statusEl.style.color = '#f90';
                }});
        }}

        function updateStorageSettings() {{
            const storage = document.getElementById('timelapse_storage').value;
            document.querySelectorAll('.usb-path-setting').forEach(el => {{
                el.style.display = storage === 'usb' ? '' : 'none';
            }});
        }}

        // Folder picker for USB path
        let folderPickerModal = null;
        let currentPickerPath = '/mnt/udisk';

        function openFolderPicker() {{
            if (!folderPickerModal) {{
                folderPickerModal = document.createElement('div');
                folderPickerModal.id = 'folder-picker-modal';
                folderPickerModal.innerHTML = `
                    <div style="position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,0.7);z-index:1000;display:flex;align-items:center;justify-content:center;">
                        <div style="background:#222;border-radius:8px;padding:20px;width:400px;max-width:90%;max-height:80vh;display:flex;flex-direction:column;">
                            <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:15px;">
                                <h3 style="margin:0;">Select Folder</h3>
                                <button onclick="closeFolderPicker()" style="background:none;border:none;color:#fff;font-size:20px;cursor:pointer;">&times;</button>
                            </div>
                            <div id="picker-path" style="font-family:monospace;background:#333;padding:8px;border-radius:4px;margin-bottom:10px;word-break:break-all;"></div>
                            <div id="picker-list" style="flex:1;overflow-y:auto;border:1px solid #444;border-radius:4px;min-height:200px;max-height:300px;"></div>
                            <div style="display:flex;gap:10px;margin-top:15px;">
                                <button onclick="createNewFolder()" class="secondary" style="flex:1;">New Folder</button>
                                <button onclick="selectCurrentFolder()" style="flex:1;">Select This Folder</button>
                            </div>
                        </div>
                    </div>
                `;
                document.body.appendChild(folderPickerModal);
            }}
            folderPickerModal.style.display = 'block';
            currentPickerPath = '/mnt/udisk';
            loadFolderContents(currentPickerPath);
        }}

        function closeFolderPicker() {{
            if (folderPickerModal) {{
                folderPickerModal.style.display = 'none';
            }}
        }}

        function loadFolderContents(path) {{
            currentPickerPath = path;
            document.getElementById('picker-path').textContent = path;
            document.getElementById('picker-list').innerHTML = '<div style="padding:20px;text-align:center;color:#888;">Loading...</div>';

            fetch('/api/timelapse/browse?path=' + encodeURIComponent(path))
                .then(r => r.json())
                .then(data => {{
                    if (data.error) {{
                        document.getElementById('picker-list').innerHTML = '<div style="padding:20px;text-align:center;color:#f44;">Error: ' + data.error + '</div>';
                        return;
                    }}
                    const listEl = document.getElementById('picker-list');
                    listEl.innerHTML = '';

                    // Parent directory link
                    if (path !== '/mnt/udisk') {{
                        const parent = path.substring(0, path.lastIndexOf('/')) || '/mnt/udisk';
                        const parentDiv = document.createElement('div');
                        parentDiv.style.cssText = 'padding:10px;cursor:pointer;border-bottom:1px solid #333;display:flex;align-items:center;gap:10px;';
                        parentDiv.innerHTML = '<span>&#128194;</span> <span>..</span>';
                        parentDiv.onmouseover = function() {{ this.style.background='#333'; }};
                        parentDiv.onmouseout = function() {{ this.style.background=''; }};
                        parentDiv.onclick = function() {{ loadFolderContents(parent); }};
                        listEl.appendChild(parentDiv);
                    }}

                    // Folder entries
                    if (data.folders && data.folders.length > 0) {{
                        data.folders.forEach(function(folder) {{
                            const fullPath = path + '/' + folder;
                            const div = document.createElement('div');
                            div.style.cssText = 'padding:10px;cursor:pointer;border-bottom:1px solid #333;display:flex;align-items:center;gap:10px;';
                            div.innerHTML = '<span>&#128193;</span> <span>' + folder + '</span>';
                            div.onmouseover = function() {{ this.style.background='#333'; }};
                            div.onmouseout = function() {{ this.style.background=''; }};
                            div.onclick = function() {{ loadFolderContents(fullPath); }};
                            listEl.appendChild(div);
                        }});
                    }}

                    if (listEl.children.length === 0) {{
                        listEl.innerHTML = '<div style="padding:20px;text-align:center;color:#888;">Empty folder</div>';
                    }}
                }})
                .catch(err => {{
                    document.getElementById('picker-list').innerHTML = '<div style="padding:20px;text-align:center;color:#f44;">Failed to load</div>';
                }});
        }}

        function selectCurrentFolder() {{
            document.getElementById('timelapse_usb_path').value = currentPickerPath;
            closeFolderPicker();
        }}

        function createNewFolder() {{
            const name = prompt('Enter new folder name:');
            if (!name) return;
            fetch('/api/timelapse/mkdir', {{
                method: 'POST',
                headers: {{'Content-Type': 'application/json'}},
                body: JSON.stringify({{path: currentPickerPath + '/' + name}})
            }})
            .then(r => r.json())
            .then(data => {{
                if (data.success) {{
                    loadFolderContents(currentPickerPath);
                }} else {{
                    alert('Failed to create folder: ' + (data.error || 'Unknown error'));
                }}
            }})
            .catch(() => alert('Failed to create folder'));
        }}

        // Update stats every second
        function updateStats() {{
            fetch('/api/stats')
                .then(r => r.json())
                .then(data => {{
                    document.getElementById('stat-cpu-total').textContent = data.cpu.total + '%';
                    document.getElementById('stat-mjpeg-fps').textContent = data.fps.mjpeg;
                    document.getElementById('stat-h264-fps').textContent = data.fps.h264;
                    document.getElementById('stat-cpu-encoder').textContent = data.encoder_cpu + '%';
                    document.getElementById('stat-cpu-streamer').textContent = data.streamer_cpu + '%';
                    // Update client count
                    if (data.clients) {{
                        const totalClients = (data.clients.mjpeg || 0) + (data.clients.flv || 0);
                        document.getElementById('stat-clients').textContent = totalClients;
                    }}
                    // Update MJPEG FPS for percentage calculations
                    if (data.fps.mjpeg > 0) {{
                        currentMjpegFps = data.fps.mjpeg;
                        updateFpsLabel();
                    }}
                    // Track detected camera fps for display (slider max stays at declared capability)
                    // Keep saved_skip_ratio in sync with server
                    if (data.saved_skip_ratio) {{
                        savedSkipRatio = data.saved_skip_ratio;
                    }}
                    // Update slider only if auto_skip checkbox is CHECKED (local UI state)
                    // This ensures toggling off stops the dynamic updates immediately
                    const autoSkipCheckbox = document.querySelector('[name=auto_skip]');
                    if (autoSkipCheckbox && autoSkipCheckbox.checked) {{
                        const pct = skipRatioToPct(data.skip_ratio);
                        document.getElementById('fps_pct_slider').value = pct;
                        document.getElementById('fps_pct_input').value = pct;
                        document.getElementById('skip_ratio_hidden').value = data.skip_ratio;
                        updateFpsLabel();
                    }}
                }})
                .catch(() => {{}});
        }}
        // Start stats update - poll every 2 seconds to reduce CPU
        if (!statsInterval) {{
            statsInterval = setInterval(updateStats, 2000);
        }}
        // Initialize FPS labels on load
        updateFpsLabel();
        updateMjpegFpsLabel();

        // Handle form submission
        document.getElementById('settings-form').addEventListener('submit', function(e) {{
            e.preventDefault();
            const formData = new FormData(this);
            const data = new URLSearchParams();
            // Use querySelector().value for inputs that may be disabled (disabled inputs excluded from FormData)
            data.append('encoder_type', document.querySelector('[name=encoder_type]').value);
            data.append('gkcam_all_frames', formData.has('gkcam_all_frames') ? '1' : '0');
            data.append('autolanmode', formData.has('autolanmode') ? '1' : '0');
            data.append('logging', formData.has('logging') ? '1' : '0');
            data.append('h264_enabled', formData.has('h264_enabled') ? '1' : '0');
            data.append('skip_ratio', document.querySelector('[name=skip_ratio]').value);
            data.append('auto_skip', formData.has('auto_skip') ? '1' : '0');
            data.append('target_cpu', document.querySelector('[name=target_cpu]').value);
            data.append('bitrate', document.querySelector('[name=bitrate]').value);
            data.append('h264_resolution', document.querySelector('[name=h264_resolution]')?.value || '1280x720');
            // Get mjpeg_fps from the correct slider based on encoder type
            const selectedEncoderType = document.querySelector('[name=encoder_type]').value;
            const mjpegFpsValue = selectedEncoderType === 'rkmpi-yuyv'
                ? document.getElementById('mjpeg_fps_slider_yuyv').value
                : document.getElementById('mjpeg_fps_slider').value;
            data.append('mjpeg_fps', mjpegFpsValue);
            // Display capture settings
            if (formData.has('display_enabled')) {{
                data.append('display_enabled', 'on');
            }}
            data.append('display_fps', document.querySelector('[name=display_fps]').value);

            // Check if settings require restart
            const newMjpegFps = parseInt(mjpegFpsValue) || 10;
            const newGkcamAllFrames = formData.has('gkcam_all_frames');
            const newLogging = formData.has('logging');
            const newBitrate = parseInt(document.querySelector('[name=bitrate]').value) || 512;
            const newH264Resolution = document.querySelector('[name=h264_resolution]')?.value || '1280x720';
            const needsRkmpiRestart = (newMjpegFps !== currentMjpegFpsTarget) && currentEncoderType === 'rkmpi';
            const needsGkcamRestart = (newGkcamAllFrames !== currentGkcamAllFrames) && currentEncoderType === 'gkcam';
            const needsLoggingRestart = (newLogging !== currentLogging);
            const needsBitrateRestart = (newBitrate !== currentBitrate) && (currentEncoderType === 'rkmpi' || currentEncoderType === 'rkmpi-yuyv');
            const needsResolutionRestart = (newH264Resolution !== currentH264Resolution) && (currentEncoderType === 'rkmpi' || currentEncoderType === 'rkmpi-yuyv');
            const needsRestart = needsRkmpiRestart || needsGkcamRestart || needsLoggingRestart || needsBitrateRestart || needsResolutionRestart;

            if (needsRestart) {{
                // Show loading overlay and trigger restart
                const overlay = document.getElementById('loading-overlay');
                const statusEl = document.getElementById('loading-status');
                overlay.classList.add('active');
                statusEl.textContent = 'Saving settings...';

                // Stop stats polling during restart to avoid connection errors
                if (statsInterval) {{
                    clearInterval(statsInterval);
                    statsInterval = null;
                }}

                fetch('/control', {{
                    method: 'POST',
                    body: data
                }}).then(() => {{
                    statusEl.textContent = 'Restarting encoder...';
                    return fetch('/api/restart');
                }}).then(() => {{
                    statusEl.textContent = 'Waiting for encoder...';
                    // Use pollForRestart for consistent restart handling
                    pollForRestart();
                }}).catch(err => {{
                    overlay.classList.remove('active');
                    alert('Restart failed: ' + err);
                }});
            }} else {{
                fetch('/control', {{
                    method: 'POST',
                    body: data
                }}).then(() => location.reload());
            }}
        }});

        // Timelapse form submission
        const timelapseForm = document.getElementById('timelapse-form');
        if (timelapseForm) {{
            timelapseForm.addEventListener('submit', function(e) {{
                e.preventDefault();
                const formData = new FormData(this);
                const data = new URLSearchParams();

                // Boolean settings
                data.append('timelapse_enabled', formData.has('timelapse_enabled') ? '1' : '0');
                data.append('timelapse_variable_fps', formData.has('timelapse_variable_fps') ? '1' : '0');
                data.append('timelapse_flip_x', formData.has('timelapse_flip_x') ? '1' : '0');
                data.append('timelapse_flip_y', formData.has('timelapse_flip_y') ? '1' : '0');

                // String/number settings
                data.append('timelapse_mode', formData.get('timelapse_mode') || 'layer');
                data.append('timelapse_hyperlapse_interval', formData.get('timelapse_hyperlapse_interval') || '30');
                data.append('timelapse_storage', formData.get('timelapse_storage') || 'internal');
                data.append('timelapse_usb_path', formData.get('timelapse_usb_path') || '/mnt/udisk/timelapse');
                data.append('moonraker_host', formData.get('moonraker_host') || '127.0.0.1');
                data.append('moonraker_port', formData.get('moonraker_port') || '7125');
                data.append('timelapse_output_fps', formData.get('timelapse_output_fps') || '30');
                data.append('timelapse_target_length', formData.get('timelapse_target_length') || '10');
                data.append('timelapse_variable_fps_min', formData.get('timelapse_variable_fps_min') || '5');
                data.append('timelapse_variable_fps_max', formData.get('timelapse_variable_fps_max') || '60');
                data.append('timelapse_crf', formData.get('timelapse_crf') || '23');
                data.append('timelapse_duplicate_last_frame', formData.get('timelapse_duplicate_last_frame') || '0');
                data.append('timelapse_stream_delay', formData.get('timelapse_stream_delay') || '0.05');

                fetch('/api/timelapse/settings', {{
                    method: 'POST',
                    body: data
                }}).then(r => r.json())
                .then(data => {{
                    if (data.status === 'ok') {{
                        // Refresh status indicators
                        checkUsbStatus();
                        checkMoonrakerStatus();
                        alert('Timelapse settings saved');
                    }} else {{
                        alert('Error saving settings: ' + (data.message || 'Unknown error'));
                    }}
                }})
                .catch(err => alert('Error: ' + err));
            }});
        }}

        // Initialize timelapse UI on page load
        document.addEventListener('DOMContentLoaded', function() {{
            updateTimelapseSettings();
        }});

        // Snapshot preview is static - click image to refresh (reduces CPU usage)
    </script>
</body>
</html>'''

        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            f"Content-Length: {len(html)}\r\n"
            "Connection: close\r\n"
            "\r\n"
        ) + html
        client.sendall(response.encode())

    def _handle_control_post(self, client, body):
        """Handle control POST request"""
        params = {}
        for param in body.split('&'):
            if '=' in param:
                k, v = param.split('=', 1)
                params[k] = v

        # Update settings
        if 'encoder_type' in params:
            new_encoder_type = params['encoder_type']
            if new_encoder_type in ('gkcam', 'rkmpi', 'rkmpi-yuyv'):
                self.encoder_type = new_encoder_type
        if 'gkcam_all_frames' in params:
            self.gkcam_all_frames = params['gkcam_all_frames'] == '1'
        else:
            # Checkbox not sent if unchecked
            self.gkcam_all_frames = False
        if 'autolanmode' in params:
            self.autolanmode = params['autolanmode'] == '1'
        else:
            self.autolanmode = False
        if 'logging' in params:
            self.logging = params['logging'] == '1'
        else:
            self.logging = False
        if 'h264_enabled' in params:
            self.h264_enabled = params['h264_enabled'] == '1'
        if 'skip_ratio' in params:
            try:
                new_skip = max(1, int(params['skip_ratio']))
                self.saved_skip_ratio = new_skip  # Save manual setting
                self.skip_ratio = new_skip
            except ValueError:
                pass
        if 'auto_skip' in params:
            new_auto_skip = params['auto_skip'] == '1'
            # When disabling auto_skip, restore the saved manual setting
            if self.auto_skip and not new_auto_skip:
                self.skip_ratio = self.saved_skip_ratio
            self.auto_skip = new_auto_skip
        else:
            # Checkbox unchecked - restore saved setting
            if self.auto_skip:
                self.skip_ratio = self.saved_skip_ratio
            self.auto_skip = False
        if 'target_cpu' in params:
            try:
                self.target_cpu = max(25, min(90, int(params['target_cpu'])))
            except ValueError:
                pass
        if 'bitrate' in params:
            try:
                self.bitrate = max(100, min(4000, int(params['bitrate'])))
            except ValueError:
                pass
        if 'mjpeg_fps' in params:
            try:
                # For rkmpi-yuyv, allow up to 30 (Moonraker config only, not actual encoder)
                max_fps = 30 if self.encoder_type == 'rkmpi-yuyv' else self.max_camera_fps
                self.mjpeg_fps_target = max(2, min(max_fps, int(params['mjpeg_fps'])))
            except ValueError:
                pass
        if 'h264_resolution' in params:
            # Validate resolution format
            res = params['h264_resolution']
            if res in ('1280x720', '640x360'):
                self.h264_resolution = res
        # Display capture settings
        if 'display_enabled' in params:
            self.display_enabled = params['display_enabled'] == 'on'
        else:
            self.display_enabled = False
        if 'display_fps' in params:
            try:
                self.display_fps = max(1, min(10, int(params['display_fps'])))
            except ValueError:
                pass

        # Update control file (for rkmpi encoder)
        if self.is_rkmpi_mode():
            self.write_ctrl_file()

        # Save to persistent config
        self.save_config()

        # Update moonraker webcam config with new target FPS
        if self.current_ip:
            update_moonraker_webcam_config(self.current_ip, self.streaming_port, self.mjpeg_fps_target)

        # Redirect back
        response = (
            "HTTP/1.1 303 See Other\r\n"
            "Location: /control\r\n"
            "Connection: close\r\n"
            "\r\n"
        )
        client.sendall(response.encode())

    def _serve_api_stats(self, client):
        """Serve API stats JSON"""
        # Record request time to keep CPU monitoring active
        self.last_stats_request = time.time()
        cpu_stats = self.cpu_monitor.get_stats()

        # Get FPS and client counts from appropriate source
        if self.encoder_type == 'gkcam':
            # gkcam mode: use local counters
            if self.gkcam_reader:
                h264_fps_val = round(self.gkcam_reader.fps_counter.get_fps(), 1)
            else:
                h264_fps_val = 0.0
            if self.gkcam_transcoder:
                mjpeg_fps_val = round(self.gkcam_transcoder.fps_counter.get_fps(), 1)
            else:
                mjpeg_fps_val = round(self.mjpeg_fps.get_fps(), 1)
            mjpeg_clients = 0  # Not tracked in gkcam mode
            flv_clients = 0
        elif self.is_rkmpi_mode():
            # rkmpi server mode: use encoder stats from control file
            mjpeg_fps_val = round(self.encoder_mjpeg_fps, 1)
            h264_fps_val = round(self.encoder_h264_fps, 1)
            mjpeg_clients = self.encoder_mjpeg_clients
            flv_clients = self.encoder_flv_clients
        else:
            # Fallback to local counters
            mjpeg_fps_val = round(self.mjpeg_fps.get_fps(), 1)
            h264_fps_val = round(self.h264_fps.get_fps(), 1)
            mjpeg_clients = 0
            flv_clients = 0

        stats = {
            'cpu': cpu_stats,
            'encoder_cpu': cpu_stats['encoder_cpu'],
            'streamer_cpu': cpu_stats['streamer_cpu'],
            'fps': {
                'mjpeg': mjpeg_fps_val,
                'h264': h264_fps_val
            },
            'clients': {
                'mjpeg': mjpeg_clients,
                'flv': flv_clients
            },
            'encoder_type': self.encoder_type,
            'gkcam_all_frames': self.gkcam_all_frames,
            'gkcam_connected': self.gkcam_reader.connected if self.gkcam_reader else False,
            'h264_enabled': self.h264_enabled,
            'skip_ratio': self.skip_ratio,
            'saved_skip_ratio': self.saved_skip_ratio,
            'auto_skip': self.auto_skip,
            'target_cpu': self.target_cpu,
            'autolanmode': self.autolanmode,
            'mjpeg_fps_target': self.mjpeg_fps_target,
            'max_camera_fps': self.max_camera_fps,
            'detected_camera_fps': self.detected_camera_fps,
            'session_id': self.session_id,
            'display_enabled': self.display_enabled,
            'display_fps': self.display_fps
        }
        body = json.dumps(stats)
        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Connection: close\r\n"
            "\r\n"
        ) + body
        client.sendall(response.encode())

    def _serve_homepage(self, client):
        """Serve homepage with stream links and API endpoints"""
        sp = self.streaming_port
        cp = self.control_port

        html = f'''<!DOCTYPE html><html><head><title>H264 Streamer</title>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<style>
body{{font-family:sans-serif;margin:20px;background:#1a1a1a;color:#fff}}
.container{{max-width:800px;margin:0 auto}}
h1{{color:#4CAF50;margin-bottom:5px}}
.subtitle{{color:#888;margin-bottom:20px}}
.section{{background:#2d2d2d;padding:15px;margin:15px 0;border-radius:8px}}
.section h2{{margin:0 0 10px 0;color:#888;font-size:14px;text-transform:uppercase}}
button{{background:#4CAF50;color:#fff;border:none;padding:10px 20px;border-radius:4px;cursor:pointer;font-size:14px;margin:2px}}
button:hover{{background:#45a049}}
button.secondary{{background:#555}}
button.secondary:hover{{background:#666}}
.stream-row{{display:flex;align-items:center;margin:8px 0;padding:8px;background:#222;border-radius:4px}}
.stream-url{{flex:1;font-family:monospace;font-size:13px;color:#4CAF50;word-break:break-all}}
.stream-btns{{display:flex;gap:5px}}
.stream-btns button{{padding:5px 10px;font-size:12px}}
.copy-btn{{background:#444;padding:5px 8px !important}}
.copy-btn:hover{{background:#555}}
.endpoint-row{{display:flex;margin:6px 0;padding:6px 0;border-bottom:1px solid #333}}
.endpoint-path{{font-family:monospace;color:#4CAF50;min-width:180px}}
.endpoint-desc{{color:#aaa;font-size:13px}}
.copied{{background:#2e7d32 !important}}
</style></head><body>
<div class='container'>
<h1>H264 Streamer</h1>
<p class='subtitle'>HTTP streaming server for Anycubic printers</p>

<div class='section'>
<h2>Control Panel</h2>
<p style='color:#aaa;margin:0 0 10px 0;font-size:14px'>Configure streaming settings, camera controls, and preview video.</p>
<button onclick='openControl()'>Open Control Panel</button>
</div>

<div class='section'>
<h2>Video Streams</h2>
<div id='streams'></div>
</div>

<div class='section'>
<h2>API Endpoints</h2>
<p style='color:#888;font-size:12px;margin:0 0 10px 0'>Available on control port (<span id='cp'>{cp}</span>)</p>
<div class='endpoint-row'><span class='endpoint-path'>/control</span><span class='endpoint-desc'>Web control panel with settings and preview</span></div>
<div class='endpoint-row'><span class='endpoint-path'>/api/stats</span><span class='endpoint-desc'>JSON stats (FPS, CPU, clients)</span></div>
<div class='endpoint-row'><span class='endpoint-path'>/api/config</span><span class='endpoint-desc'>JSON full running configuration</span></div>
<div class='endpoint-row'><span class='endpoint-path'>/status</span><span class='endpoint-desc'>Plain text status summary</span></div>
<div class='endpoint-row'><span class='endpoint-path'>/timelapse</span><span class='endpoint-desc'>Timelapse management page</span></div>
<div class='endpoint-row'><span class='endpoint-path'>/api/timelapse/list</span><span class='endpoint-desc'>JSON list of timelapse recordings</span></div>
<div class='endpoint-row'><span class='endpoint-path'>/api/camera/controls</span><span class='endpoint-desc'>JSON camera controls with ranges</span></div>
<div class='endpoint-row'><span class='endpoint-path'>/api/touch</span><span class='endpoint-desc'>POST touch events to printer LCD</span></div>
</div>
</div>

<script>
var sp={sp},cp={cp};
var host=location.hostname;
var streamBase='http://'+host+':'+sp;
var ctrlBase='http://'+host+':'+cp;
function openControl(){{window.open(ctrlBase+'/control','_blank')}}
function openStream(url){{window.open(url,'_blank')}}
function copyText(text,btn){{
var ta=document.createElement('textarea');
ta.value=text;ta.style.position='fixed';ta.style.left='-9999px';
document.body.appendChild(ta);ta.select();
try{{document.execCommand('copy');
btn.classList.add('copied');btn.textContent='Copied!';
setTimeout(function(){{btn.classList.remove('copied');btn.textContent='Copy'}},1500)
}}catch(e){{alert('Copy failed: '+text)}}
document.body.removeChild(ta)}}
function addStream(name,url){{
var d=document.getElementById('streams');
var r=document.createElement('div');r.className='stream-row';
var u=encodeURIComponent(url);
r.innerHTML='<span class="stream-url">'+url+'</span>'+
'<div class="stream-btns"><button onclick="openStream(decodeURIComponent(\\''+u+'\\'))" class="secondary">Open</button>'+
'<button onclick="copyText(decodeURIComponent(\\''+u+'\\'),this)" class="copy-btn">Copy</button></div>';
d.appendChild(r)}}
addStream('MJPEG Stream',streamBase+'/stream');
addStream('Snapshot',streamBase+'/snapshot');
addStream('H.264 FLV','http://'+host+':18088/flv');
addStream('Display Stream',streamBase+'/display');
addStream('Display Snapshot',streamBase+'/display/snapshot');
</script></body></html>'''

        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            f"Content-Length: {len(html)}\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n"
        ) + html
        client.sendall(response.encode())

    def _serve_api_config(self, client):
        """Serve full running configuration as JSON for external tools (e.g., ACProxyCam)"""
        config = {
            # Encoder configuration
            'encoder_type': self.encoder_type,
            'streaming_port': self.streaming_port,
            'control_port': self.control_port,

            # H.264 settings
            'h264_enabled': self.h264_enabled,
            'h264_resolution': self.h264_resolution,
            'h264_bitrate': self.bitrate,

            # MJPEG settings
            'mjpeg_fps': self.mjpeg_fps_target,
            'jpeg_quality': self.jpeg_quality,

            # Auto-skip / CPU settings
            'skip_ratio': self.skip_ratio,
            'auto_skip': self.auto_skip,
            'target_cpu': self.target_cpu,

            # Display capture
            'display_enabled': self.display_enabled,
            'display_fps': self.display_fps,

            # LAN mode
            'autolanmode': self.autolanmode,

            # Camera info
            'device': self.camera_device,
            'width': self.camera_width,
            'height': self.camera_height,
            'internal_usb_port': self.internal_usb_port,

            # Operating mode
            'mode': self.mode,

            # Timelapse settings
            'timelapse_enabled': self.timelapse_enabled,
            'timelapse_mode': self.timelapse_mode,
            'timelapse_hyperlapse_interval': self.timelapse_hyperlapse_interval,
            'timelapse_storage': self.timelapse_storage,
            'timelapse_usb_path': self.timelapse_usb_path,
            'timelapse_output_fps': self.timelapse_output_fps,
            'timelapse_variable_fps': self.timelapse_variable_fps,
            'timelapse_target_length': self.timelapse_target_length,
            'timelapse_crf': self.timelapse_crf,
            'timelapse_duplicate_last_frame': self.timelapse_duplicate_last_frame,
            'timelapse_stream_delay': self.timelapse_stream_delay,
            'timelapse_flip_x': self.timelapse_flip_x,
            'timelapse_flip_y': self.timelapse_flip_y,

            # Session info
            'session_id': self.session_id,
        }
        body = json.dumps(config)
        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n"
            "\r\n"
        ) + body
        client.sendall(response.encode())

    def _handle_restart(self, client):
        """Handle restart request - spawns background process and returns immediately"""
        # Find app.sh path
        script_dir = os.path.dirname(os.path.realpath(__file__))
        app_sh = os.path.join(script_dir, 'app.sh')

        # Send response first
        body = json.dumps({'status': 'restarting'})
        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Connection: close\r\n"
            "\r\n"
        ) + body
        client.sendall(response.encode())
        client.close()

        # Spawn background restart process (detached from this process)
        # Use nohup and redirect to /dev/null so it survives our death
        restart_cmd = f"sleep 1 && {app_sh} stop && sleep 1 && {app_sh} start"
        subprocess.Popen(
            ['nohup', 'sh', '-c', restart_cmd],
            stdout=open('/dev/null', 'w'),
            stderr=open('/dev/null', 'w'),
            stdin=open('/dev/null', 'r'),
            preexec_fn=os.setpgrp  # Detach from parent process group
        )
        print(f"Restart requested, spawning background restart...", flush=True)

    def _handle_led_control(self, client, on: bool):
        """Handle LED control request - send MQTT command"""
        success = mqtt_send_led_control(on)
        body = json.dumps({'status': 'ok' if success else 'failed', 'led': 'on' if on else 'off'})
        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Connection: close\r\n"
            "\r\n"
        ) + body
        client.sendall(response.encode())

    def _serve_camera_controls(self, client):
        """Serve current camera control values as JSON"""
        controls = {
            'brightness': {'value': self.cam_brightness, 'min': 0, 'max': 255, 'default': 0},
            'contrast': {'value': self.cam_contrast, 'min': 0, 'max': 255, 'default': 32},
            'saturation': {'value': self.cam_saturation, 'min': 0, 'max': 132, 'default': 85},
            'hue': {'value': self.cam_hue, 'min': -180, 'max': 180, 'default': 0},
            'gamma': {'value': self.cam_gamma, 'min': 90, 'max': 150, 'default': 100},
            'sharpness': {'value': self.cam_sharpness, 'min': 0, 'max': 30, 'default': 3},
            'gain': {'value': self.cam_gain, 'min': 0, 'max': 1, 'default': 1},
            'backlight': {'value': self.cam_backlight, 'min': 0, 'max': 7, 'default': 0},
            'wb_auto': {'value': self.cam_wb_auto, 'min': 0, 'max': 1, 'default': 1},
            'wb_temp': {'value': self.cam_wb_temp, 'min': 2800, 'max': 6500, 'default': 4000},
            'exposure_auto': {'value': self.cam_exposure_auto, 'min': 1, 'max': 3, 'default': 3,
                              'options': {1: 'Manual', 3: 'Auto'}},
            'exposure': {'value': self.cam_exposure, 'min': 10, 'max': 2500, 'default': 156},
            'exposure_priority': {'value': self.cam_exposure_priority, 'min': 0, 'max': 1, 'default': 0},
            'power_line': {'value': self.cam_power_line, 'min': 0, 'max': 2, 'default': 1,
                           'options': {0: 'Disabled', 1: '50 Hz', 2: '60 Hz'}},
        }
        body = json.dumps(controls)
        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n"
            "\r\n"
        ) + body
        client.sendall(response.encode())

    def _handle_camera_reset(self, client):
        """Reset all camera controls to their default values"""
        # Default values for all controls
        defaults = {
            'cam_brightness': 0,
            'cam_contrast': 32,
            'cam_saturation': 85,
            'cam_hue': 0,
            'cam_gamma': 100,
            'cam_sharpness': 3,
            'cam_gain': 1,
            'cam_backlight': 0,
            'cam_wb_auto': 1,
            'cam_wb_temp': 4000,
            'cam_exposure_auto': 3,
            'cam_exposure': 156,
            'cam_exposure_priority': 0,
            'cam_power_line': 1,
        }

        # Apply all defaults
        for attr, value in defaults.items():
            setattr(self, attr, value)
            self._write_camera_ctrl(attr, value)

        # Save to config
        self.save_config()

        body = json.dumps({'status': 'ok', 'message': 'Camera controls reset to defaults'})
        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n"
            "\r\n"
        ) + body
        client.sendall(response.encode())

    def _handle_camera_set(self, client, body: str):
        """Handle camera control change - writes to ctrl file for immediate effect"""
        try:
            data = json.loads(body) if body else {}
            control = data.get('control')
            value = data.get('value')

            if control is None or value is None:
                raise ValueError("Missing control or value")

            # Map control names to ctrl file keys and instance vars
            control_map = {
                'brightness': ('cam_brightness', 'cam_brightness', 0, 255),
                'contrast': ('cam_contrast', 'cam_contrast', 0, 255),
                'saturation': ('cam_saturation', 'cam_saturation', 0, 132),
                'hue': ('cam_hue', 'cam_hue', -180, 180),
                'gamma': ('cam_gamma', 'cam_gamma', 90, 150),
                'sharpness': ('cam_sharpness', 'cam_sharpness', 0, 30),
                'gain': ('cam_gain', 'cam_gain', 0, 1),
                'backlight': ('cam_backlight', 'cam_backlight', 0, 7),
                'wb_auto': ('cam_wb_auto', 'cam_wb_auto', 0, 1),
                'wb_temp': ('cam_wb_temp', 'cam_wb_temp', 2800, 6500),
                'exposure_auto': ('cam_exposure_auto', 'cam_exposure_auto', 1, 3),
                'exposure': ('cam_exposure', 'cam_exposure', 10, 2500),
                'exposure_priority': ('cam_exposure_priority', 'cam_exposure_priority', 0, 1),
                'power_line': ('cam_power_line', 'cam_power_line', 0, 2),
            }

            if control not in control_map:
                raise ValueError(f"Unknown control: {control}")

            ctrl_key, attr_name, min_val, max_val = control_map[control]
            value = int(value)
            value = max(min_val, min(max_val, value))

            # Update instance variable
            setattr(self, attr_name, value)

            # Write to ctrl file for rkmpi_enc to apply
            self._write_camera_ctrl(ctrl_key, value)

            # Save to config for persistence
            self.save_config()

            result = {'status': 'ok', 'control': control, 'value': value}
        except Exception as e:
            result = {'status': 'error', 'message': str(e)}

        body = json.dumps(result)
        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n"
            "\r\n"
        ) + body
        client.sendall(response.encode())

    def _write_camera_ctrl(self, ctrl_key: str, value: int):
        """Write a single camera control to the cmd file for rkmpi_enc to apply.

        Uses cmd_file (append-only) to avoid race conditions.
        """
        try:
            with open(self.cmd_file, 'a') as f:
                f.write(f"{ctrl_key}={value}\n")
        except Exception as e:
            print(f"Failed to write camera ctrl: {e}", flush=True)

    def _apply_initial_camera_controls(self):
        """Apply saved camera controls when encoder starts"""
        import time
        time.sleep(2)  # Wait for encoder to be ready

        if not self.cam_controls_applied:
            print("Applying saved camera controls...", flush=True)
            # Write all camera controls to ctrl file
            controls = [
                ('cam_brightness', self.cam_brightness),
                ('cam_contrast', self.cam_contrast),
                ('cam_saturation', self.cam_saturation),
                ('cam_hue', self.cam_hue),
                ('cam_gamma', self.cam_gamma),
                ('cam_sharpness', self.cam_sharpness),
                ('cam_gain', self.cam_gain),
                ('cam_backlight', self.cam_backlight),
                ('cam_wb_auto', self.cam_wb_auto),
                ('cam_wb_temp', self.cam_wb_temp),
                ('cam_exposure_auto', self.cam_exposure_auto),
                ('cam_exposure', self.cam_exposure),
                ('cam_exposure_priority', self.cam_exposure_priority),
                ('cam_power_line', self.cam_power_line),
            ]
            for key, value in controls:
                self._write_camera_ctrl(key, value)
            self.cam_controls_applied = True
            print("Camera controls applied", flush=True)

    def _handle_touch(self, client, body: str):
        """Handle touch injection request"""
        try:
            data = json.loads(body) if body else {}
            x = int(data.get('x', 0))
            y = int(data.get('y', 0))
            duration = int(data.get('duration', 100))

            # Clamp coordinates to valid display range (depends on orientation)
            x = max(0, min(g_display_width, x))
            y = max(0, min(g_display_height, y))
            duration = max(10, min(1000, duration))  # 10ms to 1s

            # Transform display coordinates to touch panel coordinates
            touch_x, touch_y = transform_touch_coordinates(x, y, g_display_orientation)

            # Clamp touch coordinates to panel range (always 800x480)
            touch_x = max(0, min(FB_WIDTH, touch_x))
            touch_y = max(0, min(FB_HEIGHT, touch_y))

            success = inject_touch(touch_x, touch_y, duration)
            result = {'status': 'ok' if success else 'failed', 'x': x, 'y': y, 'touch_x': touch_x, 'touch_y': touch_y, 'duration': duration}
        except Exception as e:
            result = {'status': 'error', 'message': str(e)}

        body = json.dumps(result)
        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Connection: close\r\n"
            "\r\n"
        ) + body
        client.sendall(response.encode())

    def _serve_status(self, client):
        """Serve status JSON"""
        status = {
            'running': self.running,
            'encoder_type': self.encoder_type,
            'gkcam_all_frames': self.gkcam_all_frames,
            'camera_device': self.camera_device,
            'resolution': f"{self.camera_width}x{self.camera_height}" if self.is_rkmpi_mode() else 'gkcam',
            'h264_enabled': self.h264_enabled,
            'skip_ratio': self.skip_ratio,
            'autolanmode': self.autolanmode,
            'encoder_pid': self.encoder_pid,
            'gkcam_connected': self.gkcam_reader.connected if self.gkcam_reader else False,
            'ip': self.current_ip,
            'session_id': self.session_id
        }
        body = json.dumps(status)
        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Connection: close\r\n"
            "\r\n"
        ) + body
        client.sendall(response.encode())

    # ========================================================================
    # Timelapse Management
    # ========================================================================

    def _sanitize_filename(self, name: str) -> str:
        """Sanitize filename to prevent path traversal attacks"""
        # Remove any path components
        name = os.path.basename(name)
        # Remove any null bytes or other dangerous characters
        name = name.replace('\x00', '').replace('..', '')
        return name

    def _get_query_param(self, path: str, param: str, default: str = '') -> str:
        """Extract a query parameter from URL path"""
        if '?' not in path:
            return default
        query = path.split('?', 1)[1]
        for part in query.split('&'):
            if '=' in part:
                key, value = part.split('=', 1)
                if key == param:
                    return urllib.parse.unquote(value)
        return default

    def _get_timelapse_dir(self, storage: str) -> str:
        """Get timelapse directory for given storage type"""
        if storage == 'usb' and os.path.ismount('/mnt/udisk'):
            return self.timelapse_usb_path
        return TIMELAPSE_DIR

    def _generate_timelapse_thumbnail(self, mp4_path: str, timelapse_dir: str, base_name: str):
        """
        Generate thumbnail for a timelapse video using ffprobe/ffmpeg.
        Returns (thumbnail_filename, frame_count, duration) or (None, 0, 0) on error.
        """
        FFPROBE = "/ac_lib/lib/third_bin/ffprobe"
        FFMPEG = "/ac_lib/lib/third_bin/ffmpeg"
        FFMPEG_LIBS = "/ac_lib/lib/third_lib"

        # Check if ffprobe exists
        if not os.path.exists(FFPROBE):
            return None, 0, 0

        try:
            # Get video info using ffprobe
            env = os.environ.copy()
            env['LD_LIBRARY_PATH'] = FFMPEG_LIBS + ':' + env.get('LD_LIBRARY_PATH', '')

            result = subprocess.run(
                [FFPROBE, '-v', 'error', '-select_streams', 'v:0',
                 '-show_entries', 'stream=nb_frames,duration',
                 '-of', 'csv=p=0', mp4_path],
                capture_output=True, text=True, timeout=30, env=env
            )

            if result.returncode != 0:
                return None, 0, 0

            # Parse output: "frames,duration" or just "frames" or "duration"
            parts = result.stdout.strip().split(',')
            frames = 0
            duration = 0.0

            for part in parts:
                part = part.strip()
                if not part or part == 'N/A':
                    continue
                try:
                    if '.' in part:
                        duration = float(part)
                    else:
                        frames = int(part)
                except ValueError:
                    continue

            # If we got duration but not frames, estimate frames from duration
            if frames == 0 and duration > 0:
                frames = int(duration * TIMELAPSE_FPS)
            # If we got frames but not duration, calculate duration
            elif duration == 0 and frames > 0:
                duration = frames / TIMELAPSE_FPS

            if frames == 0:
                return None, 0, 0

            # Generate thumbnail from last frame
            thumbnail_name = f"{base_name}_{frames}.jpg"
            thumbnail_path = os.path.join(timelapse_dir, thumbnail_name)

            # Extract last frame as thumbnail (seek to near end, get first frame from there)
            seek_time = max(0, duration - 0.5) if duration > 0 else 0

            result = subprocess.run(
                [FFMPEG, '-y', '-ss', str(seek_time), '-i', mp4_path,
                 '-vframes', '1', '-q:v', '2', thumbnail_path],
                capture_output=True, timeout=30, env=env
            )

            if result.returncode == 0 and os.path.exists(thumbnail_path):
                return thumbnail_name, frames, round(duration, 1)
            else:
                return None, frames, round(duration, 1)

        except (subprocess.TimeoutExpired, OSError, Exception) as e:
            print(f"Thumbnail generation failed for {mp4_path}: {e}", flush=True)
            return None, 0, 0

    def _get_timelapse_recordings(self, storage: str = 'internal'):
        """Scan timelapse directory and return list of recordings with metadata"""
        recordings = []
        timelapse_dir = self._get_timelapse_dir(storage)

        if not os.path.isdir(timelapse_dir):
            return recordings

        try:
            files = os.listdir(timelapse_dir)
        except OSError:
            return recordings

        # Find all MP4 files
        mp4_files = [f for f in files if f.lower().endswith('.mp4')]

        for mp4 in mp4_files:
            mp4_path = os.path.join(timelapse_dir, mp4)
            try:
                stat_info = os.stat(mp4_path)
                size = stat_info.st_size
                mtime = stat_info.st_mtime
            except OSError:
                continue

            # Extract base name (without .mp4)
            base_name = mp4[:-4]

            # Find matching thumbnail (pattern: {base_name}_{frame_count}.jpg)
            thumbnail = None
            frames = 0
            duration = 0
            for f in files:
                if f.startswith(base_name + '_') and f.lower().endswith('.jpg'):
                    # Extract frame count from filename
                    try:
                        frame_part = f[len(base_name)+1:-4]  # e.g., "126" from "name_126.jpg"
                        frames = int(frame_part)
                        thumbnail = f
                        duration = frames / TIMELAPSE_FPS
                        break
                    except ValueError:
                        continue

            # If no thumbnail found, generate one using ffprobe/ffmpeg
            if thumbnail is None:
                thumbnail, frames, duration = self._generate_timelapse_thumbnail(
                    mp4_path, timelapse_dir, base_name
                )

            recordings.append({
                'name': base_name,
                'mp4': mp4,
                'thumbnail': thumbnail,
                'size': size,
                'frames': frames,
                'duration': round(duration, 1),
                'mtime': int(mtime)
            })

        return recordings

    def _serve_timelapse_page(self, client):
        """Serve the timelapse manager HTML page"""
        html = '''<!DOCTYPE html>
<html>
<head>
    <title>Time Lapse Recordings</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        * { box-sizing: border-box; }
        body { font-family: sans-serif; margin: 0; padding: 20px; background: #1a1a1a; color: #fff; }
        .container { max-width: 900px; margin: 0 auto; }
        h1 { color: #4CAF50; margin-bottom: 20px; }
        .header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 15px; flex-wrap: wrap; gap: 10px; }
        .header h1 { margin: 0; }
        .controls { display: flex; gap: 10px; align-items: center; }
        .summary { background: #2d2d2d; padding: 12px 15px; border-radius: 8px; margin-bottom: 15px; color: #888; }
        .recording { background: #2d2d2d; padding: 15px; margin-bottom: 10px; border-radius: 8px; display: flex; gap: 15px; align-items: flex-start; }
        .thumbnail { width: 160px; height: 90px; background: #111; border-radius: 4px; flex-shrink: 0; object-fit: cover; cursor: pointer; }
        .thumbnail-placeholder { width: 160px; height: 90px; background: #333; border-radius: 4px; flex-shrink: 0; display: flex; align-items: center; justify-content: center; color: #666; font-size: 12px; }
        .info { flex: 1; min-width: 0; }
        .filename { font-weight: bold; color: #fff; margin-bottom: 8px; word-break: break-all; }
        .meta { color: #888; font-size: 13px; margin-bottom: 10px; }
        .meta span { margin-right: 15px; }
        .actions { display: flex; gap: 8px; flex-wrap: wrap; }
        button { background: #4CAF50; color: white; border: none; padding: 8px 16px; border-radius: 4px; cursor: pointer; font-size: 13px; display: inline-flex; align-items: center; gap: 5px; }
        button:hover { background: #45a049; }
        button.secondary { background: #555; }
        button.secondary:hover { background: #666; }
        button.danger { background: #f44336; }
        button.danger:hover { background: #da190b; }
        select { padding: 8px 12px; border-radius: 4px; border: 1px solid #555; background: #333; color: #fff; cursor: pointer; }
        .empty { text-align: center; padding: 60px 20px; color: #666; }
        .empty-icon { font-size: 48px; margin-bottom: 15px; }
        .loading { text-align: center; padding: 60px 20px; color: #888; }
        /* Modal */
        .modal { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.9); z-index: 1000; justify-content: center; align-items: center; }
        .modal.active { display: flex; }
        .modal-content { position: relative; max-width: 90%; max-height: 90%; }
        .modal video { max-width: 100%; max-height: 80vh; border-radius: 8px; }
        .modal-close { position: absolute; top: -40px; right: 0; background: none; border: none; color: #fff; font-size: 30px; cursor: pointer; padding: 5px 10px; }
        .modal-close:hover { color: #f44; }
        .modal-title { color: #fff; margin-bottom: 10px; font-size: 14px; text-align: center; word-break: break-all; }
        /* Responsive */
        @media (max-width: 600px) {
            .recording { flex-direction: column; }
            .thumbnail, .thumbnail-placeholder { width: 100%; height: auto; aspect-ratio: 16/9; }
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>Time Lapse Recordings</h1>
            <div class="controls">
                <select id="storage-select" onchange="changeStorage()">
                    <option value="internal">Internal Storage</option>
                    <option value="usb">USB Drive</option>
                </select>
                <select id="sort-select" onchange="sortRecordings()">
                    <option value="date-desc">Newest first</option>
                    <option value="date-asc">Oldest first</option>
                    <option value="name-asc">Name (A-Z)</option>
                    <option value="size-desc">Largest first</option>
                </select>
                <button onclick="loadRecordings()" class="secondary">Refresh</button>
            </div>
        </div>
        <div class="summary" id="summary">Loading...</div>
        <div id="recordings-list">
            <div class="loading">Loading recordings...</div>
        </div>
    </div>

    <!-- Video Preview Modal -->
    <div class="modal" id="preview-modal" onclick="closePreview(event)">
        <div class="modal-content" onclick="event.stopPropagation()">
            <button class="modal-close" onclick="closePreview()">&times;</button>
            <div class="modal-title" id="preview-title"></div>
            <video id="preview-video" controls autoplay></video>
        </div>
    </div>

    <script>
        let recordings = [];
        let currentStorage = 'internal';
        let usbMounted = false;

        // Initialize with server settings
        function initPage() {
            // Check USB status and set default storage
            fetch('/api/timelapse/storage')
                .then(r => r.json())
                .then(data => {
                    usbMounted = data.usb_mounted;
                    const usbConfigured = data.current === 'usb';
                    currentStorage = data.current || 'internal';

                    const storageSelect = document.getElementById('storage-select');

                    // Only show storage selector if USB is configured AND mounted
                    if (usbConfigured && usbMounted) {
                        storageSelect.style.display = '';
                        storageSelect.value = currentStorage;
                    } else {
                        // Hide selector, always use internal
                        storageSelect.style.display = 'none';
                        currentStorage = 'internal';
                    }
                    loadRecordings();
                })
                .catch(() => loadRecordings());
        }

        function changeStorage() {
            currentStorage = document.getElementById('storage-select').value;
            loadRecordings();
        }

        function getStorageParam() {
            return currentStorage === 'usb' ? '?storage=usb' : '';
        }

        function formatSize(bytes) {
            if (bytes < 1024) return bytes + ' B';
            if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
            return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
        }

        function formatDuration(seconds) {
            const mins = Math.floor(seconds / 60);
            const secs = Math.floor(seconds % 60);
            return mins + ':' + secs.toString().padStart(2, '0');
        }

        function formatDate(timestamp) {
            const d = new Date(timestamp * 1000);
            const year = d.getFullYear();
            const month = (d.getMonth() + 1).toString().padStart(2, '0');
            const day = d.getDate().toString().padStart(2, '0');
            const hour = d.getHours().toString().padStart(2, '0');
            const min = d.getMinutes().toString().padStart(2, '0');
            return year + '-' + month + '-' + day + ' ' + hour + ':' + min;
        }

        function loadRecordings() {
            const storageLabel = currentStorage === 'usb' ? 'USB' : 'Internal';
            document.getElementById('summary').textContent = 'Loading ' + storageLabel + '...';
            fetch('/api/timelapse/list' + getStorageParam())
                .then(r => r.json())
                .then(data => {
                    // Check for storage error (e.g., USB not mounted)
                    if (data.error) {
                        recordings = [];
                        document.getElementById('summary').textContent = storageLabel + ': Error';
                        document.getElementById('recordings-list').innerHTML =
                            '<div class="empty"><div class="empty-icon">&#9888;</div>' + escapeHtml(data.error) + '</div>';
                        return;
                    }
                    recordings = data.recordings || [];
                    document.getElementById('summary').textContent =
                        storageLabel + ': ' + recordings.length + ' recording' + (recordings.length !== 1 ? 's' : '') +
                        ', ' + formatSize(data.total_size || 0);
                    sortRecordings();
                })
                .catch(err => {
                    document.getElementById('summary').textContent = 'Error loading recordings';
                    document.getElementById('recordings-list').innerHTML =
                        '<div class="empty"><div class="empty-icon">&#9888;</div>Failed to load recordings</div>';
                });
        }

        function sortRecordings() {
            const sort = document.getElementById('sort-select').value;
            const sorted = [...recordings];

            switch (sort) {
                case 'date-desc':
                    sorted.sort((a, b) => b.mtime - a.mtime);
                    break;
                case 'date-asc':
                    sorted.sort((a, b) => a.mtime - b.mtime);
                    break;
                case 'name-asc':
                    sorted.sort((a, b) => a.name.localeCompare(b.name));
                    break;
                case 'size-desc':
                    sorted.sort((a, b) => b.size - a.size);
                    break;
            }

            renderRecordings(sorted);
        }

        function renderRecordings(list) {
            const container = document.getElementById('recordings-list');

            if (list.length === 0) {
                container.innerHTML = '<div class="empty"><div class="empty-icon">&#128249;</div>No recordings found</div>';
                return;
            }

            const sp = getStorageParam();
            const spAmp = sp ? sp + '&' : '?';
            container.innerHTML = list.map(rec => `
                <div class="recording" data-name="${escapeHtml(rec.name)}">
                    ${rec.thumbnail
                        ? `<img class="thumbnail" src="/api/timelapse/thumb/${encodeURIComponent(rec.thumbnail)}${sp}" alt="Thumbnail" onclick="previewVideo('${escapeJs(rec.mp4)}', '${escapeJs(rec.name)}')">`
                        : '<div class="thumbnail-placeholder">No thumbnail</div>'
                    }
                    <div class="info">
                        <div class="filename">${escapeHtml(rec.mp4)}</div>
                        <div class="meta">
                            <span>${formatDate(rec.mtime)}</span>
                            <span>Duration: ${rec.frames > 0 ? formatDuration(rec.duration) : '-'}</span>
                            <span>Size: ${formatSize(rec.size)}</span>
                            <span>Frames: ${rec.frames > 0 ? rec.frames : '-'}</span>
                        </div>
                        <div class="actions">
                            <button onclick="previewVideo('${escapeJs(rec.mp4)}', '${escapeJs(rec.name)}')">&#9658; Preview</button>
                            <button class="secondary" onclick="downloadVideo('${escapeJs(rec.mp4)}')">&#11123; Download</button>
                            <button class="danger" onclick="deleteRecording('${escapeJs(rec.name)}', '${escapeJs(rec.mp4)}')">&#128465; Delete</button>
                        </div>
                    </div>
                </div>
            `).join('');
        }

        function escapeHtml(str) {
            return str.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
        }

        function escapeJs(str) {
            return str.replace(/\\\\/g, '\\\\\\\\').replace(/'/g, "\\\\'");
        }

        function previewVideo(mp4, name) {
            const modal = document.getElementById('preview-modal');
            const video = document.getElementById('preview-video');
            const title = document.getElementById('preview-title');

            title.textContent = mp4;
            video.src = '/api/timelapse/video/' + encodeURIComponent(mp4) + getStorageParam();
            modal.classList.add('active');
            video.play();
        }

        function closePreview(event) {
            if (event && event.target !== event.currentTarget) return;
            const modal = document.getElementById('preview-modal');
            const video = document.getElementById('preview-video');
            video.pause();
            video.src = '';
            modal.classList.remove('active');
        }

        function downloadVideo(mp4) {
            const a = document.createElement('a');
            a.href = '/api/timelapse/video/' + encodeURIComponent(mp4) + getStorageParam();
            a.download = mp4;
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
        }

        function deleteRecording(name, mp4) {
            if (!confirm('Delete "' + mp4 + '"?\\n\\nThis cannot be undone.')) {
                return;
            }

            fetch('/api/timelapse/delete/' + encodeURIComponent(name) + getStorageParam(), {
                method: 'DELETE'
            })
            .then(r => r.json())
            .then(data => {
                if (data.success) {
                    loadRecordings();
                } else {
                    alert('Delete failed: ' + (data.error || 'Unknown error'));
                }
            })
            .catch(err => {
                alert('Delete failed: ' + err.message);
            });
        }

        // Handle Escape key to close modal
        document.addEventListener('keydown', function(e) {
            if (e.key === 'Escape') {
                closePreview();
            }
        });

        // Load recordings on page load
        initPage();
    </script>
</body>
</html>'''

        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            f"Content-Length: {len(html)}\r\n"
            "Connection: close\r\n"
            "\r\n"
        ) + html
        client.sendall(response.encode())

    def _serve_timelapse_list(self, client, storage: str = 'internal'):
        """Return JSON list of timelapse recordings"""
        # Check if USB is requested but not available
        error = None
        if storage == 'usb':
            if not os.path.ismount('/mnt/udisk'):
                error = 'USB drive not mounted'
            elif not self.timelapse_usb_path:
                error = 'USB path not configured'
            elif not os.path.isdir(self.timelapse_usb_path):
                # Try to create the directory
                try:
                    os.makedirs(self.timelapse_usb_path, exist_ok=True)
                except OSError:
                    error = f'Cannot access USB path: {self.timelapse_usb_path}'

        if error:
            recordings = []
            total_size = 0
        else:
            recordings = self._get_timelapse_recordings(storage)
            total_size = sum(r['size'] for r in recordings)

        data = {
            'recordings': recordings,
            'total_size': total_size,
            'count': len(recordings),
            'storage': storage,
            'error': error
        }

        body = json.dumps(data)
        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Connection: close\r\n"
            "\r\n"
        ) + body
        client.sendall(response.encode())

    def _serve_timelapse_thumb(self, client, name: str, storage: str = 'internal'):
        """Serve thumbnail image"""
        # Sanitize filename
        name = self._sanitize_filename(name)
        if not name or not name.lower().endswith('.jpg'):
            self._serve_404(client)
            return

        timelapse_dir = self._get_timelapse_dir(storage)
        file_path = os.path.join(timelapse_dir, name)

        # Security: ensure path is within timelapse_dir
        try:
            real_path = os.path.realpath(file_path)
            real_timelapse_dir = os.path.realpath(timelapse_dir)
            if not real_path.startswith(real_timelapse_dir + os.sep):
                self._serve_404(client)
                return
        except OSError:
            self._serve_404(client)
            return

        if not os.path.isfile(file_path):
            self._serve_404(client)
            return

        try:
            with open(file_path, 'rb') as f:
                data = f.read()

            response = (
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/jpeg\r\n"
                f"Content-Length: {len(data)}\r\n"
                "Cache-Control: max-age=3600\r\n"
                "Connection: close\r\n"
                "\r\n"
            )
            client.sendall(response.encode() + data)
        except OSError:
            self._serve_404(client)

    def _serve_timelapse_video(self, client, name: str, request: str, storage: str = 'internal'):
        """Serve MP4 video with range request support"""
        # Sanitize filename
        name = self._sanitize_filename(name)
        if not name or not name.lower().endswith('.mp4'):
            self._serve_404(client)
            return

        timelapse_dir = self._get_timelapse_dir(storage)
        file_path = os.path.join(timelapse_dir, name)

        # Security: ensure path is within timelapse_dir
        try:
            real_path = os.path.realpath(file_path)
            real_timelapse_dir = os.path.realpath(timelapse_dir)
            if not real_path.startswith(real_timelapse_dir + os.sep):
                self._serve_404(client)
                return
        except OSError:
            self._serve_404(client)
            return

        if not os.path.isfile(file_path):
            self._serve_404(client)
            return

        try:
            file_size = os.path.getsize(file_path)
        except OSError:
            self._serve_404(client)
            return

        # Parse Range header for seeking support
        range_start = 0
        range_end = file_size - 1
        is_range_request = False

        for line in request.split('\r\n'):
            if line.lower().startswith('range:'):
                is_range_request = True
                range_spec = line.split(':', 1)[1].strip()
                if range_spec.startswith('bytes='):
                    range_spec = range_spec[6:]
                    parts = range_spec.split('-')
                    if parts[0]:
                        range_start = int(parts[0])
                    if len(parts) > 1 and parts[1]:
                        range_end = int(parts[1])
                break

        # Clamp range
        range_start = max(0, min(range_start, file_size - 1))
        range_end = max(range_start, min(range_end, file_size - 1))
        content_length = range_end - range_start + 1

        try:
            with open(file_path, 'rb') as f:
                if is_range_request:
                    f.seek(range_start)
                    data = f.read(content_length)
                    response = (
                        "HTTP/1.1 206 Partial Content\r\n"
                        "Content-Type: video/mp4\r\n"
                        f"Content-Length: {content_length}\r\n"
                        f"Content-Range: bytes {range_start}-{range_end}/{file_size}\r\n"
                        "Accept-Ranges: bytes\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                    )
                else:
                    data = f.read()
                    response = (
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: video/mp4\r\n"
                        f"Content-Length: {file_size}\r\n"
                        "Accept-Ranges: bytes\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                    )

            client.sendall(response.encode() + data)
        except OSError:
            self._serve_404(client)

    def _handle_timelapse_delete(self, client, name: str, storage: str = 'internal'):
        """Delete timelapse recording (MP4 and thumbnail)"""
        # Sanitize filename
        name = self._sanitize_filename(name)
        if not name:
            body = json.dumps({'success': False, 'error': 'Invalid filename'})
            response = (
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Type: application/json\r\n"
                f"Content-Length: {len(body)}\r\n"
                "Connection: close\r\n"
                "\r\n"
            ) + body
            client.sendall(response.encode())
            return

        timelapse_dir = self._get_timelapse_dir(storage)

        # Security check
        mp4_path = os.path.join(timelapse_dir, name + '.mp4')
        try:
            real_path = os.path.realpath(mp4_path)
            real_timelapse_dir = os.path.realpath(timelapse_dir)
            if not real_path.startswith(real_timelapse_dir + os.sep):
                body = json.dumps({'success': False, 'error': 'Invalid path'})
                response = (
                    "HTTP/1.1 403 Forbidden\r\n"
                    "Content-Type: application/json\r\n"
                    f"Content-Length: {len(body)}\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                ) + body
                client.sendall(response.encode())
                return
        except OSError:
            pass

        deleted_files = []
        errors = []

        # Delete MP4
        if os.path.isfile(mp4_path):
            try:
                os.unlink(mp4_path)
                deleted_files.append(name + '.mp4')
            except OSError as e:
                errors.append(f"Failed to delete {name}.mp4: {e}")

        # Find and delete thumbnail (pattern: {name}_{frames}.jpg)
        try:
            for f in os.listdir(timelapse_dir):
                if f.startswith(name + '_') and f.lower().endswith('.jpg'):
                    thumb_path = os.path.join(timelapse_dir, f)
                    try:
                        os.unlink(thumb_path)
                        deleted_files.append(f)
                    except OSError as e:
                        errors.append(f"Failed to delete {f}: {e}")
        except OSError:
            pass

        if deleted_files:
            body = json.dumps({'success': True, 'deleted': deleted_files})
            response = (
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                f"Content-Length: {len(body)}\r\n"
                "Connection: close\r\n"
                "\r\n"
            ) + body
        else:
            body = json.dumps({'success': False, 'error': 'File not found'})
            response = (
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: application/json\r\n"
                f"Content-Length: {len(body)}\r\n"
                "Connection: close\r\n"
                "\r\n"
            ) + body

        client.sendall(response.encode())

    def _serve_timelapse_storage(self, client):
        """Return timelapse storage info including USB mount status"""
        usb_mounted = os.path.ismount('/mnt/udisk')

        # Calculate sizes if directories exist
        internal_size = 0
        usb_size = 0

        if os.path.isdir(TIMELAPSE_DIR):
            try:
                for f in os.listdir(TIMELAPSE_DIR):
                    fpath = os.path.join(TIMELAPSE_DIR, f)
                    if os.path.isfile(fpath):
                        internal_size += os.path.getsize(fpath)
            except OSError:
                pass

        if usb_mounted and os.path.isdir(self.timelapse_usb_path):
            try:
                for f in os.listdir(self.timelapse_usb_path):
                    fpath = os.path.join(self.timelapse_usb_path, f)
                    if os.path.isfile(fpath):
                        usb_size += os.path.getsize(fpath)
            except OSError:
                pass

        data = {
            'current': self.timelapse_storage,
            'usb_mounted': usb_mounted,
            'usb_path': self.timelapse_usb_path,
            'internal_path': TIMELAPSE_DIR,
            'internal_size': internal_size,
            'usb_size': usb_size
        }

        body = json.dumps(data)
        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Connection: close\r\n"
            "\r\n"
        ) + body
        client.sendall(response.encode())

    def _serve_timelapse_browse(self, client, path):
        """Browse folders on USB drive for folder picker"""
        # Security: restrict to /mnt/udisk
        if not path.startswith('/mnt/udisk'):
            path = '/mnt/udisk'

        # Normalize path to prevent traversal
        path = os.path.normpath(path)
        if not path.startswith('/mnt/udisk'):
            path = '/mnt/udisk'

        if not os.path.ismount('/mnt/udisk'):
            data = {'error': 'USB drive not mounted'}
        elif not os.path.isdir(path):
            data = {'error': 'Directory not found'}
        else:
            try:
                folders = []
                for entry in os.listdir(path):
                    full_path = os.path.join(path, entry)
                    if os.path.isdir(full_path) and not entry.startswith('.'):
                        folders.append(entry)
                folders.sort()
                data = {'path': path, 'folders': folders}
            except OSError as e:
                data = {'error': str(e)}

        body = json.dumps(data)
        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Connection: close\r\n"
            "\r\n"
        ) + body
        client.sendall(response.encode())

    def _handle_timelapse_mkdir(self, client, body):
        """Create new folder on USB drive"""
        try:
            data = json.loads(body)
            path = data.get('path', '')
        except:
            path = ''

        # Security: restrict to /mnt/udisk
        if not path.startswith('/mnt/udisk/'):
            result = {'success': False, 'error': 'Invalid path'}
        else:
            # Normalize and verify path
            path = os.path.normpath(path)
            if not path.startswith('/mnt/udisk/'):
                result = {'success': False, 'error': 'Invalid path'}
            elif not os.path.ismount('/mnt/udisk'):
                result = {'success': False, 'error': 'USB drive not mounted'}
            else:
                try:
                    os.makedirs(path, exist_ok=True)
                    result = {'success': True, 'path': path}
                except OSError as e:
                    result = {'success': False, 'error': str(e)}

        body_resp = json.dumps(result)
        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body_resp)}\r\n"
            "Connection: close\r\n"
            "\r\n"
        ) + body_resp
        client.sendall(response.encode())

    def _serve_timelapse_moonraker_status(self, client):
        """Return Moonraker connection status"""
        connected = False
        print_state = 'standby'
        current_layer = 0
        total_layers = 0

        if self.moonraker_client:
            connected = self.moonraker_client.is_connected()
            state = self.moonraker_client.get_print_state()
            print_state = state.get('state', 'standby')
            current_layer = state.get('current_layer', 0)
            total_layers = state.get('total_layers', 0)

        data = {
            'connected': connected,
            'print_state': print_state,
            'current_layer': current_layer,
            'total_layers': total_layers,
            'timelapse_enabled': self.timelapse_enabled,
            'timelapse_active': self.timelapse_active,
            'timelapse_frames': self.timelapse_frames
        }

        body = json.dumps(data)
        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Connection: close\r\n"
            "\r\n"
        ) + body
        client.sendall(response.encode())

    def _handle_timelapse_settings(self, client, body: str):
        """Handle timelapse settings POST"""
        params = {}
        for param in body.split('&'):
            if '=' in param:
                k, v = param.split('=', 1)
                params[k] = urllib.parse.unquote(v)

        # Parse boolean settings
        old_enabled = self.timelapse_enabled
        self.timelapse_enabled = params.get('timelapse_enabled', '0') == '1'
        self.timelapse_variable_fps = params.get('timelapse_variable_fps', '0') == '1'
        self.timelapse_flip_x = params.get('timelapse_flip_x', '0') == '1'
        self.timelapse_flip_y = params.get('timelapse_flip_y', '0') == '1'

        # Parse string settings
        self.timelapse_mode = params.get('timelapse_mode', 'layer')
        if self.timelapse_mode not in ('layer', 'hyperlapse'):
            self.timelapse_mode = 'layer'

        self.timelapse_storage = params.get('timelapse_storage', 'internal')
        if self.timelapse_storage not in ('internal', 'usb'):
            self.timelapse_storage = 'internal'

        # USB path - must start with /mnt/udisk for security
        usb_path = params.get('timelapse_usb_path', self.timelapse_usb_path)
        if usb_path and usb_path.startswith('/mnt/udisk'):
            self.timelapse_usb_path = usb_path

        self.moonraker_host = params.get('moonraker_host', '127.0.0.1')

        # Parse numeric settings with validation
        try:
            self.timelapse_hyperlapse_interval = max(5, min(300, int(params.get('timelapse_hyperlapse_interval', 30))))
        except ValueError:
            pass
        try:
            self.moonraker_port = max(1, min(65535, int(params.get('moonraker_port', 7125))))
        except ValueError:
            pass
        try:
            self.timelapse_output_fps = max(1, min(120, int(params.get('timelapse_output_fps', 30))))
        except ValueError:
            pass
        try:
            self.timelapse_target_length = max(1, min(300, int(params.get('timelapse_target_length', 10))))
        except ValueError:
            pass
        try:
            self.timelapse_variable_fps_min = max(1, min(60, int(params.get('timelapse_variable_fps_min', 5))))
        except ValueError:
            pass
        try:
            self.timelapse_variable_fps_max = max(1, min(120, int(params.get('timelapse_variable_fps_max', 60))))
        except ValueError:
            pass
        try:
            self.timelapse_crf = max(0, min(51, int(params.get('timelapse_crf', 23))))
        except ValueError:
            pass
        try:
            self.timelapse_duplicate_last_frame = max(0, min(60, int(params.get('timelapse_duplicate_last_frame', 0))))
        except ValueError:
            pass
        try:
            self.timelapse_stream_delay = max(0, min(5, float(params.get('timelapse_stream_delay', 0.05))))
        except ValueError:
            pass

        # Save config
        self.save_config()

        # Manage Moonraker client based on timelapse_enabled
        if self.timelapse_enabled and not old_enabled:
            # Start Moonraker client
            self._start_moonraker_client()
        elif not self.timelapse_enabled and old_enabled:
            # Stop Moonraker client
            self._stop_moonraker_client()
        elif self.timelapse_enabled and self.moonraker_client:
            # Settings changed, restart client if host/port changed
            if self.moonraker_client.host != self.moonraker_host or self.moonraker_client.port != self.moonraker_port:
                self._stop_moonraker_client()
                self._start_moonraker_client()

        # Send response
        body_resp = json.dumps({'status': 'ok'})
        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body_resp)}\r\n"
            "Connection: close\r\n"
            "\r\n"
        ) + body_resp
        client.sendall(response.encode())

    def _start_moonraker_client(self):
        """Start Moonraker WebSocket client"""
        if self.moonraker_client:
            return  # Already running

        # Enable custom timelapse mode in rkmpi_enc to ignore Anycubic RPC timelapse
        self._send_timelapse_command("timelapse_custom_mode:1")

        # Set temp directory to app folder (avoids /tmp space issues on long prints)
        app_dir = os.path.dirname(os.path.abspath(__file__))
        self._send_timelapse_command(f"timelapse_temp_dir:{app_dir}/timelapse_frames")

        self.moonraker_client = MoonrakerClient(self.moonraker_host, self.moonraker_port)

        # Set up callbacks for timelapse
        self.moonraker_client.on_print_start = self._on_print_start
        self.moonraker_client.on_layer_change = self._on_layer_change
        self.moonraker_client.on_print_complete = self._on_print_complete
        self.moonraker_client.on_print_cancel = self._on_print_cancel

        self.moonraker_client.start()
        print(f"Moonraker client started for {self.moonraker_host}:{self.moonraker_port}", flush=True)

    def _stop_moonraker_client(self):
        """Stop Moonraker WebSocket client"""
        if not self.moonraker_client:
            return

        self.moonraker_client.stop()
        self.moonraker_client = None

        # Disable custom timelapse mode to allow Anycubic RPC timelapse
        self._send_timelapse_command("timelapse_custom_mode:0")

        print("Moonraker client stopped", flush=True)

    def _on_print_start(self, filename):
        """Callback when print starts"""
        if not self.timelapse_enabled:
            return

        print(f"Timelapse: Print started - {filename}", flush=True)
        self.timelapse_active = True
        self.timelapse_frames = 0
        self.timelapse_filename = filename

        # Determine output path
        output_path = self._get_timelapse_output_path()

        # Strip .gcode extension from filename
        base_name = filename
        if base_name.lower().endswith('.gcode'):
            base_name = base_name[:-6]

        # Send timelapse init command to rkmpi_enc
        self._send_timelapse_command(f"timelapse_init:{base_name}:{output_path}")
        self._send_timelapse_command(f"timelapse_fps:{self.timelapse_output_fps}")
        self._send_timelapse_command(f"timelapse_crf:{self.timelapse_crf}")
        if self.timelapse_variable_fps:
            self._send_timelapse_command(f"timelapse_variable_fps:{self.timelapse_variable_fps_min}:{self.timelapse_variable_fps_max}:{self.timelapse_target_length}")
        if self.timelapse_duplicate_last_frame > 0:
            self._send_timelapse_command(f"timelapse_duplicate_last:{self.timelapse_duplicate_last_frame}")
        self._send_timelapse_command(f"timelapse_flip:{1 if self.timelapse_flip_x else 0}:{1 if self.timelapse_flip_y else 0}")

        # Start hyperlapse timer if in hyperlapse mode
        if self.timelapse_mode == 'hyperlapse':
            self._start_hyperlapse_timer()

    def _start_hyperlapse_timer(self):
        """Start the hyperlapse capture timer"""
        if self.hyperlapse_timer:
            return  # Already running

        def capture_loop():
            while self.timelapse_active and self.timelapse_mode == 'hyperlapse':
                # Wait for stream delay
                if self.timelapse_stream_delay > 0:
                    time.sleep(self.timelapse_stream_delay)

                # Capture frame
                if self.timelapse_active:  # Check again after delay
                    self._send_timelapse_command("timelapse_capture")
                    self.timelapse_frames += 1
                    print(f"Timelapse: Hyperlapse frame {self.timelapse_frames}", flush=True)

                # Wait for interval
                time.sleep(self.timelapse_hyperlapse_interval)

        self.hyperlapse_timer = threading.Thread(target=capture_loop, daemon=True)
        self.hyperlapse_timer.start()
        print(f"Timelapse: Hyperlapse timer started ({self.timelapse_hyperlapse_interval}s interval)", flush=True)

    def _stop_hyperlapse_timer(self):
        """Stop the hyperlapse capture timer"""
        # Timer will stop naturally when timelapse_active becomes False
        self.hyperlapse_timer = None

    def _on_layer_change(self, layer, total_layers):
        """Callback when layer changes"""
        if not self.timelapse_enabled or not self.timelapse_active:
            return

        # Only capture in layer mode
        if self.timelapse_mode != 'layer':
            return

        print(f"Timelapse: Layer {layer}/{total_layers}", flush=True)

        # Wait for stream delay
        if self.timelapse_stream_delay > 0:
            time.sleep(self.timelapse_stream_delay)

        # Send capture command
        self._send_timelapse_command("timelapse_capture")
        self.timelapse_frames += 1

    def _on_print_complete(self, filename):
        """Callback when print completes"""
        if not self.timelapse_active:
            return

        print(f"Timelapse: Print complete - {filename}, {self.timelapse_frames} frames", flush=True)

        # Stop hyperlapse timer first (sets timelapse_active = False in loop check)
        self.timelapse_active = False
        self._stop_hyperlapse_timer()

        self._send_timelapse_command("timelapse_finalize")

    def _on_print_cancel(self, filename, reason):
        """Callback when print is cancelled - still save the timelapse"""
        if not self.timelapse_active:
            return

        print(f"Timelapse: Print cancelled - {filename}, reason: {reason}, saving {self.timelapse_frames} frames", flush=True)

        # Stop hyperlapse timer first
        self.timelapse_active = False
        self._stop_hyperlapse_timer()

        # Save the timelapse even on cancel - failed prints are often the most interesting
        self._send_timelapse_command("timelapse_finalize")

    def _get_timelapse_output_path(self):
        """Get the timelapse output directory based on current storage setting"""
        if self.timelapse_storage == 'usb' and os.path.ismount('/mnt/udisk'):
            # Create USB timelapse directory if needed
            try:
                os.makedirs(self.timelapse_usb_path, exist_ok=True)
                return self.timelapse_usb_path
            except OSError:
                print(f"Failed to create USB timelapse directory, falling back to internal", flush=True)

        return TIMELAPSE_DIR

    def _send_timelapse_command(self, command):
        """Send a timelapse command to rkmpi_enc via command file.

        Uses separate cmd_file (/tmp/h264_cmd) instead of ctrl_file to avoid
        race conditions - ctrl_file is periodically overwritten with settings/stats.
        """
        try:
            with open(self.cmd_file, 'a') as f:
                f.write(f"{command}\n")
        except Exception as e:
            print(f"Error sending timelapse command: {e}", flush=True)

    def _redirect_to_streaming(self, client, path, port=None):
        """Redirect to streaming server (rkmpi_enc handles streaming on a different port)"""
        if port is None:
            port = self.streaming_port
        # Use JavaScript redirect to handle the port change dynamically
        html = f'''<!DOCTYPE html>
<html><head>
<script>window.location.href = 'http://' + location.hostname + ':{port}{path}';</script>
</head><body>Redirecting to streaming server...</body></html>'''
        response = (
            f"HTTP/1.1 200 OK\r\n"
            f"Content-Type: text/html\r\n"
            f"Content-Length: {len(html)}\r\n"
            f"Connection: close\r\n"
            f"\r\n"
            f"{html}"
        )
        client.sendall(response.encode())

    def _serve_404(self, client):
        """Serve 404"""
        response = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot Found"
        client.sendall(response.encode())

    def _serve_503(self, client):
        """Serve 503"""
        response = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 19\r\nConnection: close\r\n\r\nService Unavailable"
        client.sendall(response.encode())

    def stop(self):
        """Stop the streamer"""
        print("Stopping streamer...", flush=True)
        self.running = False

        # Stop gkcam reader and transcoder (if gkcam mode)
        if self.gkcam_reader:
            self.gkcam_reader.stop()
        if self.gkcam_transcoder:
            self.gkcam_transcoder.stop()

        # Stop encoder (if rkmpi mode)
        if self.encoder_process:
            self.encoder_process.terminate()
            try:
                self.encoder_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.encoder_process.kill()

        # Stop MQTT/RPC responder subprocesses
        if self.rpc_subprocess:
            try:
                self.rpc_subprocess.terminate()
                self.rpc_subprocess.wait(timeout=2)
            except:
                try:
                    self.rpc_subprocess.kill()
                except:
                    pass
            self.rpc_subprocess = None

        if self.mqtt_subprocess:
            try:
                self.mqtt_subprocess.terminate()
                self.mqtt_subprocess.wait(timeout=2)
            except:
                try:
                    self.mqtt_subprocess.kill()
                except:
                    pass
            self.mqtt_subprocess = None

        # Stop Moonraker client
        self._stop_moonraker_client()

        # Stop camera stream (go-klipper mode only)
        if self.mode != 'vanilla-klipper':
            stop_camera_stream()

        # Cleanup
        try:
            os.unlink(self.ctrl_file)
        except FileNotFoundError:
            pass
        try:
            os.unlink("/tmp/h264_stream.fifo")
        except FileNotFoundError:
            pass


def run_rpc_responder_standalone():
    """Run RPC responder as isolated subprocess with distinct process title"""
    import select

    # Change process title so it's identifiable in top/ps
    try:
        import ctypes
        libc = ctypes.CDLL('libc.so.6')
        title = b'h264_rpc_responder'
        libc.prctl(15, title, 0, 0, 0)  # PR_SET_NAME = 15
    except:
        pass

    print("RPC Responder subprocess started", flush=True)

    def connect_and_listen():
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        except:
            pass
        sock.settimeout(30)

        try:
            sock.connect(('127.0.0.1', 18086))
            print("RPC: Connected to port 18086", flush=True)
        except Exception as e:
            print(f"RPC: Connect failed - {e}", flush=True)
            time.sleep(5)
            return

        needle = b'"video_stream_request"'

        while True:
            try:
                readable, _, _ = select.select([sock], [], [], 0.5)
            except:
                break

            if not readable:
                continue

            try:
                data = sock.recv(4096)
                if not data:
                    print("RPC: Connection closed", flush=True)
                    sock.close()
                    return
            except Exception as e:
                print(f"RPC: Receive error - {e}", flush=True)
                break

            if needle not in data:
                continue

            pos = data.find(needle)
            start = data.rfind(b'\x03', 0, pos)
            start = start + 1 if start != -1 else 0
            end = data.find(b'\x03', pos)
            if end == -1:
                continue

            try:
                msg = json.loads(data[start:end].decode('utf-8'))
                method = msg.get('method', '')
                params = msg.get('params', {})

                if method == 'process_status_update':
                    status = params.get('status', {})
                    video_request = status.get('video_stream_request')

                    if video_request:
                        req_id = video_request.get('id', 0)
                        req_method = video_request.get('method', '')
                        if req_method in ('startLanCapture', 'stopLanCapture'):
                            print(f"RPC: {req_method}", flush=True)
                            response = {
                                "id": 0,
                                "method": "Video/VideoStreamReply",
                                "params": {
                                    "eventtime": 0,
                                    "status": {
                                        "video_stream_reply": {
                                            "id": req_id,
                                            "method": req_method,
                                            "result": {}
                                        }
                                    }
                                }
                            }
                            response_bytes = json.dumps(response, indent='\t').encode('utf-8') + b'\x03'
                            try:
                                sock.sendall(response_bytes)
                            except:
                                pass
            except json.JSONDecodeError:
                pass

        sock.close()

    # Main loop - reconnect on disconnect
    while True:
        try:
            connect_and_listen()
        except Exception as e:
            print(f"RPC: Error - {e}, reconnecting in 3s", flush=True)
            time.sleep(3)


def run_mqtt_responder_standalone():
    """Run MQTT responder as isolated subprocess with distinct process title"""

    # Change process title so it's identifiable in top/ps
    try:
        import ctypes
        libc = ctypes.CDLL('libc.so.6')
        title = b'h264_mqtt_responder'
        libc.prctl(15, title, 0, 0, 0)  # PR_SET_NAME = 15
    except:
        pass

    print("MQTT Responder subprocess started", flush=True)

    # Load credentials
    creds = load_mqtt_credentials()
    if not creds or not creds.get('deviceId'):
        print("MQTT: No credentials available", flush=True)
        return

    model_id = load_model_id()
    if not model_id:
        print("MQTT: Could not load model ID", flush=True)
        return

    device_id = creds['deviceId']
    handled_msgids = set()
    msgid_cleanup_time = 0

    def connect_and_listen():
        nonlocal handled_msgids, msgid_cleanup_time

        context = ssl.create_default_context()
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(10)
        ssl_sock = context.wrap_socket(sock)
        ssl_sock.connect(('127.0.0.1', 9883))

        # Send CONNECT
        client_id = f"h264resp_{uuid.uuid4().hex[:8]}"
        ssl_sock.send(mqtt_build_connect(client_id, creds['username'], creds['password']))

        # Read CONNACK
        response = ssl_sock.recv(4)
        if len(response) < 4 or response[0] != MQTT_CONNACK or response[3] != 0:
            print(f"MQTT: Connection failed", flush=True)
            ssl_sock.close()
            time.sleep(5)
            return

        print("MQTT: Connected to broker", flush=True)

        # Subscribe to video topics
        def subscribe(topic, packet_id=1):
            topic_bytes = topic.encode('utf-8')
            payload = bytearray([0x00, packet_id])
            payload += bytearray([len(topic_bytes) >> 8, len(topic_bytes) & 0xFF])
            payload.extend(topic_bytes)
            payload.append(0x00)
            subscribe_pkt = bytearray([MQTT_SUBSCRIBE])
            subscribe_pkt.extend(mqtt_encode_remaining_length(len(payload)))
            subscribe_pkt.extend(payload)
            ssl_sock.send(subscribe_pkt)
            try:
                ssl_sock.settimeout(5)
                ssl_sock.recv(5)
            except:
                pass

        subscribe(f"anycubic/anycubicCloud/v1/web/printer/{model_id}/{device_id}/video", 1)
        subscribe(f"anycubic/anycubicCloud/v1/slicer/printer/{model_id}/{device_id}/video", 2)
        subscribe(f"anycubic/anycubicCloud/v1/printer/public/{model_id}/{device_id}/video/report", 3)
        print("MQTT: Subscribed to video topics", flush=True)

        ssl_sock.settimeout(0.5)
        buffer = b""

        while True:
            # Rate limit to prevent CPU spin
            time.sleep(0.05)

            try:
                data = ssl_sock.recv(4096)
                if not data:
                    continue
                buffer += data
                while len(buffer) > 2:
                        # Decode packet length
                        pkt_type = buffer[0] >> 4
                        i = 1
                        mult, remaining_len = 1, 0
                        while i < len(buffer) and buffer[i] & 0x80:
                            remaining_len += (buffer[i] & 0x7F) * mult
                            mult *= 128
                            i += 1
                        if i >= len(buffer):
                            break
                        remaining_len += (buffer[i] & 0x7F) * mult
                        header_end = i + 1
                        pkt_len = header_end + remaining_len
                        if len(buffer) < pkt_len:
                            break

                        # Process PUBLISH packets only
                        if pkt_type == 3:
                            try:
                                topic_len = (buffer[header_end] << 8) | buffer[header_end + 1]
                                topic_start = header_end + 2
                                topic = buffer[topic_start:topic_start + topic_len].decode('utf-8')
                                payload = buffer[topic_start + topic_len:pkt_len]

                                if '/video' in topic and '/report' not in topic:
                                    msg = json.loads(payload.decode('utf-8'))
                                    action = msg.get('action')
                                    msgid = msg.get('msgid')

                                    if action in ('startCapture', 'stopCapture'):
                                        if msgid and msgid in handled_msgids:
                                            pass  # Skip duplicate
                                        else:
                                            if msgid:
                                                now = time.time()
                                                if now - msgid_cleanup_time > 60:
                                                    handled_msgids.clear()
                                                    msgid_cleanup_time = now
                                                handled_msgids.add(msgid)

                                            print(f"MQTT: {action}", flush=True)
                                            # Send report
                                            report_topic = f"anycubic/anycubicCloud/v1/printer/public/{model_id}/{device_id}/video/report"
                                            state = "pushStopped" if action == "stopCapture" else "initSuccess"
                                            report_payload = json.dumps({
                                                "type": "video",
                                                "action": action,
                                                "timestamp": int(time.time() * 1000),
                                                "msgid": str(uuid.uuid4()),
                                                "state": state,
                                                "code": 200,
                                                "msg": "",
                                                "data": None
                                            })
                                            ssl_sock.send(mqtt_build_publish(report_topic, report_payload, qos=0))

                                elif '/video/report' in topic:
                                    msg = json.loads(payload.decode('utf-8'))
                                    action = msg.get('action')
                                    msgid = msg.get('msgid')
                                    if action == 'stopCapture' and msgid and msgid not in handled_msgids:
                                        print(f"MQTT: Countering spurious stopCapture", flush=True)
                                        report_topic = f"anycubic/anycubicCloud/v1/printer/public/{model_id}/{device_id}/video/report"
                                        counter_msgid = str(uuid.uuid4())
                                        handled_msgids.add(counter_msgid)
                                        counter_payload = json.dumps({
                                            "type": "video",
                                            "action": "startCapture",
                                            "timestamp": int(time.time() * 1000),
                                            "msgid": counter_msgid,
                                            "state": "initSuccess",
                                            "code": 200,
                                            "msg": "",
                                            "data": None
                                        })
                                        ssl_sock.send(mqtt_build_publish(report_topic, counter_payload, qos=0))
                            except:
                                pass

                        buffer = buffer[pkt_len:]
            except socket.timeout:
                pass
            except Exception as e:
                print(f"MQTT: Error - {e}", flush=True)
                break

        ssl_sock.close()

    # Main loop - reconnect on disconnect
    while True:
        try:
            connect_and_listen()
        except Exception as e:
            print(f"MQTT: Error - {e}, reconnecting in 5s", flush=True)
            time.sleep(5)


def run_flv_server_standalone(fifo_path, width, height):
    """Run FLV server as isolated subprocess with distinct process title"""

    # Change process title so it's identifiable in top/ps
    try:
        import ctypes
        libc = ctypes.CDLL('libc.so.6')
        title = b'h264_flv_server'
        libc.prctl(15, title, 0, 0, 0)  # PR_SET_NAME = 15
    except:
        pass

    print(f"FLV Server subprocess started (fifo={fifo_path}, {width}x{height})", flush=True)

    # Shared state
    running = True
    h264_buffer = []
    h264_lock = threading.Lock()
    cached_sps = None
    cached_pps = None
    client_count = 0
    client_lock = threading.Lock()
    rpc_subprocess = None
    mqtt_subprocess = None

    def h264_reader():
        nonlocal cached_sps, cached_pps
        while running:
            try:
                with open(fifo_path, 'rb') as fifo:
                    buffer = bytearray()
                    while running:
                        chunk = fifo.read(4096)
                        if not chunk:
                            break
                        buffer.extend(chunk)

                        while len(buffer) > 4:
                            idx4 = buffer.find(RE_NAL_START_4, 1)
                            idx3 = buffer.find(RE_NAL_START_3, 1)
                            if idx4 == -1:
                                idx = idx3
                            elif idx3 == -1:
                                idx = idx4
                            else:
                                idx = min(idx4, idx3)

                            if idx == -1 or idx > 65536:
                                if len(buffer) > 65536:
                                    nal_bytes = bytes(buffer[:65536])
                                    with h264_lock:
                                        h264_buffer.append(nal_bytes)
                                    del buffer[:65536]
                                break

                            nal_bytes = bytes(buffer[:idx])
                            if nal_bytes:
                                with h264_lock:
                                    h264_buffer.append(nal_bytes)
                                if len(nal_bytes) > 4:
                                    nal_start = 4 if nal_bytes[:4] == RE_NAL_START_4 else 3
                                    if nal_start < len(nal_bytes):
                                        nal_type = nal_bytes[nal_start] & 0x1F
                                        if nal_type == 7:
                                            cached_sps = nal_bytes[nal_start:]
                                        elif nal_type == 8:
                                            cached_pps = nal_bytes[nal_start:]
                            del buffer[:idx]
            except Exception as e:
                if running:
                    print(f"FLV H.264 reader error: {e}", flush=True)
                time.sleep(1)

    def serve_client(client):
        nonlocal client_count, rpc_subprocess, mqtt_subprocess

        # Spawn responder subprocesses on first client
        with client_lock:
            client_count += 1
            print(f"FLV client connected (total: {client_count})", flush=True)

            if client_count == 1:
                script_path = os.path.abspath(__file__)
                try:
                    rpc_subprocess = subprocess.Popen(
                        ['python', script_path, '--rpc-responder'],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT
                    )
                    print(f"RPC subprocess spawned (PID {rpc_subprocess.pid})", flush=True)
                except Exception as e:
                    print(f"Failed to spawn RPC: {e}", flush=True)

                try:
                    mqtt_subprocess = subprocess.Popen(
                        ['python', script_path, '--mqtt-responder'],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT
                    )
                    print(f"MQTT subprocess spawned (PID {mqtt_subprocess.pid})", flush=True)
                except Exception as e:
                    print(f"Failed to spawn MQTT: {e}", flush=True)

        try:
            # Read HTTP request
            client.settimeout(30)
            request = client.recv(4096)
            if b'GET /flv' not in request and b'GET / ' not in request:
                client.close()
                return

            # Send HTTP response
            response = (
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: 99999999999\r\n"
                "\r\n"
            )
            client.sendall(response.encode())
            client.settimeout(None)

            # Create FLV muxer
            muxer = FLVMuxer(width, height, 10, initial_sps=cached_sps, initial_pps=cached_pps)

            # Send FLV header
            client.sendall(muxer.get_header())
            if muxer.cached_config_tag:
                client.sendall(muxer.cached_config_tag)

            # Stream FLV
            last_idx = 0
            sent_keyframe = False

            while running:
                with h264_lock:
                    current_len = len(h264_buffer)

                if current_len > last_idx:
                    with h264_lock:
                        frames = h264_buffer[last_idx:current_len]
                    last_idx = current_len

                    for frame in frames:
                        flv_tag, contains_keyframe, contains_sps_pps = muxer.mux_nal_unit(frame)
                        if flv_tag:
                            if not sent_keyframe and not contains_keyframe and not contains_sps_pps:
                                time.sleep(0.01)
                                continue
                            try:
                                client.sendall(flv_tag)
                                if contains_keyframe:
                                    sent_keyframe = True
                            except:
                                return

                    # Trim buffer
                    if current_len > 100:
                        with h264_lock:
                            del h264_buffer[:current_len - 50]
                        last_idx = 50
                else:
                    time.sleep(0.05)

        except Exception as e:
            pass
        finally:
            try:
                client.close()
            except:
                pass

            # Kill subprocesses when last client disconnects
            with client_lock:
                client_count -= 1
                print(f"FLV client disconnected (total: {client_count})", flush=True)

                if client_count == 0:
                    if rpc_subprocess:
                        try:
                            rpc_subprocess.terminate()
                            rpc_subprocess.wait(timeout=2)
                        except:
                            try:
                                rpc_subprocess.kill()
                            except:
                                pass
                        rpc_subprocess = None
                        print("RPC subprocess terminated", flush=True)

                    if mqtt_subprocess:
                        try:
                            mqtt_subprocess.terminate()
                            mqtt_subprocess.wait(timeout=2)
                        except:
                            try:
                                mqtt_subprocess.kill()
                            except:
                                pass
                        mqtt_subprocess = None
                        print("MQTT subprocess terminated", flush=True)

    # Start H.264 reader thread
    threading.Thread(target=h264_reader, daemon=True).start()

    # Run HTTP server
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('0.0.0.0', 18088))
    server.listen(5)
    print("FLV server listening on port 18088", flush=True)

    while running:
        try:
            client, addr = server.accept()
            threading.Thread(target=serve_client, args=(client,), daemon=True).start()
        except Exception as e:
            if running:
                print(f"FLV server error: {e}", flush=True)

    server.close()


def main():
    parser = argparse.ArgumentParser(description='Combined MJPEG/H.264 Camera Streamer')
    parser.add_argument('--rpc-responder', action='store_true',
                        help='Run as RPC responder subprocess only')
    parser.add_argument('--mqtt-responder', action='store_true',
                        help='Run as MQTT responder subprocess only')
    parser.add_argument('--flv-server', action='store_true',
                        help='Run as FLV server subprocess only')
    parser.add_argument('--fifo-path', type=str, default='',
                        help='H.264 FIFO path (for --flv-server)')
    parser.add_argument('--width', type=int, default=1280,
                        help='Camera width (for --flv-server)')
    parser.add_argument('--height', type=int, default=720,
                        help='Camera height (for --flv-server)')
    parser.add_argument('--mode', choices=['go-klipper', 'vanilla-klipper'], default='go-klipper',
                        help='Operating mode: go-klipper (Anycubic firmware) or vanilla-klipper (external Klipper)')
    parser.add_argument('--streaming-port', type=int, default=8080,
                        help='MJPEG/snapshot streaming port for rkmpi_enc (default: 8080)')
    parser.add_argument('--control-port', type=int, default=8081,
                        help='Control panel and API port (default: 8081)')
    parser.add_argument('--encoder-type', choices=['gkcam', 'rkmpi', 'rkmpi-yuyv'], default='rkmpi-yuyv',
                        help='Encoder type: gkcam, rkmpi (MJPEG capture), or rkmpi-yuyv (default, YUYV capture with HW JPEG)')
    parser.add_argument('--gkcam-all-frames', action='store_true',
                        help='Decode all frames in gkcam mode (not just keyframes)')
    parser.add_argument('--port', type=int, default=8080,
                        help='Main HTTP server port (default: 8080)')
    parser.add_argument('--autolanmode', action='store_true',
                        help='Auto-enable LAN mode if not enabled')
    parser.add_argument('--no-autolanmode', action='store_true',
                        help='Disable auto LAN mode')
    parser.add_argument('--logging', action='store_true',
                        help='Enable debug logging')
    parser.add_argument('--auto-skip', action='store_true',
                        help='Enable automatic skip ratio based on CPU usage')
    parser.add_argument('--target-cpu', type=int, default=25,
                        help='Target max CPU usage %% for auto-skip (default: 25)')
    parser.add_argument('--skip-ratio', type=int, default=4,
                        help='Manual H.264 skip ratio (default: 4 = 25%%)')
    parser.add_argument('--bitrate', type=int, default=512,
                        help='H.264 bitrate in kbps (default: 512)')
    parser.add_argument('--mjpeg-fps', type=int, default=10,
                        help='MJPEG camera framerate (default: 10)')
    parser.add_argument('--jpeg-quality', type=int, default=85,
                        help='JPEG quality for HW encode in rkmpi-yuyv mode (1-99, default: 85)')
    parser.add_argument('--h264-resolution', type=str, default='1280x720',
                        help='H.264 encoding resolution for rkmpi mode (default: 1280x720)')

    args = parser.parse_args()

    # Initialize display orientation (needed for touch coordinate transformation)
    init_display_orientation()

    # If running as RPC responder subprocess, just do that
    if args.rpc_responder:
        run_rpc_responder_standalone()
        return

    # If running as MQTT responder subprocess, just do that
    if args.mqtt_responder:
        run_mqtt_responder_standalone()
        return

    # If running as FLV server subprocess, just do that
    if args.flv_server:
        run_flv_server_standalone(args.fifo_path, args.width, args.height)
        return

    # Load saved config (overrides defaults but not explicit command-line args)
    config_path = "/useremain/home/rinkhals/apps/29-h264-streamer.config"
    saved_config = {}
    try:
        with open(config_path, 'r') as f:
            saved_config = json.load(f)
        print(f"Loaded config: bitrate={saved_config.get('bitrate')}, fps={saved_config.get('mjpeg_fps')}", flush=True)
    except Exception as e:
        print(f"Config load failed: {e}", flush=True)

    # Map hyphenated args to underscored attrs
    # Use saved config values if available, otherwise use defaults
    # Sanitize mode to remove any invisible Unicode characters (e.g., U+2069)
    raw_mode = saved_config.get('mode', getattr(args, 'mode', 'go-klipper'))
    # Keep only ASCII printable chars and validate
    sanitized_mode = ''.join(c for c in str(raw_mode) if c.isascii() and c.isprintable())
    args.mode = sanitized_mode if sanitized_mode in ('go-klipper', 'vanilla-klipper') else 'go-klipper'
    # Sanitize encoder_type similarly
    raw_encoder = saved_config.get('encoder_type', getattr(args, 'encoder_type', 'gkcam'))
    sanitized_encoder = ''.join(c for c in str(raw_encoder) if c.isascii() and c.isprintable())
    args.encoder_type = sanitized_encoder if sanitized_encoder in ('gkcam', 'rkmpi', 'rkmpi-yuyv') else 'gkcam'
    args.gkcam_all_frames = saved_config.get('gkcam_all_frames', 'false') == 'true' if 'gkcam_all_frames' in saved_config else getattr(args, 'gkcam_all_frames', False)
    # Default: autolanmode enabled (from config or command line)
    if 'autolanmode' in saved_config:
        args.autolanmode = saved_config['autolanmode'] == 'true'
    else:
        args.autolanmode = not args.no_autolanmode
    # Debug logging
    if 'logging' in saved_config:
        args.logging = saved_config['logging'] == 'true'
    else:
        args.logging = getattr(args, 'logging', False)
    # Auto-skip and related settings
    if 'auto_skip' in saved_config:
        args.auto_skip = saved_config['auto_skip'] == 'true'
    else:
        args.auto_skip = getattr(args, 'auto_skip', True)
    if 'h264_enabled' in saved_config:
        args.h264_enabled = saved_config['h264_enabled'] == 'true'
    else:
        args.h264_enabled = getattr(args, 'h264_enabled', True)
    args.target_cpu = max(25, min(90, int(saved_config.get('target_cpu', getattr(args, 'target_cpu', 25)))))
    args.skip_ratio = max(1, min(20, int(saved_config.get('skip_ratio', getattr(args, 'skip_ratio', 4)))))
    args.bitrate = max(100, min(4000, int(saved_config.get('bitrate', getattr(args, 'bitrate', 512)))))
    args.mjpeg_fps = max(2, min(30, int(saved_config.get('mjpeg_fps', getattr(args, 'mjpeg_fps', 10)))))
    args.streaming_port = int(saved_config.get('streaming_port', getattr(args, 'streaming_port', 8080)))
    args.control_port = int(saved_config.get('control_port', getattr(args, 'control_port', 8081)))
    args.h264_resolution = saved_config.get('h264_resolution', getattr(args, 'h264_resolution', '1280x720'))
    # Display capture settings (disabled by default)
    args.display_enabled = saved_config.get('display_enabled', 'false') == 'true'
    args.display_fps = max(1, min(10, int(saved_config.get('display_fps', 5))))

    # Camera controls (loaded from config, applied on startup)
    args.cam_brightness = int(saved_config.get('cam_brightness', 0))
    args.cam_contrast = int(saved_config.get('cam_contrast', 32))
    args.cam_saturation = int(saved_config.get('cam_saturation', 85))
    args.cam_hue = int(saved_config.get('cam_hue', 0))
    args.cam_gamma = int(saved_config.get('cam_gamma', 100))
    args.cam_sharpness = int(saved_config.get('cam_sharpness', 3))
    args.cam_gain = int(saved_config.get('cam_gain', 1))
    args.cam_backlight = int(saved_config.get('cam_backlight', 0))
    args.cam_wb_auto = int(saved_config.get('cam_wb_auto', 1))
    args.cam_wb_temp = int(saved_config.get('cam_wb_temp', 4000))
    args.cam_exposure_auto = int(saved_config.get('cam_exposure_auto', 3))
    args.cam_exposure = int(saved_config.get('cam_exposure', 156))
    args.cam_exposure_priority = int(saved_config.get('cam_exposure_priority', 0))
    args.cam_power_line = int(saved_config.get('cam_power_line', 1))

    # Advanced timelapse settings (Moonraker integration)
    args.timelapse_enabled = saved_config.get('timelapse_enabled', 'false') == 'true'
    args.timelapse_mode = saved_config.get('timelapse_mode', 'layer')
    args.timelapse_hyperlapse_interval = max(5, min(300, int(saved_config.get('timelapse_hyperlapse_interval', 30))))
    args.timelapse_storage = saved_config.get('timelapse_storage', 'internal')
    args.timelapse_usb_path = saved_config.get('timelapse_usb_path', '/mnt/udisk/timelapse')
    args.moonraker_host = saved_config.get('moonraker_host', '127.0.0.1')
    args.moonraker_port = max(1, min(65535, int(saved_config.get('moonraker_port', 7125))))
    args.timelapse_output_fps = max(1, min(120, int(saved_config.get('timelapse_output_fps', 30))))
    args.timelapse_variable_fps = saved_config.get('timelapse_variable_fps', 'false') == 'true'
    args.timelapse_target_length = max(1, min(300, int(saved_config.get('timelapse_target_length', 10))))
    args.timelapse_variable_fps_min = max(1, min(60, int(saved_config.get('timelapse_variable_fps_min', 5))))
    args.timelapse_variable_fps_max = max(1, min(120, int(saved_config.get('timelapse_variable_fps_max', 60))))
    args.timelapse_crf = max(0, min(51, int(saved_config.get('timelapse_crf', 23))))
    args.timelapse_duplicate_last_frame = max(0, min(60, int(saved_config.get('timelapse_duplicate_last_frame', 0))))
    args.timelapse_stream_delay = max(0, min(5, float(saved_config.get('timelapse_stream_delay', 0.05))))
    args.timelapse_flip_x = saved_config.get('timelapse_flip_x', 'false') == 'true'
    args.timelapse_flip_y = saved_config.get('timelapse_flip_y', 'false') == 'true'

    app = StreamerApp(args)

    def signal_handler(sig, frame):
        print("Shutting down...", flush=True)
        app.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    print("Combined MJPEG/H.264 Camera Streamer", flush=True)
    print("====================================", flush=True)

    if app.start():
        # Wait for shutdown
        while app.running:
            time.sleep(1)
    else:
        print("Failed to start streamer", flush=True)
        sys.exit(1)


if __name__ == '__main__':
    main()
