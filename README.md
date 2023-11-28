# OBS Linux Vulkan/OpenGL Game Capture

An OBS plugin for capturing Vulkan and OpenGL games on Linux.

## Installation

### AUR
64 bit: [obs-vkcapture-git](https://aur.archlinux.org/packages/obs-vkcapture-git/)

32 Bit: [lib32-obs-vkcapture-git](https://aur.archlinux.org/packages/lib32-obs-vkcapture-git/)

### Flatpak
```flatpak install com.obsproject.Studio.Plugin.OBSVkCapture```

```flatpak install org.freedesktop.Platform.VulkanLayer.OBSVkCapture```

#### OBS plugin: [com.obsproject.Studio.Plugin.OBSVkCapture](https://github.com/flathub/com.obsproject.Studio.Plugin.OBSVkCapture)

* This ONLY gives the plugin.
* You will need to install native ```obs-vkcapture``` if you want to capture native games.
* You will need to install Flatpak ```Capture Tools``` if you want to capture Flatpak games.

#### Capture Tools: [org.freedesktop.Platform.VulkanLayer.OBSVkCapture](https://github.com/flathub/org.freedesktop.Platform.VulkanLayer.OBSVkCapture)

* This is needed for Flatpak games. It doesn't matter whether you are running Flatpak OBS or native OBS.

### Manual

#### Dependencies

* cmake
* libobs
* libvulkan
* libgl
* libegl
* libX11 (optional)
* libxcb (optional)
* libwayland-client (optional)
* wayland-scanner (optional)

To install system-wide you need to use ```sudo make install```
```
    mkdir build && cd build
    cmake -DCMAKE_INSTALL_PREFIX=/usr ..
    make && make install
```

## Usage

1. Add `Game Capture` to your OBS scene.
2. Start the game with capture enabled:

    1. If Vulkan, use ```env OBS_VKCAPTURE=1 <APP>```
    2. If OpenGL, use ```obs-gamecapture <APP>```
    3. If obs-gamecapture doesn't work on Flatpak for OpenGL, try:

    ``` flatpak run --env=LD_PRELOAD=/usr/lib/extensions/vulkan/OBSVkCapture/lib/x86_64-linux-gnu/libobs_glcapture.so <APP>```

* Replace ```<APP>``` with the application you are trying to start.
* If using Steam launch options, replace ```<APP>```  with ```%command%```

## Troubleshooting

**NVIDIA**

Driver version >= 515.43.04 and `nvidia-drm.modeset=1` kernel parameter are required. In Wayland session make sure OBS is running on Wayland and not XWayland.

**OpenGL game crashing or not capturing video**

A potential workaround for this is to use Zink for converting OpenGL to Vulkan and then using Vulkan capture instead. May cause issues or reduced framerates with certain games/programs.

```env MESA_LOADER_DRIVER_OVERRIDE=zink OBS_VKCAPTURE=1 <APP>```

**No Game Capture source available in OBS 27**

If you are on X11, make sure you run OBS with EGL enabled: `OBS_USE_EGL=1 obs`.
