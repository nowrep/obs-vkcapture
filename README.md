# OBS Linux Vulkan/OpenGL game capture

OBS plugin for Vulkan/OpenGL game capture on Linux.

Requires OBS with EGL support (currently unreleased, you need to build from git).  
On X11 you need to explicitly enable EGL: `OBS_USE_EGL=1 obs`.

AUR: [obs-vkcapture-git](https://aur.archlinux.org/packages/obs-vkcapture-git/)

## Building

    mkdir build && cd build
    cmake -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib ..
    make && make install

## Usage

1. Add `Game Capture` to your OBS scene.
2. Start the game with capture enabled

### Vulkan

    obs-vkcapture game

### OpenGL

    obs-glcapture game

Only supports capturing one game at a time.
