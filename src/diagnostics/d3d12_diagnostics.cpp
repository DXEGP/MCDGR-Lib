module;

#include "_d3d12_module_prelude.h"
#include <dbghelp.h>
#include <cwchar>
#pragma comment(lib, "dbghelp.lib")

module mcdr.d3d12.device;

namespace d3d12_mc {
static int gLogLevel = 0;

// P184 补记：关停/初始化这类"进度标记"是否**恒定输出**（true = 无视 gLogLevel；false = 当普通
// INFO，被 logLevel 过滤）。由配置项 `[logging] shutdownTrace` 经 `mcdrSetShutdownTraceAlways`
// 推进来。默认 true：destroyDevice/destroySurface 一旦卡住，这些行是唯一线索。
static bool gShutdownTraceAlways = true;

// P250：配置项 `[logging] nativeLogFile`（JVM 经 mcdrSetLogFile 推进来）。原生日志默认只走
// stderr，而 Prism 之类的启动器不把子进程 stdout/stderr 落盘 —— 于是"用户端出问题，却拿不到
// native 行"成了常态。这里提供一条文件镜像：设了路径就逐行追加（带 fflush，进程被强杀也不丢）。
static std::mutex gMirrorMutex;
static std::string gConfiguredLogPath;
static FILE* gMirrorFile = nullptr;

static void mirrorLogToFile(const char* line) {
    const std::lock_guard<std::mutex> lock(gMirrorMutex);
    // 配置项优先；旧环境变量 `DX12_NATIVE_LOG_FILE` 只作为**已弃用的逃生口**保留，且仅在配置为空
    // 时生效（AGENTS.md：环境变量不得成为功能的唯一入口）。两者都为空 ⇒ 不写文件。
    const char* path = !gConfiguredLogPath.empty() ? gConfiguredLogPath.c_str()
                                                  : std::getenv("DX12_NATIVE_LOG_FILE");
    if (path == nullptr || path[0] == '\0') return;
    if (gMirrorFile == nullptr) {
        gMirrorFile = std::fopen(path, "a");
        if (gMirrorFile == nullptr) return;
    }
    std::fprintf(gMirrorFile, "%s\n", line);
    std::fflush(gMirrorFile);
}

void setLogFile(const char* utf8Path) {
    // 空路径 = 关闭镜像并释放句柄；换路径同样先关旧句柄，下一次写日志时按新路径重开。
    const std::lock_guard<std::mutex> lock(gMirrorMutex);
    gConfiguredLogPath = (utf8Path != nullptr) ? utf8Path : "";
    if (gMirrorFile != nullptr) {
        std::fclose(gMirrorFile);
        gMirrorFile = nullptr;
    }
}

// JVM 侧是 log4j 的行格式；这里逐字复刻同样的形状（见 mcdr_device.cppm 的声明），
// 让 native 与 JVM 两路输出在同一个控制台里可以按时间戳直接对齐。旧前缀
// `[dx12][t=<QPC 毫秒>]` 做不到这一点：那个「进程运行到第几毫秒」在 JVM 行里不存在。
//
// 时间块逐字对齐 JVM 的实际输出（本机 log4j 的 pattern 是 `ddMM月yyyy HH:mm:ss.SSS`，
// 实测行首 `[1510月2026 15:29:44.942]`：日 15 + 月 10 + 「月」+ 年 2026），字段顺序
// 与分隔符都不能改，否则两路输出排在一起时时间戳对不上。
int formatLogPrefix(char* out, size_t outSize, const char* level, const char* logger) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return std::snprintf(out, outSize, "[%02u%02u月%04u %02u:%02u:%02u.%03u] [thread-%lu/%s] [%s/]: ",
                         static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wMonth),
                         static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wHour),
                         static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond),
                         static_cast<unsigned>(st.wMilliseconds),
                         static_cast<unsigned long>(GetCurrentThreadId()), level, logger);
}

// 单行出口：前缀 + 正文 + 换行，同时写 stderr 与（可选的）镜像文件。
static void emitLogLine(const char* level, const char* line) {
    char full[2304];
    const int n = formatLogPrefix(full, sizeof(full), level, "dxr_mc");
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(full)) return;  // 前缀都放不下：丢行，不写半行
    std::snprintf(full + n, sizeof(full) - static_cast<size_t>(n), "%s", line);
    std::fprintf(stderr, "%s\n", full);
    fflush(stderr);
    mirrorLogToFile(full);
}

void dbgLog(const char* fmt, ...) {
    if (gLogLevel < 1) return; // WARN=1
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emitLogLine("WARN", line);
}
void dbgLogError(const char* fmt, ...) {
    if (gLogLevel < 0) return; // ERR=0：始终输出（gLogLevel 被钳制在 0..3，故永不返回）
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emitLogLine("ERROR", line);
}
void dbgLogWarn(const char* fmt, ...) {
    if (gLogLevel < 1) return; // WARN=1，与 dbgLog 同门槛（仅用于区分语义）
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emitLogLine("WARN", line);
}
void dbgLogInfo(const char* fmt, ...) {
    if (gLogLevel < 2) return; // INFO=2
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emitLogLine("INFO", line);
}
// P184 补记：进度标记（关停/初始化这类）。按 **INFO** 标注 —— 以前借用 dbgLogError，于是每次
// 正常退出都会刷一串 `[ERROR]`，既误导用户，也让真正要找 ERROR 的人被噪音淹掉。
// 输出策略由配置 `[logging] shutdownTrace` 决定（`mcdrSetShutdownTraceAlways`）：
//   true （默认）：无视 gLogLevel 恒定输出 —— 退出路径卡死时这是唯一线索；
//   false        ：等同普通 INFO，被 logLevel 过滤掉。
void dbgLogProgress(const char* fmt, ...) {
    if (!gShutdownTraceAlways && gLogLevel < 2) return; // INFO=2
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emitLogLine("INFO", line);
}
void dbgLogDebug(const char* fmt, ...) {
    if (gLogLevel < 3) return; // DEBUG=3
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emitLogLine("DEBUG", line);
}
void setLogLevel(int level) { gLogLevel = (level < 0) ? 0 : (level > 3) ? 3 : level; }
// P184 补记：配置项 [logging] shutdownTrace → 进度标记是否恒定输出（见 dbgLogProgress）。
void setShutdownTraceAlways(bool always) { gShutdownTraceAlways = always; }

static DWORD gInfoQueueCookie = 0;
static void CALLBACK dx12DebugMessageCallback(D3D12_MESSAGE_CATEGORY /*category*/,
                                              D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
                                              LPCSTR description, void* /*context*/) {
    static const char* kSev[] = {"CORRUPTION", "ERROR", "WARNING", "INFO", "MESSAGE"};
    const int si = static_cast<int>(severity);
    const char* sevName = (si >= 0 && si <= 4) ? kSev[si] : "?";
    const char* text = description ? description : "(null)";
    // 分级按 severity：0=CORRUPTION / 1=ERROR / 2=WARNING 默认可见；
    // 3=INFO / 4=MESSAGE 只在 INFO 级别以上输出（原先这两级被无条件丢弃）。
    if (si <= static_cast<int>(D3D12_MESSAGE_SEVERITY_ERROR)) {
        dbgLogError("D3D12 %s #%d: %s", sevName, static_cast<int>(id), text);
    } else if (si == static_cast<int>(D3D12_MESSAGE_SEVERITY_WARNING)) {
        dbgLogWarn("D3D12 %s #%d: %s", sevName, static_cast<int>(id), text);
    } else {
        dbgLogInfo("D3D12 %s #%d: %s", sevName, static_cast<int>(id), text);
    }
}

namespace {

// P306c：崩溃栈符号化（只解析本模块）。
//
// Release 构建会在 MCDR.dll 旁边生成 MCDR.pdb，于是崩溃现场可以把
// "MCDR.dll+0x5dc6f" 还原成 "d3d12_fsffg.cpp:271 FsrFgFrameGenProvider::beforePresent+0x15"。
// 关键约束：符号加载要读文件、拿加载器锁，**不能**放在异常处理路径上 —— 崩溃线程很可能
// 正握着那把锁，一做 I/O 就死等。所以 PDB 在安装处理器时预热进内存，过滤器里只查表。
bool gCrashSymbolsReady = false;
HMODULE gCrashOwnModule = nullptr;

void prepareCrashSymbols(HMODULE ownModule) {
    if (ownModule == nullptr) return;
    char imagePath[MAX_PATH] = {};
    if (::GetModuleFileNameA(ownModule, imagePath, MAX_PATH) == 0) return;
    // 搜索路径固定成 DLL 自己所在的目录：只认同目录的 PDB，绝不因为机器上设了
    // _NT_SYMBOL_PATH 就去连符号服务器（那会让首次建建设备时卡在网络上）。
    std::string searchPath(imagePath);
    const size_t slash = searchPath.find_last_of("\\/");
    if (slash == std::string::npos) return;
    searchPath.resize(slash);
    ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    HANDLE process = ::GetCurrentProcess();
    if (!::SymInitialize(process, searchPath.c_str(), FALSE)) return;
    if (::SymLoadModuleEx(process, nullptr, imagePath, nullptr,
                          reinterpret_cast<DWORD64>(ownModule), 0, nullptr, 0) == 0) {
        return;
    }
    // 一次成功解析足以把整份 PDB 读进内存；没有 PDB 时这里失败，回退成 模块+偏移。
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256] = {};
    auto* info = reinterpret_cast<SYMBOL_INFO*>(storage);
    info->SizeOfStruct = sizeof(SYMBOL_INFO);
    info->MaxNameLen = 255;
    DWORD64 displacement = 0;
    gCrashSymbolsReady = ::SymFromAddr(process, reinterpret_cast<DWORD64>(&prepareCrashSymbols),
                                       &displacement, info) != FALSE;
}

// 只查表；查不到就返回 false，调用方退回 “模块+偏移”。
bool describeOwnFrame(void* address, char* text, size_t cap) {
    if (!gCrashSymbolsReady) return false;
    HANDLE process = ::GetCurrentProcess();
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256] = {};
    auto* info = reinterpret_cast<SYMBOL_INFO*>(storage);
    info->SizeOfStruct = sizeof(SYMBOL_INFO);
    info->MaxNameLen = 255;
    DWORD64 displacement = 0;
    const DWORD64 addr = reinterpret_cast<DWORD64>(address);
    if (!::SymFromAddr(process, addr, &displacement, info)) return false;
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    if (::SymGetLineFromAddr64(process, addr, &lineDisplacement, &line) && line.FileName != nullptr) {
        // PDB 里是构建机上的绝对路径，只留文件名。
        const char* file = line.FileName;
        for (const char* p = line.FileName; *p; ++p) {
            if (*p == '\\' || *p == '/') file = p + 1;
        }
        std::snprintf(text, cap, "%s:%lu %s+0x%llx", file,
                      static_cast<unsigned long>(line.LineNumber), info->Name,
                      static_cast<unsigned long long>(displacement));
        return true;
    }
    std::snprintf(text, cap, "%s+0x%llx", info->Name, static_cast<unsigned long long>(displacement));
    return true;
}

}  // namespace

static LONG WINAPI dx12CrashFilter(EXCEPTION_POINTERS* ep) {
    if (ep != nullptr && ep->ExceptionRecord != nullptr) {
        EXCEPTION_RECORD* rec = ep->ExceptionRecord;
        char modPath[MAX_PATH] = "<unknown>";
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                   | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(rec->ExceptionAddress), &mod)) {
            GetModuleFileNameA(mod, modPath, MAX_PATH);
        }
        std::fprintf(stderr, "FATAL native exception code=0x%08X addr=%p tid=%lu module=%s\n",
                     static_cast<unsigned>(rec->ExceptionCode), rec->ExceptionAddress,
                     static_cast<unsigned long>(GetCurrentThreadId()), modPath);
        dumpPendingDeleteCrashContext();
        if (deviceContext().device) {
            std::fprintf(stderr, "FATAL deviceRemovedReason=0x%08X\n",
                         static_cast<unsigned>(deviceContext().device->GetDeviceRemovedReason()));
        }
        void* frames[48];
        USHORT nframes = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        std::fprintf(stderr, "FATAL backtrace: %u frames\n", static_cast<unsigned>(nframes));
        for (USHORT i = 0; i < nframes; ++i) {
            HMODULE fm = nullptr;
            char fpath[MAX_PATH] = "<unknown>";
            unsigned long long rva = 0;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(frames[i]), &fm)) {
                GetModuleFileNameA(fm, fpath, MAX_PATH);
                rva = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i])
                                                      - reinterpret_cast<uintptr_t>(fm));
            }
            const char* fb = fpath;
            for (const char* p = fpath; *p; ++p) {
                if (*p == '\\' || *p == '/') fb = p + 1;
            }
            // P306c：本模块的帧连带 文件:行 一起打；别的模块维持 模块+偏移。
            char sym[512] = {};
            if (fm != nullptr && fm == gCrashOwnModule
                && describeOwnFrame(frames[i], sym, sizeof(sym))) {
                std::fprintf(stderr, "  [%02u] %s\n", static_cast<unsigned>(i), sym);
            } else {
                std::fprintf(stderr, "  [%02u] %s+0x%llx\n", static_cast<unsigned>(i), fb, rva);
            }
        }
        std::fflush(stderr);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void installCrashHandlerOnce() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    // P306c：先把本模块的 PDB 预热进内存，过滤器里才能只查表不碰文件 I/O。
    if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                 | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(&dx12CrashFilter), &gCrashOwnModule)) {
        prepareCrashSymbols(gCrashOwnModule);
    }
    SetUnhandledExceptionFilter(&dx12CrashFilter);
}


int nativeLogLevel() { return gLogLevel; }

void registerDebugMessageCallback(ID3D12InfoQueue1* queue) {
    if (FAILED(queue->RegisterMessageCallback(&dx12DebugMessageCallback,
                                             D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr,
                                             &gInfoQueueCookie))) {
        dbgLogError("infoQueue: RegisterMessageCallback failed");
    } else {
        dbgLogInfo("infoQueue: message callback registered cookie=%lu",
                   static_cast<unsigned long>(gInfoQueueCookie));
    }
}

}
