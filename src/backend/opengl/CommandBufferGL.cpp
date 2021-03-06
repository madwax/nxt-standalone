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

#include "backend/opengl/CommandBufferGL.h"

#include "backend/Commands.h"
#include "backend/opengl/BufferGL.h"
#include "backend/opengl/ComputePipelineGL.h"
#include "backend/opengl/InputStateGL.h"
#include "backend/opengl/OpenGLBackend.h"
#include "backend/opengl/PersistentPipelineStateGL.h"
#include "backend/opengl/PipelineLayoutGL.h"
#include "backend/opengl/RenderPipelineGL.h"
#include "backend/opengl/SamplerGL.h"
#include "backend/opengl/TextureGL.h"

#include <cstring>

namespace backend { namespace opengl {

    namespace {

        GLenum IndexFormatType(nxt::IndexFormat format) {
            switch (format) {
                case nxt::IndexFormat::Uint16:
                    return GL_UNSIGNED_SHORT;
                case nxt::IndexFormat::Uint32:
                    return GL_UNSIGNED_INT;
                default:
                    UNREACHABLE();
            }
        }

        GLenum VertexFormatType(nxt::VertexFormat format) {
            switch (format) {
                case nxt::VertexFormat::FloatR32G32B32A32:
                case nxt::VertexFormat::FloatR32G32B32:
                case nxt::VertexFormat::FloatR32G32:
                case nxt::VertexFormat::FloatR32:
                    return GL_FLOAT;
                default:
                    UNREACHABLE();
            }
        }

        // Push constants are implemented using OpenGL uniforms, however they aren't part of the
        // global OpenGL state but are part of the program state instead. This means that we have to
        // reapply push constants on pipeline change.
        //
        // This structure tracks the current values of push constants as well as dirty bits for push
        // constants that should be applied before the next draw or dispatch.
        class PushConstantTracker {
          public:
            void OnBeginPass() {
                for (auto stage : IterateStages(kAllStages)) {
                    mValues[stage].fill(0);
                    // No need to set dirty bits as a pipeline will be set before the next operation
                    // using push constants.
                }
            }

            void OnSetPushConstants(nxt::ShaderStageBit stages,
                                    uint32_t count,
                                    uint32_t offset,
                                    const uint32_t* data) {
                for (auto stage : IterateStages(stages)) {
                    memcpy(&mValues[stage][offset], data, count * sizeof(uint32_t));

                    // Use 64 bit masks and make sure there are no shift UB
                    static_assert(kMaxPushConstants <= 8 * sizeof(unsigned long long) - 1, "");
                    mDirtyBits[stage] |= ((1ull << count) - 1ull) << offset;
                }
            }

            void OnSetPipeline(PipelineBase* pipeline) {
                for (auto stage : IterateStages(kAllStages)) {
                    mDirtyBits[stage] = pipeline->GetPushConstants(stage).mask;
                }
            }

            void Apply(PipelineBase* pipeline, PipelineGL* glPipeline) {
                for (auto stage : IterateStages(kAllStages)) {
                    const auto& pushConstants = pipeline->GetPushConstants(stage);
                    const auto& glPushConstants = glPipeline->GetGLPushConstants(stage);

                    for (uint32_t constant :
                         IterateBitSet(mDirtyBits[stage] & pushConstants.mask)) {
                        GLint location = glPushConstants[constant];
                        switch (pushConstants.types[constant]) {
                            case PushConstantType::Int:
                                glUniform1i(location,
                                            *reinterpret_cast<GLint*>(&mValues[stage][constant]));
                                break;
                            case PushConstantType::UInt:
                                glUniform1ui(location,
                                             *reinterpret_cast<GLuint*>(&mValues[stage][constant]));
                                break;
                            case PushConstantType::Float:
                                glUniform1f(location,
                                            *reinterpret_cast<GLfloat*>(&mValues[stage][constant]));
                                break;
                        }
                    }

                    mDirtyBits[stage].reset();
                }
            }

          private:
            PerStage<std::array<uint32_t, kMaxPushConstants>> mValues;
            PerStage<std::bitset<kMaxPushConstants>> mDirtyBits;
        };

        // Vertex buffers and index buffers are implemented as part of an OpenGL VAO that
        // corresponds to an InputState. On the contrary in NXT they are part of the global state.
        // This means that we have to re-apply these buffers on an InputState change.
        class InputBufferTracker {
          public:
            void OnBeginPass() {
                // We don't know what happened between this pass and the last one, just reset the
                // input state so everything gets reapplied.
                mLastInputState = nullptr;
            }

            void OnSetIndexBuffer(BufferBase* buffer) {
                mIndexBufferDirty = true;
                mIndexBuffer = ToBackend(buffer);
            }

            void OnSetVertexBuffers(uint32_t startSlot,
                                    uint32_t count,
                                    Ref<BufferBase>* buffers,
                                    uint32_t* offsets) {
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t slot = startSlot + i;
                    mVertexBuffers[slot] = ToBackend(buffers[i].Get());
                    mVertexBufferOffsets[slot] = offsets[i];
                }

                // Use 64 bit masks and make sure there are no shift UB
                static_assert(kMaxVertexInputs <= 8 * sizeof(unsigned long long) - 1, "");
                mDirtyVertexBuffers |= ((1ull << count) - 1ull) << startSlot;
            }

            void OnSetPipeline(RenderPipelineBase* pipeline) {
                InputStateBase* inputState = pipeline->GetInputState();
                if (mLastInputState == inputState) {
                    return;
                }

                mIndexBufferDirty = true;
                mDirtyVertexBuffers |= inputState->GetInputsSetMask();

                mLastInputState = ToBackend(inputState);
            }

            void Apply() {
                if (mIndexBufferDirty && mIndexBuffer != nullptr) {
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mIndexBuffer->GetHandle());
                    mIndexBufferDirty = false;
                }

                for (uint32_t slot :
                     IterateBitSet(mDirtyVertexBuffers & mLastInputState->GetInputsSetMask())) {
                    for (uint32_t location :
                         IterateBitSet(mLastInputState->GetAttributesUsingInput(slot))) {
                        auto attribute = mLastInputState->GetAttribute(location);

                        GLuint buffer = mVertexBuffers[slot]->GetHandle();
                        uint32_t offset = mVertexBufferOffsets[slot];

                        auto input = mLastInputState->GetInput(slot);
                        auto components = VertexFormatNumComponents(attribute.format);
                        auto formatType = VertexFormatType(attribute.format);

                        glBindBuffer(GL_ARRAY_BUFFER, buffer);
                        glVertexAttribPointer(
                            location, components, formatType, GL_FALSE, input.stride,
                            reinterpret_cast<void*>(
                                static_cast<intptr_t>(offset + attribute.offset)));
                    }
                }

                mDirtyVertexBuffers.reset();
            }

          private:
            bool mIndexBufferDirty = false;
            Buffer* mIndexBuffer = nullptr;

            std::bitset<kMaxVertexInputs> mDirtyVertexBuffers;
            std::array<Buffer*, kMaxVertexInputs> mVertexBuffers;
            std::array<uint32_t, kMaxVertexInputs> mVertexBufferOffsets;

            InputState* mLastInputState = nullptr;
        };

    }  // namespace

    CommandBuffer::CommandBuffer(CommandBufferBuilder* builder)
        : CommandBufferBase(builder), mCommands(builder->AcquireCommands()) {
    }

    CommandBuffer::~CommandBuffer() {
        FreeCommands(&mCommands);
    }

    void CommandBuffer::Execute() {
        Command type;
        PipelineBase* lastPipeline = nullptr;
        PipelineGL* lastGLPipeline = nullptr;
        RenderPipeline* lastRenderPipeline = nullptr;
        uint32_t indexBufferOffset = 0;

        PersistentPipelineState persistentPipelineState;
        persistentPipelineState.SetDefaultState();

        PushConstantTracker pushConstants;
        InputBufferTracker inputBuffers;

        RenderPass* currentRenderPass = nullptr;
        Framebuffer* currentFramebuffer = nullptr;
        uint32_t currentSubpass = 0;
        GLuint currentFBO = 0;

        while (mCommands.NextCommandId(&type)) {
            switch (type) {
                case Command::BeginComputePass: {
                    mCommands.NextCommand<BeginComputePassCmd>();
                    pushConstants.OnBeginPass();
                } break;

                case Command::BeginRenderPass: {
                    auto* cmd = mCommands.NextCommand<BeginRenderPassCmd>();
                    currentRenderPass = ToBackend(cmd->renderPass.Get());
                    currentFramebuffer = ToBackend(cmd->framebuffer.Get());
                    currentSubpass = 0;
                } break;

                case Command::BeginRenderSubpass: {
                    mCommands.NextCommand<BeginRenderSubpassCmd>();
                    pushConstants.OnBeginPass();
                    inputBuffers.OnBeginPass();

                    // TODO(kainino@chromium.org): This is added to possibly
                    // work around an issue seen on Windows/Intel. It should
                    // break any feedback loop before the clears, even if
                    // there shouldn't be any negative effects from this.
                    // Investigate whether it's actually needed.
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                    // TODO(kainino@chromium.org): possible future
                    // optimization: create these framebuffers at
                    // Framebuffer build time (or maybe CommandBuffer build
                    // time) so they don't have to be created and destroyed
                    // at draw time.
                    glGenFramebuffers(1, &currentFBO);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, currentFBO);

                    const auto& subpass = currentRenderPass->GetSubpassInfo(currentSubpass);

                    // Mapping from attachmentSlot to GL framebuffer
                    // attachment points. Defaults to zero (GL_NONE).
                    std::array<GLenum, kMaxColorAttachments> drawBuffers = {};

                    // Construct GL framebuffer

                    unsigned int attachmentCount = 0;
                    for (unsigned int location : IterateBitSet(subpass.colorAttachmentsSet)) {
                        uint32_t attachment = subpass.colorAttachments[location];

                        auto textureView = currentFramebuffer->GetTextureView(attachment);
                        GLuint texture = ToBackend(textureView->GetTexture())->GetHandle();

                        // Attach color buffers.
                        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + location,
                                               GL_TEXTURE_2D, texture, 0);
                        drawBuffers[location] = GL_COLOR_ATTACHMENT0 + location;
                        attachmentCount = location + 1;

                        // TODO(kainino@chromium.org): the color clears (later in
                        // this function) may be undefined for other texture formats.
                        ASSERT(textureView->GetTexture()->GetFormat() ==
                               nxt::TextureFormat::R8G8B8A8Unorm);
                    }
                    glDrawBuffers(attachmentCount, drawBuffers.data());

                    if (subpass.depthStencilAttachmentSet) {
                        uint32_t attachmentSlot = subpass.depthStencilAttachment;

                        auto textureView = currentFramebuffer->GetTextureView(attachmentSlot);
                        GLuint texture = ToBackend(textureView->GetTexture())->GetHandle();
                        nxt::TextureFormat format = textureView->GetTexture()->GetFormat();

                        // Attach depth/stencil buffer.
                        GLenum glAttachment = 0;
                        // TODO(kainino@chromium.org): it may be valid to just always use
                        // GL_DEPTH_STENCIL_ATTACHMENT here.
                        if (TextureFormatHasDepth(format)) {
                            if (TextureFormatHasStencil(format)) {
                                glAttachment = GL_DEPTH_STENCIL_ATTACHMENT;
                            } else {
                                glAttachment = GL_DEPTH_ATTACHMENT;
                            }
                        } else {
                            glAttachment = GL_STENCIL_ATTACHMENT;
                        }

                        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, glAttachment, GL_TEXTURE_2D,
                                               texture, 0);

                        // TODO(kainino@chromium.org): the depth/stencil clears (later in
                        // this function) may be undefined for other texture formats.
                        ASSERT(format == nxt::TextureFormat::D32FloatS8Uint);
                    }

                    // Clear framebuffer attachments as needed

                    for (unsigned int location : IterateBitSet(subpass.colorAttachmentsSet)) {
                        uint32_t attachmentSlot = subpass.colorAttachments[location];
                        const auto& attachmentInfo =
                            currentRenderPass->GetAttachmentInfo(attachmentSlot);

                        // Only perform load op on first use
                        if (attachmentInfo.firstSubpass == currentSubpass) {
                            // Load op - color
                            if (attachmentInfo.colorLoadOp == nxt::LoadOp::Clear) {
                                const auto& clear = currentFramebuffer->GetClearColor(location);
                                glClearBufferfv(GL_COLOR, location, clear.color);
                            }
                        }
                    }

                    if (subpass.depthStencilAttachmentSet) {
                        uint32_t attachmentSlot = subpass.depthStencilAttachment;
                        const auto& attachmentInfo =
                            currentRenderPass->GetAttachmentInfo(attachmentSlot);

                        // Only perform load op on first use
                        if (attachmentInfo.firstSubpass == currentSubpass) {
                            // Load op - depth/stencil
                            const auto& clear = currentFramebuffer->GetClearDepthStencil(
                                subpass.depthStencilAttachment);
                            bool doDepthClear = TextureFormatHasDepth(attachmentInfo.format) &&
                                                (attachmentInfo.depthLoadOp == nxt::LoadOp::Clear);
                            bool doStencilClear =
                                TextureFormatHasStencil(attachmentInfo.format) &&
                                (attachmentInfo.stencilLoadOp == nxt::LoadOp::Clear);
                            if (doDepthClear && doStencilClear) {
                                glClearBufferfi(GL_DEPTH_STENCIL, 0, clear.depth, clear.stencil);
                            } else if (doDepthClear) {
                                glClearBufferfv(GL_DEPTH, 0, &clear.depth);
                            } else if (doStencilClear) {
                                const GLint clearStencil = clear.stencil;
                                glClearBufferiv(GL_STENCIL, 0, &clearStencil);
                            }
                        }
                    }

                    glBlendColor(0, 0, 0, 0);
                    glViewport(0, 0, currentFramebuffer->GetWidth(),
                               currentFramebuffer->GetHeight());
                } break;

                case Command::CopyBufferToBuffer: {
                    CopyBufferToBufferCmd* copy = mCommands.NextCommand<CopyBufferToBufferCmd>();
                    auto& src = copy->source;
                    auto& dst = copy->destination;

                    glBindBuffer(GL_PIXEL_PACK_BUFFER, ToBackend(src.buffer)->GetHandle());
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, ToBackend(dst.buffer)->GetHandle());
                    glCopyBufferSubData(GL_PIXEL_PACK_BUFFER, GL_PIXEL_UNPACK_BUFFER, src.offset,
                                        dst.offset, copy->size);

                    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                } break;

                case Command::CopyBufferToTexture: {
                    CopyBufferToTextureCmd* copy = mCommands.NextCommand<CopyBufferToTextureCmd>();
                    auto& src = copy->source;
                    auto& dst = copy->destination;
                    Buffer* buffer = ToBackend(src.buffer.Get());
                    Texture* texture = ToBackend(dst.texture.Get());
                    GLenum target = texture->GetGLTarget();
                    auto format = texture->GetGLFormat();

                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer->GetHandle());
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(target, texture->GetHandle());

                    ASSERT(texture->GetDimension() == nxt::TextureDimension::e2D);
                    glPixelStorei(GL_UNPACK_ROW_LENGTH,
                                  copy->rowPitch / TextureFormatPixelSize(texture->GetFormat()));
                    glTexSubImage2D(target, dst.level, dst.x, dst.y, dst.width, dst.height,
                                    format.format, format.type,
                                    reinterpret_cast<void*>(static_cast<uintptr_t>(src.offset)));
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                } break;

                case Command::CopyTextureToBuffer: {
                    CopyTextureToBufferCmd* copy = mCommands.NextCommand<CopyTextureToBufferCmd>();
                    auto& src = copy->source;
                    auto& dst = copy->destination;
                    Texture* texture = ToBackend(src.texture.Get());
                    Buffer* buffer = ToBackend(dst.buffer.Get());
                    auto format = texture->GetGLFormat();

                    // The only way to move data from a texture to a buffer in GL is via
                    // glReadPixels with a pack buffer. Create a temporary FBO for the copy.
                    ASSERT(texture->GetDimension() == nxt::TextureDimension::e2D);
                    glBindTexture(GL_TEXTURE_2D, texture->GetHandle());

                    GLuint readFBO = 0;
                    glGenFramebuffers(1, &readFBO);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFBO);

                    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                           texture->GetHandle(), src.level);

                    glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer->GetHandle());
                    glPixelStorei(GL_PACK_ROW_LENGTH,
                                  copy->rowPitch / TextureFormatPixelSize(texture->GetFormat()));
                    ASSERT(src.depth == 1 && src.z == 0);
                    void* offset = reinterpret_cast<void*>(static_cast<uintptr_t>(dst.offset));
                    glReadPixels(src.x, src.y, src.width, src.height, format.format, format.type,
                                 offset);
                    glPixelStorei(GL_PACK_ROW_LENGTH, 0);

                    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                    glDeleteFramebuffers(1, &readFBO);
                } break;

                case Command::Dispatch: {
                    DispatchCmd* dispatch = mCommands.NextCommand<DispatchCmd>();
                    pushConstants.Apply(lastPipeline, lastGLPipeline);
                    glDispatchCompute(dispatch->x, dispatch->y, dispatch->z);
                    // TODO(cwallez@chromium.org): add barriers to the API
                    glMemoryBarrier(GL_ALL_BARRIER_BITS);
                } break;

                case Command::DrawArrays: {
                    DrawArraysCmd* draw = mCommands.NextCommand<DrawArraysCmd>();
                    pushConstants.Apply(lastPipeline, lastGLPipeline);
                    inputBuffers.Apply();

                    if (draw->firstInstance > 0) {
                        glDrawArraysInstancedBaseInstance(
                            lastRenderPipeline->GetGLPrimitiveTopology(), draw->firstVertex,
                            draw->vertexCount, draw->instanceCount, draw->firstInstance);
                    } else {
                        // This branch is only needed on OpenGL < 4.2
                        glDrawArraysInstanced(lastRenderPipeline->GetGLPrimitiveTopology(),
                                              draw->firstVertex, draw->vertexCount,
                                              draw->instanceCount);
                    }
                } break;

                case Command::DrawElements: {
                    DrawElementsCmd* draw = mCommands.NextCommand<DrawElementsCmd>();
                    pushConstants.Apply(lastPipeline, lastGLPipeline);
                    inputBuffers.Apply();

                    nxt::IndexFormat indexFormat = lastRenderPipeline->GetIndexFormat();
                    size_t formatSize = IndexFormatSize(indexFormat);
                    GLenum formatType = IndexFormatType(indexFormat);

                    if (draw->firstInstance > 0) {
                        glDrawElementsInstancedBaseInstance(
                            lastRenderPipeline->GetGLPrimitiveTopology(), draw->indexCount,
                            formatType,
                            reinterpret_cast<void*>(draw->firstIndex * formatSize +
                                                    indexBufferOffset),
                            draw->instanceCount, draw->firstInstance);
                    } else {
                        // This branch is only needed on OpenGL < 4.2
                        glDrawElementsInstanced(
                            lastRenderPipeline->GetGLPrimitiveTopology(), draw->indexCount,
                            formatType,
                            reinterpret_cast<void*>(draw->firstIndex * formatSize +
                                                    indexBufferOffset),
                            draw->instanceCount);
                    }
                } break;

                case Command::EndComputePass: {
                    mCommands.NextCommand<EndComputePassCmd>();
                } break;

                case Command::EndRenderPass: {
                    mCommands.NextCommand<EndRenderPassCmd>();
                } break;

                case Command::EndRenderSubpass: {
                    mCommands.NextCommand<EndRenderSubpassCmd>();
                    glDeleteFramebuffers(1, &currentFBO);
                    currentFBO = 0;
                    currentSubpass += 1;
                } break;

                case Command::SetComputePipeline: {
                    SetComputePipelineCmd* cmd = mCommands.NextCommand<SetComputePipelineCmd>();
                    ToBackend(cmd->pipeline)->ApplyNow();
                    lastGLPipeline = ToBackend(cmd->pipeline).Get();
                    lastPipeline = ToBackend(cmd->pipeline).Get();
                    pushConstants.OnSetPipeline(lastPipeline);
                } break;

                case Command::SetRenderPipeline: {
                    SetRenderPipelineCmd* cmd = mCommands.NextCommand<SetRenderPipelineCmd>();
                    ToBackend(cmd->pipeline)->ApplyNow(persistentPipelineState);
                    lastRenderPipeline = ToBackend(cmd->pipeline).Get();
                    lastGLPipeline = ToBackend(cmd->pipeline).Get();
                    lastPipeline = ToBackend(cmd->pipeline).Get();

                    pushConstants.OnSetPipeline(lastPipeline);
                    inputBuffers.OnSetPipeline(lastRenderPipeline);
                } break;

                case Command::SetPushConstants: {
                    SetPushConstantsCmd* cmd = mCommands.NextCommand<SetPushConstantsCmd>();
                    uint32_t* data = mCommands.NextData<uint32_t>(cmd->count);
                    pushConstants.OnSetPushConstants(cmd->stages, cmd->count, cmd->offset, data);
                } break;

                case Command::SetStencilReference: {
                    SetStencilReferenceCmd* cmd = mCommands.NextCommand<SetStencilReferenceCmd>();
                    persistentPipelineState.SetStencilReference(cmd->reference);
                } break;

                case Command::SetBlendColor: {
                    SetBlendColorCmd* cmd = mCommands.NextCommand<SetBlendColorCmd>();
                    glBlendColor(cmd->r, cmd->g, cmd->b, cmd->a);
                } break;

                case Command::SetBindGroup: {
                    SetBindGroupCmd* cmd = mCommands.NextCommand<SetBindGroupCmd>();
                    size_t groupIndex = cmd->index;
                    BindGroup* group = ToBackend(cmd->group.Get());

                    const auto& indices =
                        ToBackend(lastPipeline->GetLayout())->GetBindingIndexInfo()[groupIndex];
                    const auto& layout = group->GetLayout()->GetBindingInfo();

                    for (uint32_t binding : IterateBitSet(layout.mask)) {
                        switch (layout.types[binding]) {
                            case nxt::BindingType::UniformBuffer: {
                                BufferView* view =
                                    ToBackend(group->GetBindingAsBufferView(binding));
                                GLuint buffer = ToBackend(view->GetBuffer())->GetHandle();
                                GLuint uboIndex = indices[binding];

                                glBindBufferRange(GL_UNIFORM_BUFFER, uboIndex, buffer,
                                                  view->GetOffset(), view->GetSize());
                            } break;

                            case nxt::BindingType::Sampler: {
                                GLuint sampler =
                                    ToBackend(group->GetBindingAsSampler(binding))->GetHandle();
                                GLuint samplerIndex = indices[binding];

                                for (auto unit :
                                     lastGLPipeline->GetTextureUnitsForSampler(samplerIndex)) {
                                    glBindSampler(unit, sampler);
                                }
                            } break;

                            case nxt::BindingType::SampledTexture: {
                                TextureView* view =
                                    ToBackend(group->GetBindingAsTextureView(binding));
                                Texture* texture = ToBackend(view->GetTexture());
                                GLuint handle = texture->GetHandle();
                                GLenum target = texture->GetGLTarget();
                                GLuint textureIndex = indices[binding];

                                for (auto unit :
                                     lastGLPipeline->GetTextureUnitsForTexture(textureIndex)) {
                                    glActiveTexture(GL_TEXTURE0 + unit);
                                    glBindTexture(target, handle);
                                }
                            } break;

                            case nxt::BindingType::StorageBuffer: {
                                BufferView* view =
                                    ToBackend(group->GetBindingAsBufferView(binding));
                                GLuint buffer = ToBackend(view->GetBuffer())->GetHandle();
                                GLuint ssboIndex = indices[binding];

                                glBindBufferRange(GL_SHADER_STORAGE_BUFFER, ssboIndex, buffer,
                                                  view->GetOffset(), view->GetSize());
                            } break;
                        }
                    }
                } break;

                case Command::SetIndexBuffer: {
                    SetIndexBufferCmd* cmd = mCommands.NextCommand<SetIndexBufferCmd>();
                    indexBufferOffset = cmd->offset;
                    inputBuffers.OnSetIndexBuffer(cmd->buffer.Get());
                } break;

                case Command::SetVertexBuffers: {
                    SetVertexBuffersCmd* cmd = mCommands.NextCommand<SetVertexBuffersCmd>();
                    auto buffers = mCommands.NextData<Ref<BufferBase>>(cmd->count);
                    auto offsets = mCommands.NextData<uint32_t>(cmd->count);
                    inputBuffers.OnSetVertexBuffers(cmd->startSlot, cmd->count, buffers, offsets);
                } break;

                case Command::TransitionBufferUsage: {
                    TransitionBufferUsageCmd* cmd =
                        mCommands.NextCommand<TransitionBufferUsageCmd>();

                    cmd->buffer->UpdateUsageInternal(cmd->usage);
                } break;

                case Command::TransitionTextureUsage: {
                    TransitionTextureUsageCmd* cmd =
                        mCommands.NextCommand<TransitionTextureUsageCmd>();

                    cmd->texture->UpdateUsageInternal(cmd->usage);
                } break;
            }
        }

        // HACK: cleanup a tiny bit of state to make this work with
        // virtualized contexts enabled in Chromium
        glBindSampler(0, 0);
    }

}}  // namespace backend::opengl
