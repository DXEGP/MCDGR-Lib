module;

#include "_d3d12_module_prelude.h"
#if __has_include(<dxcapi.h>)
#include <dxcapi.h>
#define MCDR_HAVE_DXCAPI 1
#else
#define MCDR_HAVE_DXCAPI 0
#endif

module mcdr.d3d12.device;

import mcdr.error;

namespace d3d12_mc {
#if MCDR_HAVE_DXCAPI
namespace {

using PFN_DxcCreateInstance = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID);

// 最小可编译 mesh shader：只用于验证 DXC 能产出 ms_6_x 的 DXIL，不参与渲染。
constexpr char kMeshSmokeHlsl[] = R"hlsl(
struct MeshVertex { float4 position : SV_Position; };

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void main(out vertices MeshVertex verts[3], out indices uint3 tris[1]) {
    SetMeshOutputCounts(3, 1);
    verts[0].position = float4(0.0, 0.0, 0.0, 1.0);
    verts[1].position = float4(1.0, 0.0, 0.0, 1.0);
    verts[2].position = float4(0.0, 1.0, 0.0, 1.0);
    tris[0] = uint3(0, 1, 2);
}
)hlsl";

std::string hexHr(HRESULT hr) {
    char buf[16] = {};
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(hr));
    return buf;
}

// 多行 DXC 诊断只留第一行（日志/JSON 里不宜铺开）。


// 2026-10-14 现代化改造：从被调方的 mcdr::Error 里取出纯文本 message。
// 调用方一律自己补 "函数名: " 前缀（与改造前 err 里直接写整句的习惯一致），
// 因此这里刻意不用 mcdr::describe()——那会带出被调方的 where 前缀。


bool fileExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// MCDR.dll 自身所在目录（将来随模组分发 dxcompiler.dll/dxil.dll 时放这里）。
std::wstring moduleDirectory() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&moduleDirectory), &self)) {
        return std::wstring();
    }
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return std::wstring();
    }
    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash);
}

// Windows SDK 里版本号最大的 bin\<ver>\x64（含 dxcompiler.dll/dxil.dll 的那一份）。
std::wstring sdkDxcDirectory() {
    const std::wstring root = L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\";
    std::wstring best;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return std::wstring();
    }
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            continue;
        }
        const std::wstring ver(fd.cFileName);
        if (ver == L"." || ver == L".." || (!best.empty() && ver <= best)) {
            continue;
        }
        if (fileExists(root + ver + L"\\x64\\dxcompiler.dll")) {
            best = ver;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best.empty() ? std::wstring() : root + best + L"\\x64";
}

// 搜索顺序：MCDR_DXC_DIR 环境变量 → MCDR.dll 同目录 → SDK 最新版本。
std::wstring findDxcDirectory() {
    // 实机验收开关：`MCDR_DXC_DISABLE=1` 时**三站全不查**，直接返回空目录。
    //
    // 为什么需要它：`MCDR_DXC_DIR` 只是**追加一个候选**，不是权威覆盖 —— 即便把它指向空目录，
    // 模块目录与 Windows SDK 那两站照样能把 dxcompiler.dll 供上（实测：把 `run\dxrmc\` 里两个
    // DLL 改名后，客户端仍从 `C:\Program Files (x86)\Windows Kits\10\bin\…\dxcompiler.dll`
    // 拿到，`dxc=yes` 依旧）。系统目录不能改，所以"确实找不到 DXC"这种现场只能靠一个显式
    // 开关来验证 —— 2026-10-25 起找不到 DXC 不再回退 D3DCompile(5.1)，而是图形管线直接报错，
    // 这条开关的用途就变成"验证失败路径本身"。
    //
    // 只认完全等于 "1" 的值；未设置、空串、其它值一律照旧 —— 验收专用，不是产品行为开关。
    if (const wchar_t* off = _wgetenv(L"MCDR_DXC_DISABLE");
        off != nullptr && off[0] == L'1' && off[1] == L'\0') {
        return std::wstring();
    }
    std::vector<std::wstring> candidates;
    if (const wchar_t* env = _wgetenv(L"MCDR_DXC_DIR")) {
        if (env[0] != L'\0') {
            candidates.emplace_back(env);
        }
    }
    const std::wstring selfDir = moduleDirectory();
    if (!selfDir.empty()) {
        candidates.push_back(selfDir);
    }
    const std::wstring sdk = sdkDxcDirectory();
    if (!sdk.empty()) {
        candidates.push_back(sdk);
    }
    for (const std::wstring& dir : candidates) {
        if (fileExists(dir + L"\\dxcompiler.dll")) {
            return dir;
        }
    }
    return std::wstring();
}


std::wstring toWide(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        return std::wstring();
    }
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}



// 用 DXC 把 HLSL 编到 DXIL；成功时 outDxil 收到字节码，失败时返回 mcdr::Error（诊断首行）。
// P72：入口点与源码长度都变成显式参数（原实现写死 entry="main" 且要求 NUL 结尾），
// 这样 as_6_x / ms_6_x / ps_6_x 都可以走同一条编译路径。
mcdr::Result<void> compileWithDxc(IDxcUtils* utils, IDxcCompiler3* compiler, const char* source,
                                  size_t sourceLen, const std::string& entry, const std::string& target,
                                  std::vector<uint8_t>& outDxil) {
    ComPtr<IDxcBlobEncoding> srcBlob;
    if (FAILED(utils->CreateBlobFromPinned(source, static_cast<UINT32>(sourceLen), DXC_CP_UTF8,
                                           &srcBlob))) {
        return mcdr::err("compileWithDxc", "CreateBlobFromPinned failed");
    }
    DxcBuffer buffer{};
    buffer.Ptr = srcBlob->GetBufferPointer();
    buffer.Size = srcBlob->GetBufferSize();
    buffer.Encoding = DXC_CP_UTF8;

    const std::wstring targetW = toWide(target);
    const std::wstring entryW = toWide(entry);
    const wchar_t* args[] = {L"-T",  targetW.c_str(), L"-E", entryW.c_str(),
                             L"-HV", L"2018",         L"-O3"};
    ComPtr<IDxcResult> result;
    const HRESULT hr =
        compiler->Compile(&buffer, args, static_cast<UINT32>(std::size(args)), nullptr,
                          IID_PPV_ARGS(&result));
    if (FAILED(hr) || !result) {
        return mcdr::err("compileWithDxc", "IDxcCompiler3::Compile entry failed hr=" + hexHr(hr));
    }
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    if (FAILED(status)) {
        ComPtr<IDxcBlobEncoding> errBlob;
        std::string dxcError;
        if (SUCCEEDED(result->GetErrorBuffer(&errBlob)) && errBlob) {
            dxcError.assign(static_cast<const char*>(errBlob->GetBufferPointer()),
                            static_cast<size_t>(errBlob->GetBufferSize()));
        } else {
            dxcError = "DXC compile failed hr=" + hexHr(status);
        }
        return mcdr::err("compileWithDxc", std::move(dxcError));
    }
    ComPtr<IDxcBlob> object;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object) {
        return mcdr::err("compileWithDxc", "DXC produced no DXIL object");
    }
    const auto* bytes = static_cast<const uint8_t*>(object->GetBufferPointer());
    outDxil.assign(bytes, bytes + object->GetBufferSize());
    return {};
}

// ---------------------------------------------------------------------------
// P99（DXIL 磁盘缓存）：把 DXC 的编译结果缓存到磁盘。
// 动机（用户报告「启动加载界面冻结 / 加载世界弹出瞬间帧时间骤增」）：DXC 每次编译
// 实测 0.4-3 s 且全部落在主线程；一个会话要编译几十个着色器（主图形管线 + blit +
// 速度/后处理），启动阶段累计停顿十几秒（P98 日志：519/697/553/423/726/496/2020/
// 3079/2938 ms 等 gap）。同一份 HLSL + 同一 profile + 同一 dxcompiler.dll 产出的 DXIL
// 是确定的，因此第二次启动可直接读回缓存、跳过 DXC。
// 键 = FNV-1a 64(dxc 版本标签 + profile + entry + HLSL 源码)；dxcompiler.dll 被替换
// （大小/时间戳变化）时键自动变化，旧缓存自然失效。
// 开关：MCDR_DXIL_CACHE=0 关闭；MCDR_DXIL_CACHE_DIR=<dir> 指定目录（默认 MCDR.dll
// 同目录下的 MCDR_dxil_cache）。
std::wstring dxilCacheDirectory() {
    if (const char* env = std::getenv("MCDR_DXIL_CACHE_DIR")) {
        if (env[0] != '\0') return toWide(env);
    }
    const std::wstring selfDir = moduleDirectory();
    if (selfDir.empty()) return {};
    return selfDir + L"\\MCDR_dxil_cache";
}

uint64_t fnv1a64(const void* data, size_t len, uint64_t h = 1469598103934665603ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ull;
    }
    return h;
}

std::wstring dxilCachePath(const std::wstring& dir, uint64_t key) {
    return dir + L"\\" + std::to_wstring(key) + L".dxil";
}

bool dxilCacheLoad(const std::wstring& dir, uint64_t key, std::vector<uint8_t>& out) {
    if (dir.empty()) return false;
    const HANDLE f = CreateFileW(dxilCachePath(dir, key).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0 || size.QuadPart > (64 << 20)) {
        CloseHandle(f);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(f);
    if (!ok || static_cast<size_t>(read) != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

void dxilCacheStore(const std::wstring& dir, uint64_t key, const std::vector<uint8_t>& dxil) {
    if (dir.empty() || dxil.empty()) return;
    CreateDirectoryW(dir.c_str(), nullptr);  // 已存在时返回 ERROR_ALREADY_EXISTS，忽略
    const HANDLE f = CreateFileW(dxilCachePath(dir, key).c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, dxil.data(), static_cast<DWORD>(dxil.size()), &written, nullptr);
    CloseHandle(f);
}

}  // namespace
#endif  // MCDR_HAVE_DXCAPI


void probeDxc(DeviceContext::Capabilities& caps) {
#if !MCDR_HAVE_DXCAPI
    caps.dxcNote = "built without <dxcapi.h>";
    (void)caps;
#else
    const std::wstring dir = findDxcDirectory();
    if (dir.empty()) {
        // 保留既有文案（其它调用方/工具会 grep 它），只在验收开关生效时额外说明**原因**：
        // 正常机器上"找不到 dxcompiler.dll"与"我们主动短路了搜索链"是两件事，日志必须能区分，
        // 否则这条回退路径的验收结论就成了空话。
        caps.dxcNote = "dxcompiler.dll not found (MCDR_DXC_DIR / MCDR.dll dir / Windows SDK)";
        if (const wchar_t* off = _wgetenv(L"MCDR_DXC_DISABLE");
            off != nullptr && off[0] == L'1' && off[1] == L'\0') {
            dbgLogInfo("DXC discovery was short-circuited by MCDR_DXC_DISABLE=1 — this is the "
                       "acceptance switch for the \"no DXC\" path (graphics pipelines now fail "
                       "instead of falling back to D3DCompile SM5.1), not a missing-SDK condition");
        }
        return;
    }
    const std::wstring dllPath = dir + L"\\dxcompiler.dll";
    caps.dxcPath = backendWideToUtf8(dllPath);

    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (mod == nullptr) {
        caps.dxcNote = "LoadLibraryW(dxcompiler.dll) failed err=" + std::to_string(GetLastError());
        return;
    }
    const auto createInstance =
        reinterpret_cast<PFN_DxcCreateInstance>(GetProcAddress(mod, "DxcCreateInstance"));
    if (createInstance == nullptr) {
        caps.dxcNote = "DxcCreateInstance export missing";
        return;
    }
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    if (FAILED(createInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) || !utils
        || FAILED(createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) || !compiler) {
        caps.dxcNote = "DxcCreateInstance failed (dxil.dll missing next to dxcompiler.dll?)";
        return;
    }
    caps.dxcAvailable = true;

    // 冒烟编译目标：**固定 ms_6_9**（2026-10-25 起图形管线硬要求 SM 6.9，没有 5.1 兜底）。
    // 这里只回答"DXC 能不能产出 6_9 的 DXIL"，与设备能力无关 —— 设备那一半由
    // graphicsShaderModelProfile() 查 caps.shaderModel 单独把关。
    // 实测 DXC 1.9.2602.17 接受 vs/ps/as_6_9；1.8 及更早对 6_9 报 invalid profile
    // （6_10 至今没有 DXC 认，所以 6_9 就是天花板）。
    const std::string profile = "ms_6_9";
    caps.dxcProfile = profile;

    std::vector<uint8_t> dxil;
    auto smoke = compileWithDxc(utils.Get(), compiler.Get(), kMeshSmokeHlsl, std::strlen(kMeshSmokeHlsl),
                                "main", profile, dxil);
    if (smoke) {
        caps.meshShaderCompileOk = true;
        caps.dxcNote = "mesh smoke compile OK (" + profile + ", "
                       + std::to_string(dxil.size()) + " bytes DXIL)";
    } else {
        caps.dxcNote = "mesh smoke compile failed: " + firstLine(smoke.error().message);
    }
#endif
}

// ---------------------------------------------------------------------------
// P72：可复用的 DXC 编译入口（mesh shader 基础）
// ---------------------------------------------------------------------------
// probeDxc 只回答"这台机器能不能编译 mesh shader"，随后就把 IDxcUtils/IDxcCompiler3
// 释放掉了；mesh 管线真正需要的是一个运行期可反复调用的编译入口：Java 侧给 HLSL 源码
// + 入口点 + profile（as_6_6 / ms_6_6 / ps_6_0 ...），拿回 DXIL 字节码交给
// createMeshPipeline。这里把 dxcompiler.dll 与两个接口缓存成进程级单例，只加载一次。
mcdr::Result<void> compileHlslToDxil(const std::string& source, const std::string& entry,
                                    const std::string& profile, std::vector<uint8_t>& outDxil) {
#if !MCDR_HAVE_DXCAPI
    (void)source;
    (void)entry;
    (void)profile;
    (void)outDxil;
    return mcdr::err("compileHlslToDxil", "compileHlslToDxil: built without <dxcapi.h>");
#else
    struct CachedDxc {
        HMODULE module = nullptr;
        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcCompiler3> compiler;
        bool tried = false;
        std::string note;
        // P99：dxcompiler.dll 的版本标签（大小 + 最后写入时间），参与 DXIL 缓存键。
        std::string versionTag;
    };
    // 进程级单例；C++11 起函数内 static 初始化线程安全。
    // 故意 new 出来且永不 delete（等价于"进程级泄漏"）：**绝不能让这个单例在进程退出时被析构**。
    // 析构里对 DXC 的 COM 对象做 Release，会在 CRT 退出阶段触发 ucrtbase 的 fail-fast
    // （事件日志：java.exe / 出错模块 ucrtbase.dll / 异常代码 0xc0000409 / BEX64），
    // 于是"渲染完全正常、destroyDevice: done"之后整个进程仍以 -1073740791 退出，
    // Gradle 报 BUILD FAILED。与 dxcore.dll 故意不 FreeLibrary（见下方 queryDxCoreInfo）同理。
    // 取证：MCDR\src\core\d3d12_device.cpp 本函数 + _port\gsm68_*_info.err.log + 事件日志 Id=1000。
    static CachedDxc& cached = *new CachedDxc();
    if (!cached.tried) {
        cached.tried = true;
        const std::wstring dir = findDxcDirectory();
        if (dir.empty()) {
            cached.note = "dxcompiler.dll not found (MCDR_DXC_DIR / MCDR.dll dir / Windows SDK)";
            // 同 capa 探测那一处：验收开关生效时说明**原因**，否则"主动短路"与"真缺 SDK"
            // 在日志里长得一样，回退路径就无法取证。
            if (const wchar_t* off = _wgetenv(L"MCDR_DXC_DISABLE");
                off != nullptr && off[0] == L'1' && off[1] == L'\0') {
                dbgLogInfo("DXC discovery was short-circuited by MCDR_DXC_DISABLE=1 (acceptance "
                           "switch for the no-DXC path; graphics pipelines now fail instead of "
                           "falling back to D3DCompile SM5.1)");
            }
        } else {
            const std::wstring dllPath = dir + L"\\dxcompiler.dll";
            // P99：记录 dxcompiler.dll 的大小+写入时间，作为 DXIL 缓存键的一部分。
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(dllPath.c_str(), GetFileExInfoStandard, &fad)) {
                cached.versionTag = std::to_string(fad.nFileSizeHigh) + ":" +
                                    std::to_string(fad.nFileSizeLow) + ":" +
                                    std::to_string(fad.ftLastWriteTime.dwHighDateTime) + ":" +
                                    std::to_string(fad.ftLastWriteTime.dwLowDateTime);
            }
            cached.module = LoadLibraryW(dllPath.c_str());
            if (cached.module == nullptr) {
                cached.note =
                    "LoadLibraryW(dxcompiler.dll) failed err=" + std::to_string(GetLastError());
            } else {
                const auto createInstance = reinterpret_cast<PFN_DxcCreateInstance>(
                    GetProcAddress(cached.module, "DxcCreateInstance"));
                if (createInstance == nullptr) {
                    cached.note = "DxcCreateInstance export missing";
                } else if (FAILED(createInstance(CLSID_DxcUtils, IID_PPV_ARGS(&cached.utils)))
                           || !cached.utils
                           || FAILED(createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&cached.compiler)))
                           || !cached.compiler) {
                    cached.note = "DxcCreateInstance failed (dxil.dll missing next to dxcompiler.dll?)";
                } else {
                    cached.note = "dxcompiler.dll loaded from " + backendWideToUtf8(dllPath);
                }
            }
        }
        dbgLogInfo("compileHlslToDxil: DXC init — %s", cached.note.c_str());
    }
    if (!cached.utils || !cached.compiler) {
        return mcdr::err("compileHlslToDxil", "compileHlslToDxil: " + cached.note);
    }
    if (source.empty()) {
        return mcdr::err("compileHlslToDxil", "compileHlslToDxil: empty HLSL source");
    }
    if (profile.empty()) {
        return mcdr::err("compileHlslToDxil", "compileHlslToDxil: empty profile");
    }
    const std::string entryName = entry.empty() ? std::string("main") : entry;
    // P99：先查 DXIL 磁盘缓存（键含 dxcompiler.dll 版本 + profile + entry + 源码）。
    const char* cacheEnv = std::getenv("MCDR_DXIL_CACHE");
    const bool cacheEnabled = !(cacheEnv != nullptr && cacheEnv[0] == '0');
    const std::wstring cacheDir = cacheEnabled ? dxilCacheDirectory() : std::wstring();
    uint64_t cacheKey = 0;
    if (cacheEnabled) {
        cacheKey = fnv1a64(cached.versionTag.data(), cached.versionTag.size());
        cacheKey = fnv1a64(profile.data(), profile.size(), cacheKey);
        cacheKey = fnv1a64(entryName.data(), entryName.size(), cacheKey);
        cacheKey = fnv1a64(source.data(), source.size(), cacheKey);
        if (dxilCacheLoad(cacheDir, cacheKey, outDxil)) {
            dbgLogInfo("compileHlslToDxil: %s entry=%s -> %zu bytes DXIL (disk cache hit)",
                       profile.c_str(), entryName.c_str(), outDxil.size());
            return {};
        }
    }
    const double p98CompileT0 = nowMs();
    {
        auto compiled = compileWithDxc(cached.utils.Get(), cached.compiler.Get(), source.data(),
                                       source.size(), entryName, profile, outDxil);
        if (!compiled) return std::unexpected<mcdr::Error>(std::move(compiled.error()));
    }
    // P98：把编译耗时打出来——这是启动/入场冻结的主要来源之一（缓存未命中时 0.4-3 s）。
    dbgLogInfo("compileHlslToDxil: %s entry=%s -> %zu bytes DXIL (took %.0f ms)", profile.c_str(),
               entryName.c_str(), outDxil.size(), nowMs() - p98CompileT0);
    if (cacheEnabled) {
        dxilCacheStore(cacheDir, cacheKey, outDxil);
    }
    return {};
#endif
}


std::string backendWideToUtf8(const std::wstring& w) {
    if (w.empty()) {
        return std::string();
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

std::string formatBackendResult(HRESULT hr) {
    char buf[64];
    snprintf(buf, sizeof(buf), "HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

std::string graphicsShaderModelProfile() {
    if (!deviceContext().caps.dxcAvailable || !deviceContext().caps.dxcSupportsSm69()) return {};
    if (effectiveShaderModel(deviceContext().caps) < D3D_SHADER_MODEL_6_9) return {};
    return "6_9";
}

// 6.9 硬要求满足不了时的统一说明：把"缺 DXC / DXC 不认 6.9 / 设备只到 6.x"分开说清楚，
// 否则现场只剩一句 CreatePipelineState 失败，排查要重新走一遍 P84 那套。
std::string sm69RequirementText() {
    if (!deviceContext().caps.dxcAvailable) {
        return "shader model 6.9 is required and there is no SM 5.1 fallback any more, but "
               "dxcompiler.dll was not found (MCDR_DXC_DIR / MCDR.dll dir / Windows SDK)";
    }
    if (!deviceContext().caps.dxcSupportsSm69()) {
        return "shader model 6.9 is required and there is no SM 5.1 fallback any more, but this "
               "dxcompiler.dll cannot compile 6_9 (profile probe: " + deviceContext().caps.dxcProfile +
               "; needs DXC 1.9.2502 or newer)";
    }
    return "shader model 6.9 is required and there is no SM 5.1 fallback any more, but the device "
           "reports SM " + std::string(shaderModelName(deviceContext().caps.shaderModel)) +
           " (Agility SDK 619's D3D12Core.dll must sit next to MCDR.dll and be opted in)";
}


std::string firstLine(const std::string& text) {
    const size_t nl = text.find_first_of("\r\n");
    std::string line = nl == std::string::npos ? text : text.substr(0, nl);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    if (line.size() > 220) {
        line.resize(220);
        line += "...";
    }
    return line;
}
std::string errorMessage(const mcdr::Error& e) { return e.message; }

}
