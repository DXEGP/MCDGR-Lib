// dx12-mc 异步渲染基础设施：Fence 管理器
// 用 SetEventOnCompletion + 监控线程等待最早的待完成 fence，完成时批量回调（不轮询）；
// 回调在锁外调用，允许回调内再次注册/注销而不自锁。见 docs\async\README.md

module;

#include "_d3d12_module_prelude.h"

export module mcdr.d3d12.async_fence;

import mcdr.error;  // mcdr::Result<T> / mcdr::err()

export namespace d3d12_mc {

using Microsoft::WRL::ComPtr;

class AsyncFenceManager {
public:
    // success=false 表示超时或被 shutdown 取消。
    using FenceCallback = std::function<void(UINT64 fenceValue, bool success)>;

    AsyncFenceManager() = default;
    ~AsyncFenceManager();

    AsyncFenceManager(const AsyncFenceManager&) = delete;
    AsyncFenceManager& operator=(const AsyncFenceManager&) = delete;

    // 创建内部 ID3D12Fence（初值 0）并启动监控线程；失败返回 mcdr::Error。
    mcdr::Result<void> init(ID3D12Device* device, ID3D12CommandQueue* queue);
    bool valid() const { return m_fence != nullptr; }

    bool signal(UINT64 fenceValue);
    UINT64 completedValue() const;

    void registerCallback(UINT64 fenceValue, FenceCallback callback);
    void registerCallback(UINT64 fenceValue, FenceCallback callback, UINT64 timeoutMs);
    void unregisterCallback(UINT64 fenceValue);
    bool isFenceComplete(UINT64 fenceValue) const;

    // 阻塞等待 fence 达到 fenceValue；timeoutMs==0 表示无限等待。
    bool waitFence(UINT64 fenceValue, UINT64 timeoutMs);

    struct Stats {
        UINT64 totalCallbacks = 0;
        UINT64 pendingCallbacks = 0;
        UINT64 completedCallbacks = 0;
        UINT64 timedOutCallbacks = 0;
        UINT64 avgWaitTimeUs = 0;
    };
    Stats getStats() const;

    // 停止监控线程；未触发的回调以 success=false 收尾。可重复调用。
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
    HANDLE m_wakeEvent = nullptr;  // manual-reset：有新登记时唤醒监控线程

    mutable Stats m_stats;
    mutable std::mutex m_statsMutex;
};

}  // namespace d3d12_mc
