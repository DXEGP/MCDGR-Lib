module;

#include "_d3d12_module_prelude.h"

export module mcdr.d3d12.workgraph;

import mcdr.error;

export namespace mcdr::workgraph {

struct RootBinding {
    uint32_t kind;
    uint32_t shaderRegister;
    uint32_t registerSpace;
};

struct ProgramDescription {
    const void* dxil;
    size_t dxilSize;
    const wchar_t* programName;
    const wchar_t* entryName;
    uint32_t entryArrayIndex;
    uint32_t recordSize;
    const RootBinding* bindings;
    uint32_t bindingCount;
    uint64_t backingBudget;
    uint64_t backingInstanceBudget = 0;
};

mcdr::Result<void*> createProgram(const ProgramDescription& description);
void destroyProgram(void* program);
mcdr::Result<void> dispatch(void* context, void* program, const void* records,
                           uint32_t recordCount, uint32_t recordStride,
                           const uint64_t* rootAddresses, uint32_t addressCount);
mcdr::Result<void> dispatchGpu(void* context, void* program, uint64_t inputAddress,
                              const uint64_t* rootAddresses, uint32_t addressCount);
// Schema 1: D3D12_NODE_GPU_INPUT at inputOffset, with all referenced records
// contained in the declared input span. The trusted GPU producer owns that
// descriptor's contents; this validates/pins the resource spans, not GPU bytes.
// No GPU virtual address is accepted from the host-facing caller.
mcdr::Result<void> dispatchGpuBuffers(void* context, void* program, uint32_t inputSchema,
                                     void* inputBuffer, uint64_t inputOffset, uint64_t inputBytes,
                                     void* const* buffers, const uint64_t* offsets,
                                     const uint64_t* lengths, uint32_t bindingCount);
mcdr::Result<void> dispatchBuffers(void* context, void* program, const void* records,
                                  uint32_t recordCount, uint32_t recordStride,
                                  void* const* buffers, const uint64_t* offsets,
                                  const uint64_t* lengths, uint32_t bindingCount);

}
