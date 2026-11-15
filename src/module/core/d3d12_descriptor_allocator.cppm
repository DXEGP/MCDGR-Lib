// dx12-mc 原生 D3D12 层（C++）：线程安全分段描述符分配器。
//
// 并发写 SHADER_VISIBLE 堆必须互斥或分区隔离；本类采用分区隔离：堆切成
// inFlightFrames × workerCount 段，worker 在第 frame 帧独占段
// (frame % inFlightFrames) * workerCount + worker —— 帧维 + worker 维双正交，故任意
// 两个并发录制的 worker 永远命中不同段 → 并发写零竞争。即使误用同一段，段内 bump
// 用 std::atomic fetch_add（索引唯一）、槽位复用区用 std::mutex 保护，同段内仍互斥安全。
//
// 语义约定：录制线程 allocSlot + cpuStartFor 拿到段起始 CPU 句柄后只写自己的段（CPU 侧
// 写 SHADER_VISIBLE 堆合法），复制描述符不在这里做；重用段 N 前必须确保 GPU 已读完段 N
// （fence 保证），到期段 resetSection() 归零游标即可整段复用，无需逐槽 free；
// 构造 / reset() / 析构须在"所有段无并发分配"的帧边界执行。见 docs\async\README.md

module;

#include "_d3d12_module_prelude.h"

export module mcdr.d3d12.descriptor_allocator;

export namespace d3d12_mc {

using Microsoft::WRL::ComPtr;

class SegmentedDescriptorAllocator {
public:
    // 构造并立即创建底层描述符堆；参数非法或 CreateDescriptorHeap 失败时 valid()==false
    // （其余 getter 返回 0/空句柄），调用方须先检查 valid()。
    // sections 须 = inFlightFrames × workerCount（>=1 且能被 workerCount 整除）；
    // shaderVisible=false 为 CPU-only 堆（只能作 CopyDescriptorsSimple 源/目标）。
    // 每段描述符数与总数上限：SAMPLER 的 SHADER_VISIBLE 堆硬上限 2048、CBV_SRV_UAV 为
    // 1,000,000（Tier1），超限时 CreateDescriptorHeap 失败。
    SegmentedDescriptorAllocator(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, bool shaderVisible,
                                 UINT sections, UINT descriptorsPerSection, UINT workerCount = 1);

    // 堆创建是否成功（失败原因多为参数非法/描述符总数超平台上限）。
    bool valid() const { return m_heap != nullptr; }

    // ---- 只读配置 ----
    UINT sectionCount() const { return m_sections; }
    UINT descriptorsPerSection() const { return m_descriptorsPerSection; }
    UINT workerCount() const { return m_workerCount; }
    // 帧维段数 = sections / workerCount（sectionFor 内部按 frame % inFlightFrames 取帧带）。
    UINT inFlightFrames() const { return m_workerCount ? m_sections / m_workerCount : 0; }
    UINT increment() const { return m_inc; }
    bool shaderVisible() const { return m_shaderVisible; }
    ID3D12DescriptorHeap* heap() const { return m_heap.Get(); }

    // ---- 段寻址（录制线程只读，无锁） ----
    // frameIndex 与 worker 正交：返回该 (frame, worker) 独占的段号 [0, sections)，
    // 保证并发录制的不同 worker 永不命中同一段。workerIndex 越界返回 UINT_MAX。
    UINT sectionFor(UINT frameIndex, UINT workerIndex) const;

    // 段内第 0 号描述符的 CPU 句柄（录制线程按 increment() 步长写自己的段）；越界返回空句柄（ptr=0）。
    D3D12_CPU_DESCRIPTOR_HANDLE cpuStartFor(UINT section) const;
    // 段内第 0 号描述符的 GPU 句柄（绑定 descriptor table 用）；仅 SHADER_VISIBLE 堆有效，越界返回空句柄。
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStartFor(UINT section) const;

    // ---- 槽位分配 / 释放（线程安全） ----
    // 在 section 内分配槽位，返回【段内索引】，地址 = cpuStartFor(section).ptr + index * increment()。
    // 段满返回 UINT_MAX。线程安全：快路径 atomic fetch_add（不同段零竞争）；耗尽后回退 freeList 复用区（段内 mutex）。
    UINT allocSlot(UINT section);

    // 归还槽位供复用（与 allocSlot 返回的索引一一配对）；槽位内容不擦除，复用时由录制线程覆写。
    void freeSlot(UINT section, UINT index);

    // 整段游标归零 + 清空复用列表；调用前提是该段 GPU 读取已由 fence 完成、且无并发 allocSlot。
    void resetSection(UINT section);
    // 全部段 resetSection（须在无任何并发分配的帧边界调用）。
    void reset();

private:
    struct Section {
        std::atomic<UINT> next{0};   // 段内线性分配游标（fetch_add 无锁快路径）
        std::mutex mutex;            // 保护 freeList：同段慢路径/free/reset 互斥
        std::vector<UINT> freeList;  // 释放槽位 LIFO（供线性耗尽后复用）
    };

    ComPtr<ID3D12DescriptorHeap> m_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart{};  // 仅 SHADER_VISIBLE 堆有效
    UINT m_inc = 0;
    UINT m_sections = 0;
    UINT m_descriptorsPerSection = 0;
    UINT m_workerCount = 1;
    bool m_shaderVisible = false;
    // 每段分配状态（索引 = 段号）。用 unique_ptr<Section[]> 而非 vector：Section
    // 含 std::atomic/std::mutex（不可拷贝/移动），数组按默认构造原地建段、不搬迁。
    std::unique_ptr<Section[]> m_sectionsState;
};

}  // namespace d3d12_mc
