module;

#include "_d3d12_module_prelude.h"

export module mcdr.d3d12.async_fence;

import mcdr.error;

export namespace d3d12_mc {

using Microsoft::WRL::ComPtr;

class AsyncFenceManager {
public:
    using FenceCallback = std::function<void(UINT64 fenceValue, bool success)>;

    AsyncFenceManager() = default;
    ~AsyncFenceManager();

    AsyncFenceManager(const AsyncFenceManager&) = delete;
    AsyncFenceManager& operator=(const AsyncFenceManager&) = delete;

    mcdr::Result<void> init(ID3D12Device* device, ID3D12CommandQueue* queue);
    bool valid() const { return m_fence != nullptr; }

    bool signal(UINT64 fenceValue);
    UINT64 completedValue() const;

    void registerCallback(UINT64 fenceValue, FenceCallback callback);
    void registerCallback(UINT64 fenceValue, FenceCallback callback, UINT64 timeoutMs);
    void unregisterCallback(UINT64 fenceValue);
    bool isFenceComplete(UINT64 fenceValue) const;

    bool waitFence(UINT64 fenceValue, UINT64 timeoutMs);

    struct Stats {
        UINT64 totalCallbacks = 0;
        UINT64 pendingCallbacks = 0;
        UINT64 completedCallbacks = 0;
        UINT64 timedOutCallbacks = 0;
        UINT64 avgWaitTimeUs = 0;
    };
    Stats getStats() const;

    void shutdown();

private:
    struct PendingCallback {
        UINT64 fenceValue = 0;
        FenceCallback callback;
        UINT64 timeoutMs = 0;
        UINT64 startTimeMs = 0;
    };

    void monitorThread();
    void processCompletedFences();
    void wakeMonitor();

    ComPtr<ID3D12Fence> m_fence;
    ComPtr<ID3D12CommandQueue> m_queue;

    std::unordered_map<UINT64, PendingCallback> m_pending;
    mutable std::mutex m_mutex;

    std::thread m_monitorThread;
    std::atomic<bool> m_running{false};
    HANDLE m_wakeEvent = nullptr;

    mutable Stats m_stats;
    mutable std::mutex m_statsMutex;
};

}
