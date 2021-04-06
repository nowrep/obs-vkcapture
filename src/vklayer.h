/*
OBS Linux Vulkan game capture
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

#pragma once

#include <vulkan/vulkan.h>

#define DEF_FUNC(x) PFN_vk##x x

struct vk_inst_funcs {
    DEF_FUNC(GetInstanceProcAddr);
    DEF_FUNC(DestroyInstance);
    DEF_FUNC(GetPhysicalDeviceQueueFamilyProperties);
    DEF_FUNC(GetPhysicalDeviceMemoryProperties);
    DEF_FUNC(EnumerateDeviceExtensionProperties);
};

struct vk_device_funcs {
    DEF_FUNC(GetDeviceProcAddr);
    DEF_FUNC(DestroyDevice);
    DEF_FUNC(CreateSwapchainKHR);
    DEF_FUNC(DestroySwapchainKHR);
    DEF_FUNC(QueuePresentKHR);
    DEF_FUNC(AllocateMemory);
    DEF_FUNC(FreeMemory);
    DEF_FUNC(BindImageMemory2);
    DEF_FUNC(GetSwapchainImagesKHR);
    DEF_FUNC(CreateImage);
    DEF_FUNC(DestroyImage);
    DEF_FUNC(GetImageMemoryRequirements2);
    DEF_FUNC(ResetCommandPool);
    DEF_FUNC(BeginCommandBuffer);
    DEF_FUNC(EndCommandBuffer);
    DEF_FUNC(CmdCopyImage);
    DEF_FUNC(CmdPipelineBarrier);
    DEF_FUNC(GetDeviceQueue);
    DEF_FUNC(QueueSubmit);
    DEF_FUNC(CreateCommandPool);
    DEF_FUNC(DestroyCommandPool);
    DEF_FUNC(AllocateCommandBuffers);
    DEF_FUNC(CreateFence);
    DEF_FUNC(DestroyFence);
    DEF_FUNC(WaitForFences);
    DEF_FUNC(ResetFences);
    DEF_FUNC(GetImageSubresourceLayout);
    DEF_FUNC(GetMemoryFdKHR);
};

#undef DEF_FUNC
