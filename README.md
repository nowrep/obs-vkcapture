# OBS Linux Vulkan/OpenGL game capture

OBS plugin for Vulkan/OpenGL game capture on Linux.

Requires OBS >= 27.
On X11 you need to explicitly enable EGL: `OBS_USE_EGL=1 obs`.

AUR: [obs-vkcapture-git](https://aur.archlinux.org/packages/obs-vkcapture-git/)

**Flatpak:**
* OBS plugin: [com.obsproject.Studio.Plugin.OBSVkCapture](https://github.com/flathub/com.obsproject.Studio.Plugin.OBSVkCapture)
* Capture tools: [org.freedesktop.Platform.VulkanLayer.OBSVkCapture](https://github.com/flathub/org.freedesktop.Platform.VulkanLayer.OBSVkCapture)

## About Flatpak
If you use Flatpak OBS, you need to install Flatpak *OBS plugin*.  
If you use Flatpak Steam, you need to install Flatpak *Capture tools* to be able to capture games running inside Flatpak Steam runtine.

For capturing games outside Flatpak runtime, you need native build regardless of if you are using Flatpak OBS or not.

## Dependencies

* cmake
* libobs
* libvulkan
* libgl
* libX11 (optional)
* libxcb (optional)
* libwayland-client (optional)

## Building

    mkdir build && cd build
    cmake -DCMAKE_INSTALL_PREFIX=/usr ..
    make && make install

## Usage

1. Add `Game Capture` to your OBS scene.
2. Start the game with capture enabled `obs-gamecapture %command%`.
3. (Recommended) Start the game with only Vulkan capture enabled `env OBS_VKCAPTURE=1 %command%`.

## Known Issues

* Vulkan capture requires VK_EXT_external_memory_dma_buf (NVIDIA driver >= 515.43.04)
* OpenGL GLX capture doesn't work with NVIDIA driver

## Troubleshooting

**Cannot create EGLImage: Arguments are inconsistent**

If you get this error with Vulkan capture, try starting the game with `OBS_VKCAPTURE_LINEAR=1` environment variable.

**No Game Capture source available in OBS**

If you are on X11, make sure you run OBS with EGL enabled: `OBS_USE_EGL=1 obs`.
