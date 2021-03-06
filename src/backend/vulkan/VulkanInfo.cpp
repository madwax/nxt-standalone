// Copyright 2017 The NXT Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "backend/vulkan/VulkanInfo.h"

#include "backend/vulkan/VulkanBackend.h"

#include <cstring>

namespace {
    bool IsLayerName(const VkLayerProperties& layer, const char* name) {
        return strncmp(layer.layerName, name, VK_MAX_EXTENSION_NAME_SIZE) == 0;
    }

    bool IsExtensionName(const VkExtensionProperties& extension, const char* name) {
        return strncmp(extension.extensionName, name, VK_MAX_EXTENSION_NAME_SIZE) == 0;
    }
}  // namespace

namespace backend { namespace vulkan {

    const char kLayerNameLunargStandardValidation[] = "VK_LAYER_LUNARG_standard_validation";

    const char kExtensionNameExtDebugReport[] = "VK_EXT_debug_report";
    const char kExtensionNameKhrSurface[] = "VK_KHR_surface";
    const char kExtensionNameKhrSwapchain[] = "VK_KHR_swapchain";

    bool GatherGlobalInfo(const Device& device, VulkanGlobalInfo* info) {
        // Gather the info about the instance layers
        {
            uint32_t count = 0;
            VkResult result = device.fn.EnumerateInstanceLayerProperties(&count, nullptr);
            // From the Vulkan spec result should be success if there are 0 layers,
            // incomplete otherwise. This means that both values represent a success.
            // This is the same for all Enumarte functions
            if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
                return false;
            }

            info->layers.resize(count);
            result = device.fn.EnumerateInstanceLayerProperties(&count, info->layers.data());
            if (result != VK_SUCCESS) {
                return false;
            }

            for (const auto& layer : info->layers) {
                if (IsLayerName(layer, kLayerNameLunargStandardValidation)) {
                    info->standardValidation = true;
                }
            }
        }

        // Gather the info about the instance extensions
        {
            uint32_t count = 0;
            VkResult result =
                device.fn.EnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
            if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
                return false;
            }

            info->extensions.resize(count);
            result = device.fn.EnumerateInstanceExtensionProperties(nullptr, &count,
                                                                    info->extensions.data());
            if (result != VK_SUCCESS) {
                return false;
            }

            for (const auto& extension : info->extensions) {
                if (IsExtensionName(extension, kExtensionNameExtDebugReport)) {
                    info->debugReport = true;
                }
                if (IsExtensionName(extension, kExtensionNameKhrSurface)) {
                    info->surface = true;
                }
            }
        }

        // TODO(cwallez@chromium:org): Each layer can expose additional extensions, query them?

        return true;
    }

    bool GetPhysicalDevices(const Device& device, std::vector<VkPhysicalDevice>* physicalDevices) {
        VkInstance instance = device.GetInstance();

        uint32_t count = 0;
        VkResult result = device.fn.EnumeratePhysicalDevices(instance, &count, nullptr);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            return false;
        }

        physicalDevices->resize(count);
        result = device.fn.EnumeratePhysicalDevices(instance, &count, physicalDevices->data());
        if (result != VK_SUCCESS) {
            return false;
        }

        return true;
    }

    bool GatherDeviceInfo(const Device& device,
                          VkPhysicalDevice physicalDevice,
                          VulkanDeviceInfo* info) {
        // Gather general info about the device
        device.fn.GetPhysicalDeviceProperties(physicalDevice, &info->properties);
        device.fn.GetPhysicalDeviceFeatures(physicalDevice, &info->features);

        // Gather info about device memory.
        {
            VkPhysicalDeviceMemoryProperties memory;
            device.fn.GetPhysicalDeviceMemoryProperties(physicalDevice, &memory);

            info->memoryTypes.assign(memory.memoryTypes,
                                     memory.memoryTypes + memory.memoryTypeCount);
            info->memoryHeaps.assign(memory.memoryHeaps,
                                     memory.memoryHeaps + memory.memoryHeapCount);
        }

        // Gather info about device queue families
        {
            uint32_t count = 0;
            device.fn.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, nullptr);

            info->queueFamilies.resize(count);
            device.fn.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count,
                                                             info->queueFamilies.data());
        }

        // Gather the info about the device layers
        {
            uint32_t count = 0;
            VkResult result =
                device.fn.EnumerateDeviceLayerProperties(physicalDevice, &count, nullptr);
            if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
                return false;
            }

            info->layers.resize(count);
            result = device.fn.EnumerateDeviceLayerProperties(physicalDevice, &count,
                                                              info->layers.data());
            if (result != VK_SUCCESS) {
                return false;
            }
        }

        // Gather the info about the device extensions
        {
            uint32_t count = 0;
            VkResult result = device.fn.EnumerateDeviceExtensionProperties(physicalDevice, nullptr,
                                                                           &count, nullptr);
            if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
                return false;
            }

            info->extensions.resize(count);
            result = device.fn.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count,
                                                                  info->extensions.data());
            if (result != VK_SUCCESS) {
                return false;
            }

            for (const auto& extension : info->extensions) {
                if (IsExtensionName(extension, kExtensionNameKhrSwapchain)) {
                    info->swapchain = true;
                }
            }
        }

        // TODO(cwallez@chromium.org): gather info about formats

        return true;
    }

}}  // namespace backend::vulkan
