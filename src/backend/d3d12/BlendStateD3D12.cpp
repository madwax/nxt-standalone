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

#include "backend/d3d12/BlendStateD3D12.h"

#include "backend/d3d12/D3D12Backend.h"
#include "common/Assert.h"

namespace backend { namespace d3d12 {

    namespace {
        D3D12_BLEND D3D12Blend(nxt::BlendFactor factor) {
            switch (factor) {
                case nxt::BlendFactor::Zero:
                    return D3D12_BLEND_ZERO;
                case nxt::BlendFactor::One:
                    return D3D12_BLEND_ONE;
                case nxt::BlendFactor::SrcColor:
                    return D3D12_BLEND_SRC_COLOR;
                case nxt::BlendFactor::OneMinusSrcColor:
                    return D3D12_BLEND_INV_SRC_COLOR;
                case nxt::BlendFactor::SrcAlpha:
                    return D3D12_BLEND_SRC_ALPHA;
                case nxt::BlendFactor::OneMinusSrcAlpha:
                    return D3D12_BLEND_INV_SRC_ALPHA;
                case nxt::BlendFactor::DstColor:
                    return D3D12_BLEND_DEST_COLOR;
                case nxt::BlendFactor::OneMinusDstColor:
                    return D3D12_BLEND_INV_DEST_COLOR;
                case nxt::BlendFactor::DstAlpha:
                    return D3D12_BLEND_DEST_ALPHA;
                case nxt::BlendFactor::OneMinusDstAlpha:
                    return D3D12_BLEND_INV_DEST_ALPHA;
                case nxt::BlendFactor::SrcAlphaSaturated:
                    return D3D12_BLEND_SRC_ALPHA_SAT;
                case nxt::BlendFactor::BlendColor:
                    return D3D12_BLEND_BLEND_FACTOR;
                case nxt::BlendFactor::OneMinusBlendColor:
                    return D3D12_BLEND_INV_BLEND_FACTOR;
                default:
                    UNREACHABLE();
            }
        }

        D3D12_BLEND_OP D3D12BlendOperation(nxt::BlendOperation operation) {
            switch (operation) {
                case nxt::BlendOperation::Add:
                    return D3D12_BLEND_OP_ADD;
                case nxt::BlendOperation::Subtract:
                    return D3D12_BLEND_OP_SUBTRACT;
                case nxt::BlendOperation::ReverseSubtract:
                    return D3D12_BLEND_OP_REV_SUBTRACT;
                case nxt::BlendOperation::Min:
                    return D3D12_BLEND_OP_MIN;
                case nxt::BlendOperation::Max:
                    return D3D12_BLEND_OP_MAX;
                default:
                    UNREACHABLE();
            }
        }

        uint8_t D3D12RenderTargetWriteMask(nxt::ColorWriteMask colorWriteMask) {
            static_assert(static_cast<D3D12_COLOR_WRITE_ENABLE>(nxt::ColorWriteMask::Red) ==
                              D3D12_COLOR_WRITE_ENABLE_RED,
                          "ColorWriteMask values must match");
            static_assert(static_cast<D3D12_COLOR_WRITE_ENABLE>(nxt::ColorWriteMask::Green) ==
                              D3D12_COLOR_WRITE_ENABLE_GREEN,
                          "ColorWriteMask values must match");
            static_assert(static_cast<D3D12_COLOR_WRITE_ENABLE>(nxt::ColorWriteMask::Blue) ==
                              D3D12_COLOR_WRITE_ENABLE_BLUE,
                          "ColorWriteMask values must match");
            static_assert(static_cast<D3D12_COLOR_WRITE_ENABLE>(nxt::ColorWriteMask::Alpha) ==
                              D3D12_COLOR_WRITE_ENABLE_ALPHA,
                          "ColorWriteMask values must match");
            return static_cast<uint8_t>(colorWriteMask);
        }
    }  // namespace

    BlendState::BlendState(BlendStateBuilder* builder) : BlendStateBase(builder) {
        auto& info = GetBlendInfo();
        mBlendDesc.BlendEnable = info.blendEnabled;
        mBlendDesc.SrcBlend = D3D12Blend(info.colorBlend.srcFactor);
        mBlendDesc.DestBlend = D3D12Blend(info.colorBlend.dstFactor);
        mBlendDesc.BlendOp = D3D12BlendOperation(info.colorBlend.operation);
        mBlendDesc.SrcBlendAlpha = D3D12Blend(info.alphaBlend.srcFactor);
        mBlendDesc.DestBlendAlpha = D3D12Blend(info.alphaBlend.dstFactor);
        mBlendDesc.BlendOpAlpha = D3D12BlendOperation(info.alphaBlend.operation);
        mBlendDesc.RenderTargetWriteMask = D3D12RenderTargetWriteMask(info.colorWriteMask);
        mBlendDesc.LogicOpEnable = false;
        mBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
    }

    const D3D12_RENDER_TARGET_BLEND_DESC& BlendState::GetD3D12BlendDesc() const {
        return mBlendDesc;
    }

}}  // namespace backend::d3d12
