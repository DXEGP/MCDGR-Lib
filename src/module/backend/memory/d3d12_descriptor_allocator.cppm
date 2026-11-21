module;

#include "_d3d12_module_prelude.h"

export module mcdr.d3d12.descriptor_allocator;

export namespace d3d12_mc {

using Microsoft::WRL::ComPtr;

class SegmentedDescriptorAllocator {
public:
    SegmentedDescriptorAllocator(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, bool shaderVisible,
                                 UINT sections, UINT descriptorsPerSection, UINT workerCount = 1);

    bool valid() const { return m_heap != nullptr; }

    UINT sectionCount() const { return m_sections; }
    UINT descriptorsPerSection() const { return m_descriptorsPerSection; }
    UINT workerCount() const { return m_workerCount; }
    UINT inFlightFrames() const { return m_workerCount ? m_sections / m_workerCount : 0; }
    UINT increment() const { return m_inc; }
    bool shaderVisible() const { return m_shaderVisible; }
    ID3D12DescriptorHeap* heap() const { return m_heap.Get(); }

    UINT sectionFor(UINT frameIndex, UINT workerIndex) const;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuStartFor(UINT section) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStartFor(UINT section) const;

    UINT allocSlot(UINT section);

    void freeSlot(UINT section, UINT index);

    void resetSection(UINT section);
    void reset();

private:
    struct Section {
        std::atomic<UINT> next{0};
        std::mutex mutex;
        std::vector<UINT> freeList;
    };

    ComPtr<ID3D12DescriptorHeap> m_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart{};
    UINT m_inc = 0;
    UINT m_sections = 0;
    UINT m_descriptorsPerSection = 0;
    UINT m_workerCount = 1;
    bool m_shaderVisible = false;
    std::unique_ptr<Section[]> m_sectionsState;
};

}
