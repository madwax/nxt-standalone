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

#ifndef BACKEND_VULKAN_MEMORYALLOCATOR_H_
#define BACKEND_VULKAN_MEMORYALLOCATOR_H_

#include "backend/vulkan/vulkan_platform.h"
#include "common/SerialQueue.h"

namespace backend { namespace vulkan {

    class Device;
    class MemoryAllocator;

    class DeviceMemoryAllocation {
      public:
        ~DeviceMemoryAllocation();
        VkDeviceMemory GetMemory() const;
        size_t GetMemoryOffset() const;
        uint8_t* GetMappedPointer() const;

      private:
        friend class MemoryAllocator;
        VkDeviceMemory mMemory = VK_NULL_HANDLE;
        size_t mOffset = 0;
        uint8_t* mMappedPointer = nullptr;
    };

    class MemoryAllocator {
      public:
        MemoryAllocator(Device* device);
        ~MemoryAllocator();

        bool Allocate(VkMemoryRequirements requirements,
                      bool mappable,
                      DeviceMemoryAllocation* allocation);
        void Free(DeviceMemoryAllocation* allocation);

        void Tick(Serial finishedSerial);

      private:
        Device* mDevice = nullptr;
        SerialQueue<VkDeviceMemory> mReleasedMemory;
    };

}}  // namespace backend::vulkan

#endif  // BACKEND_VULKAN_MEMORYALLOCATOR_H_
