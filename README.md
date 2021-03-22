# OBS Linux Vulkan game capture

OBS plugin for Vulkan game capture on Linux.

Requires OBS with EGL support (currently unreleased, you need to build from git).

## Building

    mkdir build && cd build
    cmake -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib ..
    make && make install

## Usage

1. Add `Game Capture (Vulkan)` to your OBS scene.
2. Start the game with `OBS_VKCAPTURE=1` environment variable set.

Example:

    OBS_VKCAPTURE=1 game

Only supports capturing one game at a time.
