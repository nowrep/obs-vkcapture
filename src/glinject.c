/*
OBS Linux Vulkan/OpenGL game capture
Copyright (C) 2021 David Rosca <nowrep@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#define _GNU_SOURCE
#define GL_GLEXT_PROTOTYPES

#include "glinject.h"
#include "capture.h"
#include "utils.h"
#include "dlsym.h"
#include "plugin-macros.h"
#include "vklayer.h"

#include <dlfcn.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>

static bool gl_seen = false;
static bool vk_seen = false;
static struct gl_funcs gl_f;
static struct egl_funcs egl_f;
static struct glx_funcs glx_f;
static struct x11_funcs x11_f;
static struct vk_funcs vk_f;

static bool vkcapture_glvulkan = false;

struct gl_data {
    void *display;
    void *surface;
    int width;
    int height;
    GLuint fbo;
    GLuint texture;
    void *image;
    int buf_fourcc;
    int buf_strides[4];
    int buf_offsets[4];
    uint64_t buf_modifier;
    uint32_t winid;
    int nfd;
    int buf_fds[4];

    bool glx;
    unsigned long xpixmap;
    void *glxpixmap;

    VkInstance vkinst;
    VkPhysicalDevice vkphys_dev;
    VkDevice vkdev;
    VkImage vkimage;
    VkDeviceMemory vkmemory;

    uint8_t device_uuid[16];

    bool valid;
};
static struct gl_data data;

#define GETADDR(s, p, func) \
    p.func = (typeof(p.func))real_dlsym(RTLD_NEXT, #s #func); \
    if (!p.func && handle) { \
        p.func = (typeof(p.func))real_dlsym(handle, #s #func); \
    } \
    if (!p.func) { \
        hlog("Failed to resolve " #s #func); \
        return false; \
    } \

#define GETVKADDR(func) GETADDR(vk, vk_f, func)
#define GETEGLADDR(func) GETADDR(egl, egl_f, func)
#define GETGLXADDR(func) GETADDR(glX, glx_f, func)

#define GETPROCADDR(s, p, func) \
    p.func = (typeof(p.func))p.GetProcAddress(#s #func); \
    if (!p.func) { \
        hlog("Failed to resolve " #s #func); \
        return false; \
    } \

#define GETGLPROCADDR(func) GETPROCADDR(gl, gl_f, func)
#define GETEGLPROCADDR(func) GETPROCADDR(egl, egl_f, func)
#define GETGLXPROCADDR(func) GETPROCADDR(glX, glx_f, func)

#define GETXADDR(func) \
    x11_f.func = (typeof(x11_f.func))real_dlsym(handle, #func); \
    if (!x11_f.func) { \
        hlog("Failed to resolve " #func); \
        return false; \
    } \

static bool gl_init_funcs(bool glx)
{
    if (gl_seen) {
        return glx ? glx_f.valid && x11_f.valid : egl_f.valid;
    }

    hlog("Init %s %s (%s)", glx ? "GLX" : "EGL", PLUGIN_VERSION,
#ifdef __x86_64__
        "64bit");
#else
        "32bit");
#endif

    gl_seen = true;
    egl_f.valid = false;
    glx_f.valid = false;
    x11_f.valid = false;

    vkcapture_glvulkan = getenv("OBS_VKCAPTURE_GLVULKAN");

    capture_init();
    memset(&data, 0, sizeof(struct gl_data));
    memset(data.buf_fds, -1, sizeof(data.buf_fds));
    data.glx = glx;

    if (glx) {
        void *handle = dlopen("libGLX.so.0", RTLD_LAZY);
        if (!handle) {
            hlog("Failed to open libGLX.so.0");
            return false;
        }
        GETGLXADDR(GetProcAddress);
        GETGLXADDR(GetProcAddressARB);
        GETGLXPROCADDR(DestroyContext);
        GETGLXPROCADDR(SwapBuffers);
        GETGLXPROCADDR(SwapBuffersMscOML);
        GETGLXPROCADDR(CreatePixmap);
        GETGLXPROCADDR(DestroyPixmap);
        GETGLXPROCADDR(ChooseFBConfig);
        GETGLXPROCADDR(BindTexImageEXT);
        GETGLXPROCADDR(QueryDrawable);
        GETGLXPROCADDR(ChooseVisual);
        gl_f.GetProcAddress = glx_f.GetProcAddress;
        glx_f.valid = true;

        handle = dlopen("libX11.so.6", RTLD_LAZY);
        if (!handle) {
            hlog("Failed to open libX11.so.6");
            return false;
        }
        GETXADDR(XCreatePixmap);
        GETXADDR(XFreePixmap);
        GETXADDR(XFree);

        handle = dlopen("libX11-xcb.so.1", RTLD_LAZY);
        if (!handle) {
            hlog("Failed to open libX11-xcb.so.1");
            return false;
        }
        GETXADDR(XGetXCBConnection);

        handle = dlopen("libxcb-dri3.so.0", RTLD_LAZY);
        if (!handle) {
            hlog("Failed to open libxcb-dri3.so.0");
            return false;
        }
        GETXADDR(xcb_dri3_buffers_from_pixmap);
        GETXADDR(xcb_dri3_buffers_from_pixmap_reply);
        GETXADDR(xcb_dri3_buffers_from_pixmap_reply_fds);
        GETXADDR(xcb_dri3_buffers_from_pixmap_strides);
        GETXADDR(xcb_dri3_buffers_from_pixmap_offsets);
        x11_f.valid = true;
    } else {
        void *handle = dlopen("libEGL.so.1", RTLD_LAZY);
        if (!handle) {
            hlog("Failed to open libEGL.so.1");
            return false;
        }
        GETEGLADDR(GetProcAddress);
        GETEGLPROCADDR(DestroyContext);
        GETEGLPROCADDR(GetCurrentContext);
        GETEGLPROCADDR(CreateWindowSurface);
        GETEGLPROCADDR(CreateImage);
        GETEGLPROCADDR(DestroyImage);
        GETEGLPROCADDR(QuerySurface);
        GETEGLPROCADDR(SwapBuffers);
        GETEGLPROCADDR(ExportDMABUFImageQueryMESA);
        GETEGLPROCADDR(ExportDMABUFImageMESA);
        gl_f.GetProcAddress = egl_f.GetProcAddress;
        egl_f.valid = true;
    }

    GETGLPROCADDR(GenFramebuffers);
    GETGLPROCADDR(GenTextures);
    GETGLPROCADDR(TexImage2D);
    GETGLPROCADDR(TexParameteri);
    GETGLPROCADDR(GetIntegerv);
    GETGLPROCADDR(BindTexture);
    GETGLPROCADDR(DeleteFramebuffers);
    GETGLPROCADDR(DeleteTextures);
    GETGLPROCADDR(Enable);
    GETGLPROCADDR(Disable);
    GETGLPROCADDR(IsEnabled);
    GETGLPROCADDR(BindFramebuffer);
    GETGLPROCADDR(FramebufferTexture2D);
    GETGLPROCADDR(ReadBuffer);
    GETGLPROCADDR(DrawBuffer);
    GETGLPROCADDR(BlitFramebuffer);
    GETGLPROCADDR(GetError);
    GETGLPROCADDR(GetString);
    GETGLPROCADDR(GetUnsignedBytei_vEXT);
    GETGLPROCADDR(CreateMemoryObjectsEXT);
    GETGLPROCADDR(MemoryObjectParameterivEXT);
    GETGLPROCADDR(ImportMemoryFdEXT);
    GETGLPROCADDR(TexStorageMem2DEXT);
    GETGLPROCADDR(IsMemoryObjectEXT);

    data.valid = true;

    return true;
}

static bool vulkan_init_funcs()
{
    if (vk_seen) {
        return vk_f.valid;
    }

    vk_seen = true;
    vk_f.valid = false;

    void *handle = dlopen("libvulkan.so.1", RTLD_LAZY);
    if (!handle) {
        hlog("Failed to open libvulkan.so.1");
        return false;
    }

    GETVKADDR(GetInstanceProcAddr);
    GETVKADDR(GetDeviceProcAddr);
    GETVKADDR(CreateInstance);
    GETVKADDR(DestroyInstance);
    GETVKADDR(CreateDevice);
    GETVKADDR(DestroyDevice);

    vk_f.valid = true;

    return true;
}

#undef GETADDR
#undef GETVKADDR
#undef GETEGLADDR
#undef GETPROCADDR
#undef GETEGLPROCADDR

#define GETVKPROC(p, d, func) \
    vk_f.func = (typeof(vk_f.func))vk_f.p(data.d, "vk" #func); \
    if (!vk_f.func) { \
        hlog("Failed to resolve vk" #func); \
        goto fail; \
    } \

#define GETINSTPROC(func) GETVKPROC(GetInstanceProcAddr, vkinst, func)
#define GETDEVPROC(func) GETVKPROC(GetDeviceProcAddr, vkdev, func)

static bool vulkan_init()
{
    if (data.vkdev) {
        return true;
    }

    if (!vulkan_init_funcs()) {
        return false;
    }

    gl_f.GetUnsignedBytei_vEXT(0x9597, 0, data.device_uuid);

    const char *instance_extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };

    const char *device_extensions[] = {
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_MAINTENANCE1_EXTENSION_NAME,
        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    };

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "OBS vkcapture";
    appInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo instanceInfo = {};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = sizeof(instance_extensions) / sizeof(*instance_extensions);
    instanceInfo.ppEnabledExtensionNames = instance_extensions;

    char *disable_vkcapture = getenv("DISABLE_OBS_VKCAPTURE");
    setenv("DISABLE_OBS_VKCAPTURE", "1", 1);

    VkResult res = vk_f.CreateInstance(&instanceInfo, NULL, &data.vkinst);

    if (disable_vkcapture) {
        setenv("DISABLE_OBS_VKCAPTURE", disable_vkcapture, 1);
    } else {
        unsetenv("DISABLE_OBS_VKCAPTURE");
    }

    if (res != VK_SUCCESS) {
        hlog("Vulkan: Failed to create instance %s", result_to_str(res));
        return false;
    }

    GETINSTPROC(EnumeratePhysicalDevices);
    GETINSTPROC(GetPhysicalDeviceProperties2);
    GETINSTPROC(GetPhysicalDeviceMemoryProperties);
    GETINSTPROC(GetPhysicalDeviceFormatProperties2KHR);
    GETINSTPROC(GetPhysicalDeviceImageFormatProperties2KHR);

    uint32_t deviceCount = 16;
    VkPhysicalDevice physicalDevices[16];
    res = vk_f.EnumeratePhysicalDevices(data.vkinst, &deviceCount, physicalDevices);
    if (res != VK_SUCCESS) {
        hlog("Vulkan: Failed to enumerate physical devices %s", result_to_str(res));
        goto fail;
    }
    for (uint32_t i = 0; i < deviceCount; ++i) {
        VkPhysicalDeviceIDProperties propsID = {};
        propsID.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 props = {};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = &propsID;
        vk_f.GetPhysicalDeviceProperties2(physicalDevices[i], &props);
        if (memcmp(data.device_uuid, propsID.deviceUUID, sizeof(data.device_uuid)) == 0) {
            data.vkphys_dev = physicalDevices[i];
            break;
        }
    }
    if (!data.vkphys_dev) {
        hlog("Vulkan: Failed to find matching device");
        goto fail;
    }

    float queuePriority = 1.0;
    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = 0;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo deviceInfo = {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = sizeof(device_extensions) / sizeof(*device_extensions);
    deviceInfo.ppEnabledExtensionNames = device_extensions;
    res = vk_f.CreateDevice(data.vkphys_dev, &deviceInfo, NULL, &data.vkdev);
    if (res != VK_SUCCESS) {
        /* Try without VK_EXT_image_drm_format_modifier */
        deviceInfo.enabledExtensionCount--;
        res = vk_f.CreateDevice(data.vkphys_dev, &deviceInfo, NULL, &data.vkdev);
    }
    if (res != VK_SUCCESS) {
        hlog("Vulkan: Failed to create device %s", result_to_str(res));
        goto fail;
    }

    GETDEVPROC(CreateImage);
    GETDEVPROC(DestroyImage);
    GETDEVPROC(AllocateMemory);
    GETDEVPROC(FreeMemory);
    GETDEVPROC(GetImageSubresourceLayout);
    GETDEVPROC(GetImageMemoryRequirements2KHR);
    GETDEVPROC(BindImageMemory2KHR);
    GETDEVPROC(GetMemoryFdKHR);

    vk_f.GetImageDrmFormatModifierPropertiesEXT = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)
        vk_f.GetDeviceProcAddr(data.vkdev, "vkGetImageDrmFormatModifierPropertiesEXT");
    if (!vk_f.GetImageDrmFormatModifierPropertiesEXT) {
        hlog("DRM format modifier support not available");
    }

    return true;

fail:
    if (data.vkdev) {
        vk_f.DestroyDevice(data.vkdev, NULL);
        data.vkdev = VK_NULL_HANDLE;
    }
    if (data.vkinst) {
        vk_f.DestroyInstance(data.vkinst, NULL);
        data.vkinst = VK_NULL_HANDLE;
    }
    return false;
}

#undef GETVKPROC
#undef GETINSTPROC
#undef GETDEVPROC

static bool vulkan_shtex_init()
{
    if (!vulkan_init()) {
        return false;
    }

    gl_f.GenFramebuffers(1, &data.fbo);
    if (data.fbo == 0) {
        hlog("Failed to initialize FBO");
        return false;
    }

    const bool no_modifiers = capture_allocate_no_modifiers();
    const bool linear = capture_allocate_linear();
    const bool map_host = capture_allocate_map_host();
    const bool same_device = capture_compare_device_uuid(data.device_uuid);

    hlog("Texture %s %ux%u", "GL_RGBA (Vulkan)", data.width, data.height);

    VkExternalMemoryImageCreateInfo ext_mem_image_info = {};
    ext_mem_image_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_mem_image_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT | VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;

    VkImageCreateInfo img_info = {};
    img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.pNext = &ext_mem_image_info;
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.format = VK_FORMAT_B8G8R8A8_UNORM;
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    img_info.extent.width = data.width;
    img_info.extent.height = data.height;
    img_info.extent.depth = 1;
    img_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    img_info.tiling = VK_IMAGE_TILING_LINEAR;

    int num_planes = 1;
    uint64_t *image_modifiers = NULL;
    VkImageDrmFormatModifierListCreateInfoEXT image_modifier_list = {};
    struct VkDrmFormatModifierPropertiesEXT *modifier_props = NULL;
    uint32_t modifier_prop_count = 0;

    if (!no_modifiers && vk_f.GetImageDrmFormatModifierPropertiesEXT) {
        VkDrmFormatModifierPropertiesListEXT modifier_props_list = {};
        modifier_props_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;

        VkFormatProperties2KHR format_props = {};
        format_props.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
        format_props.pNext = &modifier_props_list;

        vk_f.GetPhysicalDeviceFormatProperties2KHR(data.vkphys_dev, img_info.format, &format_props);

        modifier_props = malloc(modifier_props_list.drmFormatModifierCount * sizeof(struct VkDrmFormatModifierPropertiesEXT));
        modifier_props_list.pDrmFormatModifierProperties = modifier_props;

        vk_f.GetPhysicalDeviceFormatProperties2KHR(data.vkphys_dev, img_info.format, &format_props);

#ifndef NDEBUG
        hlog("Available modifiers:");
#endif
        for (uint32_t i = 0; i < modifier_props_list.drmFormatModifierCount; i++) {
            if (linear && modifier_props[i].drmFormatModifier != DRM_FORMAT_MOD_LINEAR) {
                continue;
            }
            VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_info = {};
            mod_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
            mod_info.drmFormatModifier = modifier_props[i].drmFormatModifier;
            mod_info.sharingMode = img_info.sharingMode;

            VkPhysicalDeviceImageFormatInfo2 format_info = {};
            format_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
            format_info.pNext = &mod_info;
            format_info.format = img_info.format;
            format_info.type = VK_IMAGE_TYPE_2D;
            format_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
            format_info.usage = img_info.usage;
            format_info.flags = img_info.flags;

            VkImageFormatProperties2KHR format_props = {};
            format_props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;

            VkResult result = vk_f.GetPhysicalDeviceImageFormatProperties2KHR(data.vkphys_dev, &format_info, &format_props);
            if (result == VK_SUCCESS) {
#ifndef NDEBUG
                hlog(" %d: modifier:%"PRIu64" planes:%d", i,
                        modifier_props[i].drmFormatModifier,
                        modifier_props[i].drmFormatModifierPlaneCount);
#endif
                modifier_props[modifier_prop_count++] = modifier_props[i];
            }
        }

        if (modifier_prop_count > 0) {
            image_modifiers = malloc(sizeof(uint64_t) * modifier_prop_count);
            for (uint32_t i = 0; i < modifier_prop_count; ++i) {
                image_modifiers[i] = modifier_props[i].drmFormatModifier;
            }

            image_modifier_list.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
            image_modifier_list.drmFormatModifierCount = modifier_prop_count;
            image_modifier_list.pDrmFormatModifiers = image_modifiers;
            img_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
            ext_mem_image_info.pNext = &image_modifier_list;
        } else {
            hlog("No suitable DRM modifier found!");
        }
    }

    VkResult res = vk_f.CreateImage(data.vkdev, &img_info, NULL, &data.vkimage);
    free(image_modifiers);
    if (res != VK_SUCCESS) {
        hlog("Vulkan: Failed to create image %s", result_to_str(res));
        return false;
    }

    VkImageMemoryRequirementsInfo2 memri = {};
    memri.image = data.vkimage;
    memri.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;

    VkMemoryDedicatedRequirements mdr = {};
    mdr.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;

    VkMemoryRequirements2 memr = {};
    memr.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    memr.pNext = &mdr;

    vk_f.GetImageMemoryRequirements2KHR(data.vkdev, &memri, &memr);

    /* -------------------------------------------------------- */
    /* get memory type index                                    */

    VkPhysicalDeviceMemoryProperties pdmp;
    vk_f.GetPhysicalDeviceMemoryProperties(data.vkphys_dev, &pdmp);

    VkExportMemoryAllocateInfo memory_export_info = {};
    memory_export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    memory_export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT | VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;

    VkMemoryDedicatedAllocateInfo memory_dedicated_info = {};
    memory_dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    memory_dedicated_info.pNext = &memory_export_info;
    memory_dedicated_info.image = data.vkimage;

    VkMemoryAllocateInfo memi = {};
    memi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memi.pNext = &memory_dedicated_info;
    memi.allocationSize = memr.memoryRequirements.size;

    bool allocated = false;
    uint32_t mem_req_bits = same_device ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    if (map_host) {
        mem_req_bits = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    }
    for (uint32_t i = 0; i < pdmp.memoryTypeCount; ++i) {
        if ((memr.memoryRequirements.memoryTypeBits & (1 << i)) &&
                (pdmp.memoryTypes[i].propertyFlags &
                 mem_req_bits) == mem_req_bits) {
            memi.memoryTypeIndex = i;
            res = vk_f.AllocateMemory(data.vkdev, &memi, NULL, &data.vkmemory);
            allocated = res == VK_SUCCESS;
            if (allocated)
                break;
            hlog("Vulkan: AllocateMemory failed (DEVICE_LOCAL): %s", result_to_str(res));
        }
    }
    if (!allocated && !map_host) {
        /* Try again without DEVICE_LOCAL */
        for (uint32_t i = 0; i < pdmp.memoryTypeCount; ++i) {
            if ((memr.memoryRequirements.memoryTypeBits & (1 << i)) &&
                    (pdmp.memoryTypes[i].propertyFlags &
                     mem_req_bits) != mem_req_bits) {
                memi.memoryTypeIndex = i;
                res = vk_f.AllocateMemory(data.vkdev, &memi, NULL, &data.vkmemory);
                allocated = res == VK_SUCCESS;
                if (allocated)
                    break;
                hlog("Vulkan: AllocateMemory failed (not DEVICE_LOCAL) %s", result_to_str(res));
            }
        }
    }

    if (!allocated) {
        hlog("Failed to allocate memory of any type");
        return false;
    }

    VkBindImageMemoryInfo bimi = {};
    bimi.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
    bimi.image = data.vkimage;
    bimi.memory = data.vkmemory;
    bimi.memoryOffset = 0;
    res = vk_f.BindImageMemory2KHR(data.vkdev, 1, &bimi);
    if (res != VK_SUCCESS) {
        hlog("Vulkan: BindImageMemory2KHR failed %s", result_to_str(res));
        return false;
    }

    VkMemoryGetFdInfoKHR memFdInfo = {};
    memFdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    memFdInfo.memory = data.vkmemory;
    memFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;
    int fd = -1;
    res = vk_f.GetMemoryFdKHR(data.vkdev, &memFdInfo, &fd);
    if (res != VK_SUCCESS) {
        hlog("Vulkan: GetMemoryFdKHR opaque_fd failed %s", result_to_str(res));
        return false;
    }

    // Reset error
    while (gl_f.GetError() != GL_NO_ERROR) { }

    GLuint glmem;
    gl_f.CreateMemoryObjectsEXT(1, &glmem);
    GLint dedicated = GL_TRUE;
    gl_f.MemoryObjectParameterivEXT(glmem, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    gl_f.ImportMemoryFdEXT(glmem, memi.allocationSize, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);

    gl_f.GenTextures(1, &data.texture);
    gl_f.BindTexture(GL_TEXTURE_2D, data.texture);
    gl_f.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, img_info.tiling == VK_IMAGE_TILING_LINEAR || linear ? GL_LINEAR_TILING_EXT : GL_OPTIMAL_TILING_EXT);
    gl_f.TexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, data.width, data.height, glmem, 0);
    gl_f.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl_f.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    if (!gl_f.IsMemoryObjectEXT(glmem) || gl_f.GetError() != GL_NO_ERROR) {
        hlog("Vulkan: OpenGL import failed");
        return false;
    }

    memFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int dmabuf_fd = -1;
    res = vk_f.GetMemoryFdKHR(data.vkdev, &memFdInfo, &dmabuf_fd);
    if (res != VK_SUCCESS) {
        hlog("Vulkan: GetMemoryFdKHR dma_buf failed %s", result_to_str(res));
        return false;
    }

    if (!no_modifiers && vk_f.GetImageDrmFormatModifierPropertiesEXT) {
        VkImageDrmFormatModifierPropertiesEXT image_mod_props = {};
        image_mod_props.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
        res = vk_f.GetImageDrmFormatModifierPropertiesEXT(data.vkdev, data.vkimage, &image_mod_props);
        if (VK_SUCCESS != res) {
            hlog("GetImageDrmFormatModifierPropertiesEXT failed %s", result_to_str(res));
            data.buf_modifier = DRM_FORMAT_MOD_INVALID;
        } else {
            data.buf_modifier = image_mod_props.drmFormatModifier;
            for (uint32_t i = 0; i < modifier_prop_count; ++i) {
                if (modifier_props[i].drmFormatModifier == data.buf_modifier) {
                    num_planes = modifier_props[i].drmFormatModifierPlaneCount;
                    break;
                }
            }
        }
        free(modifier_props);
    } else {
        data.buf_modifier = DRM_FORMAT_MOD_INVALID;
    }

    for (int i = 0; i < num_planes; i++) {
        VkImageSubresource sbr = {};
        if (!no_modifiers && vk_f.GetImageDrmFormatModifierPropertiesEXT) {
            sbr.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << i;
        } else {
            sbr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        }
        sbr.mipLevel = 0;
        sbr.arrayLayer = 0;
        VkSubresourceLayout layout;
        vk_f.GetImageSubresourceLayout(data.vkdev, data.vkimage, &sbr, &layout);

        data.buf_fds[i] = i == 0 ? dmabuf_fd : os_dupfd_cloexec(dmabuf_fd);
        data.buf_strides[i] = layout.rowPitch;
        data.buf_offsets[i] = layout.offset;
    }
    data.nfd = num_planes;
    data.buf_fourcc = DRM_FORMAT_ABGR8888;

#ifndef NDEBUG
    hlog("Got planes %d fd %d", data.nfd, data.buf_fds[0]);
    if (data.buf_modifier != DRM_FORMAT_MOD_INVALID) {
        hlog("Got modifier %"PRIu64, data.buf_modifier);
    }
#endif

    return true;
}

static void querySurface(int *width, int *height)
{
    if (data.glx) {
        unsigned w, h;
        glx_f.QueryDrawable(data.display, data.surface, P_GLX_WIDTH, &w);
        glx_f.QueryDrawable(data.display, data.surface, P_GLX_HEIGHT, &h);
        *width = w;
        *height = h;
    } else {
        egl_f.QuerySurface(data.display, data.surface, P_EGL_WIDTH, width);
        egl_f.QuerySurface(data.display, data.surface, P_EGL_HEIGHT, height);
    }
}

static void gl_free()
{
    const bool was_capturing = data.nfd;

    if (data.nfd) {
        for (int i = 0; i < data.nfd; ++i) {
            close(data.buf_fds[i]);
            data.buf_fds[i] = -1;
        }
        data.nfd = 0;
    }

    if (data.image) {
        egl_f.DestroyImage(data.display, data.image);
        data.image = NULL;
    }

    if (data.xpixmap) {
        x11_f.XFreePixmap(data.display, data.xpixmap);
        data.xpixmap = 0;
    }

    if (data.glxpixmap) {
        glx_f.DestroyPixmap(data.display, data.glxpixmap);
        data.glxpixmap = NULL;
    }

    if (data.fbo) {
        gl_f.DeleteFramebuffers(1, &data.fbo);
        data.fbo = 0;
    }

    if (data.texture) {
        gl_f.DeleteTextures(1, &data.texture);
        data.texture = 0;
    }

    if (data.vkimage) {
        vk_f.DestroyImage(data.vkdev, data.vkimage, NULL);
        data.vkimage = VK_NULL_HANDLE;
    }

    if (data.vkmemory) {
        vk_f.FreeMemory(data.vkdev, data.vkmemory, NULL);
        data.vkmemory = VK_NULL_HANDLE;
    }

    capture_stop();

    if (was_capturing) {
        hlog("------------------- opengl capture freed -------------------");
    }
}

static void gl_copy_backbuffer(GLuint dst)
{
    gl_f.Disable(GL_FRAMEBUFFER_SRGB);
    gl_f.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl_f.BindFramebuffer(GL_DRAW_FRAMEBUFFER, data.fbo);
    gl_f.BindTexture(GL_TEXTURE_2D, dst);
    gl_f.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
    gl_f.ReadBuffer(GL_BACK);
    gl_f.DrawBuffer(GL_COLOR_ATTACHMENT0);
    gl_f.BlitFramebuffer(0, 0, data.width, data.height, 0, 0, data.width, data.height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

static void gl_shtex_capture()
{
    GLboolean last_srgb;
    GLint last_read_fbo;
    GLint last_draw_fbo;
    GLint last_tex;

    last_srgb = gl_f.IsEnabled(GL_FRAMEBUFFER_SRGB);
    gl_f.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &last_read_fbo);
    gl_f.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &last_draw_fbo);
    gl_f.GetIntegerv(GL_TEXTURE_BINDING_2D, &last_tex);

    gl_copy_backbuffer(data.texture);

    gl_f.BindTexture(GL_TEXTURE_2D, last_tex);
    gl_f.BindFramebuffer(GL_DRAW_FRAMEBUFFER, last_draw_fbo);
    gl_f.BindFramebuffer(GL_READ_FRAMEBUFFER, last_read_fbo);
    if (last_srgb) {
        gl_f.Enable(GL_FRAMEBUFFER_SRGB);
    } else {
        gl_f.Disable(GL_FRAMEBUFFER_SRGB);
    }
}

static bool gl_shtex_init()
{
    if (vkcapture_glvulkan) {
        return false;
    }

    if (data.glx) {
        // GLX on NVIDIA is all kinds of broken...
        const char *vendor = (const char*)gl_f.GetString(GL_VENDOR);
        if (strcmp(vendor, "NVIDIA Corporation") == 0) {
            return false;
        }
    }

    gl_f.GenFramebuffers(1, &data.fbo);
    if (data.fbo == 0) {
        hlog("Failed to initialize FBO");
        return false;
    }

    hlog("Texture %s %ux%u", "GL_RGBA", data.width, data.height);

    gl_f.GenTextures(1, &data.texture);
    gl_f.BindTexture(GL_TEXTURE_2D, data.texture);
    gl_f.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, data.width, data.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gl_f.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl_f.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    if (data.glx) {
        unsigned long root = P_DefaultRootWindow(data.display);
        data.xpixmap = x11_f.XCreatePixmap(data.display, root, data.width, data.height, 24);

        const int pixmap_config[] = {
            P_GLX_BIND_TO_TEXTURE_RGBA_EXT, true,
            P_GLX_DRAWABLE_TYPE, P_GLX_PIXMAP_BIT,
            P_GLX_BIND_TO_TEXTURE_TARGETS_EXT, P_GLX_TEXTURE_2D_BIT_EXT,
            P_GLX_DOUBLEBUFFER, false,
            P_GLX_RED_SIZE, 8,
            P_GLX_GREEN_SIZE, 8,
            P_GLX_BLUE_SIZE, 8,
            P_GLX_ALPHA_SIZE, 8,
            0
        };
        int nelements;
        void **fbc = glx_f.ChooseFBConfig(data.display, P_DefaultScreen(data.display), pixmap_config, &nelements);
        if (nelements <= 0) {
            hlog("Failed to choose FBConfig");
            goto fail;
        }

        const int pixmapAttribs[] = {
            P_GLX_TEXTURE_TARGET_EXT, P_GLX_TEXTURE_2D_EXT,
            P_GLX_TEXTURE_FORMAT_EXT, P_GLX_TEXTURE_FORMAT_RGBA_EXT,
            P_GLX_MIPMAP_TEXTURE_EXT, false,
            0
        };
        data.glxpixmap = glx_f.CreatePixmap(data.display, fbc[0], data.xpixmap, pixmapAttribs);
        x11_f.XFree(fbc);

        glx_f.BindTexImageEXT(data.display, data.glxpixmap, P_GLX_FRONT_LEFT_EXT, NULL);

        void *xcb_con = x11_f.XGetXCBConnection(data.display);
        P_xcb_dri3_buffers_from_pixmap_cookie_t cookie = x11_f.xcb_dri3_buffers_from_pixmap(xcb_con, data.xpixmap);
        P_xcb_dri3_buffers_from_pixmap_reply_t *reply = x11_f.xcb_dri3_buffers_from_pixmap_reply(xcb_con, cookie, NULL);
        if (!reply) {
            hlog("Failed to get buffer from pixmap");
            goto fail;
        }
        data.nfd = reply->nfd;
        for (uint8_t i = 0; i < reply->nfd; ++i) {
            data.buf_fds[i] = x11_f.xcb_dri3_buffers_from_pixmap_reply_fds(xcb_con, reply)[i];
            data.buf_strides[i] = x11_f.xcb_dri3_buffers_from_pixmap_strides(reply)[i];
            data.buf_offsets[i] = x11_f.xcb_dri3_buffers_from_pixmap_offsets(reply)[i];
        }
        data.buf_fourcc = DRM_FORMAT_ARGB8888;
        data.buf_modifier = reply->modifier;
        free(reply);
    } else {
        data.image = egl_f.CreateImage(data.display, egl_f.GetCurrentContext(), P_EGL_GL_TEXTURE_2D, data.texture, NULL);
        if (!data.image) {
            hlog("Failed to create EGL image");
            goto fail;
        }
        const int queried = egl_f.ExportDMABUFImageQueryMESA(data.display, data.image, &data.buf_fourcc, &data.nfd, &data.buf_modifier);
        if (!queried) {
            hlog("Failed to query dmabuf export");
            goto fail;
        }
        const int exported = egl_f.ExportDMABUFImageMESA(data.display, data.image, data.buf_fds, data.buf_strides, data.buf_offsets);
        if (!exported) {
            hlog("Failed dmabuf export");
            goto fail;
        }
    }

    return true;

fail:
    data.nfd = 0;
    if (data.fbo) {
        gl_f.DeleteFramebuffers(1, &data.fbo);
        data.fbo = 0;
    }
    if (data.xpixmap) {
        x11_f.XFreePixmap(data.display, data.xpixmap);
        data.xpixmap = 0;
    }
    if (data.glxpixmap) {
        glx_f.DestroyPixmap(data.display, data.glxpixmap);
        data.glxpixmap = NULL;
    }
    if (data.image) {
        egl_f.DestroyImage(data.display, data.image);
        data.image = NULL;
    }
    return false;
}

static bool gl_init(void *display, void *surface)
{
    data.display = display;
    data.surface = surface;
    querySurface(&data.width, &data.height);

    if (data.glx) {
        data.winid = (uintptr_t)surface;
    }

    GLint last_tex;
    gl_f.GetIntegerv(GL_TEXTURE_BINDING_2D, &last_tex);

    bool init = gl_shtex_init();
    if (!init) {
        init = vulkan_shtex_init();
    }

    gl_f.BindTexture(GL_TEXTURE_2D, last_tex);

    if (!init) {
        hlog("shtex init failed");
        return false;
    }

    capture_init_shtex(data.width, data.height, data.buf_fourcc,
            data.buf_strides, data.buf_offsets, data.buf_modifier,
            data.winid, /*flip*/true, 0, data.nfd, data.buf_fds);

    hlog("------------------ opengl capture started ------------------");

    return true;
}

static void gl_capture(void *display, void *surface)
{
    capture_update_socket();

    if (capture_should_stop()) {
        gl_free();
    }

    if (capture_should_init()) {
        if (!gl_init(display, surface)) {
            gl_free();
            data.valid = false;
            hlog("gl_init failed");
        }
    }

    if (capture_ready() && data.surface == surface) {
        int width, height;
        querySurface(&width, &height);
        if (data.height != height || data.width != width) {
            if (width != 0 && height != 0) {
                gl_free();
            }
            return;
        }
        gl_shtex_capture();
    }
}

/* ======================================================================== */

void *eglGetProcAddress(const char *procName);
unsigned eglDestroyContext(void *display, void *context);
unsigned eglSwapBuffers(void *display, void *surface);
void *eglCreateWindowSurface(void *display, void *config, void *win, const intptr_t *attrib_list);

static struct {
    void *func;
    const char *name;
} egl_hooks_map[] = {
#define ADD_HOOK(fn) { (void*)fn, #fn }
    ADD_HOOK(eglGetProcAddress),
    ADD_HOOK(eglSwapBuffers),
    ADD_HOOK(eglDestroyContext),
    ADD_HOOK(eglCreateWindowSurface)
#undef ADD_HOOK
};

void *obs_vkcapture_eglGetProcAddress(const char *name)
{
    for (int i = 0; i < sizeof(egl_hooks_map) / sizeof(egl_hooks_map[0]); ++i) {
        if (strcmp(name, egl_hooks_map[i].name) == 0) {
            return egl_hooks_map[i].func;
        }
    }
    return NULL;
}

void *eglGetProcAddress(const char *procName)
{
    if (!gl_init_funcs(/*glx*/false)) {
        return NULL;
    }
    void *func = obs_vkcapture_eglGetProcAddress(procName);
    return func ? func : egl_f.GetProcAddress(procName);
}

unsigned eglDestroyContext(void *display, void *context)
{
    if (!gl_init_funcs(/*glx*/false)) {
        return 0;
    }

    gl_free();

    return egl_f.DestroyContext(display, context);
}

unsigned eglSwapBuffers(void *display, void *surface)
{
    if (!gl_init_funcs(/*glx*/false)) {
        return 0;
    }

    if (data.valid) {
        gl_capture(display, surface);
    }

    return egl_f.SwapBuffers(display, surface);
}

void *eglCreateWindowSurface(void *display, void *config, void *win, const intptr_t *attrib_list)
{
    if (!gl_init_funcs(/*glx*/false)) {
        return 0;
    }

    void *res = egl_f.CreateWindowSurface(display, config, win, attrib_list);
    if (res) {
        data.winid = (uintptr_t)win;
    }

    return res;
}

/* ======================================================================== */

void *glXGetProcAddress(const char *procName);
void *glXGetProcAddressARB(const char *procName);
void glXDestroyContext(void *display, void *context);
void glXSwapBuffers(void *display, void *surface);
int64_t glXSwapBuffersMscOML(void *display, void *drawable, int64_t target_msc, int64_t divisor, int64_t remainder);

static struct {
    void *func;
    const char *name;
} glx_hooks_map[] = {
#define ADD_HOOK(fn) { (void*)fn, #fn }
    ADD_HOOK(glXGetProcAddress),
    ADD_HOOK(glXGetProcAddressARB),
    ADD_HOOK(glXSwapBuffers),
    ADD_HOOK(glXSwapBuffersMscOML),
    ADD_HOOK(glXDestroyContext)
#undef ADD_HOOK
};

void *obs_vkcapture_glXGetProcAddress(const char *name)
{
    for (int i = 0; i < sizeof(glx_hooks_map) / sizeof(glx_hooks_map[0]); ++i) {
        if (strcmp(name, glx_hooks_map[i].name) == 0) {
            return glx_hooks_map[i].func;
        }
    }
    return NULL;
}

void *glXGetProcAddress(const char *procName)
{
    if (!gl_init_funcs(/*glx*/true)) {
        return NULL;
    }
    void *func = obs_vkcapture_glXGetProcAddress(procName);
    return func ? func : glx_f.GetProcAddress(procName);
}

void *glXGetProcAddressARB(const char *procName)
{
    if (!gl_init_funcs(/*glx*/true)) {
        return NULL;
    }
    void *func = obs_vkcapture_glXGetProcAddress(procName);
    return func ? func : glx_f.GetProcAddressARB(procName);
}

void glXDestroyContext(void *display, void *context)
{
    if (!gl_init_funcs(/*glx*/true)) {
        return;
    }

    gl_free();

    glx_f.DestroyContext(display, context);
}

void glXSwapBuffers(void *display, void *drawable)
{
    if (!gl_init_funcs(/*glx*/true)) {
        return;
    }

    if (data.valid) {
        gl_capture(display, drawable);
    }

    glx_f.SwapBuffers(display, drawable);
}

int64_t glXSwapBuffersMscOML(void *display, void *drawable, int64_t target_msc, int64_t divisor, int64_t remainder)
{
    if (!gl_init_funcs(/*glx*/true)) {
        return 0;
    }

    if (data.valid) {
        gl_capture(display, drawable);
    }

    return glx_f.SwapBuffersMscOML(display, drawable, target_msc, divisor, remainder);
}
