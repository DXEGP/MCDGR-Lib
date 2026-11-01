// dx12-mc 异步渲染基础设施：分区描述符分配器
//
// 把 DeviceContext 的 SHADER_VISIBLE drawHeap 按 (frameSlot, worker) 切成互不相交的
// 连续区域，worker 在录制 bundle 期间用自己的原子游标独占一段，直接写入瞬时
// CBV/SRV（CPU 侧写 SHADER_VISIBLE 堆合法），bundle 内 SetGraphicsRootDescriptorTable
// 用堆 GPU 基址 + 区域偏移。区域互不相交 => 跨帧、跨 worker 无数据竞争。
// 每个帧槽 section 起始的 reservedPerSection 个槽位保留给同步路径 ring；
// 帧槽数固定 kAsyncFrameSlots，worker 数可配（<= kMaxAsyncWorkers）。见 docs\async\README.md

module;

#include "_d3d12_module_prelude.h"

export module mcdr.d3d12.async_descriptor;

import mcdr.error;  // mcdr::Result<T> / mcdr::err()

export namespace d3d12_mc {

using Microsoft::WRL::ComPtr;

// 帧槽数：计划的最大在飞帧数（现有 drawHeap ring 亦为 x4）。
constexpr UINT kAsyncFrameSlots = 4;
// 单帧槽内可划分的 worker 分区上限。
constexpr UINT kMaxAsyncWorkers = 8;
// 每个帧槽起始处保留给同步路径 ring 的槽位数（blit / 小批量同步 draw）。
// 图集上传（TextureAtlas.uploadInitialContents）会在同一条未提交的命令列表内录制
// mipLevelCount × staticSprites.size() 个 draw，整轮资源重载的图集共用同一 encoder，
// 累计槽位远超 32768；取 131072 作保守上界：每帧段 131072 + 24576 = 155648，
// ×4 段 = 622592 仍低于 CBV_SRV_UAV 堆 1,000,000 上限。
constexpr UINT kAsyncSyncRingReserve = 131072;
// 每个帧槽内 worker 分区总和；与 kAsyncSyncRingReserve 严格不重叠，容量不足时
// 回退串行录制（降级但不越界）。
constexpr UINT kAsyncWorkerSlotsPerSection = 24576;

class PartitionedDescriptorAllocator {
public:
    PartitionedDescriptorAllocator() = default;
    ~PartitionedDescriptorAllocator() = default;

    PartitionedDescriptorAllocator(const PartitionedDescriptorAllocator&) = delete;
    PartitionedDescriptorAllocator& operator=(const PartitionedDescriptorAllocator&) = delete;

    // heap 为 DeviceContext::drawHeap（CBV_SRV_UAV / SHADER_VISIBLE，非拥有，仅多持一个
    // 引用保活）。reservedPerSection 为每个帧槽起始保留给同步 ring 的槽位数（异步模式下
    // 仍有少量同步 draw：小批量 chunk、surface blit），必须与 worker 分区互不重叠。
    mcdr::Result<void> init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT heapSize, UINT workerCount,
                            UINT reservedPerSection = 0);

    bool valid() const { return m_heap != nullptr && m_slotsPerRegion > 0; }
    UINT workerCount() const { return m_workerCount; }
    UINT slotsPerRegion() const { return m_slotsPerRegion; }
    UINT heapSize() const { return m_heapSize; }
    UINT incrementSize() const { return m_increment; }

    // (frameSlot, worker) 区域的绝对起始槽位；参数越界返回 UINT_MAX。
    UINT regionBaseSlot(UINT frameSlot, UINT worker) const;

    // 成功返回绝对起始槽位；区域容量不足返回 UINT_MAX。
    UINT allocate(UINT frameSlot, UINT worker, UINT count);

    // 调用前提：该帧槽对应的 GPU 工作已完成（无并发分配）。
    void resetFrame(UINT frameSlot);

    // 绝对槽位的 CPU / GPU 句柄；slot 越界时返回空句柄（ptr=0）。
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle(UINT slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle(UINT slot) const;

    // 写入辅助：CBV 即把 buffer 的 [offset, offset+size) 写为常量缓冲视图。
    bool writeCBV(UINT slot, ID3D12Resource* buffer, UINT64 offset, UINT64 size);
    bool writeDescriptor(UINT slot, D3D12_CPU_DESCRIPTOR_HANDLE src);

    ID3D12DescriptorHeap* heap() const { return m_heap.Get(); }

private:
    UINT regionIndex(UINT frameSlot, UINT worker) const { return frameSlot * m_workerCount + worker; }

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12DescriptorHeap> m_heap;
    UINT m_heapSize = 0;
    UINT m_workerCount = 0;
    UINT m_slotsPerRegion = 0;
    UINT m_sectionSize = 0;
    UINT m_reservedPerSection = 0;
    UINT m_increment = 0;
    std::array<std::atomic<UINT>, static_cast<size_t>(kAsyncFrameSlots) * kMaxAsyncWorkers> m_cursors;
};

}  // namespace d3d12_mc
