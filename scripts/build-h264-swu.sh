#!/bin/bash
#
# Build h264-streamer SWU package using Rinkhals docker image
#
# Usage: ./build-h264-swu.sh MODEL OUTPUT_DIR
# Example: ./build-h264-swu.sh KS1 ./dist
#
# This script is designed to run INSIDE the Rinkhals docker container:
#   docker run --rm -v /path/to/anycubic:/workspace ghcr.io/jbatonnet/rinkhals/build \
#       /workspace/scripts/build-h264-swu.sh KS1 /workspace/dist
#

set -e

KOBRA_MODEL_CODE=${1:-KS1}
OUTPUT_DIR=${2:-/workspace/dist}
WORKSPACE=${WORKSPACE:-/workspace}

# Validate model code
case "$KOBRA_MODEL_CODE" in
    K2P|K3|K3V2|KS1|KS1M|K3M)
        ;;
    *)
        echo "Error: Invalid model code '$KOBRA_MODEL_CODE'"
        echo "Valid models: K2P, K3, K3V2, KS1, KS1M, K3M"
        exit 1
        ;;
esac

echo "Building h264-streamer SWU for $KOBRA_MODEL_CODE"
echo "Workspace: $WORKSPACE"
echo "Output: $OUTPUT_DIR"

# Source paths
APP_SRC="$WORKSPACE/h264-streamer/29-h264-streamer"
ENCODER_SRC="$APP_SRC/rkmpi_enc"

# Verify source exists
if [ ! -f "$APP_SRC/app.sh" ]; then
    echo "Error: App source not found at $APP_SRC"
    exit 1
fi

if [ ! -f "$ENCODER_SRC" ]; then
    echo "Error: rkmpi_enc binary not found at $ENCODER_SRC"
    echo "Please build the encoder first and run 'make install-h264'"
    exit 1
fi

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Create temp directory for SWU content
UPDATE_DIR=/tmp/update_swu_$$
rm -rf "$UPDATE_DIR"
mkdir -p "$UPDATE_DIR/app"

# Create update.sh installer script
cat > "$UPDATE_DIR/update.sh" << 'INSTALLER'
#!/bin/sh

beep() {
    echo 1 > /sys/class/pwm/pwmchip0/pwm0/enable 2>/dev/null
    usleep $(($1 * 1000)) 2>/dev/null || sleep 1
    echo 0 > /sys/class/pwm/pwmchip0/pwm0/enable 2>/dev/null
}

APP_NAME="29-h264-streamer"
APP_SRC=/useremain/update_swu/app
APP_DST=/useremain/home/rinkhals/apps/$APP_NAME

APP_VERSION=$(cat $APP_SRC/app.json 2>/dev/null | sed -nr 's/.*"version"\s*:\s*"?([0-9.]+)"?.*/\1/p')

echo "Installing $APP_NAME version $APP_VERSION..."

mkdir -p $APP_DST
cp -r $APP_SRC/* $APP_DST/

chmod +x $APP_DST/app.sh
chmod +x $APP_DST/h264_monitor.sh 2>/dev/null
chmod +x $APP_DST/rkmpi_enc 2>/dev/null

touch $APP_DST/.enabled

echo "$APP_NAME installed successfully"
beep 500
INSTALLER

chmod +x "$UPDATE_DIR/update.sh"

# Copy app files
cp -r "$APP_SRC"/* "$UPDATE_DIR/app/"

# Clean up files that shouldn't be in the package
rm -rf "$UPDATE_DIR/app/__pycache__"
rm -f "$UPDATE_DIR/app/claude.md"
rm -f "$UPDATE_DIR/app/test_*.sh"
rm -f "$UPDATE_DIR/app/test_*.py"

# Make sure encoder is executable
chmod +x "$UPDATE_DIR/app/rkmpi_enc"

# Create tarball
SWU_DIR="$OUTPUT_DIR/update_swu"
mkdir -p "$SWU_DIR"
rm -rf "$SWU_DIR"/*

cd "$UPDATE_DIR"
tar -cf "$SWU_DIR/setup.tar" .
gzip "$SWU_DIR/setup.tar"
md5sum "$SWU_DIR/setup.tar.gz" | awk '{ print $1 }' > "$SWU_DIR/setup.tar.gz.md5"

# Get password for model
case "$KOBRA_MODEL_CODE" in
    K2P|K3|K3V2)
        PASSWORD="U2FsdGVkX19deTfqpXHZnB5GeyQ/dtlbHjkUnwgCi+w="
        ;;
    KS1|KS1M)
        PASSWORD="U2FsdGVkX1+lG6cHmshPLI/LaQr9cZCjA8HZt6Y8qmbB7riY"
        ;;
    K3M)
        PASSWORD="4DKXtEGStWHpPgZm8Xna9qluzAI8VJzpOsEIgd8brTLiXs8fLSu3vRx8o7fMf4h6"
        ;;
esac

# Create SWU (password-protected zip)
SWU_PATH="$OUTPUT_DIR/h264-streamer-${KOBRA_MODEL_CODE}.swu"
rm -f "$SWU_PATH"

cd "$OUTPUT_DIR"
zip -0 -P "$PASSWORD" -r "$(basename $SWU_PATH)" update_swu

# Cleanup
rm -rf "$SWU_DIR"
rm -rf "$UPDATE_DIR"

echo "Created: $SWU_PATH"
ls -la "$SWU_PATH"
