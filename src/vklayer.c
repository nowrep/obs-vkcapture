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

#include "vklayer.h"
#include "capture.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <inttypes.h>
#include <vulkan/vk_layer.h>

// Based on obs-studio/plugins/win-capture/graphics-hook/vulkan-capture.c

/* ======================================================================== */
/* defs/statics                                                             */

/* use the loader's dispatch table pointer as a key for internal data maps */
#define GET_LDT(x) (*(void **)x)

/* #define DEBUG_EXTRA 1 */

#define MAX_PRESENT_SWAP_SEMAPHORE_COUNT 32
static VkPipelineStageFlagBits semaphore_dst_stage_masks[MAX_PRESENT_SWAP_SEMAPHORE_COUNT];

static bool vulkan_seen = false;

static bool vkcapture_linear = false;

/* ======================================================================== */
/* hook data                                                                */

struct vk_obj_node {
    uint64_t obj;
    struct vk_obj_node *next;
};

struct vk_obj_list {
    struct vk_obj_node *root;
    pthread_mutex_t mutex;
};

struct vk_swap_data {
    struct vk_obj_node node;

    VkExtent2D image_extent;
    VkFormat format;
    VkColorSpaceKHR color_space;
    uint64_t winid;
    VkImage export_image;
    VkFormat export_format;
    VkDeviceMemory export_mem;
    VkImage *swap_images;
    uint32_t image_count;

    int dmabuf_nfd;
    int dmabuf_fds[4];
    int dmabuf_strides[4];
    int dmabuf_offsets[4];
    uint64_t dmabuf_modifier;
    bool captured;
};

struct vk_queue_data {
    struct vk_obj_node node;

    uint32_t fam_idx;
    bool supports_transfer;
    struct vk_frame_data *frames;
    uint32_t frame_index;
    uint32_t frame_count;
};

struct vk_frame_data {
    VkCommandPool cmd_pool;
    VkCommandBuffer cmd_buffer;
    VkFence fence;
    VkSemaphore semaphore;
    bool cmd_buffer_busy;
};

struct vk_surf_data {
    struct vk_obj_node node;

    uint64_t winid;
};

struct vk_inst_data {
    struct vk_obj_node node;

    VkInstance instance;

    bool valid;

    struct vk_inst_funcs funcs;
    struct vk_obj_list surfaces;
};

struct vk_data {
    struct vk_obj_node node;

    VkDevice device;
    VkDriverId driver_id;
    uint8_t device_uuid[16];

    bool valid;

    struct vk_device_funcs funcs;
    VkPhysicalDevice phy_device;
    struct vk_obj_list swaps;
    struct vk_swap_data *cur_swap;

    struct vk_obj_list queues;
    VkQueue graphics_queue;

    VkExternalMemoryProperties external_mem_props;

    struct vk_inst_data *inst_data;

    VkAllocationCallbacks ac_storage;
    const VkAllocationCallbacks *ac;
};

/* ------------------------------------------------------------------------- */

static void *vk_alloc(const VkAllocationCallbacks *ac, size_t size,
        size_t alignment, enum VkSystemAllocationScope scope)
{
    return ac ? ac->pfnAllocation(ac->pUserData, size, alignment, scope)
        : malloc(size);
}

static void vk_free(const VkAllocationCallbacks *ac, void *memory)
{
    if (ac)
        ac->pfnFree(ac->pUserData, memory);
    else
        free(memory);
}

static void add_obj_data(struct vk_obj_list *list, uint64_t obj, void *data)
{
    pthread_mutex_lock(&list->mutex);

    struct vk_obj_node *const node = (struct vk_obj_node*)data;
    node->obj = obj;
    node->next = list->root;
    list->root = node;

    pthread_mutex_unlock(&list->mutex);
}

static struct vk_obj_node *get_obj_data(struct vk_obj_list *list, uint64_t obj)
{
    struct vk_obj_node *data = NULL;

    pthread_mutex_lock(&list->mutex);

    struct vk_obj_node *node = list->root;
    while (node) {
        if (node->obj == obj) {
            data = node;
            break;
        }

        node = node->next;
    }

    pthread_mutex_unlock(&list->mutex);

    return data;
}

static struct vk_obj_node *remove_obj_data(struct vk_obj_list *list,
        uint64_t obj)
{
    struct vk_obj_node *data = NULL;

    pthread_mutex_lock(&list->mutex);

    struct vk_obj_node *prev = NULL;
    struct vk_obj_node *node = list->root;
    while (node) {
        if (node->obj == obj) {
            data = node;
            if (prev)
                prev->next = node->next;
            else
                list->root = node->next;
            break;
        }

        prev = node;
        node = node->next;
    }

    pthread_mutex_unlock(&list->mutex);

    return data;
}

static void init_obj_list(struct vk_obj_list *list)
{
    list->root = NULL;
    pthread_mutex_init(&list->mutex, NULL);
}

static struct vk_obj_node *obj_walk_begin(struct vk_obj_list *list)
{
    pthread_mutex_lock(&list->mutex);
    return list->root;
}

static struct vk_obj_node *obj_walk_next(struct vk_obj_node *node)
{
    return node->next;
}

static void obj_walk_end(struct vk_obj_list *list)
{
    pthread_mutex_unlock(&list->mutex);
}

/* ------------------------------------------------------------------------- */

static struct vk_obj_list devices;

static struct vk_data *alloc_device_data(const VkAllocationCallbacks *ac)
{
    struct vk_data *data = vk_alloc(ac, sizeof(struct vk_data),
            _Alignof(struct vk_data),
            VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    return data;
}

static void init_device_data(struct vk_data *data, VkDevice device)
{
    add_obj_data(&devices, (uintptr_t)GET_LDT(device), data);
    data->device = device;
}

static struct vk_data *get_device_data(VkDevice device)
{
    return (struct vk_data *)get_obj_data(&devices,
            (uintptr_t)GET_LDT(device));
}

static struct vk_data *get_device_data_by_queue(VkQueue queue)
{
    return (struct vk_data *)get_obj_data(&devices,
            (uintptr_t)GET_LDT(queue));
}

static struct vk_data *remove_device_data(VkDevice device)
{
    return (struct vk_data *)remove_obj_data(&devices,
            (uintptr_t)GET_LDT(device));
}

/* ------------------------------------------------------------------------- */

static struct vk_queue_data *add_queue_data(struct vk_data *data, VkQueue queue,
        uint32_t fam_idx,
        bool supports_transfer,
        bool supports_graphics,
        const VkAllocationCallbacks *ac)
{
    struct vk_queue_data *const queue_data =
        vk_alloc(ac, sizeof(struct vk_queue_data),
                _Alignof(struct vk_queue_data),
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    add_obj_data(&data->queues, (uintptr_t)queue, queue_data);
    queue_data->fam_idx = fam_idx;
    queue_data->supports_transfer = supports_transfer;
    queue_data->frames = NULL;
    queue_data->frame_index = 0;
    queue_data->frame_count = 0;
    if (supports_graphics)
        data->graphics_queue = queue;
    return queue_data;
}

static struct vk_queue_data *get_queue_data(struct vk_data *data, VkQueue queue)
{
    return (struct vk_queue_data *)get_obj_data(&data->queues,
            (uintptr_t)queue);
}

static void remove_free_queue_all(struct vk_data *data,
        const VkAllocationCallbacks *ac)
{
    struct vk_queue_data *queue_data =
        (struct vk_queue_data *)data->queues.root;
    while (data->queues.root) {
        remove_obj_data(&data->queues, queue_data->node.obj);
        vk_free(ac, queue_data);

        queue_data = (struct vk_queue_data *)data->queues.root;
    }
}

static struct vk_queue_data *queue_walk_begin(struct vk_data *data)
{
    return (struct vk_queue_data *)obj_walk_begin(&data->queues);
}

static struct vk_queue_data *queue_walk_next(struct vk_queue_data *queue_data)
{
    return (struct vk_queue_data *)obj_walk_next(
            (struct vk_obj_node *)queue_data);
}

static void queue_walk_end(struct vk_data *data)
{
    obj_walk_end(&data->queues);
}

/* ------------------------------------------------------------------------- */

static struct vk_swap_data *alloc_swap_data(const VkAllocationCallbacks *ac)
{
    struct vk_swap_data *const swap_data = vk_alloc(
            ac, sizeof(struct vk_swap_data), _Alignof(struct vk_swap_data),
            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    return swap_data;
}

static void init_swap_data(struct vk_swap_data *swap_data, struct vk_data *data,
        VkSwapchainKHR sc)
{
    add_obj_data(&data->swaps, (uint64_t)sc, swap_data);
}

static struct vk_swap_data *get_swap_data(struct vk_data *data,
        VkSwapchainKHR sc)
{
    return (struct vk_swap_data *)get_obj_data(&data->swaps, (uint64_t)sc);
}

static void remove_free_swap_data(struct vk_data *data, VkSwapchainKHR sc,
        const VkAllocationCallbacks *ac)
{
    struct vk_swap_data *const swap_data =
        (struct vk_swap_data *)remove_obj_data(&data->swaps,
                (uint64_t)sc);
    vk_free(ac, swap_data);
}

static struct vk_swap_data *swap_walk_begin(struct vk_data *data)
{
    return (struct vk_swap_data *)obj_walk_begin(&data->swaps);
}

static struct vk_swap_data *swap_walk_next(struct vk_swap_data *swap_data)
{
    return (struct vk_swap_data *)obj_walk_next(
            (struct vk_obj_node *)swap_data);
}

static void swap_walk_end(struct vk_data *data)
{
    obj_walk_end(&data->swaps);
}

/* ------------------------------------------------------------------------- */

static void vk_shtex_clear_fence(const struct vk_data *data,
        struct vk_frame_data *frame_data)
{
    const VkFence fence = frame_data->fence;
    if (frame_data->cmd_buffer_busy) {
        VkDevice device = data->device;
        const struct vk_device_funcs *funcs = &data->funcs;
        funcs->WaitForFences(device, 1, &fence, VK_TRUE, ~0ull);
        funcs->ResetFences(device, 1, &fence);
        frame_data->cmd_buffer_busy = false;
    }
}

static void vk_shtex_wait_until_pool_idle(struct vk_data *data,
        struct vk_queue_data *queue_data)
{
    for (uint32_t frame_idx = 0; frame_idx < queue_data->frame_count;
            frame_idx++) {
        struct vk_frame_data *frame_data =
            &queue_data->frames[frame_idx];
        if (frame_data->cmd_pool != VK_NULL_HANDLE)
            vk_shtex_clear_fence(data, frame_data);
    }
}

static void vk_shtex_wait_until_idle(struct vk_data *data)
{
    struct vk_queue_data *queue_data = queue_walk_begin(data);

    while (queue_data) {
        vk_shtex_wait_until_pool_idle(data, queue_data);

        queue_data = queue_walk_next(queue_data);
    }

    queue_walk_end(data);
}

static void vk_shtex_free(struct vk_data *data)
{
    vk_shtex_wait_until_idle(data);

    struct vk_swap_data *swap = swap_walk_begin(data);

    while (swap) {
        VkDevice device = data->device;
        if (swap->export_image)
            data->funcs.DestroyImage(device, swap->export_image,
                    data->ac);

        swap->dmabuf_nfd = 0;
        for (int i = 0; i < 4; ++i) {
            if (swap->dmabuf_fds[i] >= 0) {
                close(swap->dmabuf_fds[i]);
                swap->dmabuf_fds[i] = -1;
            }
        }

        if (swap->export_mem)
            data->funcs.FreeMemory(device, swap->export_mem, NULL);

        swap->export_mem = VK_NULL_HANDLE;
        swap->export_image = VK_NULL_HANDLE;

        swap->captured = false;

        swap = swap_walk_next(swap);
    }

    swap_walk_end(data);

    data->cur_swap = NULL;
    capture_stop();

    hlog("------------------- vulkan capture freed -------------------");
}

/* ------------------------------------------------------------------------- */

static void add_surf_data(struct vk_inst_data *idata, VkSurfaceKHR surf,
        uint64_t winid, const VkAllocationCallbacks *ac)
{
    struct vk_surf_data *surf_data = vk_alloc(
            ac, sizeof(struct vk_surf_data), _Alignof(struct vk_surf_data),
            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (surf_data) {
        surf_data->winid = winid;

        add_obj_data(&idata->surfaces, (uint64_t)surf, surf_data);
    }
}

static uint64_t find_surf_winid(struct vk_inst_data *idata, VkSurfaceKHR surf)
{
    struct vk_surf_data *surf_data = (struct vk_surf_data *)get_obj_data(
            &idata->surfaces, (uint64_t)surf);
    return surf_data ? surf_data->winid : 0;
}

static void remove_free_surf_data(struct vk_inst_data *idata, VkSurfaceKHR surf,
        const VkAllocationCallbacks *ac)
{
    struct vk_surf_data *surf_data = (struct vk_surf_data *)remove_obj_data(
            &idata->surfaces, (uint64_t)surf);
    vk_free(ac, surf_data);
}

/* ------------------------------------------------------------------------- */

static struct vk_obj_list instances;

static struct vk_inst_data *alloc_inst_data(const VkAllocationCallbacks *ac)
{
    struct vk_inst_data *idata = vk_alloc(
            ac, sizeof(struct vk_inst_data), _Alignof(struct vk_inst_data),
            VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
    return idata;
}

static void init_inst_data(struct vk_inst_data *idata, VkInstance instance)
{
    add_obj_data(&instances, (uintptr_t)GET_LDT(instance), idata);
    idata->instance = instance;
}

static struct vk_inst_data *get_inst_data(VkInstance instance)
{
    return (struct vk_inst_data *)get_obj_data(&instances,
            (uintptr_t)GET_LDT(instance));
}

static struct vk_inst_funcs *get_inst_funcs(VkInstance instance)
{
    struct vk_inst_data *idata =
        (struct vk_inst_data *)get_inst_data(instance);
    return &idata->funcs;
}

static struct vk_inst_data *
get_inst_data_by_physical_device(VkPhysicalDevice physicalDevice)
{
    return (struct vk_inst_data *)get_obj_data(
            &instances, (uintptr_t)GET_LDT(physicalDevice));
}

static struct vk_inst_funcs *
get_inst_funcs_by_physical_device(VkPhysicalDevice physicalDevice)
{
    struct vk_inst_data *idata =
        (struct vk_inst_data *)get_inst_data_by_physical_device(
                physicalDevice);
    return &idata->funcs;
}

static void remove_free_inst_data(VkInstance inst,
        const VkAllocationCallbacks *ac)
{
    struct vk_inst_data *idata = (struct vk_inst_data *)remove_obj_data(
            &instances, (uintptr_t)GET_LDT(inst));
    vk_free(ac, idata);
}

/* ======================================================================== */
/* capture                                                                  */

static bool allow_modifier(struct vk_data *data, uint64_t modifier)
{
    /* DCC modifiers doesn't work when importing on radeonsi with amdvlk/amdpro drivers */
    if (data->driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE || data->driver_id == VK_DRIVER_ID_AMD_PROPRIETARY) {
        return !IS_AMD_FMT_MOD(modifier) || !AMD_FMT_MOD_GET(DCC, modifier);
    }
    return true;
}

static const struct {
    int32_t drm;
    VkFormat vk;
} vk_format_table[] = {
    { DRM_FORMAT_ARGB8888, VK_FORMAT_B8G8R8A8_UNORM },
    { DRM_FORMAT_ARGB8888, VK_FORMAT_B8G8R8A8_SRGB },
    { DRM_FORMAT_XRGB8888, VK_FORMAT_B8G8R8A8_UNORM },
    { DRM_FORMAT_XRGB8888, VK_FORMAT_B8G8R8A8_SRGB },
    { DRM_FORMAT_ABGR8888, VK_FORMAT_R8G8B8A8_UNORM },
    { DRM_FORMAT_ABGR8888, VK_FORMAT_R8G8B8A8_SRGB },
    { DRM_FORMAT_XBGR8888, VK_FORMAT_R8G8B8A8_UNORM },
    { DRM_FORMAT_XBGR8888, VK_FORMAT_R8G8B8A8_SRGB },
    { DRM_FORMAT_ARGB2101010, VK_FORMAT_A2R10G10B10_UNORM_PACK32 },
    { DRM_FORMAT_XRGB2101010, VK_FORMAT_A2R10G10B10_UNORM_PACK32 },
    { DRM_FORMAT_ABGR2101010, VK_FORMAT_A2B10G10R10_UNORM_PACK32 },
    { DRM_FORMAT_XBGR2101010, VK_FORMAT_A2B10G10R10_UNORM_PACK32 },
    { DRM_FORMAT_ABGR16161616, VK_FORMAT_R16G16B16A16_UNORM },
    { DRM_FORMAT_XBGR16161616, VK_FORMAT_R16G16B16A16_UNORM },
    { DRM_FORMAT_ABGR16161616F, VK_FORMAT_R16G16B16A16_SFLOAT },
    { DRM_FORMAT_XBGR16161616F, VK_FORMAT_R16G16B16A16_SFLOAT },
};

static int32_t vk_format_to_drm(VkFormat vk)
{
    for (size_t i = 0; i < sizeof(vk_format_table) / sizeof(vk_format_table[0]); ++i) {
        if (vk_format_table[i].vk == vk) {
            return vk_format_table[i].drm;
        }
    }
    return -1;
}

static inline bool vk_shtex_init_vulkan_tex(struct vk_data *data,
        struct vk_swap_data *swap)
{
    struct vk_device_funcs *funcs = &data->funcs;
    struct vk_inst_funcs *ifuncs =
        get_inst_funcs_by_physical_device(data->phy_device);

    const bool no_modifiers = capture_allocate_no_modifiers();
    const bool linear = vkcapture_linear || capture_allocate_linear();
    const bool map_host = capture_allocate_map_host();
    const bool same_device = capture_compare_device_uuid(data->device_uuid);

    hlog("Texture %s %ux%u", vk_format_to_str(swap->format), swap->image_extent.width, swap->image_extent.height);

    if (vk_format_to_drm(swap->format) != -1) {
        swap->export_format = swap->format;
    } else {
        swap->export_format = VK_FORMAT_B8G8R8A8_UNORM;
        hlog("Converting to %s", vk_format_to_str(swap->export_format));
    }

    if (!same_device) {
        hlog("OBS is running on different GPU");
    }

    VkExternalMemoryImageCreateInfo ext_mem_image_info = {};
    ext_mem_image_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_mem_image_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo img_info = {};
    img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.pNext = &ext_mem_image_info;
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.format = swap->export_format;
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    img_info.extent.width = swap->image_extent.width;
    img_info.extent.height = swap->image_extent.height;
    img_info.extent.depth = 1;
    img_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    img_info.tiling = VK_IMAGE_TILING_LINEAR;

    int num_planes = 1;
    uint64_t *image_modifiers = NULL;
    VkImageDrmFormatModifierListCreateInfoEXT image_modifier_list = {};
    struct VkDrmFormatModifierPropertiesEXT *modifier_props = NULL;
    uint32_t modifier_prop_count = 0;

    if (!no_modifiers && funcs->GetImageDrmFormatModifierPropertiesEXT) {
        VkDrmFormatModifierPropertiesListEXT modifier_props_list = {};
        modifier_props_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;

        VkFormatProperties2KHR format_props = {};
        format_props.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
        format_props.pNext = &modifier_props_list;

        ifuncs->GetPhysicalDeviceFormatProperties2KHR(data->phy_device,
                img_info.format, &format_props);

        modifier_props =
            vk_alloc(data->ac, modifier_props_list.drmFormatModifierCount * sizeof(struct VkDrmFormatModifierPropertiesEXT),
                    _Alignof(struct VkDrmFormatModifierPropertiesEXT), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);

        modifier_props_list.pDrmFormatModifierProperties = modifier_props;

        ifuncs->GetPhysicalDeviceFormatProperties2KHR(data->phy_device,
                img_info.format, &format_props);

#ifndef NDEBUG
        hlog("Available modifiers:");
#endif
        for (uint32_t i = 0; i < modifier_props_list.drmFormatModifierCount; i++) {
            if (linear && modifier_props[i].drmFormatModifier != DRM_FORMAT_MOD_LINEAR) {
                continue;
            }
            if (!allow_modifier(data, modifier_props[i].drmFormatModifier)) {
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
            format_props.pNext = NULL;

            VkResult result = ifuncs->GetPhysicalDeviceImageFormatProperties2KHR(data->phy_device,
                    &format_info, &format_props);
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
            image_modifiers =
                vk_alloc(data->ac, sizeof(uint64_t) * modifier_prop_count,
                        _Alignof(uint64_t), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);

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

    VkDevice device = data->device;

    VkResult res;
    res = funcs->CreateImage(device, &img_info, data->ac, &swap->export_image);
    vk_free(data->ac, image_modifiers);
    if (VK_SUCCESS != res) {
        hlog("Failed to CreateImage %s", result_to_str(res));
        swap->export_image = VK_NULL_HANDLE;
        return false;
    }

    VkImageMemoryRequirementsInfo2 memri = {};
    memri.image = swap->export_image;
    memri.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;

    VkMemoryDedicatedRequirements mdr = {};
    mdr.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;

    VkMemoryRequirements2 memr = {};
    memr.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    memr.pNext = &mdr;

    funcs->GetImageMemoryRequirements2KHR(device, &memri, &memr);

    /* -------------------------------------------------------- */
    /* get memory type index                                    */

    VkPhysicalDeviceMemoryProperties pdmp;
    ifuncs->GetPhysicalDeviceMemoryProperties(data->phy_device, &pdmp);

    VkExportMemoryAllocateInfo memory_export_info = {};
    memory_export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    memory_export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkMemoryDedicatedAllocateInfo memory_dedicated_info = {};
    memory_dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    memory_dedicated_info.pNext = &memory_export_info;
    memory_dedicated_info.image = swap->export_image;

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
            res = funcs->AllocateMemory(device, &memi, NULL, &swap->export_mem);
            allocated = res == VK_SUCCESS;
            if (allocated)
                break;
            hlog("AllocateMemory failed (DEVICE_LOCAL): %s", result_to_str(res));
        }
    }
    if (!allocated && !map_host) {
        /* Try again without DEVICE_LOCAL */
        for (uint32_t i = 0; i < pdmp.memoryTypeCount; ++i) {
            if ((memr.memoryRequirements.memoryTypeBits & (1 << i)) &&
                    (pdmp.memoryTypes[i].propertyFlags &
                     mem_req_bits) != mem_req_bits) {
                memi.memoryTypeIndex = i;
                res = funcs->AllocateMemory(device, &memi, NULL, &swap->export_mem);
                allocated = res == VK_SUCCESS;
                if (allocated)
                    break;
                hlog("AllocateMemory failed (not DEVICE_LOCAL) %s", result_to_str(res));
            }
        }
    }

    if (!allocated) {
        hlog("Failed to allocate memory of any type");
        funcs->DestroyImage(device, swap->export_image, data->ac);
        swap->export_image = VK_NULL_HANDLE;
        return false;
    }

    VkBindImageMemoryInfo bimi = {};
    bimi.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
    bimi.image = swap->export_image;
    bimi.memory = swap->export_mem;
    bimi.memoryOffset = 0;
    res = funcs->BindImageMemory2KHR(device, 1, &bimi);
    if (VK_SUCCESS != res) {
        hlog("BindImageMemory2KHR failed %s", result_to_str(res));
        funcs->DestroyImage(device, swap->export_image, data->ac);
        swap->export_image = VK_NULL_HANDLE;
        return false;
    }

    int fd = -1;
    VkMemoryGetFdInfoKHR gfdi = {};
    gfdi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    gfdi.memory = swap->export_mem;
    gfdi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    res = funcs->GetMemoryFdKHR(device, &gfdi, &fd);
    if (VK_SUCCESS != res) {
        hlog("GetMemoryFdKHR failed %s", result_to_str(res));
        funcs->DestroyImage(device, swap->export_image, data->ac);
        swap->export_image = VK_NULL_HANDLE;
        return false;
    }

    if (!no_modifiers && funcs->GetImageDrmFormatModifierPropertiesEXT) {
        VkImageDrmFormatModifierPropertiesEXT image_mod_props = {};
        image_mod_props.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
        res = funcs->GetImageDrmFormatModifierPropertiesEXT(device, swap->export_image, &image_mod_props);
        if (VK_SUCCESS != res) {
            hlog("GetImageDrmFormatModifierPropertiesEXT failed %s", result_to_str(res));
            swap->dmabuf_modifier = DRM_FORMAT_MOD_INVALID;
        } else {
            swap->dmabuf_modifier = image_mod_props.drmFormatModifier;
            for (uint32_t i = 0; i < modifier_prop_count; ++i) {
                if (modifier_props[i].drmFormatModifier == swap->dmabuf_modifier) {
                    num_planes = modifier_props[i].drmFormatModifierPlaneCount;
                    break;
                }
            }
        }
        vk_free(data->ac, modifier_props);
    } else {
        swap->dmabuf_modifier = DRM_FORMAT_MOD_INVALID;
    }

    for (int i = 0; i < num_planes; i++) {
        VkImageSubresource sbr = {};
        if (!no_modifiers && funcs->GetImageDrmFormatModifierPropertiesEXT) {
            sbr.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << i;
        } else {
            sbr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        }
        sbr.mipLevel = 0;
        sbr.arrayLayer = 0;
        VkSubresourceLayout layout;
        funcs->GetImageSubresourceLayout(device, swap->export_image, &sbr, &layout);

        swap->dmabuf_fds[i] = i == 0 ? fd : os_dupfd_cloexec(fd);
        swap->dmabuf_strides[i] = layout.rowPitch;
        swap->dmabuf_offsets[i] = layout.offset;
    }
    swap->dmabuf_nfd = num_planes;

#ifndef NDEBUG
    hlog("Got planes %d fd %d", swap->dmabuf_nfd, swap->dmabuf_fds[0]);
    if (swap->dmabuf_modifier != DRM_FORMAT_MOD_INVALID) {
        hlog("Got modifier %"PRIu64, swap->dmabuf_modifier);
    }
#endif

    return true;
}

static bool vk_shtex_init(struct vk_data *data, struct vk_swap_data *swap)
{
    if (!vk_shtex_init_vulkan_tex(data, swap)) {
        return false;
    }

    data->cur_swap = swap;

    capture_init_shtex(swap->image_extent.width, swap->image_extent.height,
        vk_format_to_drm(swap->export_format),
        swap->dmabuf_strides, swap->dmabuf_offsets, swap->dmabuf_modifier,
        swap->winid, /*flip*/false, swap->color_space,
        swap->dmabuf_nfd, swap->dmabuf_fds);

    hlog("------------------ vulkan capture started ------------------");
    return true;
}

static void vk_shtex_create_frame_objects(struct vk_data *data,
        struct vk_queue_data *queue_data,
        uint32_t image_count)
{
    queue_data->frames =
        vk_alloc(data->ac, image_count * sizeof(struct vk_frame_data),
                _Alignof(struct vk_frame_data),
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    memset(queue_data->frames, 0, image_count * sizeof(struct vk_frame_data));
    queue_data->frame_index = 0;
    queue_data->frame_count = image_count;

    VkDevice device = data->device;
    for (uint32_t image_index = 0; image_index < image_count;
            image_index++) {
        struct vk_frame_data *frame_data =
            &queue_data->frames[image_index];

        VkCommandPoolCreateInfo cpci;
        cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.pNext = NULL;
        cpci.flags = 0;
        cpci.queueFamilyIndex = queue_data->fam_idx;

        VkResult res = data->funcs.CreateCommandPool(
                device, &cpci, data->ac, &frame_data->cmd_pool);
#ifdef DEBUG_EXTRA
        hlog("CreateCommandPool %s", result_to_str(res));
#endif

        VkCommandBufferAllocateInfo cbai;
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.pNext = NULL;
        cbai.commandPool = frame_data->cmd_pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;

        res = data->funcs.AllocateCommandBuffers(
                device, &cbai, &frame_data->cmd_buffer);
#ifdef DEBUG_EXTRA
        hlog("AllocateCommandBuffers %s", result_to_str(res));
#endif
        GET_LDT(frame_data->cmd_buffer) = GET_LDT(device);

        VkFenceCreateInfo fci = {};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.pNext = NULL;
        fci.flags = 0;
        res = data->funcs.CreateFence(device, &fci, data->ac,
               &frame_data->fence);
#ifdef DEBUG_EXTRA
        hlog("CreateFence %s", result_to_str(res));
#endif

        VkSemaphoreCreateInfo sci = {};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sci.pNext = NULL;
        sci.flags = 0;
        res = data->funcs.CreateSemaphore(device, &sci, data->ac, &frame_data->semaphore);

#ifdef DEBUG_EXTRA
        hlog("CreateSemaphore %s", result_to_str(res));
#endif
    }
}

static void vk_shtex_destroy_fence(struct vk_data *data, bool *cmd_buffer_busy,
        VkFence *fence)
{
    VkDevice device = data->device;

    if (*cmd_buffer_busy) {
        data->funcs.WaitForFences(device, 1, fence, VK_TRUE, ~0ull);
        *cmd_buffer_busy = false;
    }

    data->funcs.DestroyFence(device, *fence, data->ac);
    *fence = VK_NULL_HANDLE;
}

static void vk_shtex_destroy_frame_objects(struct vk_data *data,
        struct vk_queue_data *queue_data)
{
    VkDevice device = data->device;

    for (uint32_t frame_idx = 0; frame_idx < queue_data->frame_count;
            frame_idx++) {
        struct vk_frame_data *frame_data =
            &queue_data->frames[frame_idx];
        bool *cmd_buffer_busy = &frame_data->cmd_buffer_busy;
        VkFence *fence = &frame_data->fence;
        vk_shtex_destroy_fence(data, cmd_buffer_busy, fence);

        data->funcs.DestroySemaphore(device, frame_data->semaphore,
                data->ac);
        data->funcs.DestroyCommandPool(device, frame_data->cmd_pool,
                data->ac);
        frame_data->cmd_pool = VK_NULL_HANDLE;
    }

    vk_free(data->ac, queue_data->frames);
    queue_data->frames = NULL;
    queue_data->frame_count = 0;
}

static void vk_shtex_capture(struct vk_data *data,
        struct vk_device_funcs *funcs,
        struct vk_swap_data *swap, uint32_t idx,
        VkQueue queue, VkPresentInfoKHR *info)
{
    VkResult res = VK_SUCCESS;

    VkCommandBufferBeginInfo begin_info;
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.pNext = NULL;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    begin_info.pInheritanceInfo = NULL;

    VkImageMemoryBarrier mb[2];
    VkImageMemoryBarrier *src_mb = &mb[0];
    VkImageMemoryBarrier *dst_mb = &mb[1];

    /* ------------------------------------------------------ */
    /* do image copy                                          */

    const uint32_t image_index = info->pImageIndices[idx];
    VkImage cur_backbuffer = swap->swap_images[image_index];

    struct vk_queue_data *queue_data = get_queue_data(data, queue);
    uint32_t fam_idx = queue_data->fam_idx;

    const uint32_t image_count = swap->image_count;
    if (queue_data->frame_count < image_count) {
        if (queue_data->frame_count > 0)
            vk_shtex_destroy_frame_objects(data, queue_data);
        vk_shtex_create_frame_objects(data, queue_data, image_count);
    }

    const uint32_t frame_index = queue_data->frame_index;
    struct vk_frame_data *frame_data = &queue_data->frames[frame_index];
    queue_data->frame_index = (frame_index + 1) % queue_data->frame_count;
    vk_shtex_clear_fence(data, frame_data);

    VkDevice device = data->device;

    res = funcs->ResetCommandPool(device, frame_data->cmd_pool, 0);

#ifdef DEBUG_EXTRA
    hlog("ResetCommandPool %s", result_to_str(res));
#endif

    const VkCommandBuffer cmd_buffer = frame_data->cmd_buffer;
    res = funcs->BeginCommandBuffer(cmd_buffer, &begin_info);

#ifdef DEBUG_EXTRA
    hlog("BeginCommandBuffer %s", result_to_str(res));
#endif

    /* ------------------------------------------------------ */
    /* transition cur_backbuffer to transfer source state     */

    src_mb->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    src_mb->pNext = NULL;
    src_mb->srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    src_mb->dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    src_mb->oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    src_mb->newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    src_mb->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src_mb->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src_mb->image = cur_backbuffer;
    src_mb->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    src_mb->subresourceRange.baseMipLevel = 0;
    src_mb->subresourceRange.levelCount = 1;
    src_mb->subresourceRange.baseArrayLayer = 0;
    src_mb->subresourceRange.layerCount = 1;

    /* ------------------------------------------------------ */
    /* transition exportedTexture to transfer dest state      */

    dst_mb->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dst_mb->pNext = NULL;
    dst_mb->srcAccessMask = 0;
    dst_mb->dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dst_mb->oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    dst_mb->newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst_mb->srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    dst_mb->dstQueueFamilyIndex = fam_idx;
    dst_mb->image = swap->export_image;
    dst_mb->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dst_mb->subresourceRange.baseMipLevel = 0;
    dst_mb->subresourceRange.levelCount = 1;
    dst_mb->subresourceRange.baseArrayLayer = 0;
    dst_mb->subresourceRange.layerCount = 1;

    funcs->CmdPipelineBarrier(cmd_buffer,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
            NULL, 2, mb);

    /* ------------------------------------------------------ */
    /* copy cur_backbuffer's content to our interop image     */

    if (swap->format != swap->export_format) {
        VkImageBlit blt;
        blt.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blt.srcSubresource.mipLevel = 0;
        blt.srcSubresource.baseArrayLayer = 0;
        blt.srcSubresource.layerCount = 1;
        blt.srcOffsets[0].x = 0;
        blt.srcOffsets[0].y = 0;
        blt.srcOffsets[0].z = 0;
        blt.srcOffsets[1].x = swap->image_extent.width;
        blt.srcOffsets[1].y = swap->image_extent.height;
        blt.srcOffsets[1].z = 1;
        blt.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blt.dstSubresource.mipLevel = 0;
        blt.dstSubresource.baseArrayLayer = 0;
        blt.dstSubresource.layerCount = 1;
        blt.dstOffsets[0].x = 0;
        blt.dstOffsets[0].y = 0;
        blt.dstOffsets[0].z = 0;
        blt.dstOffsets[1].x = swap->image_extent.width;
        blt.dstOffsets[1].y = swap->image_extent.height;
        blt.dstOffsets[1].z = 1;
        funcs->CmdBlitImage(cmd_buffer, cur_backbuffer,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                swap->export_image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blt,
                VK_FILTER_NEAREST);
    } else {
        VkImageCopy cpy;
        cpy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cpy.srcSubresource.mipLevel = 0;
        cpy.srcSubresource.baseArrayLayer = 0;
        cpy.srcSubresource.layerCount = 1;
        cpy.srcOffset.x = 0;
        cpy.srcOffset.y = 0;
        cpy.srcOffset.z = 0;
        cpy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cpy.dstSubresource.mipLevel = 0;
        cpy.dstSubresource.baseArrayLayer = 0;
        cpy.dstSubresource.layerCount = 1;
        cpy.dstOffset.x = 0;
        cpy.dstOffset.y = 0;
        cpy.dstOffset.z = 0;
        cpy.extent.width = swap->image_extent.width;
        cpy.extent.height = swap->image_extent.height;
        cpy.extent.depth = 1;
        funcs->CmdCopyImage(cmd_buffer, cur_backbuffer,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                swap->export_image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpy);
    }

    /* ------------------------------------------------------ */
    /* Restore the swap chain image layout to what it was
     * before.  This may not be strictly needed, but it is
     * generally good to restore things to their original
     * state.  */

    src_mb->srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    src_mb->dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    src_mb->oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    src_mb->newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    dst_mb->srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dst_mb->dstAccessMask = 0;
    dst_mb->oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst_mb->newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dst_mb->srcQueueFamilyIndex = fam_idx;
    dst_mb->dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;

    funcs->CmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, NULL, 0, NULL, 2, mb);

    funcs->EndCommandBuffer(cmd_buffer);

    /* ------------------------------------------------------ */

    VkSubmitInfo submit_info;
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = NULL;
    submit_info.waitSemaphoreCount = 0;
    submit_info.pWaitSemaphores = NULL;
    submit_info.pWaitDstStageMask = NULL;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd_buffer;
    submit_info.signalSemaphoreCount = 0;
    submit_info.pSignalSemaphores = NULL;

    if (info->waitSemaphoreCount <= MAX_PRESENT_SWAP_SEMAPHORE_COUNT) {
        submit_info.waitSemaphoreCount = info->waitSemaphoreCount;
        submit_info.pWaitSemaphores = info->pWaitSemaphores;
        submit_info.pWaitDstStageMask = semaphore_dst_stage_masks;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &frame_data->semaphore;

        info->waitSemaphoreCount = 1;
        info->pWaitSemaphores = &frame_data->semaphore;
    }

    const VkFence fence = frame_data->fence;
    res = funcs->QueueSubmit(queue, 1, &submit_info, fence);

#ifdef DEBUG_EXTRA
    hlog("QueueSubmit %s", result_to_str(res));
#endif

    if (res == VK_SUCCESS)
        frame_data->cmd_buffer_busy = true;
}

static inline bool valid_rect(struct vk_swap_data *swap)
{
    return !!swap->image_extent.width && !!swap->image_extent.height &&
        (swap->image_extent.width > 1 || swap->image_extent.height > 1);
}

static void vk_capture(struct vk_data *data, VkQueue queue,
        VkPresentInfoKHR *info)
{
    // Use first swapchain ??
    struct vk_swap_data *swap = get_swap_data(data, info->pSwapchains[0]);

    capture_update_socket();

    if (capture_should_stop()) {
        vk_shtex_free(data);
    }

    if (capture_should_init()) {
        if (valid_rect(swap) && !vk_shtex_init(data, swap)) {
            vk_shtex_free(data);
            data->valid = false;
            hlog("vk_shtex_init failed");
        }
    }

    if (capture_ready()) {
        if (swap != data->cur_swap) {
            vk_shtex_free(data);
            return;
        }

        vk_shtex_capture(data, &data->funcs, swap, 0, queue, info);
    }
}

static VkResult VKAPI_CALL OBS_QueuePresentKHR(VkQueue queue,
        const VkPresentInfoKHR *info)
{
    VkPresentInfoKHR api = *info;

    struct vk_data *const data = get_device_data_by_queue(queue);
    struct vk_device_funcs *const funcs = &data->funcs;

    if (data->valid) {
        vk_capture(data, data->graphics_queue ? data->graphics_queue : queue, &api);
    }

    return funcs->QueuePresentKHR(queue, &api);
}

/* ======================================================================== */
/* setup hooks                                                              */

static inline bool is_inst_link_info(VkLayerInstanceCreateInfo *lici)
{
    return lici->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
        lici->function == VK_LAYER_LINK_INFO;
}

static VkResult VKAPI_CALL OBS_CreateInstance(const VkInstanceCreateInfo *info,
        const VkAllocationCallbacks *ac,
        VkInstance *p_inst)
{
#ifndef NDEBUG
    hlog("CreateInstance");
#endif

    const char *req_extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    };
    static uint32_t req_extensions_count = 1;

    int new_count = info->enabledExtensionCount + req_extensions_count;
    const char **exts = (const char**)malloc(sizeof(char*) * new_count);
    memcpy(exts, info->ppEnabledExtensionNames, sizeof(char*) * info->enabledExtensionCount);
    for (uint32_t i = 0; i < req_extensions_count; ++i) {
        exts[info->enabledExtensionCount + i] = req_extensions[i];
    }
    VkInstanceCreateInfo *i = (VkInstanceCreateInfo*)info;
    i->enabledExtensionCount = new_count;
    i->ppEnabledExtensionNames = exts;

    /* -------------------------------------------------------- */
    /* step through chain until we get to the link info         */

    VkLayerInstanceCreateInfo *lici = (VkLayerInstanceCreateInfo *)info->pNext;
    while (lici && !is_inst_link_info(lici)) {
        lici = (VkLayerInstanceCreateInfo *)lici->pNext;
    }

    if (lici == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr gpa =
        lici->u.pLayerInfo->pfnNextGetInstanceProcAddr;

    /* -------------------------------------------------------- */
    /* move chain on for next layer                             */

    lici->u.pLayerInfo = lici->u.pLayerInfo->pNext;

    /* -------------------------------------------------------- */
    /* allocate data node                                       */

    struct vk_inst_data *idata = alloc_inst_data(ac);
    if (!idata)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    /* -------------------------------------------------------- */
    /* create instance                                          */

    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gpa(NULL, "vkCreateInstance");

    VkResult res = create(info, ac, p_inst);
#ifndef NDEBUG
    hlog("CreateInstance %s", result_to_str(res));
#endif
    bool valid = res == VK_SUCCESS;
    if (!valid) {
        /* try again with original arguments */
        res = create(info, ac, p_inst);
        if (res != VK_SUCCESS) {
            vk_free(ac, idata);
            return res;
        }
    }

    VkInstance inst = *p_inst;
    init_inst_data(idata, inst);

    /* -------------------------------------------------------- */
    /* fetch the functions we need                              */

    struct vk_inst_funcs *ifuncs = &idata->funcs;

#define GETADDR(x)                                      \
    do {                                            \
        ifuncs->x = (PFN_vk##x)gpa(inst, "vk" #x); \
        if (!ifuncs->x) {                       \
            hlog("could not get instance "  \
                    "address for vk" #x);      \
            funcs_found = false;            \
        }                                       \
    } while (false)

#define GETADDR_IF_SUPPORTED(x)                                      \
    do {                                            \
        ifuncs->x = (PFN_vk##x)gpa(inst, "vk" #x); \
    } while (false)

    bool funcs_found = true;
    GETADDR(GetInstanceProcAddr);
    GETADDR(DestroyInstance);
    GETADDR(GetPhysicalDeviceQueueFamilyProperties);
    GETADDR(GetPhysicalDeviceMemoryProperties);
    GETADDR(GetPhysicalDeviceFormatProperties2KHR);
    GETADDR(GetPhysicalDeviceImageFormatProperties2KHR);
    GETADDR(GetPhysicalDeviceProperties2KHR);
    GETADDR(EnumerateDeviceExtensionProperties);
#if HAVE_X11_XCB
    GETADDR_IF_SUPPORTED(CreateXcbSurfaceKHR);
#endif
#if HAVE_X11_XLIB
    GETADDR_IF_SUPPORTED(CreateXlibSurfaceKHR);
#endif
#if HAVE_WAYLAND
    GETADDR_IF_SUPPORTED(CreateWaylandSurfaceKHR);
#endif
    GETADDR_IF_SUPPORTED(DestroySurfaceKHR);
#undef GETADDR

    valid = valid && funcs_found;
    idata->valid = valid;

    if (valid)
        init_obj_list(&idata->surfaces);

    return res;
}

static void VKAPI_CALL OBS_DestroyInstance(VkInstance instance,
        const VkAllocationCallbacks *ac)
{
#ifndef NDEBUG
    hlog("DestroyInstance");
#endif

    struct vk_inst_funcs *ifuncs = get_inst_funcs(instance);
    PFN_vkDestroyInstance destroy_instance = ifuncs->DestroyInstance;

    remove_free_inst_data(instance, ac);

    destroy_instance(instance, ac);
}

static inline bool is_device_link_info(VkLayerDeviceCreateInfo *lici)
{
    return lici->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
        lici->function == VK_LAYER_LINK_INFO;
}

static VkResult VKAPI_CALL OBS_CreateDevice(VkPhysicalDevice phy_device,
        const VkDeviceCreateInfo *info,
        const VkAllocationCallbacks *ac,
        VkDevice *p_device)
{
#ifndef NDEBUG
    hlog("CreateDevice");
#endif

    struct vk_inst_data *idata =
        get_inst_data_by_physical_device(phy_device);
    struct vk_inst_funcs *ifuncs = &idata->funcs;
    struct vk_data *data = NULL;

    const char *req_extensions[] = {
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_MAINTENANCE1_EXTENSION_NAME,
        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME,
    };
    static uint32_t req_extensions_count = 11;

    int new_count = info->enabledExtensionCount + req_extensions_count;
    const char **exts = (const char**)malloc(sizeof(char*) * new_count);
    memcpy(exts, info->ppEnabledExtensionNames, sizeof(char*) * info->enabledExtensionCount);
    for (uint32_t i = 0; i < req_extensions_count; ++i) {
        exts[info->enabledExtensionCount + i] = req_extensions[i];
    }
    VkDeviceCreateInfo *i = (VkDeviceCreateInfo*)info;
    i->enabledExtensionCount = new_count;
    i->ppEnabledExtensionNames = exts;

    VkResult ret = VK_ERROR_INITIALIZATION_FAILED;

    VkLayerDeviceCreateInfo *ldci = (VkLayerDeviceCreateInfo*)info->pNext;

    /* -------------------------------------------------------- */
    /* step through chain until we get to the link info         */

    while (ldci && !is_device_link_info(ldci)) {
        ldci = (VkLayerDeviceCreateInfo *)ldci->pNext;
    }

    if (!ldci) {
        return ret;
    }

    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;

    gipa = ldci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    gdpa = ldci->u.pLayerInfo->pfnNextGetDeviceProcAddr;

    /* -------------------------------------------------------- */
    /* move chain on for next layer                             */

    ldci->u.pLayerInfo = ldci->u.pLayerInfo->pNext;

    /* -------------------------------------------------------- */
    /* allocate data node                                       */

    data = alloc_device_data(ac);
    if (!data)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    init_obj_list(&data->queues);
    data->graphics_queue = VK_NULL_HANDLE;

    /* -------------------------------------------------------- */
    /* create device and initialize hook data                   */

    PFN_vkCreateDevice createFunc =
        (PFN_vkCreateDevice)gipa(idata->instance, "vkCreateDevice");

    ret = createFunc(phy_device, info, ac, p_device);
#ifndef NDEBUG
    hlog("CreateDevice %s", result_to_str(ret));
#endif
    if (ret != VK_SUCCESS) {
        vk_free(ac, data);
        return ret;
    }

    VkDevice device = *p_device;
    init_device_data(data, device);

    data->valid = false; /* set true below if it doesn't go to fail */
    data->phy_device = phy_device;

    /* -------------------------------------------------------- */
    /* fetch the functions we need                              */

    struct vk_device_funcs *dfuncs = &data->funcs;
    bool funcs_found = true;

#define GETADDR(x)                                         \
    do {                                               \
        dfuncs->x = (PFN_vk##x)gdpa(device, "vk" #x); \
        if (!dfuncs->x) {                          \
            hlog("could not get device "       \
                    "address for vk" #x);         \
            funcs_found = false;               \
        }                                          \
    } while (false)

    GETADDR(GetDeviceProcAddr);
    GETADDR(DestroyDevice);
    GETADDR(CreateSwapchainKHR);
    GETADDR(DestroySwapchainKHR);
    GETADDR(QueuePresentKHR);
    GETADDR(AllocateMemory);
    GETADDR(FreeMemory);
    GETADDR(BindImageMemory2KHR);
    GETADDR(GetSwapchainImagesKHR);
    GETADDR(CreateImage);
    GETADDR(DestroyImage);
    GETADDR(GetImageMemoryRequirements2KHR);
    GETADDR(ResetCommandPool);
    GETADDR(BeginCommandBuffer);
    GETADDR(EndCommandBuffer);
    GETADDR(CmdCopyImage);
    GETADDR(CmdBlitImage);
    GETADDR(CmdPipelineBarrier);
    GETADDR(GetDeviceQueue);
    GETADDR(QueueSubmit);
    GETADDR(CreateCommandPool);
    GETADDR(DestroyCommandPool);
    GETADDR(AllocateCommandBuffers);
    GETADDR(CreateFence);
    GETADDR(DestroyFence);
    GETADDR(WaitForFences);
    GETADDR(ResetFences);
    GETADDR(GetImageSubresourceLayout);
    GETADDR(GetMemoryFdKHR);
    GETADDR(CreateSemaphore);
    GETADDR(DestroySemaphore);

    dfuncs->GetImageDrmFormatModifierPropertiesEXT = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)
        gdpa(device, "vkGetImageDrmFormatModifierPropertiesEXT");
    if (!dfuncs->GetImageDrmFormatModifierPropertiesEXT) {
        hlog("DRM format modifier support not available");
    }

#undef GETADDR

    if (!funcs_found) {
        return ret;
    }

    if (!idata->valid) {
        hlog("instance not valid");
        return ret;
    }

    uint32_t device_extension_count = 0;
    ret = ifuncs->EnumerateDeviceExtensionProperties(
            phy_device, NULL, &device_extension_count, NULL);
    if (ret != VK_SUCCESS) {
        return ret;
    }

    VkExtensionProperties *device_extensions = malloc(
            sizeof(VkExtensionProperties) * device_extension_count);
    ret = ifuncs->EnumerateDeviceExtensionProperties(
            phy_device, NULL, &device_extension_count, device_extensions);
    if (ret != VK_SUCCESS) {
        free(device_extensions);
        return ret;
    }

    const char *required_device_extensions[] = {
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME
    };

    bool extensions_found = true;
    for (int i = 0; i < sizeof(required_device_extensions) / sizeof(required_device_extensions[0]); i++) {
        const char *const ext = required_device_extensions[i];
        bool found = false;
        for (uint32_t j = 0; j < device_extension_count; j++) {
            if (!strcmp(ext, device_extensions[j].extensionName)) {
                found = true;
                break;
            }
        }
        if (!found) {
            hlog("missing device extension: %s", ext);
            extensions_found = false;
        }
    }

    free(device_extensions);

    if (!extensions_found) {
        return ret;
    }

    data->inst_data = idata;

    data->ac = NULL;
    if (ac) {
        data->ac_storage = *ac;
        data->ac = &data->ac_storage;
    }

    uint32_t queue_family_property_count = 0;
    ifuncs->GetPhysicalDeviceQueueFamilyProperties(
            phy_device, &queue_family_property_count, NULL);
    VkQueueFamilyProperties *queue_family_properties = (VkQueueFamilyProperties*)malloc(
            sizeof(VkQueueFamilyProperties) * queue_family_property_count);
    ifuncs->GetPhysicalDeviceQueueFamilyProperties(
            phy_device, &queue_family_property_count,
            queue_family_properties);

    for (uint32_t info_index = 0, info_count = info->queueCreateInfoCount;
            info_index < info_count; ++info_index) {
        const VkDeviceQueueCreateInfo *queue_info =
            &info->pQueueCreateInfos[info_index];
        for (uint32_t queue_index = 0,
                queue_count = queue_info->queueCount;
                queue_index < queue_count; ++queue_index) {
            const uint32_t family_index =
                queue_info->queueFamilyIndex;
            VkQueue queue;
            data->funcs.GetDeviceQueue(device, family_index,
                    queue_index, &queue);
            const bool supports_transfer =
                (queue_family_properties[family_index]
                 .queueFlags &
                 (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
                  VK_QUEUE_TRANSFER_BIT)) != 0;
            const bool supports_graphics =
                (queue_family_properties[family_index]
                 .queueFlags &
                 (VK_QUEUE_GRAPHICS_BIT)) != 0;
            add_queue_data(data, queue, family_index,
                    supports_transfer, supports_graphics, ac);
        }
    }

    free(queue_family_properties);

    init_obj_list(&data->swaps);
    data->cur_swap = NULL;

    VkPhysicalDeviceDriverProperties propsDriver = {};
    propsDriver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;

    VkPhysicalDeviceIDProperties propsID = {};
    propsID.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    propsID.pNext = &propsDriver;

    VkPhysicalDeviceProperties2 props = {};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &propsID;
    ifuncs->GetPhysicalDeviceProperties2KHR(phy_device, &props);

    data->driver_id = propsDriver.driverID;
    memcpy(data->device_uuid, propsID.deviceUUID, 16);

    data->valid = true;

    return ret;
}

static void VKAPI_CALL OBS_DestroyDevice(VkDevice device,
        const VkAllocationCallbacks *ac)
{
#ifndef NDEBUG
    hlog("DestroyDevice");
#endif

    struct vk_data *data = remove_device_data(device);

    if (data->valid) {
        struct vk_queue_data *queue_data = queue_walk_begin(data);

        while (queue_data) {
            vk_shtex_destroy_frame_objects(data, queue_data);

            queue_data = queue_walk_next(queue_data);
        }

        queue_walk_end(data);

        remove_free_queue_all(data, ac);
    }

    PFN_vkDestroyDevice destroy_device = data->funcs.DestroyDevice;

    vk_free(ac, data);

    destroy_device(device, ac);
}

static VkResult VKAPI_CALL
OBS_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *cinfo,
        const VkAllocationCallbacks *ac, VkSwapchainKHR *p_sc)
{
    struct vk_data *data = get_device_data(device);
    struct vk_device_funcs *funcs = &data->funcs;
    if (!data->valid)
        return funcs->CreateSwapchainKHR(device, cinfo, ac, p_sc);

    VkSwapchainCreateInfoKHR info = *cinfo;
    info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult res = funcs->CreateSwapchainKHR(device, &info, ac, p_sc);
#ifndef NDEBUG
    hlog("CreateSwapchainKHR %s", result_to_str(res));
#endif
    if (res != VK_SUCCESS) {
        /* try again with original imageUsage flags */
        return funcs->CreateSwapchainKHR(device, cinfo, ac, p_sc);
    }

    VkSwapchainKHR sc = *p_sc;
    uint32_t count = 0;
    res = funcs->GetSwapchainImagesKHR(device, sc, &count, NULL);
#ifndef NDEBUG
    hlog("GetSwapchainImagesKHR %s", result_to_str(res));
#endif
    if ((res == VK_SUCCESS) && (count > 0)) {
        struct vk_swap_data *swap_data = alloc_swap_data(ac);
        if (swap_data) {
            init_swap_data(swap_data, data, sc);
            swap_data->swap_images = vk_alloc(
                    ac, count * sizeof(VkImage), _Alignof(VkImage),
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
            res = funcs->GetSwapchainImagesKHR(
                    device, sc, &count, swap_data->swap_images);
#ifndef NDEBUG
            hlog("GetSwapchainImagesKHR %s", result_to_str(res));
#endif
            swap_data->image_extent = cinfo->imageExtent;
            swap_data->format = cinfo->imageFormat;
            swap_data->color_space = cinfo->imageColorSpace;
            swap_data->winid = find_surf_winid(data->inst_data, cinfo->surface);
            swap_data->export_image = VK_NULL_HANDLE;
            swap_data->export_mem = VK_NULL_HANDLE;
            swap_data->image_count = count;
            swap_data->dmabuf_nfd = 0;
            memset(swap_data->dmabuf_fds, -1, sizeof(swap_data->dmabuf_fds));
            swap_data->captured = false;
        }
    }

    return VK_SUCCESS;
}

static void VKAPI_CALL OBS_DestroySwapchainKHR(VkDevice device,
        VkSwapchainKHR sc,
        const VkAllocationCallbacks *ac)
{
#ifndef NDEBUG
    hlog("DestroySwapchainKHR");
#endif

    struct vk_data *data = get_device_data(device);
    struct vk_device_funcs *funcs = &data->funcs;
    PFN_vkDestroySwapchainKHR destroy_swapchain =
        funcs->DestroySwapchainKHR;

    if ((sc != VK_NULL_HANDLE) && data->valid) {
        struct vk_swap_data *swap = get_swap_data(data, sc);
        if (swap) {
            if (data->cur_swap == swap) {
                vk_shtex_free(data);
            }

            vk_free(ac, swap->swap_images);

            remove_free_swap_data(data, sc, ac);
        }
    }

    destroy_swapchain(device, sc, ac);
}

#if HAVE_X11_XCB
static VkResult VKAPI_CALL OBS_CreateXcbSurfaceKHR(
        VkInstance inst, const VkXcbSurfaceCreateInfoKHR *info,
        const VkAllocationCallbacks *ac, VkSurfaceKHR *surf)
{
#ifndef NDEBUG
    hlog("CreateXcbSurfaceKHR");
#endif

    struct vk_inst_data *idata = get_inst_data(inst);
    struct vk_inst_funcs *ifuncs = &idata->funcs;

    VkResult res = ifuncs->CreateXcbSurfaceKHR(inst, info, ac, surf);
    if ((res == VK_SUCCESS) && idata->valid)
        add_surf_data(idata, *surf, info->window, ac);
    return res;
}
#endif

#if HAVE_X11_XLIB
static VkResult VKAPI_CALL OBS_CreateXlibSurfaceKHR(
        VkInstance inst, const VkXlibSurfaceCreateInfoKHR *info,
        const VkAllocationCallbacks *ac, VkSurfaceKHR *surf)
{
#ifndef NDEBUG
    hlog("CreateXlibSurfaceKHR");
#endif

    struct vk_inst_data *idata = get_inst_data(inst);
    struct vk_inst_funcs *ifuncs = &idata->funcs;

    VkResult res = ifuncs->CreateXlibSurfaceKHR(inst, info, ac, surf);
    if ((res == VK_SUCCESS) && idata->valid)
        add_surf_data(idata, *surf, info->window, ac);
    return res;
}
#endif

#if HAVE_WAYLAND
static VkResult VKAPI_CALL OBS_CreateWaylandSurfaceKHR(
        VkInstance inst, const VkWaylandSurfaceCreateInfoKHR *info,
        const VkAllocationCallbacks *ac, VkSurfaceKHR *surf)
{
#ifndef NDEBUG
    hlog("CreateWaylandSurfaceKHR");
#endif

    struct vk_inst_data *idata = get_inst_data(inst);
    struct vk_inst_funcs *ifuncs = &idata->funcs;

    VkResult res = ifuncs->CreateWaylandSurfaceKHR(inst, info, ac, surf);
    if ((res == VK_SUCCESS) && idata->valid) {
        add_surf_data(idata, *surf, (uintptr_t)info->surface, ac);
    }
    return res;
}
#endif

static void VKAPI_CALL OBS_DestroySurfaceKHR(VkInstance inst, VkSurfaceKHR surf,
        const VkAllocationCallbacks *ac)
{
#ifndef NDEBUG
    hlog("DestroySurfaceKHR");
#endif

    struct vk_inst_data *idata = get_inst_data(inst);
    struct vk_inst_funcs *ifuncs = &idata->funcs;
    PFN_vkDestroySurfaceKHR destroy_surface = ifuncs->DestroySurfaceKHR;

    if ((surf != VK_NULL_HANDLE) && idata->valid)
        remove_free_surf_data(idata, surf, ac);

    destroy_surface(inst, surf, ac);
}

#define GETPROCADDR(func)               \
    if (!strcmp(pName, "vk" #func)) \
    return (PFN_vkVoidFunction)&OBS_##func;

#define GETPROCADDR_IF_SUPPORTED(func)  \
    if (!strcmp(pName, "vk" #func)) \
    return funcs && funcs->func ? (PFN_vkVoidFunction)&OBS_##func : NULL;

static PFN_vkVoidFunction VKAPI_CALL OBS_GetDeviceProcAddr(VkDevice device, const char *pName)
{
    struct vk_data *data = get_device_data(device);
    struct vk_device_funcs *funcs = &data->funcs;

    GETPROCADDR(GetDeviceProcAddr);
    GETPROCADDR(DestroyDevice);
    GETPROCADDR_IF_SUPPORTED(CreateSwapchainKHR);
    GETPROCADDR_IF_SUPPORTED(DestroySwapchainKHR);
    GETPROCADDR_IF_SUPPORTED(QueuePresentKHR);

    if (funcs->GetDeviceProcAddr == NULL)
        return NULL;
    return funcs->GetDeviceProcAddr(device, pName);
}

static PFN_vkVoidFunction VKAPI_CALL OBS_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
    /* instance chain functions we intercept */
    GETPROCADDR(GetInstanceProcAddr);
    GETPROCADDR(CreateInstance);

    struct vk_inst_funcs *const funcs = instance ? get_inst_funcs(instance) : NULL;

    /* other instance chain functions we intercept */
    GETPROCADDR(DestroyInstance);
#if HAVE_X11_XCB
    GETPROCADDR_IF_SUPPORTED(CreateXcbSurfaceKHR);
#endif
#if HAVE_X11_XLIB
    GETPROCADDR_IF_SUPPORTED(CreateXlibSurfaceKHR);
#endif
#if HAVE_WAYLAND
    GETPROCADDR_IF_SUPPORTED(CreateWaylandSurfaceKHR);
#endif
    GETPROCADDR_IF_SUPPORTED(DestroySurfaceKHR);

    /* device chain functions we intercept */
    GETPROCADDR(GetDeviceProcAddr);
    GETPROCADDR(CreateDevice);
    GETPROCADDR(DestroyDevice);

    if (!funcs)
        return NULL;

    const PFN_vkGetInstanceProcAddr gipa = funcs->GetInstanceProcAddr;
    return gipa ? gipa(instance, pName) : NULL;
}

#undef GETPROCADDR

VKAPI_ATTR VkResult VKAPI_CALL OBS_Negotiate(VkNegotiateLayerInterface *nli)
{
    if (nli->loaderLayerInterfaceVersion >= 2) {
        nli->sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
        nli->pNext = NULL;
        nli->pfnGetInstanceProcAddr = OBS_GetInstanceProcAddr;
        nli->pfnGetDeviceProcAddr = OBS_GetDeviceProcAddr;
        nli->pfnGetPhysicalDeviceProcAddr = NULL;
    }

    const uint32_t cur_ver = CURRENT_LOADER_LAYER_INTERFACE_VERSION;

    if (nli->loaderLayerInterfaceVersion > cur_ver) {
        nli->loaderLayerInterfaceVersion = cur_ver;
    }

    if (!vulkan_seen) {
        hlog("Init Vulkan %s (%s)", PLUGIN_VERSION,
#ifdef __x86_64__
            "64bit");
#else
            "32bit");
#endif
        init_obj_list(&instances);
        init_obj_list(&devices);
        capture_init();

        vulkan_seen = true;
        vkcapture_linear = getenv("OBS_VKCAPTURE_LINEAR");

        for (int i = 0; i < MAX_PRESENT_SWAP_SEMAPHORE_COUNT; i++) {
            semaphore_dst_stage_masks[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
    }

    return VK_SUCCESS;
}
