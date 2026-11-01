// d3d12_mesh.cppm — mcdr.d3d12.mesh 的接口单元（mesh shader 管线），见 docs\advanced\mesh.md
// mesh 管线只能用 DXC 产出的 as_6_x / ms_6_x DXIL 经 D3D12_PIPELINE_STATE_STREAM_DESC +
// ID3D12Device2::CreatePipelineState 创建；绘制走 ID3D12GraphicsCommandList6::DispatchMesh，不要用 Draw*。

module;

#include "_d3d12_module_prelude.h"

export module mcdr.d3d12.mesh;

// 只 import 不 export：导出签名里不含 device 模块的类型，消费方不需要经本模块看到 Dx12Pipeline / CommandContext。
import mcdr.d3d12.device;   // PipelineDesc / Dx12Pipeline / CommandContext / deviceContext（仅本单元内部使用）
import mcdr.error;          // Result<T> / Error / err()

export namespace d3d12_mc {

using Microsoft::WRL::ComPtr;
struct MeshPipelineDesc {
    // DXC 编出的 DXIL 字节码（不是 HLSL 源码）。asDxil 可空（不用 amplification），msDxil 必填，psDxil 可空。
    std::vector<uint8_t> asDxil;
    std::vector<uint8_t> msDxil;
    std::vector<uint8_t> psDxil;

    int colorCount = 0;
    std::vector<PipelineDesc::ColorTarget> colorTargets;
    bool hasDepth = false;
    int depthFormat = 0;  // GpuFormat ordinal（与图形管线同义；DSV 固定 D32_FLOAT，同图形管线）
    bool depthWrite = false;
    int depthCompareOp = 0;
    bool cullEnabled = false;
    int polygonMode = 0;  // 0=FILL，1=WIREFRAME
    // 与图形管线同构：0=CBV（root descriptor），1=SRV+static sampler，2=SRV（texel buffer）；
    // 顺序即 register 顺序，mcdr_dx12PushDescriptors 靠它复用。
    std::vector<PipelineDesc::Binding> bindings;
};

// 创建 mesh 管线（失败返回 mcdr::Error，成功返回句柄）。root signature 不带
// ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT（mesh 管线没有 IA 阶段）。
// 返回/入参用不透明 void*，不要写成 Dx12Pipeline*/CommandContext*：这两个类型属于
// mcdr.d3d12.device 模块，定义侧一定记成模块私有名（Dx12Pipeline@$$_Amcdr.dx12.device），
// 而消费方 abi_mesh.cpp 按 mcdr_abi.h 的前向声明只得到公共名 @1 → LNK2001。
mcdr::Result<void*> createMeshPipeline(const MeshPipelineDesc& desc);

// 用外部根签名创建（地形 mesh 变体复用 JVM 侧 b0..b3/t4/t5 绑定点，尾参扩展由 JVM 建好交进来）；
// 为 0 时等价于上一个重载。传入的是 ID3D12RootSignature* 的地址值（本模块不导出 D3D12 类型）。
mcdr::Result<void*> createMeshPipeline(const MeshPipelineDesc& desc, uint64_t externalRootSignature);

// 按绑定点布局建根签名并交出裸指针（地址值，调用方用 releaseMeshRootSignature 释放）。
// meshExtras=true 时在地形绑定之后追加两个尾参：[N] root CBV b8（小常量 meshletCount 或段表基址），
// [N+1] descriptor table 含 t14（顶点）/ t15（段表）/ t16（meshlet 表）—— 地形 PSO 与 mesh PSO 共用同一套 draw 绑定。
mcdr::Result<uint64_t> createMeshRootSignature(const std::vector<PipelineDesc::Binding>& bindings,
                                               bool meshExtras);

// 释放 createMeshRootSignature 交出的签名（传 0 安全）。
void releaseMeshRootSignature(uint64_t rootSignature);

// 本机是否具备 mesh 管线的最小条件（12_2 + mesh tier ≥ 1 + DXC 能编 ms_6_x + ID3D12Device2）；why 收到原因（可为 nullptr）。
bool meshPipelineSupported(std::string* why);

// DispatchMesh：x/y/z = 线程组数量（ctx 为 mcdr_command_ctx 句柄）；要求当前已 setPipeline 一个 mesh 管线。
mcdr::Result<void> dispatchMesh(void* ctx, int x, int y, int z);

}

// 实现见 src\advanced\d3d12_mesh_base.cpp（【普通翻译单元】）。导出函数的定义不要放进接口单元或
// 模块实现单元，且签名里不要出现 Dx12Pipeline*/CommandContext* —— 否则两侧修饰名对不上 → LNK2001。
