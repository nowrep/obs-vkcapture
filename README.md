# OBS Linux Vulkan/OpenGL game capture

OBS plugin for Vulkan/OpenGL game capture on Linux.

Requires OBS 27.
On X11 you need to explicitly enable EGL: `OBS_USE_EGL=1 obs`.

AUR: [obs-vkcapture-git](https://aur.archlinux.org/packages/obs-vkcapture-git/)

Flatpak:
* Capture tools: [org.freedesktop.Platform.VulkanLayer.OBSVkCapture](https://github.com/flathub/org.freedesktop.Platform.VulkanLayer.OBSVkCapture)
* OBS plugin: [com.obsproject.Studio.Plugin.OBSVkCapture](https://github.com/flathub/com.obsproject.Studio.Plugin.OBSVkCapture)

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

## Known Issues

* Requires VK_EXT_external_memory_dma_buf - not available in NVIDIA proprietary driver

## Troubleshooting

**Cannot create EGLImage: Arguments are inconsistent** \
**Cannot create EGLImage: EGL cannot access a requested resource (for example a context is bound in another thread).**

If you get this error with Vulkan capture, try starting the game with `OBS_VKCAPTURE_LINEAR=1` environment variable.

**No Game Capture source available in OBS**

If you are on X11, make sure you run OBS with EGL enabled: `OBS_USE_EGL=1 obs`.
