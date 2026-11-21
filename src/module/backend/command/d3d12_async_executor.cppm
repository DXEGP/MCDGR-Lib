module;

#include "_d3d12_module_prelude.h"

export module mcdr.d3d12.async_executor;

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

class MainCommandExecutor {
public:
    static constexpr UINT kFramesInFlight = 4;

    MainCommandExecutor() = default;
    ~MainCommandExecutor() = default;

    MainCommandExecutor(const MainCommandExecutor&) = delete;
    MainCommandExecutor& operator=(const MainCommandExecutor&) = delete;

    mcdr::Result<void> init(ID3D12Device* device, ID3D12CommandQueue* queue,
                            ID3D12DescriptorHeap* heaps = nullptr);
    bool valid() const { return m_slots[0].commandList != nullptr; }

    bool tryBeginFrame();

    bool addBundle(ID3D12GraphicsCommandList* bundle);
    bool addBundles(ID3D12GraphicsCommandList* const* bundles, UINT count);

    bool addResourceBarrier(const D3D12_RESOURCE_BARRIER& barrier);
    bool addTransition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

    UINT64 endFrame();

    bool isFrameInProgress() const { return m_frameInProgress; }
    UINT64 lastFenceValue() const { return m_lastFenceValue; }

    bool waitForFrame(UINT64 fenceValue, UINT64 timeoutMs);

    struct Stats {
        UINT64 framesSubmitted = 0;
        UINT64 bundlesExecuted = 0;
        UINT64 barriersExecuted = 0;
    };
    Stats getStats() const;

private:
    struct FrameSlot {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commandList;
        UINT64 submittedFenceValue = 0;
    };

    void clearFrameState();
    bool submitFrame(FrameSlot& slot);

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12Fence> m_fence;
    ID3D12DescriptorHeap* m_heap = nullptr;

    FrameSlot m_slots[kFramesInFlight];
    UINT m_slotIndex = 0;
    UINT64 m_fenceValue = 0;
    UINT64 m_lastFenceValue = 0;
    bool m_frameInProgress = false;

    std::vector<ID3D12GraphicsCommandList*> m_bundles;
    std::vector<D3D12_RESOURCE_BARRIER> m_barriers;
    mutable std::mutex m_mutex;
    mutable Stats m_stats;
};

mcdr::Result<void> MainCommandExecutor::init(ID3D12Device* device, ID3D12CommandQueue* queue,
                                            ID3D12DescriptorHeap* heaps) {
    if (!device || !queue)
        return mcdr::err("MainCommandExecutor::init", "device/queue is null");

    m_device = device;
    m_queue = queue;
    m_heap = heaps;

    HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        return mcdr::err("MainCommandExecutor::init", "CreateFence failed: " + hrText(hr));
    }

    for (UINT i = 0; i < kFramesInFlight; ++i) {
        FrameSlot& slot = m_slots[i];
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator));
        if (FAILED(hr)) {
            return mcdr::err("MainCommandExecutor::init", "CreateCommandAllocator failed: " + hrText(hr));
        }
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&slot.commandList));
        if (FAILED(hr)) {
            return mcdr::err("MainCommandExecutor::init", "CreateCommandList failed: " + hrText(hr));
        }
        slot.commandList->Close();
        slot.submittedFenceValue = 0;
    }
    m_slotIndex = 0;
    m_fenceValue = 0;
    m_lastFenceValue = 0;
    m_frameInProgress = false;
    return {};
}

void MainCommandExecutor::clearFrameState() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_bundles.clear();
    m_barriers.clear();
}

bool MainCommandExecutor::tryBeginFrame() {
    if (!valid() || m_frameInProgress) return false;

    FrameSlot& slot = m_slots[m_slotIndex];
    if (slot.submittedFenceValue > 0 && m_fence->GetCompletedValue() < slot.submittedFenceValue) {
        return false;
    }

    HRESULT hr = slot.allocator->Reset();
    if (FAILED(hr)) return false;
    hr = slot.commandList->Reset(slot.allocator.Get(), nullptr);
    if (FAILED(hr)) return false;

    ID3D12DescriptorHeap* heap = m_heap;
    if (heap) {
        ID3D12DescriptorHeap* heaps[] = {heap};
        slot.commandList->SetDescriptorHeaps(1, heaps);
    }

    clearFrameState();
    m_frameInProgress = true;
    return true;
}

bool MainCommandExecutor::addBundle(ID3D12GraphicsCommandList* bundle) {
    if (!bundle || !m_frameInProgress) return false;
    std::lock_guard<std::mutex> lk(m_mutex);
    m_bundles.push_back(bundle);
    return true;
}

bool MainCommandExecutor::addBundles(ID3D12GraphicsCommandList* const* bundles, UINT count) {
    if (!bundles || count == 0 || !m_frameInProgress) return false;
    std::lock_guard<std::mutex> lk(m_mutex);
    for (UINT i = 0; i < count; ++i) {
        if (bundles[i]) m_bundles.push_back(bundles[i]);
    }
    return true;
}

bool MainCommandExecutor::addResourceBarrier(const D3D12_RESOURCE_BARRIER& barrier) {
    if (!m_frameInProgress) return false;
    std::lock_guard<std::mutex> lk(m_mutex);
    m_barriers.push_back(barrier);
    return true;
}

bool MainCommandExecutor::addTransition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                        D3D12_RESOURCE_STATES after) {
    if (!resource) return false;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return addResourceBarrier(barrier);
}

bool MainCommandExecutor::submitFrame(FrameSlot& slot) {
    ID3D12CommandList* lists[] = {slot.commandList.Get()};
    m_queue->ExecuteCommandLists(1, lists);

    const UINT64 value = ++m_fenceValue;
    if (FAILED(m_queue->Signal(m_fence.Get(), value))) return false;

    slot.submittedFenceValue = value;
    m_lastFenceValue = value;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        ++m_stats.framesSubmitted;
    }
    return true;
}

UINT64 MainCommandExecutor::endFrame() {
    if (!m_frameInProgress || !valid()) return 0;

    FrameSlot& slot = m_slots[m_slotIndex];

    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    std::vector<ID3D12GraphicsCommandList*> bundles;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        barriers.swap(m_barriers);
        bundles.swap(m_bundles);
    }

    if (!barriers.empty()) {
        slot.commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        std::lock_guard<std::mutex> lk(m_mutex);
        m_stats.barriersExecuted += barriers.size();
    }

    for (ID3D12GraphicsCommandList* b : bundles) {
        if (b) slot.commandList->ExecuteBundle(b);
    }
    if (!bundles.empty()) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_stats.bundlesExecuted += bundles.size();
    }

    if (FAILED(slot.commandList->Close())) {
        m_frameInProgress = false;
        clearFrameState();
        return 0;
    }
    if (!submitFrame(slot)) {
        m_frameInProgress = false;
        return 0;
    }

    m_frameInProgress = false;
    m_slotIndex = (m_slotIndex + 1) % kFramesInFlight;
    return m_lastFenceValue;
}

bool MainCommandExecutor::waitForFrame(UINT64 fenceValue, UINT64 timeoutMs) {
    if (!m_fence) return false;
    if (fenceValue == 0 || m_fence->GetCompletedValue() >= fenceValue) return true;

    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) return false;
    if (FAILED(m_fence->SetEventOnCompletion(fenceValue, ev))) {
        CloseHandle(ev);
        return false;
    }
    DWORD ms = timeoutMs == 0 ? INFINITE
                              : (timeoutMs > 0xFFFFFFF0ULL ? 0xFFFFFFF0ULL : static_cast<DWORD>(timeoutMs));
    DWORD r = WaitForSingleObject(ev, ms);
    CloseHandle(ev);
    return r == WAIT_OBJECT_0;
}

MainCommandExecutor::Stats MainCommandExecutor::getStats() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_stats;
}

}
