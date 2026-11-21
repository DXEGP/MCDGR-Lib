module;

#include "_d3d12_module_prelude.h"

export module mcdr.d3d12.async_bundle;

import mcdr.error;

namespace {

std::string hrText(HRESULT hr) {
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(hr));
    return std::string(buf);
}

}

export namespace d3d12_mc {

using Microsoft::WRL::ComPtr;

constexpr UINT kBundleFrameSlots = 4;

constexpr UINT kBundleBatchesPerFrame = 16;
constexpr UINT kBundleSlotCount = kBundleFrameSlots * kBundleBatchesPerFrame;

D3D12_PRIMITIVE_TOPOLOGY bundlePrimitiveTopology(int ordinal);

class BundleRecorder {
public:
    BundleRecorder() = default;
    ~BundleRecorder() = default;

    BundleRecorder(const BundleRecorder&) = delete;
    BundleRecorder& operator=(const BundleRecorder&) = delete;

    mcdr::Result<void> init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT workerId,
                            ID3D12CommandSignature* cmdSigIndexed, ID3D12CommandSignature* cmdSigNonIndexed);

    bool valid() const { return m_bundle != nullptr; }
    UINT workerId() const { return m_workerId; }
    bool isRecording() const { return m_recording; }
    ID3D12GraphicsCommandList* commandList() const { return m_bundle.Get(); }

    bool begin(UINT frameSlot, UINT64 frameValue);
    ID3D12GraphicsCommandList* end();

    void setRootSignature(ID3D12RootSignature* rootSignature);
    void setPipelineState(ID3D12PipelineState* pso);
    void setCorrectedStride(int slot, UINT stride);
    UINT correctedStride(int slot) const {
        const auto stride = m_correctedStrides.find(slot);
        return stride == m_correctedStrides.end() ? 0u : stride->second;
    }
    void setDescriptorTable(UINT slot, D3D12_GPU_DESCRIPTOR_HANDLE handle);
    void setRootConstantBufferView(UINT rootIndex, D3D12_GPU_VIRTUAL_ADDRESS address);
    void setVertexBuffers(UINT startSlot, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views);
    void setIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view);
    void setPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology);
    void drawIndexedInstanced(UINT indexCount, UINT instanceCount, UINT startIndexLocation,
                              INT baseVertexLocation, UINT startInstanceLocation);
    void drawInstanced(UINT vertexCount, UINT instanceCount, UINT startVertexLocation,
                       UINT startInstanceLocation);
    bool drawIndexedIndirect(ID3D12Resource* commands, UINT64 offset, UINT drawCount);
    bool drawIndirect(ID3D12Resource* commands, UINT64 offset, UINT drawCount);

    struct Stats {
        UINT64 commandsRecorded = 0;
        UINT64 drawCalls = 0;
        UINT64 indexCount = 0;
        UINT64 vertexCount = 0;
        UINT64 descriptorUpdates = 0;
    };
    Stats stats() const { return m_stats; }
    void resetStats() { m_stats = Stats{}; }

private:
    ComPtr<ID3D12CommandAllocator> m_allocators[kBundleSlotCount];
    ComPtr<ID3D12GraphicsCommandList> m_lists[kBundleSlotCount];
    UINT m_activeSlot = 0;
    UINT64 m_frameOfSlot[kBundleFrameSlots] = {};
    UINT m_batchOfSlot[kBundleFrameSlots] = {};
    ComPtr<ID3D12GraphicsCommandList> m_bundle;
    ComPtr<ID3D12CommandSignature> m_cmdSigIndexed;
    ComPtr<ID3D12CommandSignature> m_cmdSigNonIndexed;
    ComPtr<ID3D12DescriptorHeap> m_heap;

    ID3D12Device* m_device = nullptr;
    UINT m_workerId = 0;
    bool m_recording = false;

    ID3D12RootSignature* m_rootSig = nullptr;
    ID3D12PipelineState* m_pso = nullptr;
    D3D12_PRIMITIVE_TOPOLOGY m_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    std::unordered_map<int, UINT> m_correctedStrides;

    Stats m_stats;
};

class BundleRecorderPool {
public:
    BundleRecorderPool() = default;
    ~BundleRecorderPool() = default;

    BundleRecorderPool(const BundleRecorderPool&) = delete;
    BundleRecorderPool& operator=(const BundleRecorderPool&) = delete;

    mcdr::Result<void> init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT workerCount,
                            ID3D12CommandSignature* cmdSigIndexed, ID3D12CommandSignature* cmdSigNonIndexed);
    void shutdown();

    bool valid() const { return !m_recorders.empty(); }
    UINT workerCount() const { return static_cast<UINT>(m_recorders.size()); }

    BundleRecorder* recorder(UINT worker);

private:
    std::vector<std::unique_ptr<BundleRecorder>> m_recorders;
};

void executeBundle(ID3D12GraphicsCommandList* mainList, ID3D12GraphicsCommandList* bundle);

D3D12_PRIMITIVE_TOPOLOGY bundlePrimitiveTopology(int ordinal) {
    switch (ordinal) {
        case 0:
        case 1: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case 2: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case 3: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        case 5: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case 6: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLEFAN;
        case 7: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

mcdr::Result<void> BundleRecorder::init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT workerId,
                                       ID3D12CommandSignature* cmdSigIndexed,
                                       ID3D12CommandSignature* cmdSigNonIndexed) {
    if (!device) return mcdr::err("BundleRecorder::init", "device is null");

    m_device = device;
    m_workerId = workerId;
    m_heap = heap;

    for (UINT i = 0; i < kBundleSlotCount; ++i) {
        HRESULT hr =
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&m_allocators[i]));
        if (FAILED(hr))
            return mcdr::err("BundleRecorder::init", "CreateCommandAllocator failed: " + hrText(hr));

        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, m_allocators[i].Get(), nullptr,
                                       IID_PPV_ARGS(&m_lists[i]));
        if (FAILED(hr))
            return mcdr::err("BundleRecorder::init", "CreateCommandList failed: " + hrText(hr));

        m_lists[i]->Close();
    }
    m_recording = false;
    for (UINT i = 0; i < kBundleFrameSlots; ++i) {
        m_frameOfSlot[i] = UINT64_MAX;
        m_batchOfSlot[i] = 0;
    }

    m_cmdSigIndexed = cmdSigIndexed;
    m_cmdSigNonIndexed = cmdSigNonIndexed;
    return {};
}

bool BundleRecorder::begin(UINT frameSlot, UINT64 frameValue) {
    if (m_recording) return false;
    const UINT fs = frameSlot % kBundleFrameSlots;
    if (m_frameOfSlot[fs] != frameValue) {
        m_frameOfSlot[fs] = frameValue;
        m_batchOfSlot[fs] = 0;
    }
    const UINT batch = m_batchOfSlot[fs];
    if (batch >= kBundleBatchesPerFrame) return false;
    m_batchOfSlot[fs] = batch + 1;

    const UINT slot = fs * kBundleBatchesPerFrame + batch;
    m_activeSlot = slot;
    m_bundle = m_lists[slot];
    if (!m_bundle || !m_allocators[slot]) return false;
    HRESULT hr = m_allocators[slot]->Reset();
    if (FAILED(hr)) return false;
    hr = m_bundle->Reset(m_allocators[slot].Get(), nullptr);
    if (FAILED(hr)) return false;

    if (m_heap) {
        ID3D12DescriptorHeap* heaps[] = {m_heap.Get()};
        m_bundle->SetDescriptorHeaps(1, heaps);
    }

    m_recording = true;
    m_rootSig = nullptr;
    m_pso = nullptr;
    m_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    m_correctedStrides.clear();
    resetStats();
    return true;
}

ID3D12GraphicsCommandList* BundleRecorder::end() {
    if (!m_bundle || !m_recording) return nullptr;
    m_recording = false;
    if (FAILED(m_bundle->Close())) return nullptr;
    return m_bundle.Get();
}

void BundleRecorder::setRootSignature(ID3D12RootSignature* rootSignature) {
    if (!m_recording || !rootSignature || m_rootSig == rootSignature) return;
    m_bundle->SetGraphicsRootSignature(rootSignature);
    m_rootSig = rootSignature;
    ++m_stats.commandsRecorded;
}

void BundleRecorder::setPipelineState(ID3D12PipelineState* pso) {
    if (!m_recording || !pso || m_pso == pso) return;
    m_bundle->SetPipelineState(pso);
    m_pso = pso;
    ++m_stats.commandsRecorded;
}

void BundleRecorder::setCorrectedStride(int slot, UINT stride) { m_correctedStrides[slot] = stride; }

void BundleRecorder::setDescriptorTable(UINT slot, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    if (!m_recording) return;
    m_bundle->SetGraphicsRootDescriptorTable(slot, handle);
    ++m_stats.commandsRecorded;
    ++m_stats.descriptorUpdates;
}

void BundleRecorder::setRootConstantBufferView(UINT rootIndex, D3D12_GPU_VIRTUAL_ADDRESS address) {
    if (!m_recording) return;
    m_bundle->SetGraphicsRootConstantBufferView(rootIndex, address);
    ++m_stats.commandsRecorded;
}

void BundleRecorder::setVertexBuffers(UINT startSlot, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views) {
    if (!m_recording || !views || count == 0) return;
    m_bundle->IASetVertexBuffers(startSlot, count, views);
    ++m_stats.commandsRecorded;
}

void BundleRecorder::setIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view) {
    if (!m_recording || !view) return;
    m_bundle->IASetIndexBuffer(view);
    ++m_stats.commandsRecorded;
}

void BundleRecorder::setPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) {
    if (!m_recording || m_topology == topology) return;
    m_bundle->IASetPrimitiveTopology(topology);
    m_topology = topology;
    ++m_stats.commandsRecorded;
}

void BundleRecorder::drawIndexedInstanced(UINT indexCount, UINT instanceCount, UINT startIndexLocation,
                                          INT baseVertexLocation, UINT startInstanceLocation) {
    if (!m_recording || indexCount == 0) return;
    m_bundle->DrawIndexedInstanced(indexCount, instanceCount, startIndexLocation, baseVertexLocation,
                                   startInstanceLocation);
    ++m_stats.drawCalls;
    m_stats.indexCount += indexCount;
}

void BundleRecorder::drawInstanced(UINT vertexCount, UINT instanceCount, UINT startVertexLocation,
                                   UINT startInstanceLocation) {
    if (!m_recording || vertexCount == 0) return;
    m_bundle->DrawInstanced(vertexCount, instanceCount, startVertexLocation, startInstanceLocation);
    ++m_stats.drawCalls;
    m_stats.vertexCount += vertexCount;
}

bool BundleRecorder::drawIndexedIndirect(ID3D12Resource* commands, UINT64 offset, UINT drawCount) {
    if (!m_recording || !commands || drawCount == 0) return false;
    if (!m_cmdSigIndexed) return false;
    m_bundle->ExecuteIndirect(m_cmdSigIndexed.Get(), drawCount, commands, offset, nullptr, 0);
    m_stats.drawCalls += drawCount;
    return true;
}

bool BundleRecorder::drawIndirect(ID3D12Resource* commands, UINT64 offset, UINT drawCount) {
    if (!m_recording || !commands || drawCount == 0) return false;
    if (!m_cmdSigNonIndexed) return false;
    m_bundle->ExecuteIndirect(m_cmdSigNonIndexed.Get(), drawCount, commands, offset, nullptr, 0);
    m_stats.drawCalls += drawCount;
    return true;
}

mcdr::Result<void> BundleRecorderPool::init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT workerCount,
                                            ID3D12CommandSignature* cmdSigIndexed,
                                            ID3D12CommandSignature* cmdSigNonIndexed) {
    if (!device || workerCount == 0)
        return mcdr::err("BundleRecorderPool::init", "device is null or workerCount == 0");
    shutdown();
    m_recorders.reserve(workerCount);
    for (UINT i = 0; i < workerCount; ++i) {
        std::unique_ptr<BundleRecorder> rec(new BundleRecorder());
        const mcdr::Result<void> r = rec->init(device, heap, i, cmdSigIndexed, cmdSigNonIndexed);
        if (!r) {
            shutdown();
            return mcdr::err("BundleRecorderPool::init", "BundleRecorder::init failed for worker " +
                                                             std::to_string(i) + ": " + r.error().message);
        }
        m_recorders.push_back(std::move(rec));
    }
    return {};
}

void BundleRecorderPool::shutdown() { m_recorders.clear(); }

BundleRecorder* BundleRecorderPool::recorder(UINT worker) {
    if (worker >= m_recorders.size()) return nullptr;
    return m_recorders[worker].get();
}

void executeBundle(ID3D12GraphicsCommandList* mainList, ID3D12GraphicsCommandList* bundle) {
    if (!mainList || !bundle) return;
    mainList->ExecuteBundle(bundle);
}

}
