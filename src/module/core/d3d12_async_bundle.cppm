// dx12-mc 异步渲染基础设施：Bundle 录制器池
//
// 每个 worker 独占一个 D3D12_COMMAND_LIST_TYPE_BUNDLE 的 allocator + command
// list，把一段“同一 pass + 同一 PSO + 同一 scissor”的绘制录制进 bundle。
// bundle 不继承父列表的描述符堆，因此只允许 SetPipelineState /
// SetGraphicsRootSignature / SetGraphicsRootDescriptorTable / IASet* /
// DrawInstanced / DrawIndexedInstanced / ExecuteIndirect。
// 禁止：任何 Clear / Copy、ResourceBarrier、ResolveSubresource、Begin/EndQuery、
// OMSetRenderTargets、RSSetViewports、RSSetScissorRects、ExecuteBundle、render
// pass begin/end、二次 SetDescriptorHeaps。
// bundle 内的 PSO 必须与父列表录制时的 RT 格式 / 深度格式 / 采样数一致。见 docs\async\README.md

module;

#include "_d3d12_module_prelude.h"

export module mcdr.d3d12.async_bundle;

import mcdr.error;  // mcdr::Result<T> / mcdr::err()

namespace {

// 模块单元不得 import 设备模块，故各实现单元自持一份。
std::string hrText(HRESULT hr) {
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(hr));
    return std::string(buf);
}

}  // namespace

export namespace d3d12_mc {

using Microsoft::WRL::ComPtr;

// 命令 allocator 只在对应 GPU 工作完成后才能 Reset，故按
// fenceValue % kBundleFrameSlots 轮转复用。
constexpr UINT kBundleFrameSlots = 4;

// 同一帧内会对同一命令列表多次 drawMultipleIndexed；已 ExecuteBundle 的 bundle 在其父
// 列表执行完成前不能 Reset allocator，故同帧第 b 批用 (frameSlot, b) 独立槽位。
// b 超过上限时 begin() 返回 false，调用方回退串行录制（功能正确，仅少这段并行）。
constexpr UINT kBundleBatchesPerFrame = 16;
constexpr UINT kBundleSlotCount = kBundleFrameSlots * kBundleBatchesPerFrame;

D3D12_PRIMITIVE_TOPOLOGY bundlePrimitiveTopology(int ordinal);

// 前向声明：步长修正须按 Dx12Pipeline::vertexStrides，但本模块不能 import 设备模块（会成环）。
struct Dx12Pipeline;

class BundleRecorder {
public:
    BundleRecorder() = default;
    ~BundleRecorder() = default;

    BundleRecorder(const BundleRecorder&) = delete;
    BundleRecorder& operator=(const BundleRecorder&) = delete;

    // 创建 kBundleSlotCount 组 BUNDLE allocator + list（初始 closed）；任一步失败返回 mcdr::Error。
    // heap 为 DeviceContext::drawHeap（shader-visible CBV_SRV_UAV，非拥有）：bundle 内若要
    // SetGraphicsRootDescriptorTable，必须自行 SetDescriptorHeaps 一次，否则运行时解析
    // GPU 句柄时空堆指针解引用崩溃。
    mcdr::Result<void> init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT workerId,
                            ID3D12CommandSignature* cmdSigIndexed, ID3D12CommandSignature* cmdSigNonIndexed);

    bool valid() const { return m_bundle != nullptr; }
    UINT workerId() const { return m_workerId; }
    bool isRecording() const { return m_recording; }
    ID3D12GraphicsCommandList* commandList() const { return m_bundle.Get(); }

    // frameSlot 须为 fenceValue % kBundleFrameSlots；frameValue 为原始帧号，用于判定同帧批次。
    // 同帧批次数超过 kBundleBatchesPerFrame 时返回 false（调用方回退串行）；end() 返回
    // bundle 命令列表句柄，失败返回 nullptr。
    bool begin(UINT frameSlot, UINT64 frameValue);
    ID3D12GraphicsCommandList* end();

    void setRootSignature(ID3D12RootSignature* rootSignature);
    void setPipelineState(ID3D12PipelineState* pso);
    // 记录所属管线的 per-slot 修正 stride（与同步路径 setVertexBuffer 一致）。
    // 只存 (slot → stride) 而不存 Dx12Pipeline*：本模块不能 import 设备模块（会成环），只有前向声明。
    void setCorrectedStride(int slot, UINT stride);
    // 该 slot 的修正 stride；无记录时返回 0（调用方回退传入值）。
    UINT correctedStride(int slot) const;
    void setDescriptorTable(UINT slot, D3D12_GPU_DESCRIPTOR_HANDLE handle);
    // CBV 的 root descriptor 地址烘焙进命令列表，不经过描述符堆。
    void setRootConstantBufferView(UINT rootIndex, D3D12_GPU_VIRTUAL_ADDRESS address);
    void setVertexBuffers(UINT startSlot, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views);
    void setIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view);
    void setPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology);
    void drawIndexedInstanced(UINT indexCount, UINT instanceCount, UINT startIndexLocation,
                              INT baseVertexLocation, UINT startInstanceLocation);
    void drawInstanced(UINT vertexCount, UINT instanceCount, UINT startVertexLocation,
                       UINT startInstanceLocation);
    // 签名为空时返回 false：绝不向 ExecuteIndirect 传 nullptr。
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
    ComPtr<ID3D12CommandSignature> m_cmdSigIndexed;     // = DeviceContext::cmdSigIndexed
    ComPtr<ID3D12CommandSignature> m_cmdSigNonIndexed;  // = DeviceContext::cmdSigNonIndexed
    // 必须与父列表执行本 bundle 时绑定的 shader-visible 堆一致，否则行为未定义。
    ComPtr<ID3D12DescriptorHeap> m_heap;

    ID3D12Device* m_device = nullptr;
    UINT m_workerId = 0;
    bool m_recording = false;

    ID3D12RootSignature* m_rootSig = nullptr;
    ID3D12PipelineState* m_pso = nullptr;
    D3D12_PRIMITIVE_TOPOLOGY m_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    // 由调用方从 Dx12Pipeline 抄入，begin 时清空；不持有 Dx12Pipeline*（见 setCorrectedStride）。
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
        case 1: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;   // LINES / DEBUG_LINES
        case 2: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;          // DEBUG_LINE_STRIP
        case 3: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        case 5: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case 6: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLEFAN;
        case 7: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;       // QUADS（不支持，回退）
        default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;      // TRIANGLES 等
    }
}

mcdr::Result<void> BundleRecorder::init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT workerId,
                                       ID3D12CommandSignature* cmdSigIndexed,
                                       ID3D12CommandSignature* cmdSigNonIndexed) {
    if (!device) return mcdr::err("BundleRecorder::init", "device is null");

    m_device = device;
    m_workerId = workerId;
    m_heap = heap;  // 非拥有；bundle 内须自设该堆

    for (UINT i = 0; i < kBundleSlotCount; ++i) {
        HRESULT hr =
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&m_allocators[i]));
        if (FAILED(hr))
            return mcdr::err("BundleRecorder::init", "CreateCommandAllocator failed: " + hrText(hr));

        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, m_allocators[i].Get(), nullptr,
                                       IID_PPV_ARGS(&m_lists[i]));
        if (FAILED(hr))
            return mcdr::err("BundleRecorder::init", "CreateCommandList failed: " + hrText(hr));

        // 创建即为 open，先 Close 到初始状态，之后 begin() 再 Reset。
        m_lists[i]->Close();
    }
    m_recording = false;
    for (UINT i = 0; i < kBundleFrameSlots; ++i) {
        m_frameOfSlot[i] = UINT64_MAX;  // 未使用过（fenceValue 从 1 开始递增）
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
    if (batch >= kBundleBatchesPerFrame) return false;  // 本帧配额用尽，回退串行
    m_batchOfSlot[fs] = batch + 1;

    const UINT slot = fs * kBundleBatchesPerFrame + batch;
    m_activeSlot = slot;
    m_bundle = m_lists[slot];
    if (!m_bundle || !m_allocators[slot]) return false;
    HRESULT hr = m_allocators[slot]->Reset();
    if (FAILED(hr)) return false;
    hr = m_bundle->Reset(m_allocators[slot].Get(), nullptr);
    if (FAILED(hr)) return false;

    // bundle 不继承父列表的描述符堆，且整段 bundle 只能设一次：必须在任何
    // SetGraphicsRootDescriptorTable 之前显式绑定 shader-visible 堆，否则运行时
    // 解析 GPU 句柄会读到空堆指针（+0x28）而访问冲突。
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
    // D3D12 禁止向 ExecuteIndirect 传 nullptr command signature。
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

}  // namespace d3d12_mc
