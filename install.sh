#!/bin/bash
set -e

BUILD_DIR="build"

# Check if build directory exists and has been built
if [ ! -d "$BUILD_DIR" ] || [ ! -f "$BUILD_DIR/linux-vkcapture.so" ]; then
    echo "Project not built yet. Running build.sh first..."
    ./build.sh
fi

if [ "$EUID" -eq 0 ]; then
    echo "Running as root. Installing to system directories (/usr)..."
    # Reconfigure for /usr
    cmake -B "$BUILD_DIR" -S . -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=lld" -DCMAKE_MODULE_LINKER_FLAGS="-fuse-ld=lld"
    cmake --build "$BUILD_DIR"
    cmake --install "$BUILD_DIR"
    echo "System install complete!"
else
    echo "Running as user. Installing to local user directories ($HOME)..."
    
    # 1. Install Vulkan layer, GL inject library, wrapper scripts to local prefix (~/.local)
    LOCAL_PREFIX="$HOME/.local"
    cmake -B "$BUILD_DIR" -S . -DCMAKE_INSTALL_PREFIX="$LOCAL_PREFIX" -DCMAKE_BUILD_TYPE=Release -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=lld" -DCMAKE_MODULE_LINKER_FLAGS="-fuse-ld=lld"
    cmake --build "$BUILD_DIR"
    cmake --install "$BUILD_DIR"

    # 2. Install OBS plugin to user config directory
    OBS_PLUGIN_DIR="$HOME/.config/obs-studio/plugins/linux-vkcapture"
    # Check Flatpak OBS
    if [ -d "$HOME/.var/app/com.obsproject.Studio/config/obs-studio" ]; then
        OBS_PLUGIN_DIR="$HOME/.var/app/com.obsproject.Studio/config/obs-studio/plugins/linux-vkcapture"
    fi

    echo "Installing OBS plugin to $OBS_PLUGIN_DIR..."
    mkdir -p "$OBS_PLUGIN_DIR/bin/64bit"
    mkdir -p "$OBS_PLUGIN_DIR/data/locale"

    # Copy binary and locales
    cp "$BUILD_DIR/linux-vkcapture.so" "$OBS_PLUGIN_DIR/bin/64bit/linux-vkcapture.so"
    cp data/locale/*.ini "$OBS_PLUGIN_DIR/data/locale/"

    echo "User install complete!"
    echo "Note: Make sure '$LOCAL_PREFIX/bin' is in your PATH."
fi
