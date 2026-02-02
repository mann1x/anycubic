# RKMPI H.264 Encoder Makefile
# Cross-compilation for RV1106 ARM
# USB Camera V4L2 capture + RV1106 hardware H.264 encoder
# With built-in HTTP/MQTT/RPC servers for native performance

# Toolchain
TOOLCHAIN_PATH = /shared/dev/rv1106-toolchain
CROSS_COMPILE = $(TOOLCHAIN_PATH)/bin/arm-rockchip830-linux-uclibcgnueabihf-
CC = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip

# SDK paths
SDK_PATH = $(CURDIR)
INC_PATH = $(SDK_PATH)/include
# Use libraries from printer (compatible with printer's uClibc)
LIB_PATH = $(SDK_PATH)/lib-printer
# OpenSSL built for ARM (static)
OPENSSL_PATH = $(SDK_PATH)/openssl-arm

# Compiler flags
# -fno-stack-protector: disable stack protection (not available in printer's uClibc)
CFLAGS = -Wall -O2 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fno-stack-protector
CFLAGS += -I$(INC_PATH)
CFLAGS += -I$(INC_PATH)/rkaiq/iq_parser_v2/j2s
# OpenSSL headers and enable TLS
CFLAGS += -I$(OPENSSL_PATH)/include -DHAVE_OPENSSL

# Linker flags
LDFLAGS = -L$(LIB_PATH)
LDFLAGS += -Wl,-rpath-link,$(LIB_PATH)
# OpenSSL static libraries
LDFLAGS += -L$(OPENSSL_PATH)/lib

# Libraries - use rockit_full for VENC, turbojpeg for software JPEG decode
# OpenSSL is linked statically, other libs dynamically
LIBS_DYNAMIC = -lrockit_full -lrockchip_mpp -lrga -ldrm -lturbojpeg -Wl,-Bstatic -lssl -lcrypto -Wl,-Bdynamic -lpthread -lm -lrt -ldl -lstdc++

# Default to dynamic linking (smaller binary, requires libs on target)
LIBS = $(LIBS_DYNAMIC)

# Target
TARGET = rkmpi_enc

# Source files
SRCS = rkmpi_enc.c \
       frame_buffer.c \
       flv_mux.c \
       http_server.c \
       json_util.c \
       mqtt_client.c \
       rpc_client.c \
       timelapse.c \
       display_capture.c \
       cJSON.c

OBJS = $(SRCS:.c=.o)

# Header dependencies
HDRS = frame_buffer.h \
       flv_mux.h \
       http_server.h \
       json_util.h \
       mqtt_client.h \
       rpc_client.h \
       timelapse.h \
       display_capture.h

.PHONY: all clean install static dynamic server-only timing

all: dynamic

# Build with timing instrumentation (for profiling)
timing: CFLAGS += -DENCODER_TIMING -DDISPLAY_TIMING
timing: clean $(TARGET)
	@echo "Built $(TARGET) with timing instrumentation"
	@echo "  Timing output will be printed to stderr every 100 frames"
	@ls -lh $(TARGET)

# Dynamic linking (smaller, ~100KB, needs libs)
dynamic: LIBS = $(LIBS_DYNAMIC)
dynamic: $(TARGET)
	@echo "Built $(TARGET) (dynamic, requires libs on target)"
	@ls -lh $(TARGET)

# Static linking (larger, ~5MB+, standalone)
static: LIBS = -Wl,-Bstatic -lsample_comm -lrockit -lrockchip_mpp -lrkaiq -lssl -lcrypto -Wl,-Bdynamic -lpthread -lm -lrt -ldl
static: $(TARGET)
	@echo "Built $(TARGET) (static, standalone)"
	@ls -lh $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)
	$(STRIP) $@

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

# Deploy to printer
PRINTER_IP ?= 192.168.178.43
PRINTER_USER ?= root
PRINTER_PASS ?= rockchip

deploy: $(TARGET)
	@echo "Deploying to $(PRINTER_IP)..."
	sshpass -p '$(PRINTER_PASS)' scp $(TARGET) $(PRINTER_USER)@$(PRINTER_IP):/tmp/
	@echo "Deployed to /tmp/$(TARGET)"

# Quick test on printer
test: deploy
	@echo "Testing on printer..."
	sshpass -p '$(PRINTER_PASS)' ssh $(PRINTER_USER)@$(PRINTER_IP) \
		"export LD_LIBRARY_PATH=/oem/usr/lib:\$$LD_LIBRARY_PATH; /tmp/$(TARGET) --help"

# Run encoder on printer in server mode
run-server: deploy
	@echo "Running encoder in server mode on printer..."
	sshpass -p '$(PRINTER_PASS)' ssh $(PRINTER_USER)@$(PRINTER_IP) \
		"export LD_LIBRARY_PATH=/oem/usr/lib:\$$LD_LIBRARY_PATH; /tmp/$(TARGET) -S -N -v &"
	@echo "Encoder running in server mode"
	@echo "  MJPEG: http://$(PRINTER_IP):8080/stream"
	@echo "  FLV:   http://$(PRINTER_IP):18088/flv"

# Run encoder on printer, save to file (legacy mode)
run: deploy
	@echo "Running encoder on printer..."
	sshpass -p '$(PRINTER_PASS)' ssh $(PRINTER_USER)@$(PRINTER_IP) \
		"export LD_LIBRARY_PATH=/oem/usr/lib:\$$LD_LIBRARY_PATH; /tmp/$(TARGET) -v > /tmp/test.h264 &"
	@echo "Encoder running, output to /tmp/test.h264"

# Copy to h264-streamer build directory
H264_STREAMER_BUILD = /shared/dev/Rinkhals/build/swu-tools/h264-streamer

install-h264: $(TARGET)
	@echo "Installing $(TARGET) to h264-streamer build..."
	cp $(TARGET) $(H264_STREAMER_BUILD)/
	@echo "Installed to $(H264_STREAMER_BUILD)/$(TARGET)"

help:
	@echo "RKMPI H.264 Encoder Build System v2.0"
	@echo ""
	@echo "Targets:"
	@echo "  all/dynamic  Build with dynamic linking (default)"
	@echo "  static       Build with static linking (standalone)"
	@echo "  timing       Build with timing instrumentation (profiling)"
	@echo "  clean        Remove build artifacts"
	@echo "  deploy       Copy binary to printer"
	@echo "  test         Deploy and show help on printer"
	@echo "  run-server   Deploy and run in server mode (HTTP/MQTT/RPC)"
	@echo "  run          Deploy and run encoder in legacy mode"
	@echo "  install-h264 Copy binary to h264-streamer build dir"
	@echo ""
	@echo "Variables:"
	@echo "  PRINTER_IP   Printer IP address (default: $(PRINTER_IP))"
	@echo ""
	@echo "Server mode endpoints:"
	@echo "  MJPEG stream:   http://\$$(PRINTER_IP):8080/stream"
	@echo "  MJPEG snapshot: http://\$$(PRINTER_IP):8080/snapshot"
	@echo "  FLV stream:     http://\$$(PRINTER_IP):18088/flv"
	@echo "  Display stream: http://\$$(PRINTER_IP):8080/display (with --display)"
	@echo "  Display snap:   http://\$$(PRINTER_IP):8080/display/snapshot"
