module;

#include "_d3d12_module_prelude.h"
#include <dbghelp.h>
#include <cwchar>
#pragma comment(lib, "dbghelp.lib")

module mcdr.d3d12.device;

namespace d3d12_mc {
static int gLogLevel = 0;

static bool gShutdownTraceAlways = true;

static std::mutex gMirrorMutex;
static std::string gConfiguredLogPath;
static FILE* gMirrorFile = nullptr;

static void mirrorLogToFile(const char* line) {
    const std::lock_guard<std::mutex> lock(gMirrorMutex);
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
    const std::lock_guard<std::mutex> lock(gMirrorMutex);
    gConfiguredLogPath = (utf8Path != nullptr) ? utf8Path : "";
    if (gMirrorFile != nullptr) {
        std::fclose(gMirrorFile);
        gMirrorFile = nullptr;
    }
}

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

static void emitLogLine(const char* level, const char* line) {
    char full[2304];
    const int n = formatLogPrefix(full, sizeof(full), level, "dxr_mc");
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(full)) return;
    std::snprintf(full + n, sizeof(full) - static_cast<size_t>(n), "%s", line);
    std::fprintf(stderr, "%s\n", full);
    fflush(stderr);
    mirrorLogToFile(full);
}

void dbgLog(const char* fmt, ...) {
    if (gLogLevel < 1) return;
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emitLogLine("WARN", line);
}
void dbgLogError(const char* fmt, ...) {
    if (gLogLevel < 0) return;
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emitLogLine("ERROR", line);
}
void dbgLogWarn(const char* fmt, ...) {
    if (gLogLevel < 1) return;
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emitLogLine("WARN", line);
}
void dbgLogInfo(const char* fmt, ...) {
    if (gLogLevel < 2) return;
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emitLogLine("INFO", line);
}
void dbgLogProgress(const char* fmt, ...) {
    if (!gShutdownTraceAlways && gLogLevel < 2) return;
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emitLogLine("INFO", line);
}
void dbgLogDebug(const char* fmt, ...) {
    if (gLogLevel < 3) return;
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emitLogLine("DEBUG", line);
}
void setLogLevel(int level) { gLogLevel = (level < 0) ? 0 : (level > 3) ? 3 : level; }
void setShutdownTraceAlways(bool always) { gShutdownTraceAlways = always; }

static DWORD gInfoQueueCookie = 0;
static void CALLBACK dx12DebugMessageCallback(D3D12_MESSAGE_CATEGORY,
                                              D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
                                              LPCSTR description, void*) {
    static const char* kSev[] = {"CORRUPTION", "ERROR", "WARNING", "INFO", "MESSAGE"};
    const int si = static_cast<int>(severity);
    const char* sevName = (si >= 0 && si <= 4) ? kSev[si] : "?";
    const char* text = description ? description : "(null)";
    if (si <= static_cast<int>(D3D12_MESSAGE_SEVERITY_ERROR)) {
        dbgLogError("D3D12 %s #%d: %s", sevName, static_cast<int>(id), text);
    } else if (si == static_cast<int>(D3D12_MESSAGE_SEVERITY_WARNING)) {
        dbgLogWarn("D3D12 %s #%d: %s", sevName, static_cast<int>(id), text);
    } else {
        dbgLogInfo("D3D12 %s #%d: %s", sevName, static_cast<int>(id), text);
    }
}

namespace {

bool gCrashSymbolsReady = false;
HMODULE gCrashOwnModule = nullptr;

void prepareCrashSymbols(HMODULE ownModule) {
    if (ownModule == nullptr) return;
    char imagePath[MAX_PATH] = {};
    if (::GetModuleFileNameA(ownModule, imagePath, MAX_PATH) == 0) return;
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
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256] = {};
    auto* info = reinterpret_cast<SYMBOL_INFO*>(storage);
    info->SizeOfStruct = sizeof(SYMBOL_INFO);
    info->MaxNameLen = 255;
    DWORD64 displacement = 0;
    gCrashSymbolsReady = ::SymFromAddr(process, reinterpret_cast<DWORD64>(&prepareCrashSymbols),
                                       &displacement, info) != FALSE;
}

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

}

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
