module;

#include "_d3d12_module_prelude.h"

export module mcdr.d3d12.async_descriptor;

import mcdr.error;

export namespace d3d12_mc {

using Microsoft::WRL::ComPtr;

constexpr UINT kAsyncFrameSlots = 4;
constexpr UINT kMaxAsyncWorkers = 8;
constexpr UINT kAsyncSyncRingReserve = 131072;
constexpr UINT kAsyncWorkerSlotsPerSection = 24576;

class PartitionedDescriptorAllocator {
public:
    PartitionedDescriptorAllocator() = default;
    ~PartitionedDescriptorAllocator() = default;

    PartitionedDescriptorAllocator(const PartitionedDescriptorAllocator&) = delete;
    PartitionedDescriptorAllocator& operator=(const PartitionedDescriptorAllocator&) = delete;

    mcdr::Result<void> init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT heapSize, UINT workerCount,
                            UINT reservedPerSection = 0);

    bool valid() const { return m_heap != nullptr && m_slotsPerRegion > 0; }
    UINT workerCount() const { return m_workerCount; }
    UINT slotsPerRegion() const { return m_slotsPerRegion; }
    UINT heapSize() const { return m_heapSize; }
    UINT incrementSize() const { return m_increment; }

    UINT regionBaseSlot(UINT frameSlot, UINT worker) const;

    UINT allocate(UINT frameSlot, UINT worker, UINT count);

    void resetFrame(UINT frameSlot);

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle(UINT slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle(UINT slot) const;

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

}
