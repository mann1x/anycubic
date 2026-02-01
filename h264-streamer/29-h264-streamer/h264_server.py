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

def find_camera_device():
    """Find USB camera device - works with any V4L2 camera"""
    # Check by-id symlinks first (most reliable)
    by_id_path = "/dev/v4l/by-id"
    if os.path.exists(by_id_path):
        try:
            for entry in os.listdir(by_id_path):
                # Look for video-index0 (main capture device, not metadata)
                if 'video-index0' in entry.lower():
                    device = os.path.realpath(os.path.join(by_id_path, entry))
                    print(f"Found camera: {entry} -> {device}", flush=True)
                    return device
        except Exception as e:
            print(f"Error scanning by-id: {e}", flush=True)

    # Try common USB camera devices on RV1106 (video10-19 range)
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
        self.mode = getattr(args, 'mode', 'go-klipper')

        # Encoder mode: 'gkcam', 'rkmpi', or 'rkmpi-yuyv'
        self.encoder_type = getattr(args, 'encoder_type', 'rkmpi-yuyv')

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
        self.h264_enabled = True
        self.skip_ratio = getattr(args, 'skip_ratio', 4)  # Default 25% (100/4)
        self.saved_skip_ratio = self.skip_ratio  # Manual setting to restore when auto_skip disabled
        self.auto_skip = getattr(args, 'auto_skip', True)  # Default enabled
        self.target_cpu = getattr(args, 'target_cpu', 25)  # Default 25%
        self.bitrate = getattr(args, 'bitrate', 512)
        self.mjpeg_fps_target = getattr(args, 'mjpeg_fps', 10)  # Target MJPEG framerate
        self.max_camera_fps = 30  # Will be detected from camera

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

        # Control file (rkmpi mode only)
        self.ctrl_file = "/tmp/h264_ctrl"

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
        """Write control file for encoder"""
        try:
            with open(self.ctrl_file, 'w') as f:
                # For rkmpi-yuyv, H.264 is always enabled (no CPU impact, all HW)
                if self.encoder_type == 'rkmpi-yuyv':
                    f.write("h264=1\n")
                else:
                    f.write(f"h264={'1' if self.h264_enabled else '0'}\n")
                f.write(f"skip={self.skip_ratio}\n")
                f.write(f"auto_skip={'1' if self.auto_skip else '0'}\n")
                f.write(f"target_cpu={self.target_cpu}\n")
        except Exception as e:
            print(f"Error writing control file: {e}", flush=True)

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
            config['auto_skip'] = 'true' if self.auto_skip else 'false'
            config['target_cpu'] = str(self.target_cpu)
            config['skip_ratio'] = str(self.saved_skip_ratio)  # Save manual setting
            config['bitrate'] = str(self.bitrate)
            config['mjpeg_fps'] = str(self.mjpeg_fps_target)
            config['streaming_port'] = str(self.streaming_port)
            config['control_port'] = str(self.control_port)

            # Write back
            with open(config_path, 'w') as f:
                json.dump(config, f, indent=2)
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

        # Find camera
        self.camera_device = find_camera_device()
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

        # Start encoder process
        if not self.start_encoder():
            print("ERROR: Failed to start encoder!", flush=True)
            return False

        # Write initial control file
        self.write_ctrl_file()

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

        cmd = [
            encoder_path,
            '-S',  # Server mode
            '-N',  # No stdout
            '-d', self.camera_device,
            '-w', str(self.camera_width),
            '-h', str(self.camera_height),
            '-f', str(self.mjpeg_fps_target),
            '-s', str(effective_skip),
            '-b', str(self.bitrate),
            '-t', str(self.target_cpu),
            '-q',  # VBR mode
            '-v',  # Verbose logging
            '--mode', self.mode,  # Operating mode (go-klipper or vanilla-klipper)
            '--streaming-port', str(self.streaming_port)  # MJPEG HTTP port
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
            print(f"  Streaming on port {self.STREAMING_PORT} (/stream, /snapshot)", flush=True)
            print(f"  FLV on port {self.FLV_PORT} (/flv)", flush=True)
            if self.mode == 'vanilla-klipper':
                print(f"  MQTT/RPC responders: disabled (vanilla-klipper mode)", flush=True)
            else:
                print(f"  MQTT/RPC responders active", flush=True)

            # Start stderr reader for logging only (not for frame data)
            threading.Thread(target=self._encoder_stderr_reader, daemon=True).start()

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

            if path == '/stream':
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
            path = parts[1].split('?')[0]

            # Parse query params
            query = {}
            if '?' in parts[1]:
                qs = parts[1].split('?')[1]
                for param in qs.split('&'):
                    if '=' in param:
                        k, v = param.split('=', 1)
                        query[k] = v

            # Route
            # Streaming endpoints redirect to streaming_port (rkmpi_enc or gkcam streaming server)
            # Control server only handles /control and /api/*
            if path == '/stream':
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
            elif path == '/api/restart':
                self._handle_restart(client)
            elif path == '/api/led/on':
                self._handle_led_control(client, True)
            elif path == '/api/led/off':
                self._handle_led_control(client, False)
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
            </div>
            <div class="links" style="margin-top:10px;display:flex;justify-content:space-between;align-items:center;">
                <div>
                    <a href="/stream" target="_blank">Open MJPEG</a>
                    <a href="/snapshot" target="_blank">Open Snapshot</a>
                    <a href="#" onclick="openFlvFullscreen();return false;">Open FLV</a>
                </div>
                <div style="display:flex;gap:5px;align-items:center;">
                    <span style="color:#888;font-size:12px;margin-right:5px;">LED:</span>
                    <button onclick="controlLed(true)" style="padding:5px 12px;font-size:12px;">On</button>
                    <button onclick="controlLed(false)" class="secondary" style="padding:5px 12px;font-size:12px;">Off</button>
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
                                <option value="rkmpi" {'selected' if self.encoder_type == 'rkmpi' else ''}>rkmpi (USB camera)</option>
                                <option value="rkmpi-yuyv" {'selected' if self.encoder_type == 'rkmpi-yuyv' else ''}>rkmpi-yuyv (HW JPEG)</option>
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
                <div class="setting" style="margin-top: 15px;">
                    <div class="setting-row">
                        <button type="submit">Apply Settings</button>
                        <span style="color:#f90;font-size:12px;">Note: Encoder type change requires app restart</span>
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
            if (tab === 'mjpeg') {{
                startMjpegStream();
            }} else {{
                stopMjpegStream();
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

        // LED control
        function controlLed(on) {{
            fetch('/api/led/' + (on ? 'on' : 'off'))
                .then(r => r.json())
                .then(data => {{
                    console.log('LED:', data);
                }})
                .catch(e => console.error('LED error:', e));
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
            // Get mjpeg_fps from the correct slider based on encoder type
            const selectedEncoderType = document.querySelector('[name=encoder_type]').value;
            const mjpegFpsValue = selectedEncoderType === 'rkmpi-yuyv'
                ? document.getElementById('mjpeg_fps_slider_yuyv').value
                : document.getElementById('mjpeg_fps_slider').value;
            data.append('mjpeg_fps', mjpegFpsValue);

            // Check if settings require restart
            const newMjpegFps = parseInt(mjpegFpsValue) || 10;
            const newGkcamAllFrames = formData.has('gkcam_all_frames');
            const newLogging = formData.has('logging');
            const newBitrate = parseInt(document.querySelector('[name=bitrate]').value) || 512;
            const needsRkmpiRestart = (newMjpegFps !== currentMjpegFpsTarget) && currentEncoderType === 'rkmpi';
            const needsGkcamRestart = (newGkcamAllFrames !== currentGkcamAllFrames) && currentEncoderType === 'gkcam';
            const needsLoggingRestart = (newLogging !== currentLogging);
            const needsBitrateRestart = (newBitrate !== currentBitrate) && (currentEncoderType === 'rkmpi' || currentEncoderType === 'rkmpi-yuyv');
            const needsRestart = needsRkmpiRestart || needsGkcamRestart || needsLoggingRestart || needsBitrateRestart;

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
            'session_id': self.session_id
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
    args.mode = saved_config.get('mode', getattr(args, 'mode', 'go-klipper'))
    args.encoder_type = saved_config.get('encoder_type', getattr(args, 'encoder_type', 'gkcam'))
    args.gkcam_all_frames = saved_config.get('gkcam_all_frames', 'false') == 'true' if 'gkcam_all_frames' in saved_config else getattr(args, 'gkcam_all_frames', False)
    # Default: autolanmode enabled (from config or command line)
    if 'autolanmode' in saved_config:
        args.autolanmode = saved_config['autolanmode'] == 'true'
    else:
        args.autolanmode = not args.no_autolanmode
    # Auto-skip and related settings
    if 'auto_skip' in saved_config:
        args.auto_skip = saved_config['auto_skip'] == 'true'
    else:
        args.auto_skip = getattr(args, 'auto_skip', True)
    args.target_cpu = max(25, min(90, int(saved_config.get('target_cpu', getattr(args, 'target_cpu', 25)))))
    args.skip_ratio = max(1, min(20, int(saved_config.get('skip_ratio', getattr(args, 'skip_ratio', 4)))))
    args.bitrate = max(100, min(4000, int(saved_config.get('bitrate', getattr(args, 'bitrate', 512)))))
    args.mjpeg_fps = max(2, min(30, int(saved_config.get('mjpeg_fps', getattr(args, 'mjpeg_fps', 10)))))
    args.streaming_port = int(saved_config.get('streaming_port', getattr(args, 'streaming_port', 8080)))
    args.control_port = int(saved_config.get('control_port', getattr(args, 'control_port', 8081)))

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
