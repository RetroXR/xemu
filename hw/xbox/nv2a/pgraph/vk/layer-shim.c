/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Chains a layer library in by hand.
 *
 * Android only loads layers for debuggable applications, which leaves a core
 * running inside somebody else's release build, or a test host started from
 * a shell on a retail device, without the validation layer. Setting
 * XEMU_VK_LAYER_PATH to the layer library makes this file do what the loader
 * would: call the layer's vkCreateInstance and vkCreateDevice with a link
 * to the real ones, and take every other entry point from the layer.
 *
 * Copyright (c) 2026 xemu contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "renderer.h"

#ifndef _WIN32

#include <dlfcn.h>
#include <vulkan/vk_layer.h>

static PFN_vkVoidFunction (*real_gipa)(VkInstance, const char *);
static PFN_vkVoidFunction (*real_gdpa)(VkDevice, const char *);
static PFN_vkVoidFunction (*layer_gipa)(VkInstance, const char *);
static VkInstance shim_instance;

static VKAPI_ATTR VkResult VKAPI_CALL
shim_create_instance(const VkInstanceCreateInfo *create_info,
                     const VkAllocationCallbacks *allocator,
                     VkInstance *instance)
{
    VkLayerInstanceLink link = {
        .pfnNextGetInstanceProcAddr = real_gipa,
    };
    VkLayerInstanceCreateInfo layer_info = {
        .sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
        .pNext = create_info->pNext,
        .function = VK_LAYER_LINK_INFO,
        .u.pLayerInfo = &link,
    };
    VkInstanceCreateInfo chained = *create_info;
    chained.pNext = &layer_info;

    VkResult (*create)(const VkInstanceCreateInfo *,
                       const VkAllocationCallbacks *, VkInstance *) =
        (void *)layer_gipa(NULL, "vkCreateInstance");
    VkResult result = create(&chained, allocator, instance);
    if (result == VK_SUCCESS) {
        shim_instance = *instance;
    }
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL
shim_create_device(VkPhysicalDevice physical_device,
                   const VkDeviceCreateInfo *create_info,
                   const VkAllocationCallbacks *allocator, VkDevice *device)
{
    VkLayerDeviceLink link = {
        .pfnNextGetInstanceProcAddr = real_gipa,
        .pfnNextGetDeviceProcAddr = real_gdpa,
    };
    VkLayerDeviceCreateInfo layer_info = {
        .sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
        .pNext = create_info->pNext,
        .function = VK_LAYER_LINK_INFO,
        .u.pLayerInfo = &link,
    };
    VkDeviceCreateInfo chained = *create_info;
    chained.pNext = &layer_info;

    VkResult (*create)(VkPhysicalDevice, const VkDeviceCreateInfo *,
                       const VkAllocationCallbacks *, VkDevice *) =
        (void *)layer_gipa(shim_instance, "vkCreateDevice");
    return create(physical_device, &chained, allocator, device);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
shim_get_instance_proc_addr(VkInstance instance, const char *name)
{
    if (!strcmp(name, "vkCreateInstance")) {
        return (PFN_vkVoidFunction)shim_create_instance;
    }
    if (!strcmp(name, "vkCreateDevice")) {
        return (PFN_vkVoidFunction)shim_create_device;
    }
    if (instance == VK_NULL_HANDLE) {
        /* The remaining global commands do not concern the layer */
        return real_gipa(NULL, name);
    }
    return layer_gipa(instance, name);
}

bool pgraph_vk_init_layer_shim(void)
{
    const char *path = g_getenv("XEMU_VK_LAYER_PATH");
    if (!path || !*path) {
        return false;
    }

    void *vulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!vulkan) {
        vulkan = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    }
    void *layer = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!vulkan || !layer) {
        fprintf(stderr, "Layer shim: cannot load %s: %s\n",
                vulkan ? path : "libvulkan", dlerror());
        return false;
    }

    real_gipa = dlsym(vulkan, "vkGetInstanceProcAddr");
    real_gdpa = dlsym(vulkan, "vkGetDeviceProcAddr");
    layer_gipa = dlsym(layer, "vkGetInstanceProcAddr");
    if (!real_gipa || !real_gdpa || !layer_gipa) {
        fprintf(stderr, "Layer shim: %s is missing entry points\n", path);
        return false;
    }

    volkInitializeCustom(shim_get_instance_proc_addr);
    fprintf(stderr, "Layer shim: chained %s\n", path);
    return true;
}

#else

bool pgraph_vk_init_layer_shim(void)
{
    return false;
}

#endif
