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

#include "backend/d3d12/BufferD3D12.h"

#include "backend/d3d12/D3D12Backend.h"
#include "backend/d3d12/ResourceAllocator.h"
#include "backend/d3d12/ResourceUploader.h"
#include "common/Assert.h"
#include "common/Constants.h"
#include "common/Math.h"

namespace backend { namespace d3d12 {

    namespace {
        D3D12_RESOURCE_FLAGS D3D12ResourceFlags(nxt::BufferUsageBit usage) {
            D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;

            if (usage & nxt::BufferUsageBit::Storage) {
                flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            }

            return flags;
        }

        D3D12_RESOURCE_STATES D3D12BufferUsage(nxt::BufferUsageBit usage) {
            D3D12_RESOURCE_STATES resourceState = D3D12_RESOURCE_STATE_COMMON;

            if (usage & nxt::BufferUsageBit::TransferSrc) {
                resourceState |= D3D12_RESOURCE_STATE_COPY_SOURCE;
            }
            if (usage & nxt::BufferUsageBit::TransferDst) {
                resourceState |= D3D12_RESOURCE_STATE_COPY_DEST;
            }
            if (usage & (nxt::BufferUsageBit::Vertex | nxt::BufferUsageBit::Uniform)) {
                resourceState |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            }
            if (usage & nxt::BufferUsageBit::Index) {
                resourceState |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
            }
            if (usage & nxt::BufferUsageBit::Storage) {
                resourceState |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }

            return resourceState;
        }

        D3D12_HEAP_TYPE D3D12HeapType(nxt::BufferUsageBit allowedUsage) {
            if (allowedUsage & nxt::BufferUsageBit::MapRead) {
                return D3D12_HEAP_TYPE_READBACK;
            } else if (allowedUsage & nxt::BufferUsageBit::MapWrite) {
                return D3D12_HEAP_TYPE_UPLOAD;
            } else {
                return D3D12_HEAP_TYPE_DEFAULT;
            }
        }
    }  // namespace

    Buffer::Buffer(Device* device, BufferBuilder* builder) : BufferBase(builder), mDevice(device) {
        D3D12_RESOURCE_DESC resourceDescriptor;
        resourceDescriptor.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDescriptor.Alignment = 0;
        resourceDescriptor.Width = GetD3D12Size();
        resourceDescriptor.Height = 1;
        resourceDescriptor.DepthOrArraySize = 1;
        resourceDescriptor.MipLevels = 1;
        resourceDescriptor.Format = DXGI_FORMAT_UNKNOWN;
        resourceDescriptor.SampleDesc.Count = 1;
        resourceDescriptor.SampleDesc.Quality = 0;
        resourceDescriptor.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resourceDescriptor.Flags = D3D12ResourceFlags(GetAllowedUsage());

        auto heapType = D3D12HeapType(GetAllowedUsage());
        auto bufferUsage = D3D12BufferUsage(GetUsage());

        // D3D12 requires buffers on the READBACK heap to have the D3D12_RESOURCE_STATE_COPY_DEST
        // state
        if (heapType == D3D12_HEAP_TYPE_READBACK) {
            bufferUsage |= D3D12_RESOURCE_STATE_COPY_DEST;
        }

        // D3D12 requires buffers on the UPLOAD heap to have the D3D12_RESOURCE_STATE_GENERIC_READ
        // state
        if (heapType == D3D12_HEAP_TYPE_UPLOAD) {
            bufferUsage |= D3D12_RESOURCE_STATE_GENERIC_READ;
        }

        mResource =
            device->GetResourceAllocator()->Allocate(heapType, resourceDescriptor, bufferUsage);
    }

    Buffer::~Buffer() {
        mDevice->GetResourceAllocator()->Release(mResource);
    }

    uint32_t Buffer::GetD3D12Size() const {
        // TODO(enga@google.com): TODO investigate if this needs to be a constraint at the API level
        return Align(GetSize(), 256);
    }

    ComPtr<ID3D12Resource> Buffer::GetD3D12Resource() {
        return mResource;
    }

    bool Buffer::GetResourceTransitionBarrier(nxt::BufferUsageBit currentUsage,
                                              nxt::BufferUsageBit targetUsage,
                                              D3D12_RESOURCE_BARRIER* barrier) {
        if (GetAllowedUsage() & (nxt::BufferUsageBit::MapRead | nxt::BufferUsageBit::MapWrite)) {
            // Transitions are never needed for mapped buffers because they are created with and
            // always need the Transfer(Dst|Src) state. Mapped buffers cannot have states outside of
            // (MapRead|TransferDst) and (MapWrite|TransferSrc)
            return false;
        }

        D3D12_RESOURCE_STATES stateBefore = D3D12BufferUsage(currentUsage);
        D3D12_RESOURCE_STATES stateAfter = D3D12BufferUsage(targetUsage);

        if (stateBefore == stateAfter) {
            return false;
        }

        barrier->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier->Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier->Transition.pResource = mResource.Get();
        barrier->Transition.StateBefore = stateBefore;
        barrier->Transition.StateAfter = stateAfter;
        barrier->Transition.Subresource = 0;

        return true;
    }

    D3D12_GPU_VIRTUAL_ADDRESS Buffer::GetVA() const {
        return mResource->GetGPUVirtualAddress();
    }

    void Buffer::OnMapReadCommandSerialFinished(uint32_t mapSerial, const void* data) {
        CallMapReadCallback(mapSerial, NXT_BUFFER_MAP_READ_STATUS_SUCCESS, data);
    }

    void Buffer::SetSubDataImpl(uint32_t start, uint32_t count, const uint32_t* data) {
        mDevice->GetResourceUploader()->BufferSubData(mResource, start * sizeof(uint32_t),
                                                      count * sizeof(uint32_t), data);
    }

    void Buffer::MapReadAsyncImpl(uint32_t serial, uint32_t start, uint32_t count) {
        D3D12_RANGE readRange = {start, start + count};
        char* data = nullptr;
        ASSERT_SUCCESS(mResource->Map(0, &readRange, reinterpret_cast<void**>(&data)));

        MapReadRequestTracker* tracker = ToBackend(GetDevice())->GetMapReadRequestTracker();
        tracker->Track(this, serial, data + start);
    }

    void Buffer::UnmapImpl() {
        // TODO(enga@google.com): When MapWrite is implemented, this should state the range that was
        // modified
        D3D12_RANGE writeRange = {};
        mResource->Unmap(0, &writeRange);
        mDevice->GetResourceAllocator()->Release(mResource);
    }

    void Buffer::TransitionUsageImpl(nxt::BufferUsageBit currentUsage,
                                     nxt::BufferUsageBit targetUsage) {
        D3D12_RESOURCE_BARRIER barrier;
        if (GetResourceTransitionBarrier(currentUsage, targetUsage, &barrier)) {
            mDevice->GetPendingCommandList()->ResourceBarrier(1, &barrier);
        }
    }

    BufferView::BufferView(BufferViewBuilder* builder) : BufferViewBase(builder) {
        mCbvDesc.BufferLocation = ToBackend(GetBuffer())->GetVA() + GetOffset();
        mCbvDesc.SizeInBytes = GetD3D12Size();

        mUavDesc.Format = DXGI_FORMAT_UNKNOWN;
        mUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        mUavDesc.Buffer.FirstElement = GetOffset();
        mUavDesc.Buffer.NumElements = GetD3D12Size();
        mUavDesc.Buffer.StructureByteStride = 1;
        mUavDesc.Buffer.CounterOffsetInBytes = 0;
        mUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    }

    uint32_t BufferView::GetD3D12Size() const {
        // TODO(enga@google.com): TODO investigate if this needs to be a constraint at the API level
        return Align(GetSize(), 256);
    }

    const D3D12_CONSTANT_BUFFER_VIEW_DESC& BufferView::GetCBVDescriptor() const {
        return mCbvDesc;
    }

    const D3D12_UNORDERED_ACCESS_VIEW_DESC& BufferView::GetUAVDescriptor() const {
        return mUavDesc;
    }

    MapReadRequestTracker::MapReadRequestTracker(Device* device) : mDevice(device) {
    }

    MapReadRequestTracker::~MapReadRequestTracker() {
        ASSERT(mInflightRequests.Empty());
    }

    void MapReadRequestTracker::Track(Buffer* buffer, uint32_t mapSerial, const void* data) {
        Request request;
        request.buffer = buffer;
        request.mapSerial = mapSerial;
        request.data = data;

        mInflightRequests.Enqueue(std::move(request), mDevice->GetSerial());
    }

    void MapReadRequestTracker::Tick(Serial finishedSerial) {
        for (auto& request : mInflightRequests.IterateUpTo(finishedSerial)) {
            request.buffer->OnMapReadCommandSerialFinished(request.mapSerial, request.data);
        }
        mInflightRequests.ClearUpTo(finishedSerial);
    }

}}  // namespace backend::d3d12
