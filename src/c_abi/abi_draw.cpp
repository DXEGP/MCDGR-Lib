module;

#include "_d3d12_module_prelude.h"
#include "mcdr_graphics_recording.h"
#include <limits>

module mcdr.abi.common;

#include "mcdr_abi.h"

import mcdr.d3d12.device;
import mcdr.d3d12.mesh;
import mcdr.d3d12.async_bundle;
import mcdr.d3d12.async_descriptor;
import mcdr.d3d12.async_executor;
import mcdr.d3d12.async_fence;
import mcdr.error;
import mcdr.render.vanilla;

using namespace d3d12_mc;

namespace {

thread_local std::vector<d3d12_mc::DrawBinding> gBindingScratch;
thread_local std::vector<d3d12_mc::DrawBinding> gInputBindingScratch;

class RasterInputScope {
public:
    explicit RasterInputScope(void* context) : context(context) {}

    ~RasterInputScope() {
        if (token) fail();
    }

    int begin(bool indexed, void* indirectCommands, const void* extraBindings = nullptr,
              uint32_t extraBindingCount = 0) {
        return d3d12_mc::vanilla::beginRasterInputPass(context, indexed ? 1u : 0u,
            indirectCommands, extraBindings, extraBindingCount, &token);
    }

    mcdr::Result<void> beforeDraw() {
        if (!token) return {};
        return d3d12_mc::applyGraphicsInputReads(context);
    }

    void fail() {
        d3d12_mc::vanilla::failRasterInputPass(context);
        d3d12_mc::completeGraphicsCommandRecording(context, mcdr::graphics_recording::Completion::Failed);
        token = nullptr;
    }

    int finish() {
        const int result = d3d12_mc::vanilla::finishRasterInputPass(context, token);
        token = nullptr;
        const int completion = d3d12_mc::completeGraphicsCommandRecording(context,
            result == 1 ? mcdr::graphics_recording::Completion::Succeeded : mcdr::graphics_recording::Completion::Failed);
        return result == 1 && completion == 1 ? 1 : -1;
    }

private:
    void* context;
    void* token = nullptr;
};

template <typename Record>
int32_t recordRasterInputs(const char* operation, mcdr_command_ctx* context, bool indexed,
                           mcdr_object* indirectCommands, Record&& record) {
    RasterInputScope inputs(context);
    if (inputs.begin(indexed, indirectCommands) < 0)
        return mcdr::abi::failWith(operation, d3d12_mc::vanilla::lastFailureText());
    if (const auto ready = inputs.beforeDraw(); !ready) {
        inputs.fail();
        return mcdr::abi::failWith(operation, ready.error().message);
    }
    if (const auto result = record(); !result) {
        inputs.fail();
        return mcdr::abi::failWith(operation, result.error().message);
    }
    if (inputs.finish() != 1)
        return mcdr::abi::failWith(operation, d3d12_mc::vanilla::lastFailureText());
    return 1;
}

}

extern "C" {

int32_t MCDR_CALL mcdr_dx12SetAdditionalColorTarget(mcdr_command_ctx* ctx, mcdr_object* target) {
    if (const auto result = d3d12_mc::setAdditionalColorTarget(ctx, target); !result)
        return mcdr::abi::failWith("mcdr_dx12SetAdditionalColorTarget", result.error().message);
    if (target && d3d12_mc::vanilla::declareAdditionalRasterAttachment(ctx, target) != 1) {
        d3d12_mc::vanilla::failRasterInputPass(ctx);
        return mcdr::abi::failWith("mcdr_dx12SetAdditionalColorTarget", d3d12_mc::vanilla::lastFailureText());
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12SetRasterMotionTarget(
    mcdr_command_ctx* ctx, mcdr_object* worldColor, mcdr_object* worldDepth, mcdr_object* motion) {
    if (const auto result = d3d12_mc::setRasterMotionTarget(ctx, worldColor, worldDepth, motion); !result) {
        return mcdr::abi::failWith("mcdr_dx12SetRasterMotionTarget", result.error().message);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12SetPipeline(mcdr_command_ctx* ctx, mcdr_pipeline* pipeline, int32_t hasDepth) {
    if (const auto r = d3d12_mc::setPipeline(ctx, pipeline, hasDepth != 0); !r) {
        return mcdr::abi::failWith("mcdr_dx12SetPipeline", r.error().message);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12SetScissor(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t width,
                                      int32_t height) {
    if (const auto r = d3d12_mc::setScissor(ctx, x, y, width, height); !r) {
        return mcdr::abi::failWith("mcdr_dx12SetScissor", r.error().message);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12SetViewport(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t width,
                                       int32_t height) {
    if (const auto r = d3d12_mc::setViewport(ctx, x, y, width, height); !r) {
        return mcdr::abi::failWith("mcdr_dx12SetViewport", r.error().message);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12SetVertexBuffer(mcdr_command_ctx* ctx, int32_t slot, mcdr_object* buffer,
                                           int64_t offset, int32_t stride) {
    if (const auto r = d3d12_mc::setVertexBuffer(ctx, slot, buffer, offset, stride); !r) {
        return mcdr::abi::failWith("mcdr_dx12SetVertexBuffer", r.error().message);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12SetVertexBufferRangeV1(mcdr_command_ctx* ctx, int32_t slot, mcdr_object* buffer,
    int64_t offset, int64_t length, int32_t stride) {
    if (length <= 0) return mcdr::abi::failWith("mcdr_dx12SetVertexBufferRangeV1", "Vertex range is empty");
    if (const auto result = d3d12_mc::setVertexBuffer(ctx, slot, buffer, offset, stride, length); !result)
        return mcdr::abi::failWith("mcdr_dx12SetVertexBufferRangeV1", result.error().message);
    return 1;
}

int32_t MCDR_CALL mcdr_dx12SetIndexBuffer(mcdr_command_ctx* ctx, mcdr_object* buffer, int32_t indexType) {
    if (const auto r = d3d12_mc::setIndexBuffer(ctx, buffer, indexType); !r) {
        return mcdr::abi::failWith("mcdr_dx12SetIndexBuffer", r.error().message);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12PushDescriptors(mcdr_command_ctx* ctx, const mcdr_binding_slot* bindings,
                                           int32_t count) {
    if (count <= 0) {
        return 1;
    }
    if (bindings == nullptr) {
        return mcdr::abi::failWith("mcdr_dx12PushDescriptors", "bindings is null");
    }

    gBindingScratch.resize(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        const mcdr_binding_slot& src = bindings[i];
        d3d12_mc::DrawBinding& dst = gBindingScratch[static_cast<size_t>(i)];
        dst.type = static_cast<uint8_t>(src.type);
        dst.buffer = src.buffer;
        dst.offset = src.offset;
        dst.length = src.length;
        dst.texelFormat = src.texelFormat;
        dst.view = src.view;
    }

    if (const auto r = d3d12_mc::pushDescriptors(ctx, gBindingScratch); !r) {
        return mcdr::abi::failWith("mcdr_dx12PushDescriptors", r.error().message);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12DrawIndexed(mcdr_command_ctx* ctx, int32_t indexCount, int32_t instanceCount,
                                       int32_t firstIndex, int32_t baseVertex, int32_t firstInstance) {
    return recordRasterInputs("mcdr_dx12DrawIndexed", ctx, true, nullptr, [&] {
        return d3d12_mc::drawIndexedInstanced(ctx, static_cast<UINT>(indexCount),
            static_cast<UINT>(instanceCount), static_cast<INT>(firstIndex),
            static_cast<INT>(baseVertex), static_cast<UINT>(firstInstance));
    });
}

int32_t MCDR_CALL mcdr_dx12Draw(mcdr_command_ctx* ctx, int32_t vertexCount, int32_t instanceCount,
                                int32_t firstVertex, int32_t firstInstance) {
    return recordRasterInputs("mcdr_dx12Draw", ctx, false, nullptr, [&] {
        return d3d12_mc::drawInstanced(ctx, static_cast<UINT>(vertexCount),
            static_cast<UINT>(instanceCount), static_cast<UINT>(firstVertex),
            static_cast<UINT>(firstInstance));
    });
}

int32_t MCDR_CALL mcdr_dx12MultiDrawIndexedPacked(mcdr_command_ctx* ctx, const int32_t* drawParameters,
                                                  int32_t instanceCount, int32_t firstInstance,
                                                  int32_t drawCount) {
    if (drawCount < 0 || (drawCount > 0 && drawParameters == nullptr)) {
        return mcdr::abi::failWith("mcdr_dx12MultiDrawIndexedPacked", "invalid draw array");
    }
    if (drawCount == 0) return 1;
    return recordRasterInputs("mcdr_dx12MultiDrawIndexedPacked", ctx, true, nullptr, [&]() -> mcdr::Result<void> {
        for (int32_t drawIndex = 0; drawIndex < drawCount; ++drawIndex) {
            const int32_t* draw = drawParameters + static_cast<size_t>(drawIndex) * 3;
            if (auto result = d3d12_mc::drawIndexedInstanced(ctx, static_cast<UINT>(draw[1]),
                static_cast<UINT>(instanceCount), static_cast<INT>(draw[0]),
                static_cast<INT>(draw[2]), static_cast<UINT>(firstInstance)); !result) return result;
        }
        return {};
    });
}

int32_t MCDR_CALL mcdr_dx12MultiDrawBound(
    mcdr_command_ctx* ctx, int32_t indexed, const int32_t* drawParameters,
    int32_t instanceCount, int32_t firstInstance, int32_t drawCount,
    const mcdr_binding_slot* sharedBindings, int32_t bindingCount,
    const int32_t* overrideIndices, int32_t overrideCount,
    const mcdr_binding_slot* drawBindings) {
    constexpr const char* operation = "mcdr_dx12MultiDrawBound";
    if ((indexed != 0 && indexed != 1) || drawCount < 0 || bindingCount < 0 ||
        overrideCount < 0 || overrideCount > bindingCount || instanceCount < 0 || firstInstance < 0) {
        return mcdr::abi::failWith(operation, "invalid draw mode or array counts");
    }
    if (drawCount == 0) return 1;
    if (!ctx || !drawParameters || (bindingCount > 0 && !sharedBindings) ||
        (overrideCount > 0 && (!overrideIndices || !drawBindings))) {
        return mcdr::abi::failWith(operation, "missing context, draw array, or bindings");
    }
    const auto draws = static_cast<size_t>(drawCount);
    const auto bindings = static_cast<size_t>(bindingCount);
    const auto overrides = static_cast<size_t>(overrideCount);
    constexpr size_t maximumBytes = std::numeric_limits<size_t>::max();
    if (draws > maximumBytes / (3 * sizeof(int32_t)) ||
        bindings > maximumBytes / sizeof(mcdr_binding_slot) ||
        bindings > maximumBytes / sizeof(d3d12_mc::DrawBinding) ||
        (overrides != 0 && draws > maximumBytes / sizeof(mcdr_binding_slot) / overrides)) {
        return mcdr::abi::failWith(operation, "draw or binding array size overflow");
    }
    for (size_t drawIndex = 0; drawIndex < draws; ++drawIndex) {
        const int32_t* draw = drawParameters + drawIndex * 3;
        if (draw[0] < 0 || draw[1] < 0) {
            return mcdr::abi::failWith(operation, "draw first or count is negative");
        }
    }
    for (size_t bindingIndex = 0; bindingIndex < bindings; ++bindingIndex) {
        if (sharedBindings[bindingIndex].type < 0 || sharedBindings[bindingIndex].type > 2) {
            return mcdr::abi::failWith(operation, "unknown shared binding type");
        }
    }
    for (size_t overrideIndex = 0; overrideIndex < overrides; ++overrideIndex) {
        if (overrideIndices[overrideIndex] < 0 || overrideIndices[overrideIndex] >= bindingCount) {
            return mcdr::abi::failWith(operation, "binding override index is out of range");
        }
    }
    for (size_t bindingIndex = 0; bindingIndex < draws * overrides; ++bindingIndex) {
        const auto& binding = drawBindings[bindingIndex];
        if (binding.type < 0 || binding.type > 2) {
            return mcdr::abi::failWith(operation, "unknown per-draw binding type");
        }
        const auto slot = static_cast<size_t>(overrideIndices[bindingIndex % overrides]);
        if (binding.type != sharedBindings[slot].type) {
            return mcdr::abi::failWith(operation, "binding override changes the shared binding layout");
        }
    }
    const auto copyBinding = [](const mcdr_binding_slot& source, d3d12_mc::DrawBinding& target) {
        target.type = static_cast<uint8_t>(source.type);
        target.buffer = source.buffer;
        target.offset = source.offset;
        target.length = source.length;
        target.texelFormat = source.texelFormat;
        target.view = source.view;
    };
    gBindingScratch.resize(bindings);
    for (size_t bindingIndex = 0; bindingIndex < bindings; ++bindingIndex) {
        copyBinding(sharedBindings[bindingIndex], gBindingScratch[bindingIndex]);
    }
    gInputBindingScratch.clear();
    for (size_t drawIndex = 0; drawIndex < draws; ++drawIndex) {
        for (size_t overrideIndex = 0; overrideIndex < overrides; ++overrideIndex) {
            bool replaced = false;
            for (size_t later = overrideIndex + 1; later < overrides; ++later) {
                if (overrideIndices[later] == overrideIndices[overrideIndex]) { replaced = true; break; }
            }
            if (replaced) continue;
            auto& extra = gInputBindingScratch.emplace_back();
            copyBinding(drawBindings[drawIndex * overrides + overrideIndex], extra);
        }
    }
    if (gInputBindingScratch.size() > UINT32_MAX)
        return mcdr::abi::failWith(operation, "input binding union exceeds native limits");
    for (size_t overrideIndex = 0; overrideIndex < overrides; ++overrideIndex) {
        copyBinding(drawBindings[overrideIndex], gBindingScratch[static_cast<size_t>(overrideIndices[overrideIndex])]);
    }
    if (const auto pushed = d3d12_mc::pushDescriptors(ctx, gBindingScratch); !pushed)
        return mcdr::abi::failWith(operation, pushed.error().message);
    RasterInputScope inputs(ctx);
    if (inputs.begin(indexed != 0, nullptr, gInputBindingScratch.data(),
        static_cast<uint32_t>(gInputBindingScratch.size())) < 0)
        return mcdr::abi::failWith(operation, d3d12_mc::vanilla::lastFailureText());
    for (size_t drawIndex = 0; drawIndex < draws; ++drawIndex) {
        if (drawIndex != 0) {
            for (size_t overrideIndex = 0; overrideIndex < overrides; ++overrideIndex) {
                copyBinding(drawBindings[drawIndex * overrides + overrideIndex],
                    gBindingScratch[static_cast<size_t>(overrideIndices[overrideIndex])]);
            }
            if (const auto result = d3d12_mc::pushDescriptors(ctx, gBindingScratch); !result) {
                inputs.fail();
                return mcdr::abi::failWith(operation, result.error().message);
            }
        }
        if (const auto ready = inputs.beforeDraw(); !ready) {
            inputs.fail();
            return mcdr::abi::failWith(operation, ready.error().message);
        }
        const int32_t* draw = drawParameters + drawIndex * 3;
        const auto result = indexed != 0
            ? d3d12_mc::drawIndexedInstanced(ctx, static_cast<UINT>(draw[1]),
                static_cast<UINT>(instanceCount), static_cast<INT>(draw[0]),
                static_cast<INT>(draw[2]), static_cast<UINT>(firstInstance))
            : d3d12_mc::drawInstanced(ctx, static_cast<UINT>(draw[1]),
                static_cast<UINT>(instanceCount), static_cast<UINT>(draw[0]),
                static_cast<UINT>(firstInstance));
        if (!result) {
            inputs.fail();
            return mcdr::abi::failWith(operation, result.error().message);
        }
    }
    if (inputs.finish() != 1) return mcdr::abi::failWith(operation, d3d12_mc::vanilla::lastFailureText());
    return 1;
}

int32_t MCDR_CALL mcdr_dx12MultiDrawIndexedSeparate(mcdr_command_ctx* ctx, const int32_t* firstIndices,
                                                    const int32_t* indexCounts, const int32_t* baseVertices,
                                                    int32_t instanceCount, int32_t firstInstance,
                                                    int32_t drawCount) {
    if (drawCount < 0 || (drawCount > 0 && (firstIndices == nullptr || indexCounts == nullptr
                                            || baseVertices == nullptr))) {
        return mcdr::abi::failWith("mcdr_dx12MultiDrawIndexedSeparate", "invalid draw array");
    }
    if (drawCount == 0) return 1;
    return recordRasterInputs("mcdr_dx12MultiDrawIndexedSeparate", ctx, true, nullptr, [&]() -> mcdr::Result<void> {
        for (int32_t drawIndex = 0; drawIndex < drawCount; ++drawIndex) {
            if (auto result = d3d12_mc::drawIndexedInstanced(ctx, static_cast<UINT>(indexCounts[drawIndex]),
                static_cast<UINT>(instanceCount), static_cast<INT>(firstIndices[drawIndex]),
                static_cast<INT>(baseVertices[drawIndex]), static_cast<UINT>(firstInstance)); !result) return result;
        }
        return {};
    });
}

int32_t MCDR_CALL mcdr_dx12MultiDrawPacked(mcdr_command_ctx* ctx, const int32_t* drawParameters,
                                           int32_t instanceCount, int32_t firstInstance,
                                           int32_t drawCount) {
    if (drawCount < 0 || (drawCount > 0 && drawParameters == nullptr)) {
        return mcdr::abi::failWith("mcdr_dx12MultiDrawPacked", "invalid draw array");
    }
    if (drawCount == 0) return 1;
    return recordRasterInputs("mcdr_dx12MultiDrawPacked", ctx, false, nullptr, [&]() -> mcdr::Result<void> {
        for (int32_t drawIndex = 0; drawIndex < drawCount; ++drawIndex) {
            const int32_t* draw = drawParameters + static_cast<size_t>(drawIndex) * 2;
            if (auto result = d3d12_mc::drawInstanced(ctx, static_cast<UINT>(draw[1]),
                static_cast<UINT>(instanceCount), static_cast<UINT>(draw[0]),
                static_cast<UINT>(firstInstance)); !result) return result;
        }
        return {};
    });
}

int32_t MCDR_CALL mcdr_dx12MultiDrawSeparate(mcdr_command_ctx* ctx, const int32_t* firstVertices,
                                             const int32_t* vertexCounts, int32_t drawCount) {
    if (drawCount < 0 || (drawCount > 0 && (firstVertices == nullptr || vertexCounts == nullptr))) {
        return mcdr::abi::failWith("mcdr_dx12MultiDrawSeparate", "invalid draw array");
    }
    if (drawCount == 0) return 1;
    return recordRasterInputs("mcdr_dx12MultiDrawSeparate", ctx, false, nullptr, [&]() -> mcdr::Result<void> {
        for (int32_t drawIndex = 0; drawIndex < drawCount; ++drawIndex) {
            if (auto result = d3d12_mc::drawInstanced(ctx, static_cast<UINT>(vertexCounts[drawIndex]), 1,
                static_cast<UINT>(firstVertices[drawIndex]), 0); !result) return result;
        }
        return {};
    });
}

int32_t MCDR_CALL mcdr_dx12DrawIndexedIndirect(mcdr_command_ctx* ctx, mcdr_object* commands, int64_t offset,
                                               int32_t drawCount) {
    return recordRasterInputs("mcdr_dx12DrawIndexedIndirect", ctx, true, commands, [&] {
        return d3d12_mc::drawIndexedIndirect(ctx, commands, offset, static_cast<UINT>(drawCount));
    });
}

int32_t MCDR_CALL mcdr_dx12DrawIndirect(mcdr_command_ctx* ctx, mcdr_object* commands, int64_t offset,
                                        int32_t drawCount) {
    return recordRasterInputs("mcdr_dx12DrawIndirect", ctx, false, commands, [&] {
        return d3d12_mc::drawIndirect(ctx, commands, offset, static_cast<UINT>(drawCount));
    });
}

}
