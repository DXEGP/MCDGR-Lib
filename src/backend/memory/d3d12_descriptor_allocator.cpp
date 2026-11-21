module;

#include "_d3d12_module_prelude.h"

module mcdr.d3d12.descriptor_allocator;

namespace d3d12_mc {

SegmentedDescriptorAllocator::SegmentedDescriptorAllocator(ID3D12Device* device,
                                                           D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                           bool shaderVisible, UINT sections,
                                                           UINT descriptorsPerSection, UINT workerCount)
    : m_shaderVisible(shaderVisible) {
    if (!device || sections == 0 || descriptorsPerSection == 0 || workerCount == 0
        || sections % workerCount != 0) {
        return;
    }

    const UINT64 total64 = static_cast<UINT64>(sections) * descriptorsPerSection;
    if (total64 > UINT_MAX) {
        return;
    }
    const UINT totalDescriptors = static_cast<UINT>(total64);

    m_inc = device->GetDescriptorHandleIncrementSize(type);

    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = totalDescriptors;
    desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap));
    if (FAILED(hr)) {
        m_heap.Reset();
        return;
    }

    m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
    if (shaderVisible) {
        m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();
    }

    m_sections = sections;
    m_descriptorsPerSection = descriptorsPerSection;
    m_workerCount = workerCount;
    m_sectionsState = std::make_unique<Section[]>(sections);
}

UINT SegmentedDescriptorAllocator::sectionFor(UINT frameIndex, UINT workerIndex) const {
    if (m_workerCount == 0) {
        return UINT_MAX;
    }
    if (workerIndex >= m_workerCount) {
        return UINT_MAX;
    }
    const UINT inFlight = m_sections / m_workerCount;
    const UINT frameBand = (inFlight != 0) ? (frameIndex % inFlight) : 0;
    return frameBand * m_workerCount + workerIndex;
}

D3D12_CPU_DESCRIPTOR_HANDLE SegmentedDescriptorAllocator::cpuStartFor(UINT section) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h{};
    if (m_heap && section < m_sections) {
        const SIZE_T sectionBytes = static_cast<SIZE_T>(section) * m_descriptorsPerSection * m_inc;
        h.ptr = m_cpuStart.ptr + sectionBytes;
    }
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE SegmentedDescriptorAllocator::gpuStartFor(UINT section) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h{};
    if (m_shaderVisible && m_heap && section < m_sections) {
        const SIZE_T sectionBytes = static_cast<SIZE_T>(section) * m_descriptorsPerSection * m_inc;
        h.ptr = m_gpuStart.ptr + sectionBytes;
    }
    return h;
}

UINT SegmentedDescriptorAllocator::allocSlot(UINT section) {
    if (section >= m_sections) {
        return UINT_MAX;
    }
    Section& st = m_sectionsState[section];

    const UINT idx = st.next.fetch_add(1, std::memory_order_relaxed);
    if (idx < m_descriptorsPerSection) {
        return idx;
    }

    std::lock_guard<std::mutex> lock(st.mutex);
    if (!st.freeList.empty()) {
        const UINT reused = st.freeList.back();
        st.freeList.pop_back();
        return reused;
    }
    return UINT_MAX;
}

void SegmentedDescriptorAllocator::freeSlot(UINT section, UINT index) {
    if (section >= m_sections || index >= m_descriptorsPerSection) {
        return;
    }
    Section& st = m_sectionsState[section];
    std::lock_guard<std::mutex> lock(st.mutex);
    st.freeList.push_back(index);
}

void SegmentedDescriptorAllocator::resetSection(UINT section) {
    if (section >= m_sections) {
        return;
    }
    Section& st = m_sectionsState[section];
    std::lock_guard<std::mutex> lock(st.mutex);
    st.next.store(0, std::memory_order_relaxed);
    st.freeList.clear();
}

void SegmentedDescriptorAllocator::reset() {
    for (UINT s = 0; s < m_sections; ++s) {
        resetSection(s);
    }
}

}
