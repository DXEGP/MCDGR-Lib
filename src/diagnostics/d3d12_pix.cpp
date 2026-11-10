module;

#include "_d3d12_module_prelude.h"
#include <dbghelp.h>
#include <cwchar>
#pragma comment(lib, "dbghelp.lib")

module mcdr.d3d12.device;

namespace d3d12_mc {
namespace {

HMODULE gPixCapturer = nullptr;
int gPixStatus = 0;  // 1=已载入 0=未请求 -1=请求了但没载入（见 setPixCapture）
bool gPixRequested = false;

// 配置项 [debug] pixDirectory 的值（mcdrSetPixDirectory 推过来）。空 = 自动检测。
// 刻意不用环境变量：配置项才是入口，环境变量只是某个人的临时逃生口，进不了文档也就进不了
// 用户的脑子（见 AGENTS.md「开关一律走 NeoForge 配置」）。
std::wstring gPixDirectory;

bool pixIsFile(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool pixIsDirectory(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// MCDR.dll 自己所在的目录（按本 TU 里的数据地址反查模块，而不是取进程 exe）。
std::wstring pixSelfDirectory() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&gPixCapturer), &self)
        || self == nullptr) {
        return std::wstring();
    }
    wchar_t buffer[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(self, buffer, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return std::wstring();
    }
    const std::wstring path(buffer, n);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::string pixToUtf8(const std::wstring& w) {
    if (w.empty()) {
        return std::string();
    }
    const int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), need,
                        nullptr, nullptr);
    return out;
}

// %ProgramFiles%\Microsoft PIX\<版本>\ —— PIX 按版本号装子目录，取版本号最大的一个。
std::wstring pixInstallDirectory() {
    const wchar_t* programFiles = _wgetenv(L"ProgramFiles");
    const std::wstring root =
        std::wstring(programFiles && *programFiles ? programFiles : L"C:\\Program Files")
        + L"\\Microsoft PIX\\";
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
        if (pixIsFile(root + ver + L"\\WinPixGpuCapturer.dll")) {
            best = ver;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best.empty() ? std::wstring() : root + best;
}

// 搜索顺序：[debug] pixDirectory（dll 本身或所在目录）→ MCDR.dll 同目录 → PIX 安装目录最新版。
std::wstring findPixCapturerDll() {
    if (!gPixDirectory.empty()) {
        if (pixIsFile(gPixDirectory)) {
            return gPixDirectory;
        }
        if (pixIsDirectory(gPixDirectory)) {
            const std::wstring inside = gPixDirectory + L"\\WinPixGpuCapturer.dll";
            if (pixIsFile(inside)) {
                return inside;
            }
        }
    }
    const std::wstring selfDir = pixSelfDirectory();
    if (!selfDir.empty()) {
        const std::wstring next = selfDir + L"\\WinPixGpuCapturer.dll";
        if (pixIsFile(next)) {
            return next;
        }
    }
    const std::wstring pixDir = pixInstallDirectory();
    if (!pixDir.empty()) {
        return pixDir + L"\\WinPixGpuCapturer.dll";
    }
    return std::wstring();
}

// 幂等：只尝试一次（成功或失败都记住）。1=已载入 0=未请求 -1=请求了但没载入。
int ensurePixCapturerLoaded() {
    static bool attempted = false;
    if (!gPixRequested) {
        // 还没人要求（配置是稍后才读到的）：保持可重试，别把这次“没请求”记成结论。
        return 0;
    }
    if (attempted) {
        return gPixStatus;
    }
    attempted = true;
    const std::wstring path = findPixCapturerDll();
    if (!path.empty()) {
        // LOAD_WITH_ALTERED_SEARCH_PATH：让捕获层从它自己所在目录解析依赖 —— 按绝对路径
        // 直接 LoadLibraryW 的话，PIX 目录并不在进程的搜索路径里。
        gPixCapturer = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (gPixCapturer) {
            dbgLogInfo("PIX GPU capture: capture layer loaded from %s", pixToUtf8(path).c_str());
            dbgLogInfo("PIX GPU capture: PIX has to be running to save a capture; expect a small "
                       "per-frame overhead while the layer is loaded");
            gPixStatus = 1;
            return 1;
        }
        dbgLogWarn("PIX GPU capture: LoadLibraryExW(%s) failed err=%lu", pixToUtf8(path).c_str(),
                   static_cast<unsigned long>(GetLastError()));
    }
    // 兜底：dll 已经在进程搜索路径里（用户自己部署，或 PIX 装过）。
    gPixCapturer = LoadLibraryW(L"WinPixGpuCapturer.dll");
    if (gPixCapturer) {
        dbgLogInfo("PIX GPU capture: capture layer loaded from the DLL search path");
        gPixStatus = 1;
        return 1;
    }
    const std::wstring pixDir = pixInstallDirectory();
    dbgLogError("PIX GPU capture: enabled but WinPixGpuCapturer.dll was not found — searched "
                "[debug] pixDirectory (%s), %s and %s; this run captures nothing",
                gPixDirectory.empty() ? "<empty>" : pixToUtf8(gPixDirectory).c_str(),
                pixToUtf8(pixSelfDirectory()).c_str(),
                pixDir.empty() ? "<no Microsoft PIX install under %ProgramFiles%>"
                               : pixToUtf8(pixDir).c_str());
    gPixStatus = -1;
    return -1;
}

}  // namespace

int loadPixCapturer() { return ensurePixCapturerLoaded(); }

namespace {

// 定义在同文件后面的 timing 捕获段里（见「PIX 程序化 timing 捕获」）。
bool ensurePixTimingRuntime();

}  // namespace

// JVM 侧的配置入口（mcdrSetPixCapture）。
int setPixCapture(bool enabled) {
    gPixRequested = enabled;
    if (!enabled) {
        dbgLogInfo("PIX GPU capture: disabled by configuration");
        return 0;
    }
    const int status = ensurePixCapturerLoaded();
    // timing 捕获（startPixTimingCapture）要的两个库也在这里载入：PIX 的捕获层挂钩 D3D12/ETW
    // 是在 LoadLibrary 那一刻做的，设备与呈现链都建好之后再载入只能抓到一半的数据。所以
    // [debug] pixCapture / pixTimingCapture 任一为真时 JVM 侧都会推一次 true 到这里。
    ensurePixTimingRuntime();
    return status;
}

// ---------------------------------------------------------------------------
// PIX 程序化 timing 捕获（mcdrStartPixTimingCapture / mcdrStopPixTimingCapture）
// ---------------------------------------------------------------------------
// 为什么需要它：PIX 界面上的持续捕获（Counters recording）把数据留在 PIX 会话里，用户不点
// Save 就一个文件也拿不到；而 attach 到进程后抓的那一帧是 PIX 自己落在
// %TEMP%\<时间戳>.wpix 的。想让捕获自动写到 run/ 目录下，只能由本进程发起：
//     PIXBeginCapture(PIX_CAPTURE_TIMING, { file_name }) … PIXEndCapture(0)
// 这条路径照搬 wgpu-mc-neo 的实现（rust\wgpu-mc-jni\src\pix.rs），三个细节必须保留：
//   * 两个库：导出 PIXBeginCapture/PIXEndCapture 的是 WinPixEventRuntime*.dll，真正干活的是
//     WinPixTimingCapturer.dll。本机的 PIX 2603.25 只带 OneCore 版运行时（NuGet 包里那个
//     WinPixEventRuntime.dll 不存在），而 OneCore 版只导出 PIXBeginCapture2 —— 与
//     PIXBeginCapture 文档等价，两个符号都试。
//   * 线程：PIX 运行库载入时会初始化 COM，渲染线程的 apartment 早已定好，从那里调用恒返回
//     RPC_E_CHANGED_MODE (0x80010106)。所以 begin 在 CreateThread 出来的线程上做（主线程等
//     ≤10 s 取结果），end 同样另起线程但**不等** —— 它要把整个捕获写盘（开内存事件时从几百
//     MB 到几 GB）。
//   * ETW 权限：timing 捕获走 ETW 会话，创建会话的权限默认只给管理员。2026-10-21 本机实测：
//     当前用户**已经**在 `Performance Log Users`（S-1-5-32-559）里（`whoami /groups` 显示
//     Enabled group），PIX 仍返回 E_ACCESSDENIED；同一条探针在提权进程里是 S_OK 并真的写出了
//     .wpix。所以 PIX 文档那句「加进组即可」在本机不成立，**必须有提权 token**；而 `gradlew`
//     会复用没提权的 daemon，从管理员终端启动也不够，得先 `gradlew --stop`。
//     这里不做提权检查，失败就把 HRESULT 与这条提示一起打出来。
namespace {

// PIXCaptureParameters 的前 14 个字段，顺序与类型取自 WinPixEventRuntime 1.0.240308001 的
// Include/WinPixEventRuntime/pix3.h（该头文件不随任何 SDK 发布，所以这里手写）。尾部
// reserved[16] 不在那个头文件里：PIX 运行时与本结构体是分别版本化的，而 API 只收指针、
// 没法告诉它结构体多长，新版 PIX 读到我们不认识的字段时会读到栈上的垃圾；留 16 个 u64 的
// 零，让它读到的都是「关」。
struct PixCaptureParameters {
    const wchar_t* file_name;
    uint32_t maximum_tooling_memory_size_mb;
    uint32_t capture_storage;
    int32_t capture_gpu_timing;
    int32_t capture_callstacks;
    int32_t capture_cpu_samples;
    uint32_t cpu_samples_per_second;
    int32_t capture_file_io;
    int32_t capture_virtual_alloc_events;
    int32_t capture_heap_alloc_events;
    int32_t capture_xmem_events;
    int32_t capture_pix_mem_events;
    int32_t capture_page_fault_events;
    int32_t capture_video_frames;
    uint64_t reserved[16];
};

using PixBeginCaptureFn = HRESULT(__stdcall*)(unsigned long, const void*);
using PixEndCaptureFn = HRESULT(__stdcall*)(unsigned long);

constexpr unsigned long kPixCaptureTiming = 1u;       // PIX_CAPTURE_TIMING
constexpr uint32_t kPixCaptureStorageMemory = 0u;     // PIX_CAPTURE_STORAGE_MEMORY
constexpr uint32_t kPixCpuSamplesPerSecond = 4000u;   // 微软自家示例用的采样率
constexpr uint32_t kPixMaximumToolingMemoryMb = 4096u;
constexpr DWORD kPixBeginWaitMs = 10000;

HMODULE gPixTimingCapturer = nullptr;
HMODULE gPixTimingRuntime = nullptr;
PixBeginCaptureFn gPixTimingBegin = nullptr;
PixEndCaptureFn gPixTimingEnd = nullptr;
bool gPixTimingLoadTried = false;
bool gPixTimingCapturing = false;
// begin 的线程拿的是这个字符串的 c_str()，所以它必须活到线程结束之后 —— 放在文件级 static
// 里，一次运行只写一遍，不参与任何生命周期管理。
std::wstring gPixTimingFilePath;

std::wstring pixFromUtf8(const char* utf8) {
    if (utf8 == nullptr || utf8[0] == '\0') {
        return std::wstring();
    }
    const int need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (need <= 1) {
        return std::wstring();
    }
    std::wstring out(static_cast<size_t>(need), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), need);
    if (written <= 1) {
        return std::wstring();
    }
    out.resize(static_cast<size_t>(written - 1));
    return out;
}

// 与 GPU 捕获层同一套搜索顺序：[debug] pixDirectory（dll 本身或所在目录）→ MCDR.dll 同目录
// → %ProgramFiles%\Microsoft PIX\ 下版本号最大的安装目录。
std::wstring findPixRuntimeFile(const wchar_t* fileName) {
    if (!gPixDirectory.empty()) {
        if (pixIsFile(gPixDirectory)) {
            // 配置项直接指到某个 dll 时，兄弟 dll 就在它旁边。
            const size_t slash = gPixDirectory.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                const std::wstring sibling = gPixDirectory.substr(0, slash + 1) + fileName;
                if (pixIsFile(sibling)) {
                    return sibling;
                }
            }
        } else if (pixIsDirectory(gPixDirectory)) {
            const std::wstring inside = gPixDirectory + L"\\" + fileName;
            if (pixIsFile(inside)) {
                return inside;
            }
        }
    }
    const std::wstring selfDir = pixSelfDirectory();
    if (!selfDir.empty()) {
        const std::wstring next = selfDir + L"\\" + fileName;
        if (pixIsFile(next)) {
            return next;
        }
    }
    const std::wstring pixDir = pixInstallDirectory();
    if (!pixDir.empty()) {
        const std::wstring candidate = pixDir + L"\\" + fileName;
        if (pixIsFile(candidate)) {
            return candidate;
        }
    }
    return std::wstring();
}

const char* pixHresultNote(HRESULT hr) {
    switch (static_cast<unsigned long>(hr)) {
        case 0x80070005ul: return ", E_ACCESSDENIED";
        case 0x8000000Aul: return ", E_PENDING";
        case 0x8007000Aul: return ", E_PENDING";
        case 0x80070057ul: return ", E_INVALIDARG";
        case 0x80004005ul: return ", E_FAIL";
        case 0x8000FFFFul: return ", E_UNEXPECTED";
        case 0x800401F0ul: return ", CO_E_NOTINITIALIZED";
        case 0x80010106ul: return ", RPC_E_CHANGED_MODE";
        default: return "";
    }
}

struct PixBeginJob {
    const wchar_t* file = nullptr;
    // E_PENDING 的初值让「等超时了」与「PIX 回了 S_OK」区分得开（S_OK == 0）。
    HRESULT result = static_cast<HRESULT>(0x8000000Au);
};

DWORD WINAPI pixBeginThread(void* raw) {
    auto* job = static_cast<PixBeginJob*>(raw);
    PixCaptureParameters params{};
    params.file_name = job->file;
    // 这两个字段在 Windows 上被 PIX 忽略，留着是因为结构体与 Xbox 版共用。
    params.maximum_tooling_memory_size_mb = kPixMaximumToolingMemoryMb;
    params.capture_storage = kPixCaptureStorageMemory;
    params.capture_gpu_timing = 1;
    // CPU 采样 + 调用栈是 timing 捕获的另一半：只有 GPU 时间只能说这一帧 5 ms，有了它们才看得
    // 出这一帧 CPU 上跑了什么、哪一次调用在等。
    params.capture_callstacks = 1;
    params.capture_cpu_samples = 1;
    params.cpu_samples_per_second = kPixCpuSamplesPerSecond;
    // 内存那一半：文件 IO、VirtualAlloc、HeapAlloc、PIX 自己的分配器、缺页事件，缺一个对应
    // 的表就是空的。
    params.capture_file_io = 1;
    params.capture_virtual_alloc_events = 1;
    params.capture_heap_alloc_events = 1;
    params.capture_xmem_events = 0;   // Xbox only
    params.capture_pix_mem_events = 1;
    params.capture_page_fault_events = 1;
    params.capture_video_frames = 0;  // Xbox only
    job->result = gPixTimingBegin(kPixCaptureTiming, &params);
    return 0;
}

DWORD WINAPI pixEndThread(void* raw) {
    (void)raw;
    // 不在这条线程上打日志：它与渲染线程并发，两边同时写同一个日志 sink 会把行撕开。
    (void)gPixTimingEnd(0u);
    return 0;
}

bool ensurePixTimingRuntime() {
    if (gPixTimingLoadTried) {
        return gPixTimingBegin != nullptr && gPixTimingEnd != nullptr;
    }
    gPixTimingLoadTried = true;
    const std::wstring capturerPath = findPixRuntimeFile(L"WinPixTimingCapturer.dll");
    if (!capturerPath.empty()) {
        gPixTimingCapturer =
            LoadLibraryExW(capturerPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (gPixTimingCapturer != nullptr) {
            dbgLogInfo("PIX timing capture: WinPixTimingCapturer.dll loaded from %s",
                       pixToUtf8(capturerPath).c_str());
        } else {
            dbgLogWarn("PIX timing capture: LoadLibraryExW(%s) failed err=%lu",
                       pixToUtf8(capturerPath).c_str(),
                       static_cast<unsigned long>(GetLastError()));
        }
    }
    // 运行时：NuGet 包里的 WinPixEventRuntime.dll 优先，其次 PIX 自带的 OneCore 版。
    const wchar_t* const runtimeNames[] = {L"WinPixEventRuntime.dll",
                                           L"WinPixEventRuntime_OneCore.dll"};
    for (const wchar_t* name : runtimeNames) {
        const std::wstring path = findPixRuntimeFile(name);
        if (path.empty()) {
            continue;
        }
        HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module == nullptr) {
            continue;
        }
        gPixTimingBegin =
            reinterpret_cast<PixBeginCaptureFn>(GetProcAddress(module, "PIXBeginCapture"));
        if (gPixTimingBegin == nullptr) {
            gPixTimingBegin =
                reinterpret_cast<PixBeginCaptureFn>(GetProcAddress(module, "PIXBeginCapture2"));
        }
        gPixTimingEnd = reinterpret_cast<PixEndCaptureFn>(GetProcAddress(module, "PIXEndCapture"));
        if (gPixTimingBegin != nullptr && gPixTimingEnd != nullptr) {
            gPixTimingRuntime = module;
            dbgLogInfo("PIX timing capture: PIXBeginCapture/PIXEndCapture from %s",
                       pixToUtf8(path).c_str());
            break;
        }
        FreeLibrary(module);
        gPixTimingBegin = nullptr;
        gPixTimingEnd = nullptr;
    }
    if (gPixTimingBegin == nullptr || gPixTimingEnd == nullptr) {
        dbgLogError("PIX timing capture: no PIXBeginCapture/PIXEndCapture in this process — looked "
                    "for WinPixEventRuntime.dll and WinPixEventRuntime_OneCore.dll in "
                    "[debug] pixDirectory (%s), %s and %s",
                    gPixDirectory.empty() ? "<empty>" : pixToUtf8(gPixDirectory).c_str(),
                    pixToUtf8(pixSelfDirectory()).c_str(), pixToUtf8(pixInstallDirectory()).c_str());
        return false;
    }
    return true;
}

}  // namespace

// 配置项 [debug] pixDirectory 的入口（mcdrSetPixDirectory）：PIX 的 dll 本身，或它们所在的目录。
// 必须在 setPixCapture() 之前调用 —— 捕获层要在第一个 D3D12 设备创建之前载入，路径那时就得定。
// 1=已设置 0=已清空（回到自动检测）-1=路径无效（非 UTF-8）。
int setPixDirectory(const char* utf8Path) {
    if (utf8Path == nullptr || utf8Path[0] == '\0') {
        gPixDirectory.clear();
        dbgLogInfo("PIX: [debug] pixDirectory is empty; searching the default locations");
        return 0;
    }
    std::wstring wide = pixFromUtf8(utf8Path);
    if (wide.empty()) {
        dbgLogWarn("PIX: [debug] pixDirectory(%s) is not valid UTF-8; ignoring it", utf8Path);
        return -1;
    }
    gPixDirectory = std::move(wide);
    // 目录写错是最常见的失败：这里就把结论说清楚，别等到载入时才报「找不到 dll」。
    if (pixIsFile(gPixDirectory)) {
        dbgLogInfo("PIX: [debug] pixDirectory -> file %s", pixToUtf8(gPixDirectory).c_str());
    } else if (pixIsDirectory(gPixDirectory)) {
        dbgLogInfo("PIX: [debug] pixDirectory -> directory %s", pixToUtf8(gPixDirectory).c_str());
    } else {
        dbgLogWarn("PIX: [debug] pixDirectory(%s) does not exist; falling back to the default "
                   "locations", pixToUtf8(gPixDirectory).c_str());
        gPixDirectory.clear();
        return -1;
    }
    return 1;
}

// 开始一次 timing 捕获，写到 utf8Path 指定的 .wpix。1=已开始 0=已经在录
// 负值=失败的 HRESULT（原因见日志）。调 JVM 那边直接把它当 HRESULT 显示。
int startPixTimingCapture(const char* utf8Path) {
    if (gPixTimingCapturing) {
        dbgLogWarn("PIX timing capture: already recording; mcdrStartPixTimingCapture ignored");
        return 0;
    }
    if (!ensurePixTimingRuntime()) {
        return static_cast<int>(E_FAIL);
    }
    gPixTimingFilePath = pixFromUtf8(utf8Path);
    if (gPixTimingFilePath.empty()) {
        dbgLogError("PIX timing capture: empty or invalid output path");
        return static_cast<int>(E_INVALIDARG);
    }
    // 这个 job 故意不回收：等待超时后线程还在往里写，delete 掉就是 use-after-free。
    auto* const job = new PixBeginJob();
    job->file = gPixTimingFilePath.c_str();
    HANDLE thread = CreateThread(nullptr, 0, &pixBeginThread, job, 0, nullptr);
    if (thread == nullptr) {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        dbgLogError("PIX timing capture: CreateThread failed (HRESULT 0x%08lX)",
                    static_cast<unsigned long>(hr));
        return static_cast<int>(hr);
    }
    const DWORD waited = WaitForSingleObject(thread, kPixBeginWaitMs);
    CloseHandle(thread);
    if (waited != WAIT_OBJECT_0) {
        dbgLogError("PIX timing capture: PIXBeginCapture did not answer within %lu ms (waited=%lu)",
                    static_cast<unsigned long>(kPixBeginWaitMs),
                    static_cast<unsigned long>(waited));
        return static_cast<int>(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    const HRESULT hr = job->result;
    if (FAILED(hr)) {
        if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
            // 上一轮被强杀时 PIX 的 ETW 会话还在（PixSysMonSession.* /
            // LongRunningTimingCaptureSession.*），PIX 就会以「已存在」拒绝新的一次捕获。
            dbgLogError("PIX timing capture: refused with ERROR_ALREADY_EXISTS for %s — a capture "
                        "session from an earlier run is still alive (that run was probably killed "
                        "mid-capture). List them with `logman query -ets` and stop the two PIX "
                        "sessions with `logman stop <name> -ets`, then press the key again",
                        pixToUtf8(gPixTimingFilePath).c_str());
            return static_cast<int>(hr);
        }
        dbgLogError("PIX timing capture: PIXBeginCapture refused (HRESULT 0x%08lX%s) for %s — a "
                    "timing capture records through ETW sessions, and creating them needs an "
                    "elevated (administrator) token on this machine: the current user is already in "
                    "`Performance Log Users` (S-1-5-32-559) and it was still refused (measured "
                    "2026-10-21, while the same call in an elevated process returned S_OK and wrote "
                    "the file). Note `gradlew` reuses a daemon started without elevation, so "
                    "launching gradlew from an elevated terminal is not enough on its own — run "
                    "`gradlew --stop` first",
                    static_cast<unsigned long>(hr), pixHresultNote(hr),
                    pixToUtf8(gPixTimingFilePath).c_str());
        return static_cast<int>(hr);
    }
    gPixTimingCapturing = true;
    dbgLogInfo("PIX timing capture: started (HRESULT 0x%08lX), writing %s",
               static_cast<unsigned long>(hr), pixToUtf8(gPixTimingFilePath).c_str());
    return 1;
}

// 停止当前捕获，PIX 在后台线程里把文件写完。1=已请求 0=没有在录
// 负值=失败的 HRESULT（原因见日志）。
int stopPixTimingCapture() {
    if (!gPixTimingCapturing) {
        return 0;
    }
    gPixTimingCapturing = false;
    if (gPixTimingEnd == nullptr) {
        return 0;
    }
    HANDLE thread = CreateThread(nullptr, 0, &pixEndThread, nullptr, 0, nullptr);
    if (thread == nullptr) {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        dbgLogError("PIX timing capture: CreateThread failed (HRESULT 0x%08lX)",
                    static_cast<unsigned long>(hr));
        return static_cast<int>(hr);
    }
    // 不等：PIXEndCapture 会把整个捕获写盘（几百 MB 起），等它等于把这一帧卡死。
    CloseHandle(thread);
    dbgLogInfo("PIX timing capture: stopping, PIX is writing %s",
               pixToUtf8(gPixTimingFilePath).c_str());
    return 1;
}


}
