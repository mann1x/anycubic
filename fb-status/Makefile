# fb_status - Framebuffer Status Display
# Cross-compilation for RV1106 ARM
# Renders text overlay on printer display using stb_truetype

# Toolchain
TOOLCHAIN_PATH = /shared/dev/rv1106-toolchain
CROSS_COMPILE = $(TOOLCHAIN_PATH)/bin/arm-rockchip830-linux-uclibcgnueabihf-
CC = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip

# Compiler flags
CFLAGS = -Wall -O2 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fno-stack-protector

# Linker flags
LDFLAGS = -lm

# Target
TARGET = fb_status

# Source files
SRCS = fb_status.c

.PHONY: all clean deploy test

all: $(TARGET)
	@echo "Built $(TARGET)"
	@ls -lh $(TARGET)

$(TARGET): $(SRCS) stb_truetype.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)
	$(STRIP) $@

clean:
	rm -f $(TARGET)

# Deploy to printer
PRINTER_IP ?= 192.168.178.43
PRINTER_USER ?= root
PRINTER_PASS ?= rockchip

deploy: $(TARGET)
	@echo "Deploying to $(PRINTER_IP)..."
	sshpass -p '$(PRINTER_PASS)' scp $(TARGET) $(PRINTER_USER)@$(PRINTER_IP):/tmp/
	@echo "Deployed to /tmp/$(TARGET)"

test: deploy
	@echo "Testing on printer..."
	sshpass -p '$(PRINTER_PASS)' ssh $(PRINTER_USER)@$(PRINTER_IP) '/tmp/$(TARGET) --help'

help:
	@echo "fb_status - Framebuffer Status Display"
	@echo ""
	@echo "Targets:"
	@echo "  all      Build fb_status binary (default)"
	@echo "  clean    Remove build artifacts"
	@echo "  deploy   Copy binary to printer"
	@echo "  test     Deploy and show help on printer"
	@echo ""
	@echo "Variables:"
	@echo "  PRINTER_IP   Printer IP address (default: $(PRINTER_IP))"
	@echo ""
	@echo "Usage on printer:"
	@echo "  fb_status save              - Save current screen (via ffmpeg)"
	@echo "  fb_status show \"message\"   - Display status message (green)"
	@echo "  fb_status error \"message\"  - Display error message (red)"
	@echo "  fb_status hide              - Restore saved screen (via ffmpeg)"
