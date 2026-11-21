module;

#include <initguid.h>
#include "_d3d12_module_prelude.h"
#include "mcdr_graphics_capture.h"
#include "mcdr_graphics_recording.h"
#include "mcdr_oit_budget.h"
#include <span>
#include "mcdr_rt_state.h"

#include "mcdr_rtx_support.h"

#include "mcdr_dlss_impl.h"
#include <cstdio>
#include <cwchar>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#if __has_include(<sl.h>)
#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#define MCDR_HAVE_STREAMLINE 1
#else
#define MCDR_HAVE_STREAMLINE 0
#endif

#include <nlohmann/json.hpp>

module mcdr.d3d12.device;

import mcdr.error;
import mcdr.d3d12.async_executor;
import mcdr.d3d12.async_bundle;
import mcdr.d3d12.async_descriptor;
import mcdr.d3d12.async_fence;

namespace d3d12_mc {

Dx12Surface* gActiveSurface = nullptr;

namespace {
std::atomic<uint64_t> nextSampledTextureOwnerGeneration{1};

void invalidateGraphicsBindingReadCache(CommandContext* context) {
    if (!context) return;
    context->graphicsBindingReadCache.clear();
    context->graphicsBindingReadCacheValid = false;
}

DeviceContext* makeDeviceContext() {
    return new DeviceContext();
}
DeviceContext& gCtx = *makeDeviceContext();
std::mutex gQueueSubmissionMutex;
std::mutex gAsyncSubmissionMutex;
std::mutex gRecordingContextsMutex;
std::unordered_map<void*, CommandContext*> gRecordingContexts;

struct PendingDelete {
    Dx12Object* obj;
    UINT64 requiredFence;
};
std::vector<PendingDelete> gPendingDeletes;
int gOpenListCount = 0;
constexpr UINT64 kFenceBlockedByOpenList = UINT64_MAX;

Dx12Object* volatile gPdDeletingObj = nullptr;
volatile long gPdDeletingIndex = -1;
volatile long gPdDeletingTotal = 0;
volatile long gPdInProgress = 0;

std::vector<UINT> gFreeSrvSlots;
std::vector<UINT> gFreeSamplerSlots;

std::mutex gPendingMutex;
std::mutex gFreeSlotMutex;
std::mutex gDeviceInitMutex;

struct PooledBuffer {
    Dx12Object* obj;
};
std::vector<PooledBuffer> gFreeBuffers;
size_t gFreeBufferBytes = 0;
std::mutex gFreeBufferMutex;
constexpr size_t kMaxFreeBufferBytes = 96ull * 1024 * 1024;

UINT64 bufferCapacity(const Dx12Object* o) {
    if (o->resource) {
        return o->resource->GetDesc().Width;
    }
    return (static_cast<UINT64>(o->size) + 255ULL) & ~255ULL;
}

Dx12Object* takePooledBuffer(D3D12_HEAP_TYPE heapType, UINT64 capacity) {
    std::lock_guard<std::mutex> lk(gFreeBufferMutex);
    size_t best = SIZE_MAX;
    UINT64 bestCap = 0;
    for (size_t i = 0; i < gFreeBuffers.size(); ++i) {
        PooledBuffer& pb = gFreeBuffers[i];
        if (pb.obj->heapType != heapType) continue;
        UINT64 cap = bufferCapacity(pb.obj);
        if (cap < capacity) continue;
        if (best == SIZE_MAX || cap < bestCap) {
            best = i;
            bestCap = cap;
        }
    }
    if (best == SIZE_MAX) return nullptr;
    Dx12Object* obj = gFreeBuffers[best].obj;
    gFreeBufferBytes -= static_cast<size_t>(bestCap);
    gFreeBuffers[best] = gFreeBuffers.back();
    gFreeBuffers.pop_back();
    return obj;
}

extern std::atomic<long long> gLiveBuf;
void accountLiveBuffer(D3D12_HEAP_TYPE heapType, long long bytes);

void recyclePooledBuffer(Dx12Object* obj) {
    UINT64 capacity = bufferCapacity(obj);
    const bool poolable = (obj->heapType != D3D12_HEAP_TYPE_READBACK);
    if (poolable) {
        std::lock_guard<std::mutex> lk(gFreeBufferMutex);
        if (gFreeBufferBytes + static_cast<size_t>(capacity) <= kMaxFreeBufferBytes) {
            gFreeBuffers.push_back(PooledBuffer{obj});
            gFreeBufferBytes += static_cast<size_t>(capacity);
            return;
        }
    }
    --gLiveBuf;
    accountLiveBuffer(obj->heapType, -static_cast<long long>(capacity));
    delete obj;
}

UINT gNextSrv = 0;
UINT gNextRtv = 0;
UINT gNextFrameRtv = 0;
UINT gNextDsv = 0;
UINT gFrameViewGeneration = 1;
UINT gNextSampler = 0;

constexpr UINT kSrvHeapSize = 262144;
constexpr UINT kRtvHeapSize = 2048;
constexpr UINT kFrameRtvHeapSize = 256;
constexpr UINT kDsvHeapSize = 256;
constexpr UINT kSamplerHeapSize = 2048;
constexpr UINT kDrawHeapPerFrame = kAsyncSyncRingReserve + kAsyncWorkerSlotsPerSection;
constexpr UINT kDrawHeapSections = 4;
constexpr UINT kDrawHeapSize = kDrawHeapPerFrame * kDrawHeapSections;

inline UINT syncRingCapacity() { return gCtx.syncRingReserve > 0 ? gCtx.syncRingReserve : kDrawHeapPerFrame; }

std::atomic<uint64_t> gStatTexCreate{0};
std::atomic<uint64_t> gStatBufCreate{0};
std::atomic<uint64_t> gStatPsoCreate{0};
std::atomic<long long> gLiveTex{0};
std::atomic<long long> gLiveBuf{0};
std::atomic<long long> gLiveUploadBytes{0};
std::atomic<long long> gLiveDefaultBytes{0};
std::atomic<long long> gLiveReadbackBytes{0};
static void accountLiveBuffer(D3D12_HEAP_TYPE heapType, long long bytes) {
    if (heapType == D3D12_HEAP_TYPE_UPLOAD) gLiveUploadBytes += bytes;
    else if (heapType == D3D12_HEAP_TYPE_READBACK) gLiveReadbackBytes += bytes;
    else gLiveDefaultBytes += bytes;
}
std::atomic<uint64_t> gStatEncoderCreate{0};

size_t pendingDeletesCount() {
    std::lock_guard<std::mutex> lk(gPendingMutex);
    return gPendingDeletes.size();
}
int popFreeSrvSlot() {
    std::lock_guard<std::mutex> lk(gFreeSlotMutex);
    if (gFreeSrvSlots.empty()) return -1;
    int slot = static_cast<int>(gFreeSrvSlots.back());
    gFreeSrvSlots.pop_back();
    return slot;
}
int popFreeSamplerSlot() {
    std::lock_guard<std::mutex> lk(gFreeSlotMutex);
    if (gFreeSamplerSlots.empty()) return -1;
    int slot = static_cast<int>(gFreeSamplerSlots.back());
    gFreeSamplerSlots.pop_back();
    return slot;
}
size_t freeSrvSlotCount() {
    std::lock_guard<std::mutex> lk(gFreeSlotMutex);
    return gFreeSrvSlots.size();
}
size_t freeSamplerSlotCount() {
    std::lock_guard<std::mutex> lk(gFreeSlotMutex);
    return gFreeSamplerSlots.size();
}

mcdr::Result<int> allocSrvSlot() {
    int slot = popFreeSrvSlot();
    if (slot >= 0) return slot;
    if (gNextSrv >= kSrvHeapSize && gOpenListCount == 0 && pendingDeletesCount() > 0) {
        flushPendingDeletes();
        slot = popFreeSrvSlot();
        if (slot >= 0) return slot;
    }
    if (gNextSrv >= kSrvHeapSize) {
        return mcdr::err("allocSrvSlot",
                         "srv heap exhausted (next=" + std::to_string(static_cast<long long>(gNextSrv)) +
                             " free=" + std::to_string(static_cast<long long>(freeSrvSlotCount())) +
                             " pending=" + std::to_string(static_cast<long long>(pendingDeletesCount())) +
                             " openLists=" + std::to_string(gOpenListCount) + ")");
    }
    return static_cast<int>(gNextSrv++);
}

mcdr::Result<int> allocSamplerSlot() {
    int slot = popFreeSamplerSlot();
    if (slot >= 0) return slot;
    if (gNextSampler >= kSamplerHeapSize && gOpenListCount == 0 && pendingDeletesCount() > 0) {
        flushPendingDeletes();
        slot = popFreeSamplerSlot();
        if (slot >= 0) return slot;
    }
    if (gNextSampler >= kSamplerHeapSize) {
        return mcdr::err("allocSamplerSlot",
                         "sampler heap exhausted (next=" + std::to_string(static_cast<long long>(gNextSampler)) +
                             " free=" + std::to_string(static_cast<long long>(freeSamplerSlotCount())) +
                             " pending=" + std::to_string(static_cast<long long>(pendingDeletesCount())) +
                             " openLists=" + std::to_string(gOpenListCount) + ")");
    }
    return static_cast<int>(gNextSampler++);
}

bool createHeap(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count,
                D3D12_DESCRIPTOR_HEAP_FLAGS flags, ComPtr<ID3D12DescriptorHeap>& out) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = count;
    desc.Flags = flags;
    return SUCCEEDED(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&out)));
}

std::string queryAdapterName(ID3D12Device* dev) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return "<factory failed>";
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(factory->EnumAdapterByLuid(dev->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
        return "<enum adapter failed>";
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) return "<get desc failed>";
    char name[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
    return name;
}

std::string deviceStatusText() {
    std::string s;
    if (!gCtx.device) return s;
    HRESULT reason = gCtx.device->GetDeviceRemovedReason();
    s += "GetDeviceRemovedReason=" + formatBackendResult(reason);
    if (gCtx.infoQueue) {
        UINT64 count = gCtx.infoQueue->GetNumStoredMessages();
        UINT64 start = count > 24 ? count - 24 : 0;
        for (UINT64 i = start; i < count; ++i) {
            SIZE_T len = 0;
            if (FAILED(gCtx.infoQueue->GetMessage(static_cast<UINT>(i), nullptr, &len))) continue;
            std::vector<char> buf(len > 0 ? len : 1);
            D3D12_MESSAGE* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(gCtx.infoQueue->GetMessage(static_cast<UINT>(i), msg, &len))) {
                s += " | msg[" + std::to_string(static_cast<long long>(i)) + "](";
                s += msg->pDescription ? msg->pDescription : "";
                s += ")";
            }
        }
    }
    return s;
}

void dumpInfoQueueMessages() {
    if (!gCtx.infoQueue) return;
    UINT64 count = gCtx.infoQueue->GetNumStoredMessages();
    if (count == 0) return;
    UINT64 start = count > 16 ? count - 16 : 0;
    for (UINT64 i = start; i < count; ++i) {
        SIZE_T len = 0;
        if (FAILED(gCtx.infoQueue->GetMessage(static_cast<UINT>(i), nullptr, &len))) continue;
        std::vector<char> buf(len > 0 ? len : 1);
        D3D12_MESSAGE* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (SUCCEEDED(gCtx.infoQueue->GetMessage(static_cast<UINT>(i), msg, &len))) {
            const char* sev = "?";
            switch (msg->Severity) {
                case D3D12_MESSAGE_SEVERITY_CORRUPTION: sev = "CORRUPTION"; break;
                case D3D12_MESSAGE_SEVERITY_ERROR: sev = "ERROR"; break;
                case D3D12_MESSAGE_SEVERITY_WARNING: sev = "WARNING"; break;
                case D3D12_MESSAGE_SEVERITY_INFO: sev = "INFO"; break;
                default: break;
            }
            const int sevInt = static_cast<int>(msg->Severity);
            if (sevInt <= static_cast<int>(D3D12_MESSAGE_SEVERITY_ERROR)) {
                dbgLogError("InfoQueue[%s] %s", sev, msg->pDescription ? msg->pDescription : "");
            } else if (sevInt == static_cast<int>(D3D12_MESSAGE_SEVERITY_WARNING)) {
                dbgLogWarn("InfoQueue[%s] %s", sev, msg->pDescription ? msg->pDescription : "");
            } else {
                dbgLogInfo("InfoQueue[%s] %s", sev, msg->pDescription ? msg->pDescription : "");
            }
        }
    }
    gCtx.infoQueue->ClearStoredMessages();
}

}

void logDeviceStatus(const char* tag) {
    const std::string s = deviceStatusText();
    if (s.empty()) return;
    dbgLogError("%s: device status: %s", tag, s.c_str());
}

bool deviceRemoved() {
    if (!gCtx.device) return true;
    return gCtx.device->GetDeviceRemovedReason() != S_OK;
}

void flushPendingDeletes(bool force) {
    if (InterlockedExchange(&gPdInProgress, 1) != 0) {
        dbgLog("flushPendingDeletes: concurrent/re-entrant call skipped (pending=%lld)",
               static_cast<long long>(pendingDeletesCount()));
        return;
    }
    UINT64 gpuDone = UINT64_MAX;
    if (!force && gCtx.queueFence) {
        gpuDone = gCtx.queueFence->GetCompletedValue();
    }
    const bool canResolveBlocked = (gOpenListCount == 0);
    const UINT64 resolveFence = gCtx.queueFenceValue;
    std::vector<PendingDelete> local;
    {
        std::lock_guard<std::mutex> lk(gPendingMutex);
        std::vector<PendingDelete> keep;
        keep.reserve(gPendingDeletes.size());
        for (PendingDelete& pd : gPendingDeletes) {
            if (pd.obj->recordingReferences.load(std::memory_order_acquire) != 0) {
                keep.push_back(pd);
                continue;
            }
            if (force) {
                local.push_back(pd);
                continue;
            }
            if (pd.requiredFence == kFenceBlockedByOpenList) {
                if (!canResolveBlocked) {
                    keep.push_back(pd);
                    continue;
                }
                pd.requiredFence = resolveFence;
            }
            if (pd.requiredFence <= gpuDone) {
                local.push_back(pd);
            } else {
                keep.push_back(pd);
            }
        }
        gPendingDeletes.swap(keep);
    }
    gPdDeletingTotal = static_cast<long>(local.size());
    if (gPdDeletingTotal > 0) {
        dbgLogDebug("flushPendingDeletes: begin count=%ld deferred=%lld gpuDone=%llu", gPdDeletingTotal,
                    static_cast<long long>(pendingDeletesCount()), static_cast<unsigned long long>(gpuDone));
    }
    long idx = 0;
    for (const PendingDelete& pd : local) {
        Dx12Object* o = pd.obj;
        gPdDeletingObj = o;
        gPdDeletingIndex = idx++;
        if (o->descSlot >= 0) {
            std::lock_guard<std::mutex> lk(gFreeSlotMutex);
            if (o->kind == Dx12Object::Kind::TextureView) {
                gFreeSrvSlots.push_back(static_cast<UINT>(o->descSlot));
            } else if (o->kind == Dx12Object::Kind::Sampler) {
                gFreeSamplerSlots.push_back(static_cast<UINT>(o->descSlot));
            }
        }
        if (!force && o->kind == Dx12Object::Kind::Buffer) {
            recyclePooledBuffer(o);
            continue;
        }
        if (o->kind == Dx12Object::Kind::Texture) --gLiveTex;
        else if (o->kind == Dx12Object::Kind::Buffer) {
            --gLiveBuf;
            if (o->resource) accountLiveBuffer(o->heapType, -static_cast<long long>(o->resource->GetDesc().Width));
        }
        delete o;
    }
    gPdDeletingObj = nullptr;
    gPdDeletingIndex = -1;
    gPdDeletingTotal = 0;
    InterlockedExchange(&gPdInProgress, 0);
    dbgLogDebug("flushPendingDeletes: done");
}

void dumpPendingDeleteCrashContext() {
        std::fprintf(stderr,
                     "FATAL context: pendingDeletes=%lld openList=%d flushInProgress=%ld "
                     "deletingObj=%p deletingIndex=%ld deletingTotal=%ld\n",
                     static_cast<long long>(gPendingDeletes.size()), gOpenListCount, gPdInProgress,
                     reinterpret_cast<void*>(gPdDeletingObj), gPdDeletingIndex, gPdDeletingTotal);
        Dx12Object* dObj = gPdDeletingObj;
        if (dObj) {
            std::fprintf(stderr,
                         "FATAL deletingObj: kind=%d descSlot=%d resource=%p "
                         "sourceTexture=%p mappedPtr=%p size=%lld\n",
                         static_cast<int>(dObj->kind), dObj->descSlot,
                         reinterpret_cast<void*>(dObj->resource.Get()),
                         reinterpret_cast<void*>(dObj->sourceTexture), dObj->mappedPtr, dObj->size);
        }
}

void recordPipelineCreation() { ++gStatPsoCreate; }

void dumpDeviceMessages() { dumpInfoQueueMessages(); }

UINT syncDrawDescriptorCapacity() { return syncRingCapacity(); }

double nowMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

namespace {

constexpr D3D_FEATURE_LEVEL kRequiredFeatureLevel = D3D_FEATURE_LEVEL_12_2;

constexpr D3D_FEATURE_LEVEL kProbeLevels[] = {
    D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
    D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
};

typedef HRESULT(WINAPI* PFN_SL_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

PFN_SL_D3D12CreateDevice interposerCreateDeviceFn() {
    static const PFN_SL_D3D12CreateDevice fn = []() -> PFN_SL_D3D12CreateDevice {
        const bool want = mcdr_dlss_streamline_requested() != 0;
        if (!want) return nullptr;
        HMODULE self = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(&interposerCreateDeviceFn), &self)) {
            return nullptr;
        }
        wchar_t path[MAX_PATH] = {};
        if (!::GetModuleFileNameW(self, path, MAX_PATH)) return nullptr;
        std::wstring modulePath(path);
        const auto slash = modulePath.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return nullptr;
        const std::wstring dir = modulePath.substr(0, slash + 1);
        HMODULE mod = ::LoadLibraryW((dir + L"sl.interposer.dll").c_str());
        if (mod == nullptr) return nullptr;
        return reinterpret_cast<PFN_SL_D3D12CreateDevice>(::GetProcAddress(mod, "D3D12CreateDevice"));
    }();
    return fn;
}

HRESULT createD3D12Device(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid, void** out) {
    const PFN_SL_D3D12CreateDevice proxy = interposerCreateDeviceFn();
    if (proxy != nullptr) {
        return proxy(adapter, level, riid, out);
    }
    return ::D3D12CreateDevice(adapter, level, riid, out);
}

D3D_FEATURE_LEVEL probeHighestFeatureLevel(IDXGIAdapter* adapter) {
    for (auto level : kProbeLevels) {
        ComPtr<ID3D12Device> probe;
        if (SUCCEEDED(D3D12CreateDevice(adapter, level, IID_PPV_ARGS(&probe)))) {
            return level;
        }
    }
    return static_cast<D3D_FEATURE_LEVEL>(0);
}

std::string adapterDescName(const DXGI_ADAPTER_DESC1& desc) {
    char name[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
    return name;
}

mcdr::Result<ComPtr<IDXGIAdapter1>> selectDeviceAdapter() {
    constexpr UINT kMaxAdapters = 32;
    std::string report;

    ComPtr<IDXGIFactory6> factory6;
    ComPtr<IDXGIFactory4> factory4;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory6)))) {
        factory6 = nullptr;
    }
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4)))) {
        return mcdr::err("selectDeviceAdapter", "CreateDXGIFactory failed");
    }

    for (UINT i = 0; i < kMaxAdapters; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory6) {
            if (FAILED(factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                            IID_PPV_ARGS(&adapter)))) {
                break;
            }
        } else if (FAILED(factory4->EnumAdapters1(i, &adapter))) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }
        const std::string name = adapterDescName(desc);
        if ((desc.Flags & static_cast<UINT>(DXGI_ADAPTER_FLAG_SOFTWARE)) != 0u) {
            report += "  [software] " + name + "\n";
            continue;
        }
        const D3D_FEATURE_LEVEL best = probeHighestFeatureLevel(adapter.Get());
        report += "  " + name + " -> " + featureLevelName(best) + "\n";
        if (best >= kRequiredFeatureLevel) {
            dbgLogInfo("adapter select: %s (highest FL=%s)", name.c_str(), featureLevelName(best));
            return adapter;
        }
    }

    const std::string noAdapter = "no adapter supports D3D_FEATURE_LEVEL_12_2; candidates:\n" + report;
    dbgLogError("%s", noAdapter.c_str());
    return mcdr::err("selectDeviceAdapter", noAdapter);
}

}

const char* featureLevelName(D3D_FEATURE_LEVEL level) {
    switch (level) {
        case D3D_FEATURE_LEVEL_12_2: return "12_2";
        case D3D_FEATURE_LEVEL_12_1: return "12_1";
        case D3D_FEATURE_LEVEL_12_0: return "12_0";
        case D3D_FEATURE_LEVEL_11_1: return "11_1";
        case D3D_FEATURE_LEVEL_11_0: return "11_0";
        case D3D_FEATURE_LEVEL_10_1: return "10_1";
        case D3D_FEATURE_LEVEL_10_0: return "10_0";
        case D3D_FEATURE_LEVEL_9_3: return "9_3";
        case D3D_FEATURE_LEVEL_9_2: return "9_2";
        case D3D_FEATURE_LEVEL_9_1: return "9_1";
        default: return "unknown";
    }
}

const char* shaderModelName(D3D_SHADER_MODEL model) {
    switch (model) {
        case D3D_SHADER_MODEL_5_1: return "5.1";
        case D3D_SHADER_MODEL_6_0: return "6.0";
        case D3D_SHADER_MODEL_6_1: return "6.1";
        case D3D_SHADER_MODEL_6_2: return "6.2";
        case D3D_SHADER_MODEL_6_3: return "6.3";
        case D3D_SHADER_MODEL_6_4: return "6.4";
        case D3D_SHADER_MODEL_6_5: return "6.5";
        case D3D_SHADER_MODEL_6_6: return "6.6";
        case D3D_SHADER_MODEL_6_7: return "6.7";
        case D3D_SHADER_MODEL_6_8: return "6.8";
        case D3D_SHADER_MODEL_6_9: return "6.9";
        default: break;
    }
    if (model == kShaderModelCeiling) return "6.10";
    return "unknown";
}

const char* shaderModelProfileSuffix(D3D_SHADER_MODEL model) {
    switch (model) {
        case D3D_SHADER_MODEL_6_0: return "6_0";
        case D3D_SHADER_MODEL_6_1: return "6_1";
        case D3D_SHADER_MODEL_6_2: return "6_2";
        case D3D_SHADER_MODEL_6_3: return "6_3";
        case D3D_SHADER_MODEL_6_4: return "6_4";
        case D3D_SHADER_MODEL_6_5: return "6_5";
        case D3D_SHADER_MODEL_6_6: return "6_6";
        case D3D_SHADER_MODEL_6_7: return "6_7";
        case D3D_SHADER_MODEL_6_8: return "6_8";
        case D3D_SHADER_MODEL_6_9: return "6_9";
        default: break;
    }
    if (model == kShaderModelCeiling) return "6_10";
    return "";
}

D3D_SHADER_MODEL effectiveShaderModel(const DeviceContext::Capabilities& caps) {
    return caps.shaderModel < kShaderModelCeiling ? caps.shaderModel : kShaderModelCeiling;
}

namespace {

bool gDlssAvailabilityLogged = false;
bool gDlssWired = false;

int32_t createDlssTexture(CommandContext* ctx, const DlssTextureDesc* desc, void** outObject,
                          void** outResource);
void destroyDlssTexture(CommandContext* ctx, void* object);
int32_t shutdownStreamline();

void wireDlssImpl() {
    if (gDlssWired) {
        return;
    }
    gDlssWired = true;
    mcdr_dlss_install_log(reinterpret_cast<void*>(&d3d12_mc::dbgLogInfo),
                          reinterpret_cast<void*>(&d3d12_mc::dbgLogWarn),
                          reinterpret_cast<void*>(&d3d12_mc::dbgLogError));
    mcdr_dlss_install_texture_ops(reinterpret_cast<void*>(&d3d12_mc::createDlssTexture),
                                  reinterpret_cast<void*>(&d3d12_mc::destroyDlssTexture));
    const auto pinTexture = +[](void* list, void* object) -> int32_t {
        auto* context = static_cast<CommandContext*>(recordingContextForCommandList(list));
        if (!context || !object) return 0;
        retainObjectForRecording(context, object);
        return 1;
    };
    mcdr_dlss_install_recording_pin(reinterpret_cast<void*>(pinTexture));
    mcdr_dlss_install_streamline_shutdown(reinterpret_cast<void*>(&shutdownStreamline));
}

void notifyDlssDeviceReady() {
    wireDlssImpl();
    {
        HMODULE self = nullptr;
        wchar_t ownPath[MAX_PATH] = {};
        if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(&notifyDlssDeviceReady), &self) != FALSE &&
            self != nullptr &&
            ::GetModuleFileNameW(self, ownPath, static_cast<DWORD>(std::size(ownPath))) > 0) {
            std::wstring dir(ownPath);
            const std::wstring::size_type slash = dir.find_last_of(L"\\/");
            dir.resize(slash == std::wstring::npos ? 0 : slash);
            if (!dir.empty()) {
                wchar_t probe[MAX_PATH] = {};
                std::swprintf(probe, std::size(probe), L"%s\\nvngx_dlss.dll", dir.c_str());
                if (::GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES) {
                    char utf8[MAX_PATH * 2] = {};
                    const int need = ::WideCharToMultiByte(CP_UTF8, 0, dir.c_str(),
                                                          static_cast<int>(dir.size()), utf8,
                                                          static_cast<int>(sizeof(utf8)) - 1, nullptr,
                                                          nullptr);
                    if (need > 0) {
                        utf8[need] = '\0';
                        mcdr_dlss_register_vendor_dir(utf8, need + 1);
                    }
                } else {
                    dbgLogInfo("this module's directory has no nvngx_dlss.dll; not registering it "
                               "as the DLSS vendor directory (the JVM can pass the real one via "
                               "mcdr_dx12SetDlssVendorDir)");
                }
            }
        }
    }
    if (char note[512] = {}; true) {
        const int32_t canRun = mcdr_dlss_can_run(reinterpret_cast<void*>(gCtx.device.Get()));
        char runtimePath[512] = {};
        dbgLogInfo("DLSS NGX init on device-ready: canRun=%d runtime=\"%s\"", canRun,
                   mcdr_dlss_runtime_path());
        mcdr_dlss_query(note, sizeof(note), nullptr, 0, runtimePath, sizeof(runtimePath), nullptr);
        dbgLogInfo("DLSS status after init: %s", note);
    }
    if (gCtx.adapter && gCtx.rtx.ngxRuntime && !gCtx.rtx.ngxRuntimePath.empty()) {
        mcdr_dlss_register_runtime_path(gCtx.rtx.ngxRuntimePath.c_str(),
                                        static_cast<int32_t>(gCtx.rtx.ngxRuntimePath.size() + 1));
    }
    if (gDlssAvailabilityLogged) {
        return;
    }
    gDlssAvailabilityLogged = true;
    const bool adapterOk = gCtx.rtx.dlssSuperResolution;
    dbgLogInfo("DLSS availability: ngxRuntime=%s%s%s%s dlssSupported=%s (why: %s) runtime=%s",
               gCtx.rtx.ngxRuntime ? "yes" : "no",
               (gCtx.rtx.ngxRuntime && !gCtx.rtx.ngxRuntimePath.empty()) ? " \"" : "",
               (gCtx.rtx.ngxRuntime && !gCtx.rtx.ngxRuntimePath.empty()) ? gCtx.rtx.ngxRuntimePath.c_str()
                                                                         : "",
               (gCtx.rtx.ngxRuntime && !gCtx.rtx.ngxRuntimePath.empty()) ? "\"" : "",
               adapterOk ? "yes" : "no",
               gCtx.rtx.dlssQueried ? (adapterOk ? "-" : gCtx.rtx.dlssSuperResolutionWhy.c_str())
                                    : gCtx.rtx.dlssQueryNote.c_str(),
               gCtx.rtx.ngxNote.empty() ? "-" : gCtx.rtx.ngxNote.c_str());
}

}

void queryDeviceCapabilities(ID3D12Device* device, DeviceContext::Capabilities& caps) {
    caps = DeviceContext::Capabilities{};
    if (!device) {
        return;
    }
    caps.queried = true;
    caps.featureLevel = gCtx.featureLevel;

    static constexpr D3D_SHADER_MODEL kShaderModelProbes[] = {
        static_cast<D3D_SHADER_MODEL>(0x6a),
        D3D_SHADER_MODEL_6_9, D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6,
        D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2,
        D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0, D3D_SHADER_MODEL_5_1,
    };
    for (D3D_SHADER_MODEL probe : kShaderModelProbes) {
        D3D12_FEATURE_DATA_SHADER_MODEL query{};
        query.HighestShaderModel = probe;
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &query, sizeof(query)))) {
            caps.shaderModel = query.HighestShaderModel;
            break;
        }
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3)))) {
        caps.barycentricsSupported = options3.BarycentricsSupported != FALSE;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7,
                                              sizeof(options7)))) {
        caps.meshShaderTier = options7.MeshShaderTier;
        caps.samplerFeedbackTier = options7.SamplerFeedbackTier;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS21 options21{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &options21,
                                              sizeof(options21)))) {
        caps.workGraphsTier = options21.WorkGraphsTier;
    }

    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5,
                                              sizeof(options5)))) {
        caps.raytracingTier = options5.RaytracingTier;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6,
                                              sizeof(options6)))) {
        caps.shadingRateTier = options6.VariableShadingRateTier;
        caps.shadingRateImageTileSize = options6.ShadingRateImageTileSize;
        caps.additionalShadingRates = options6.AdditionalShadingRatesSupported != FALSE;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                              sizeof(options)))) {
        caps.resourceBindingTier = options.ResourceBindingTier;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12,
                                              sizeof(options12)))) {
        caps.enhancedBarriers = options12.EnhancedBarriersSupported != FALSE;
    }

    probeDxc(caps);
}

mcdr::Result<void> requireCaptureBarycentrics() {
    if (!gCtx.caps.barycentricsSupported)
        return mcdr::err("capture.barycentrics", "OIT capture shader ABI 2 requires D3D12 BarycentricsSupported");
    return {};
}

std::string capabilitiesText(const DeviceContext::Capabilities& caps) {
    char buf[512] = {};
    std::snprintf(buf, sizeof(buf),
                  "FL=%s SM=%s(0x%02X)/eff=%s mesh=%d samplerFeedback=%d rt=%d rtMode=%d"
                  "vrs=%d(tile=%u,extra=%d) binding=%d enhancedBarriers=%d barycentrics=%d",
                  featureLevelName(caps.featureLevel), shaderModelName(caps.shaderModel),
                  static_cast<unsigned>(caps.shaderModel),
                  shaderModelProfileSuffix(effectiveShaderModel(caps)),
                  static_cast<int>(caps.meshShaderTier), static_cast<int>(caps.samplerFeedbackTier),
                  static_cast<int>(caps.raytracingTier), mcdr_rt_state_mode(),
                  static_cast<int>(caps.shadingRateTier),
                  caps.shadingRateImageTileSize, caps.additionalShadingRates ? 1 : 0,
                  static_cast<int>(caps.resourceBindingTier), caps.enhancedBarriers ? 1 : 0,
                  caps.barycentricsSupported ? 1 : 0);
    std::string text(buf);
    text += " dxc=";
    text += caps.dxcAvailable ? "yes" : "no";
    if (!caps.dxcProfile.empty()) {
        text += "(" + caps.dxcProfile + ")";
    }
    text += gCtx.rtx.summary();
    return text;
}

static std::string jsonFieldsOf(const nlohmann::json& obj) {
    std::string text = obj.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (text.size() < 2 || text.front() != '{' || text.back() != '}') {
        return std::string();
    }
    text.erase(text.size() - 1);
    text.erase(0, 1);
    return text;
}

static void appendJsonFields(std::string& body, const std::string& fields) {
    if (fields.empty() || body.size() < 2 || body.back() != '}') {
        return;
    }
    body.insert(body.size() - 1, "," + fields);
}

std::string capabilitiesJson(const DeviceContext::Capabilities& caps) {
    nlohmann::json j;
    j["shader_model"] = shaderModelName(caps.shaderModel);
    j["shader_model_value"] = static_cast<int>(caps.shaderModel);
    j["shader_model_effective"] = shaderModelProfileSuffix(effectiveShaderModel(caps));
    j["mesh_shader_tier"] = static_cast<int>(caps.meshShaderTier);
    j["work_graphs_tier"] = static_cast<int>(caps.workGraphsTier);
    j["sampler_feedback_tier"] = static_cast<int>(caps.samplerFeedbackTier);
    j["raytracing_tier"] = static_cast<int>(caps.raytracingTier);
    j["shading_rate_tier"] = static_cast<int>(caps.shadingRateTier);
    j["shading_rate_image_tile_size"] = caps.shadingRateImageTileSize;
    j["additional_shading_rates"] = caps.additionalShadingRates;
    j["resource_binding_tier"] = static_cast<int>(caps.resourceBindingTier);
    j["enhanced_barriers"] = caps.enhancedBarriers;
    j["barycentrics_supported"] = caps.barycentricsSupported;
    j["caps_queried"] = caps.queried;
    j["dxc_available"] = caps.dxcAvailable;
    j["dxc_profile"] = caps.dxcProfile;
    j["dxc_path"] = caps.dxcPath;
    j["dxc_note"] = caps.dxcNote;
    std::string fields = jsonFieldsOf(j);
    const std::string rtx = gCtx.rtx.jsonFragment();
    if (!rtx.empty()) {
        fields += ",";
        fields += rtx;
    }
    return fields;
}

constexpr UINT kAgilitySdkVersion = 619;

struct AgilityAttempt {
    bool requested = false;
    bool applied = false;
    HRESULT factoryHr = E_FAIL;
    HRESULT applyHr = E_FAIL;
    std::wstring dir;
};

AgilityAttempt gAgility;

std::wstring ownModuleDir() {    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ownModuleDir), &mod)
        || !mod)
        return {};
    wchar_t path[1024] = {};
    const DWORD n = GetModuleFileNameW(mod, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    if (n == 0 || n >= sizeof(path) / sizeof(path[0])) return {};
    const std::wstring full(path, n);
    const size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : full.substr(0, slash + 1);
}

void applyAgilitySdk() {
    if (gAgility.requested) return;
    gAgility.requested = true;
    const std::wstring dir = ownModuleDir();
    if (dir.empty()) {
        dbgLogWarn("agility: cannot resolve MCDR.dll directory - skipped");
        return;
    }
    const std::wstring core = dir + L"D3D12Core.dll";
    if (GetFileAttributesW(core.c_str()) == INVALID_FILE_ATTRIBUTES) {
        dbgLogWarn("agility: %s not found next to MCDR.dll - skipped",
                   backendWideToUtf8(core).c_str());
        return;
    }
    const std::string utf8Dir = backendWideToUtf8(dir);
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (!d3d12) d3d12 = LoadLibraryW(L"d3d12.dll");
    if (!d3d12) {
        dbgLogWarn("agility: d3d12.dll unavailable (err=%lu) - skipped", GetLastError());
        return;
    }
    const auto getInterface =
        reinterpret_cast<PFN_D3D12_GET_INTERFACE>(GetProcAddress(d3d12, "D3D12GetInterface"));
    if (!getInterface) {
        dbgLogWarn("agility: D3D12GetInterface missing from d3d12.dll - skipped");
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12SDKConfiguration> config;
    HRESULT hr = getInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&config));
    if (FAILED(hr) || !config) {
        dbgLogWarn("agility: D3D12GetInterface(CLSID_D3D12SDKConfiguration) hr=0x%08lX - skipped",
                   static_cast<unsigned long>(hr));
        return;
    }
    Microsoft::WRL::ComPtr<ID3D12SDKConfiguration1> config1;
    hr = config.As(&config1);
    if (FAILED(hr) || !config1) {
        dbgLogWarn("agility: QI ID3D12SDKConfiguration1 hr=0x%08lX - skipped",
                   static_cast<unsigned long>(hr));
        return;
    }
    Microsoft::WRL::ComPtr<ID3D12DeviceFactory> factory;
    gAgility.factoryHr = config1->CreateDeviceFactory(kAgilitySdkVersion, utf8Dir.c_str(),
                                                      IID_PPV_ARGS(&factory));
    if (FAILED(gAgility.factoryHr) || !factory) {
        dbgLogWarn("agility: CreateDeviceFactory(sdk=%u, \"%s\") hr=0x%08lX - skipped",
                   kAgilitySdkVersion, utf8Dir.c_str(),
                   static_cast<unsigned long>(gAgility.factoryHr));
        return;
    }
    gAgility.applyHr = factory->ApplyToGlobalState();
    if (FAILED(gAgility.applyHr)) {
        dbgLogWarn("agility: ApplyToGlobalState hr=0x%08lX - skipped",
                   static_cast<unsigned long>(gAgility.applyHr));
        return;
    }
    gAgility.dir = dir;
    gAgility.applied = true;
    dbgLogInfo("agility: opt-in ok sdk=%u core=%s factory=ok apply=ok",
               kAgilitySdkVersion, utf8Dir.c_str());
}

void logAgilityRuntime(ID3D12Device* device) {
    UINT sdkVersion = 0;
    bool haveConfig = false;
    Microsoft::WRL::ComPtr<ID3D12DeviceConfiguration> config;
    if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&config))) && config) {
        sdkVersion = config->GetDesc().SDKVersion;
        haveConfig = true;
    }
    const bool agility = haveConfig && sdkVersion >= kAgilitySdkVersion;
    dbgLogInfo("agility: agility=%s sdk=%u%s requested=%d requestedCoreDir=%s "
               "factory=0x%08lX apply=0x%08lX",
               agility ? "on" : "off", sdkVersion, haveConfig ? "" : "(n/a)",
               gAgility.requested ? 1 : 0,
               gAgility.dir.empty() ? "<none>" : backendWideToUtf8(gAgility.dir).c_str(),
               static_cast<unsigned long>(gAgility.factoryHr),
               static_cast<unsigned long>(gAgility.applyHr));
}

#if MCDR_HAVE_STREAMLINE
namespace {
HMODULE gSlModule = nullptr;
std::wstring gSlDir;
bool gSlInitAttempted = false;
bool gSlRequested = false;
sl::Result gSlInitResult = sl::Result::eErrorInitNotCalled;
PFun_slSetD3DDevice* gSlSetDevice = nullptr;
PFun_slIsFeatureLoaded* gSlIsLoaded = nullptr;

int32_t shutdownStreamline() {
    if (gSlInitResult == sl::Result::eOk) {
        auto shutdown = reinterpret_cast<PFun_slShutdown*>(::GetProcAddress(gSlModule, "slShutdown"));
        if (shutdown == nullptr) return 0;
        const sl::Result result = shutdown();
        if (result != sl::Result::eOk) {
            dbgLogError("streamline: slShutdown failed result=0x%X", static_cast<unsigned>(result));
            return 0;
        }
    }
    gSlInitAttempted = false;
    gSlRequested = false;
    gSlInitResult = sl::Result::eErrorInitNotCalled;
    gSlSetDevice = nullptr;
    gSlIsLoaded = nullptr;
    gDlssAvailabilityLogged = false;
    return 1;
}
}

constexpr uint32_t fnv1a32(const char* s, uint32_t h = 2166136261u) {
    return (*s == '\0')
               ? h
               : fnv1a32(s + 1, (h ^ static_cast<uint32_t>(static_cast<unsigned char>(*s))) * 16777619u);
}

bool streamlineRequested() {
    return mcdr_dlss_streamline_requested() != 0;
}

void streamlineInitEarly() {
    if (gSlInitAttempted) return;
    gSlInitAttempted = true;
    gSlRequested = streamlineRequested();
    gSlDir = ownModuleDir();
    if (!gSlRequested || gSlDir.empty()) return;
    const std::wstring dllPath = gSlDir + L"sl.interposer.dll";
    if (gSlModule == nullptr) gSlModule = ::LoadLibraryW(dllPath.c_str());
    if (gSlModule == nullptr) {
        dbgLogWarn("streamline: %s not found (err=%lu) - skipped", backendWideToUtf8(dllPath).c_str(),
                   ::GetLastError());
        return;
    }
    auto pInit = reinterpret_cast<PFun_slInit*>(::GetProcAddress(gSlModule, "slInit"));
    gSlSetDevice = reinterpret_cast<PFun_slSetD3DDevice*>(::GetProcAddress(gSlModule, "slSetD3DDevice"));
    gSlIsLoaded = reinterpret_cast<PFun_slIsFeatureLoaded*>(::GetProcAddress(gSlModule, "slIsFeatureLoaded"));
    if (pInit == nullptr || gSlSetDevice == nullptr || gSlIsLoaded == nullptr) {
        dbgLogWarn("streamline: %s is missing expected exports - skipped", backendWideToUtf8(dllPath).c_str());
        return;
    }
    const wchar_t* paths[1] = { gSlDir.c_str() };
    sl::Feature features[4] = { sl::kFeatureDLSS, sl::kFeatureReflex, sl::kFeaturePCL,
                               sl::kFeatureDLSS_G };
    sl::Preferences pref{};
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0.0";
    pref.applicationId = fnv1a32("dxr_mc");
    pref.projectId = "";
    pref.renderAPI = sl::RenderAPI::eD3D12;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 4;
    pref.pathToLogsAndData = gSlDir.c_str();
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                sl::PreferenceFlags::eDisableDebugText |
                sl::PreferenceFlags::eUseFrameBasedResourceTagging |
                sl::PreferenceFlags::eUseDXGIFactoryProxy;
    gSlInitResult = (*pInit)(pref, sl::kSDKVersion);
    dbgLogWarn("streamline: slInit=0x%X (early, before any DXGI/D3D12 call)", 
               static_cast<unsigned>(gSlInitResult));
}

void applyStreamline(ID3D12Device* device) {
    if (!gSlInitAttempted) streamlineInitEarly();
    if (!gSlRequested || gSlModule == nullptr || gSlInitResult != sl::Result::eOk) return;
    int slSupport = -1;
    {
        using PFn_IsFeatureSupported = sl::Result (*)(sl::Feature, const sl::AdapterInfo&);
        auto isSupported = reinterpret_cast<PFn_IsFeatureSupported>(
            ::GetProcAddress(gSlModule, "slIsFeatureSupported"));
        if (isSupported != nullptr && device != nullptr) {
            const LUID luid = device->GetAdapterLuid();
            sl::AdapterInfo info{};
            info.deviceLUID = reinterpret_cast<uint8_t*>(const_cast<LUID*>(&luid));
            info.deviceLUIDSizeInBytes = sizeof(LUID);
            slSupport = static_cast<int>(isSupported(sl::kFeatureDLSS, info));
        }
    }
    const sl::Result deviceResult = (*gSlSetDevice)(device);
    bool loaded = false;
    const sl::Result loadResult = (*gSlIsLoaded)(sl::kFeatureDLSS, loaded);
    mcdr_dlss_note_sl_loaded(loaded ? 1 : 0);
    if (!loaded) {
        dbgLogError("Streamline is up but the DLSS plugin did NOT initialize. Look for "
                    "'NGXInitValidateSnippets' in sl.log next to MCDR.dll: the installed NGX "
                    "runtime is older than the bundled nvngx_dlss.dll. DLSS stays off and "
                    "sub-pixel jitter is suppressed, so the image is stable instead of vibrating.");
    }
    dbgLogWarn("streamline: slIsFeatureSupported(DLSS)=%d setDevice=0x%X load=0x%X dlssLoaded=%s", slSupport,
               static_cast<unsigned>(deviceResult), static_cast<unsigned>(loadResult),
               loaded ? "yes" : "no");
}
#endif

void resetDescriptorAllocationState() {
    gNextSrv = 0;
    gNextRtv = 0;
    gNextFrameRtv = 0;
    gNextDsv = 0;
    std::lock_guard<std::mutex> lk(gFreeSlotMutex);
    gFreeSrvSlots.clear();
    gFreeSamplerSlots.clear();
}

bool deviceReady() {
    return gCtx.device && gCtx.queue && gCtx.queueFence && gCtx.srvHeap && gCtx.srvCpuHeap && gCtx.rtvHeap
        && gCtx.frameRtvHeap && gCtx.dsvHeap && gCtx.samplerHeap && gCtx.drawHeap && gCtx.cmdSigIndexed
        && gCtx.cmdSigNonIndexed;
}

thread_local bool t_deviceInitInProgress = false;

mcdr::Result<void> ensureDevice() {
    streamlineInitEarly();
    applyAgilitySdk();
    loadPixCapturer();
    installCrashHandlerOnce();
    if (deviceReady())
        return {};
    if (t_deviceInitInProgress) {
        return mcdr::err("ensureDevice", "recursive ensureDevice() during device initialization");
    }
    std::lock_guard<std::mutex> initLock(gDeviceInitMutex);
    if (deviceReady())
        return {};
    t_deviceInitInProgress = true;
    struct InitInProgressReset {
        ~InitInProgressReset() { t_deviceInitInProgress = false; }
    } initInProgressReset;

    bool debugEnabled = false;
    const char* dbgLayerEnv = std::getenv("DX12_DEBUG_LAYER");
    if (dbgLayerEnv && dbgLayerEnv[0] == '1') {
        if (gAgility.applied) {
            dbgLogWarn("agility: DX12_DEBUG_LAYER ignored - inbox D3D12SDKLayers would not "
                       "match the 1.619 core");
        } else {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
                debug->EnableDebugLayer();
                debugEnabled = true;
            }
        }
    }

    ComPtr<ID3D12Device> device;
    {
        auto selectRes = selectDeviceAdapter();
        bool ok = static_cast<bool>(selectRes);
        std::string selectError = selectRes ? std::string() : selectRes.error().message;
        if (ok) {
            ComPtr<IDXGIAdapter1> chosen = *selectRes;
            const HRESULT hr = createD3D12Device(chosen.Get(), kRequiredFeatureLevel,
                                                IID_PPV_ARGS(&device));
            if (FAILED(hr)) {
                DXGI_ADAPTER_DESC1 desc{};
                const std::string name = SUCCEEDED(chosen->GetDesc1(&desc))
                    ? adapterDescName(desc) : std::string("<unknown>");
                char hrText[16] = {};
                std::snprintf(hrText, sizeof(hrText), "0x%08X", static_cast<unsigned>(hr));
                selectError = "D3D12CreateDevice(12_2) failed on selected adapter '" + name +
                              "' (hr=" + hrText + ")";
                dbgLogError("%s", selectError.c_str());
                ok = false;
            }
        }
        if (!ok && debugEnabled) {
            debugEnabled = false;
            selectError.clear();
            auto selectRes2 = selectDeviceAdapter();
            if (selectRes2) {
                const ComPtr<IDXGIAdapter1> chosen2 = *selectRes2;
                ok = SUCCEEDED(createD3D12Device(chosen2.Get(), kRequiredFeatureLevel,
                                                 IID_PPV_ARGS(&device)));
            } else {
                ok = false;
                selectError = selectRes2.error().message;
            }
        }
        if (!ok) {
            if (SUCCEEDED(createD3D12Device(nullptr, kRequiredFeatureLevel, IID_PPV_ARGS(&device)))) {
                dbgLogWarn("using the system default adapter after: %s",
                           selectError.c_str());
                ok = true;
            }
        }
        if (!ok) {
            return mcdr::err("ensureDevice",
                             selectError.empty() ? std::string("no D3D12 12_2 adapter") : selectError);
        }
        gCtx.featureLevel = kRequiredFeatureLevel;
    }
    gCtx.device = device;
    gCtx.adapterName = queryAdapterName(device.Get());
    queryDeviceCapabilities(device.Get(), gCtx.caps);
    dbgLogInfo("device ready: %s FL=%s %s", gCtx.adapterName.c_str(),
               featureLevelName(gCtx.featureLevel), capabilitiesText(gCtx.caps).c_str());
    logAgilityRuntime(device.Get());
    applyStreamline(device.Get());
    {
        LUID luid = device->GetAdapterLuid();
        ComPtr<IDXGIFactory4> tmpFactory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&tmpFactory)))) {
            tmpFactory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&gCtx.adapter));
        }
        wireDlssImpl();
        gCtx.rtx = mcdr::probeRtxSupport(gCtx.adapter.Get());
    }
    notifyDlssDeviceReady();
    if (debugEnabled) {
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&gCtx.infoQueue)))) {
            gCtx.infoQueue = nullptr;
        }
        if (gCtx.infoQueue) {
            ComPtr<ID3D12InfoQueue1> iq1;
            if (SUCCEEDED(gCtx.infoQueue.As(&iq1)) && iq1) {
                registerDebugMessageCallback(iq1.Get());
            }
        }
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&gCtx.queue)))) {
        return mcdr::err("ensureDevice", "CreateCommandQueue failed");
    }
    gCtx.queue->GetTimestampFrequency(&gCtx.timestampFrequency);

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gCtx.queueFence)))) {
        return mcdr::err("ensureDevice", "CreateFence(queue) failed");
    }

    if (!createHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapSize,
                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, gCtx.srvHeap)
        ||
        !createHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapSize,
                    D3D12_DESCRIPTOR_HEAP_FLAG_NONE, gCtx.srvCpuHeap)
        || !createHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvHeapSize,
                       D3D12_DESCRIPTOR_HEAP_FLAG_NONE, gCtx.rtvHeap)
        ||
        !createHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kFrameRtvHeapSize,
                    D3D12_DESCRIPTOR_HEAP_FLAG_NONE, gCtx.frameRtvHeap)
        || !createHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvHeapSize,
                       D3D12_DESCRIPTOR_HEAP_FLAG_NONE, gCtx.dsvHeap)
        || !createHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize,
                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, gCtx.samplerHeap)
        ||
        !createHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kDrawHeapSize,
                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, gCtx.drawHeap)) {
        return mcdr::err("ensureDevice", "CreateDescriptorHeap failed");
    }
    gCtx.srvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    gCtx.rtvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    gCtx.dsvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    gCtx.samplerInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    gCtx.drawInc = gCtx.srvInc;
    resetDescriptorAllocationState();

    {
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC cs{};
        cs.NumArgumentDescs = 1;
        cs.pArgumentDescs = &arg;
        cs.ByteStride =
            20;
        if (FAILED(device->CreateCommandSignature(&cs, nullptr, IID_PPV_ARGS(&gCtx.cmdSigIndexed)))) {
            return mcdr::err("ensureDevice", "CreateCommandSignature(indexed) failed");
        }
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        cs.ByteStride = 16;
        if (FAILED(device->CreateCommandSignature(&cs, nullptr, IID_PPV_ARGS(&gCtx.cmdSigNonIndexed)))) {
            return mcdr::err("ensureDevice", "CreateCommandSignature(non-indexed) failed");
        }
    }
    return {};
}

void destroyDevice() {
    auto result = destroyDeviceChecked();
    if (!result) dbgLogError("destroyDevice: %s", result.error().message.c_str());
}

mcdr::Result<void> destroyDeviceChecked() {
    dbgLogProgress("destroyDevice stage: enter");
    if (getActiveSurface() != nullptr) return mcdr::err("destroyDevice", "A surface still owns device resources");
    {
        std::lock_guard<std::mutex> lock(gRecordingContextsMutex);
        if (!gRecordingContexts.empty()) return mcdr::err("destroyDevice", "Command contexts still own device resources");
    }
    if (!waitForRenderThreadSubmit()) return mcdr::err("destroyDevice", "Render submission did not drain");
    if (auto idle = deviceWaitIdle(); !idle) return std::unexpected<mcdr::Error>(idle.error());
    if (!fgReleaseProviderWithWait(nullptr)) return mcdr::err("destroyDevice", "Frame generation teardown is still active");
    if (!srReleaseProvider()) return mcdr::err("destroyDevice", "Reconstruction teardown is still active");
    dbgLogProgress("destroyDevice stage: queue fence wait done");
    if (mcdr_dlss_shutdown_checked() != 1) return mcdr::err("destroyDevice", "DLSS teardown did not complete");
    dbgLogProgress("destroyDevice stage: dlss shutdown done");
    gCtx.dlssOutputOverride = nullptr;
    gCtx.dlssOutputOverrideLease.reset();
    flushPendingDeletes(true);
    if (gCtx.queueFenceEvent) CloseHandle(gCtx.queueFenceEvent);
    gCtx = DeviceContext{};
    resetDescriptorAllocationState();
    dbgLogProgress("destroyDevice stage: done");
    return {};
}

DeviceContext& deviceContext() { return gCtx; }

uintptr_t getDeviceHandle() { return reinterpret_cast<uintptr_t>(gCtx.device.Get()); }

uintptr_t getQueueHandle() { return reinterpret_cast<uintptr_t>(gCtx.queue.Get()); }

uintptr_t createHiddenWindow(int width, int height) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Dx12HiddenTest";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc)) return 0;
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"Dx12HiddenTest", L"DX12 Render Test", WS_POPUP, 0, 0,
                                width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        UnregisterClassW(L"Dx12HiddenTest", wc.hInstance);
        return 0;
    }
    ShowWindow(hwnd, SW_HIDE);
    UpdateWindow(hwnd);
    return reinterpret_cast<uintptr_t>(hwnd);
}

void destroyHiddenWindow(uintptr_t hwnd) {
    if (hwnd) {
        DestroyWindow(reinterpret_cast<HWND>(hwnd));
        UnregisterClassW(L"Dx12HiddenTest", GetModuleHandleW(nullptr));
    }
}

mcdr::Result<D3D12_CPU_DESCRIPTOR_HANDLE> allocRtvHandle() {
    if (gNextRtv >= kRtvHeapSize) {
        return mcdr::err("allocRtvHandle", "RTV descriptor heap exhausted");
    }
    D3D12_CPU_DESCRIPTOR_HANDLE h = gCtx.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(gNextRtv) * gCtx.rtvInc;
    ++gNextRtv;
    return h;
}

DXGI_FORMAT toDxgiFormat(int gpuFormat) {
    switch (gpuFormat) {
        case 0: return DXGI_FORMAT_R8_UNORM;
        case 1: return DXGI_FORMAT_R8_SNORM;
        case 2: return DXGI_FORMAT_R8G8_UNORM;
        case 3: return DXGI_FORMAT_R8G8_SNORM;
        case 4: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case 5: return DXGI_FORMAT_R8G8B8A8_SNORM;
        case 6: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case 7: return DXGI_FORMAT_R8G8B8A8_SNORM;
        case 8: return DXGI_FORMAT_R16_UNORM;
        case 9: return DXGI_FORMAT_R16_SNORM;
        case 10: return DXGI_FORMAT_R16G16_UNORM;
        case 11: return DXGI_FORMAT_R16G16_SNORM;
        case 12: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case 13: return DXGI_FORMAT_R16G16B16A16_SNORM;
        case 14: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case 15: return DXGI_FORMAT_R16G16B16A16_SNORM;
        case 16: return DXGI_FORMAT_R8_UINT;
        case 17: return DXGI_FORMAT_R8_SINT;
        case 18: return DXGI_FORMAT_R8G8_UINT;
        case 19: return DXGI_FORMAT_R8G8_SINT;
        case 20: return DXGI_FORMAT_R8G8B8A8_UINT;
        case 21: return DXGI_FORMAT_R8G8B8A8_SINT;
        case 22: return DXGI_FORMAT_R8G8B8A8_UINT;
        case 23: return DXGI_FORMAT_R8G8B8A8_SINT;
        case 24: return DXGI_FORMAT_R16_UINT;
        case 25: return DXGI_FORMAT_R16_SINT;
        case 26: return DXGI_FORMAT_R16G16_UINT;
        case 27: return DXGI_FORMAT_R16G16_SINT;
        case 28: return DXGI_FORMAT_R16G16B16A16_UINT;
        case 29: return DXGI_FORMAT_R16G16B16A16_SINT;
        case 30: return DXGI_FORMAT_R16G16B16A16_UINT;
        case 31: return DXGI_FORMAT_R16G16B16A16_SINT;
        case 32: return DXGI_FORMAT_R32_UINT;
        case 33: return DXGI_FORMAT_R32_SINT;
        case 34: return DXGI_FORMAT_R32G32_UINT;
        case 35: return DXGI_FORMAT_R32G32_SINT;
        case 36: return DXGI_FORMAT_R32G32B32A32_UINT;
        case 37: return DXGI_FORMAT_R32G32B32A32_SINT;
        case 38: return DXGI_FORMAT_R32G32B32A32_UINT;
        case 39: return DXGI_FORMAT_R32G32B32A32_SINT;
        case 40: return DXGI_FORMAT_R16_FLOAT;
        case 41: return DXGI_FORMAT_R16G16_FLOAT;
        case 42: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case 43: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case 44: return DXGI_FORMAT_R32_FLOAT;
        case 45: return DXGI_FORMAT_R32G32_FLOAT;
        case 46: return DXGI_FORMAT_R32G32B32_FLOAT;
        case 47: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case 48: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case 49: return DXGI_FORMAT_R10G10B10A2_UINT;
        case 50: return DXGI_FORMAT_R11G11B10_FLOAT;
        case 51: return DXGI_FORMAT_D32_FLOAT;
        case 52: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case 53: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case 54: return DXGI_FORMAT_D16_UNORM;
        case 55: return DXGI_FORMAT_UNKNOWN;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT toDxgiVertexFormat(int gpuFormat) {
    switch (gpuFormat) {
        case 36: return DXGI_FORMAT_R32G32B32_UINT;
        case 37: return DXGI_FORMAT_R32G32B32_SINT;
        case 45: return DXGI_FORMAT_R32G32_FLOAT;
        case 46: return DXGI_FORMAT_R32G32B32_FLOAT;
        default: return toDxgiFormat(gpuFormat);
    }
}

namespace {

bool hasDepthAspect(int fmt) { return fmt == 51 || fmt == 52 || fmt == 53 || fmt == 54; }

D3D12_HEAP_TYPE pickBufferHeapType(int usage) {
    if (usage & 2) return D3D12_HEAP_TYPE_UPLOAD;
    if (usage & 1) return D3D12_HEAP_TYPE_READBACK;
    return D3D12_HEAP_TYPE_DEFAULT;
}

D3D12_RESOURCE_STATES initialStateFor(D3D12_HEAP_TYPE heap) {
    switch (heap) {
        case D3D12_HEAP_TYPE_UPLOAD: return D3D12_RESOURCE_STATE_GENERIC_READ;
        case D3D12_HEAP_TYPE_READBACK: return D3D12_RESOURCE_STATE_COPY_DEST;
        default: return D3D12_RESOURCE_STATE_COMMON;
    }
}

DXGI_FORMAT typelessDepthFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
        default: return format;
    }
}

D3D12_RESOURCE_FLAGS textureFlags(int usage, int format) {
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (usage & 8) {
        flags |= hasDepthAspect(format) ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                        : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (usage & 32) {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    return flags;
}

}

mcdr::Result<void*> createTexture(int usage, int format, int width, int height, int depthOrLayers,
                                  int mipLevels) {
    if (auto d = ensureDevice(); !d) return mcdr::err("createTexture", errorMessage(d.error()));

    DXGI_FORMAT dxgi = toDxgiFormat(format);
    if (dxgi == DXGI_FORMAT_UNKNOWN) {
        return mcdr::err("createTexture", "createTexture: unsupported format ordinal " + std::to_string(format));
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = std::max(1u, static_cast<UINT>(width));
    desc.Height = std::max(1u, static_cast<UINT>(height));
    desc.DepthOrArraySize = static_cast<UINT16>(std::max(1u, static_cast<UINT>(depthOrLayers)));
    desc.MipLevels = static_cast<UINT16>(std::max(1u, static_cast<UINT>(mipLevels)));
    desc.Format = typelessDepthFormat(dxgi);
    desc.SampleDesc.Count = 1;
    desc.Flags = textureFlags(usage, format);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto obj = std::make_unique<Dx12Object>();
    obj->kind = Dx12Object::Kind::Texture;
    ++gLiveTex;
    obj->usage = usage;
    obj->size = static_cast<long long>(desc.Width) * desc.Height;
    obj->dxgiFormat = dxgi;
    ++gStatTexCreate;
    HRESULT hr = gCtx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                  IID_PPV_ARGS(&obj->resource));
    if (FAILED(hr)) {
        std::string text = "CreateCommittedResource(texture): " + formatBackendResult(hr);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET
            || hr == DXGI_ERROR_DEVICE_HUNG) {
            text += " — " + deviceStatusText();
        }
        return mcdr::err("createTexture", text);
    }
    if (usage & 8) {
        dbgLogInfo("createTexture: RTA handle=%p w=%d h=%d layers=%d mips=%d fmt=%d usage=0x%x",
                   reinterpret_cast<void*>(obj.get()), width, height, depthOrLayers, mipLevels, format,
                   usage);
    }
    if (usage & 16) {
        dbgLogInfo("createTexture: CUBE handle=%p w=%d h=%d layers=%d mips=%d fmt=%d usage=0x%x",
               reinterpret_cast<void*>(obj.get()), width, height, depthOrLayers, mipLevels, format, usage);
    }
    return static_cast<void*>(obj.release());
}

mcdr::Result<void*> createBuffer(int usage, long long size) {
    if (auto d = ensureDevice(); !d) return mcdr::err("createBuffer", errorMessage(d.error()));
    if (size <= 0) {
        return mcdr::err("createBuffer", "createBuffer: buffer size must be positive");
    }

    D3D12_HEAP_TYPE heapType = pickBufferHeapType(usage);

    const bool uavBuffer = (usage & 1024) != 0;

    const UINT64 alignedSize = (static_cast<UINT64>(size) + 255ULL) & ~255ULL;
    if (!uavBuffer) {
        if (Dx12Object* reused = takePooledBuffer(heapType, alignedSize)) {
            reused->usage = usage;
            reused->size = size;
            reused->mappedPtr = nullptr;
            DBG_LOG_DEBUG("createBuffer: reused pooled buffer size=%lld usage=0x%x", (long long)size, usage);
            if (heapType == D3D12_HEAP_TYPE_READBACK) {
                static std::atomic<int> gRbReuseLog{0};
                const int n = gRbReuseLog.fetch_add(1);
            }
            return reused;
        }
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = alignedSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    auto obj = std::make_unique<Dx12Object>();
    obj->kind = Dx12Object::Kind::Buffer;
    ++gLiveBuf;
    accountLiveBuffer(heapType, static_cast<long long>(alignedSize));
    if (heapType == D3D12_HEAP_TYPE_READBACK) {
        static std::atomic<int> gRbLog{0};
        const int rbn = gRbLog.fetch_add(1);
    }
    obj->usage = usage;
    obj->size = size;
    obj->heapType = heapType;
    if (uavBuffer) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ++gStatBufCreate;
    HRESULT hr = gCtx.device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, initialStateFor(heapType), nullptr, IID_PPV_ARGS(&obj->resource));
    if (FAILED(hr)) {
        std::string text = "CreateCommittedResource(buffer): " + formatBackendResult(hr);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET
            || hr == DXGI_ERROR_DEVICE_HUNG) {
            text += " — " + deviceStatusText();
        }
        return mcdr::err("createBuffer", text);
    }
    return static_cast<void*>(obj.release());
}

mcdr::Result<void*> createSampler(int addressU, int addressV, int minFilter, int magFilter,
                                  int maxAnisotropy, float maxLod) {
    return createSamplerWithMipBias(addressU, addressV, minFilter, magFilter, maxAnisotropy, maxLod, 0.0f);
}

mcdr::Result<void*> createSamplerWithMipBias(int addressU, int addressV, int minFilter, int magFilter,
                                             int maxAnisotropy, float maxLod, float mipLodBias) {
    if (auto d = ensureDevice(); !d) return mcdr::err("createSampler", errorMessage(d.error()));

    D3D12_SAMPLER_DESC desc{};
    desc.AddressU = (addressU == 0) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    desc.AddressV = (addressV == 0) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    float bias = mipLodBias;
    if (!(bias >= -16.0f && bias <= 15.99f)) bias = 0.0f;
    desc.MipLODBias = bias;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = maxLod > 0.0f ? maxLod : 16.0f;
    if (maxAnisotropy > 1) {
        desc.Filter = D3D12_FILTER_ANISOTROPIC;
        desc.MaxAnisotropy = static_cast<UINT>(maxAnisotropy);
    } else {
        bool linear = (minFilter == 1) || (magFilter == 1);
        desc.Filter = linear ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
    }

    auto slotRes = allocSamplerSlot();
    if (!slotRes) return mcdr::err("createSampler", errorMessage(slotRes.error()));
    int slot = *slotRes;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = gCtx.samplerHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * gCtx.samplerInc;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = gCtx.samplerHeap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<SIZE_T>(slot) * gCtx.samplerInc;

    gCtx.device->CreateSampler(&desc, cpu);

    auto obj = std::make_unique<Dx12Object>();
    obj->kind = Dx12Object::Kind::Sampler;
    obj->cpuHandle = cpu;
    obj->gpuHandle = gpu;
    obj->descSlot = slot;
    return static_cast<void*>(obj.release());
}

mcdr::Result<void*> createTextureView(Dx12Object* texture, int baseMipLevel, int mipLevels) {
    if (auto d = ensureDevice(); !d) return mcdr::err("createTextureView", errorMessage(d.error()));
    if (!texture || texture->kind != Dx12Object::Kind::Texture || !texture->resource) {
        return mcdr::err("createTextureView", "createTextureView: invalid texture handle");
    }
    const auto textureDesc = texture->resource->GetDesc();
    const auto firstMip = static_cast<UINT>(std::max(0, baseMipLevel));
    const auto mipCount = static_cast<UINT>(std::max(1, mipLevels));
    if (textureDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || textureDesc.SampleDesc.Count != 1 ||
        firstMip >= textureDesc.MipLevels || mipCount > textureDesc.MipLevels - firstMip ||
        ((texture->usage & 16) != 0 && textureDesc.DepthOrArraySize < 6)) {
        return mcdr::err("createTextureView", "texture SRV range or dimension is invalid");
    }
    if (!(texture->usage & 4)) {
        dbgLog("createTextureView: texture usage=0x%x lacks TEXTURE_BINDING (fmt=%d) — creating SRV anyway",
               texture->usage, static_cast<int>(texture->dxgiFormat));
    }

    auto slotRes = allocSrvSlot();
    if (!slotRes) return mcdr::err("createTextureView", errorMessage(slotRes.error()));
    int slot = *slotRes;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = gCtx.srvCpuHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * gCtx.srvInc;
    D3D12_CPU_DESCRIPTOR_HANDLE gpuCpu = gCtx.srvHeap->GetCPUDescriptorHandleForHeapStart();
    gpuCpu.ptr += static_cast<SIZE_T>(slot) * gCtx.srvInc;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = gCtx.srvHeap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<SIZE_T>(slot) * gCtx.srvInc;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    DXGI_FORMAT viewFormat = texture->dxgiFormat;
    switch (viewFormat) {
        case DXGI_FORMAT_D32_FLOAT: viewFormat = DXGI_FORMAT_R32_FLOAT; break;
        case DXGI_FORMAT_D16_UNORM: viewFormat = DXGI_FORMAT_R16_UNORM; break;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: viewFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: viewFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
        default: break;
    }
    srv.Format = viewFormat;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (texture->usage & 16) {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MipLevels = static_cast<UINT>(std::max(1, mipLevels));
        srv.TextureCube.MostDetailedMip = static_cast<UINT>(std::max(0, baseMipLevel));
        dbgLogInfo("createTextureView: CUBE srv slot=%d fmt=%d mip=%d most=%d layers=%d", slot,
               static_cast<int>(texture->dxgiFormat), static_cast<int>(srv.TextureCube.MipLevels),
               static_cast<int>(srv.TextureCube.MostDetailedMip),
               static_cast<int>(texture->resource->GetDesc().DepthOrArraySize));
    } else {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = static_cast<UINT>(std::max(1, mipLevels));
        srv.Texture2D.MostDetailedMip = static_cast<UINT>(std::max(0, baseMipLevel));
    }
    gCtx.device->CreateShaderResourceView(texture->resource.Get(), &srv, cpu);
    gCtx.device->CreateShaderResourceView(texture->resource.Get(), &srv, gpuCpu);

    auto obj = std::make_unique<Dx12Object>();
    obj->kind = Dx12Object::Kind::TextureView;
    obj->cpuHandle = cpu;
    obj->gpuHandle = gpu;
    obj->descSlot = slot;
    obj->sourceTexture = texture;
    obj->srvDesc = srv;
    obj->hasSrvDesc = true;
    return static_cast<void*>(obj.release());
}

std::shared_ptr<void> retainObjectLease(void* opaqueObject) {
    auto* object = static_cast<Dx12Object*>(opaqueObject);
    if (!object) return {};
    auto packetOwner = object->packetContentOwner.lock();
    auto sourceLease = object->sourceTexture ? retainObjectLease(object->sourceTexture) : std::shared_ptr<void>{};
    object->recordingReferences.fetch_add(1, std::memory_order_relaxed);
    return std::shared_ptr<void>(object,
        [packetOwner = std::move(packetOwner), sourceLease = std::move(sourceLease)](void* retained) {
        static_cast<Dx12Object*>(retained)->recordingReferences.fetch_sub(1, std::memory_order_release);
    });
}

void retainObjectForRecording(void* opaqueContext, void* opaqueObject) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !opaqueObject || !context->listOpen) return;
    if (!context->retainedRecordingObjects.emplace(static_cast<Dx12Object*>(opaqueObject), true).second) return;
    auto lease = retainObjectLease(opaqueObject);
    context->recordingLeases[context->currentListSlot].push_back(std::move(lease));
}

void* recordingContextForCommandList(void* commandList) {
    std::lock_guard<std::mutex> lock(gRecordingContextsMutex);
    const auto found = gRecordingContexts.find(commandList);
    if (found == gRecordingContexts.end()) return nullptr;
    auto* context = found->second;
    return context->listOpen && context->commandList == commandList ? context : nullptr;
}

namespace {
void closeSampledTextureOwner(Dx12Object* object);
}

void destroyObject(Dx12Object* obj) {
    if (!obj) return;
    obj->closeRequested.store(true, std::memory_order_release);
    closeSampledTextureOwner(obj);
    if (obj->mappedPtr) {
        if (obj->resource) obj->resource->Unmap(0, nullptr);
        obj->mappedPtr = nullptr;
    }
    DBG_LOG_DEBUG("destroyObject: kind=%d size=%lld", (int)obj->kind, (long long)obj->size);
    if (obj->kind == Dx12Object::Kind::Buffer && obj->heapType == D3D12_HEAP_TYPE_READBACK) {
        static std::atomic<int> gRbDestroyLog{0};
        const int n = gRbDestroyLog.fetch_add(1);
    }
    UINT64 required = (gOpenListCount > 0) ? kFenceBlockedByOpenList : (gCtx.queueFenceValue + 1);
    {
        std::lock_guard<std::mutex> lk(gPendingMutex);
        gPendingDeletes.push_back(PendingDelete{obj, required});
    }
}

namespace {
mcdr::Result<uint32_t> resourceSubresourceCount(ID3D12Resource* resource);
bool isDepthStencilFormat(DXGI_FORMAT format);
mcdr::Result<uint32_t> sampledTextureSubresourceCount(Dx12Object& texture) {
    if (texture.sampledSubresourceCount) return texture.sampledSubresourceCount;
    const auto total = resourceSubresourceCount(texture.resource.Get());
    if (total) texture.sampledSubresourceCount = *total;
    return total;
}
struct SampledTextureVersion {
    std::shared_ptr<const void> recording;
    std::shared_ptr<std::atomic<bool>> discarded;
    ComPtr<ID3D12Fence> fence;
    uint64_t submission = 0;
    uint64_t number = 0;
    uint64_t budgetLimit = 0;
    std::shared_ptr<void> budget;
    std::shared_ptr<void> sourceLease;
    ComPtr<ID3D12Resource> source;
    std::vector<uint32_t> coverage;
    std::vector<uint8_t> initialized;
    std::shared_ptr<void> savedBudget;
    Dx12Object saved;
    bool savedReady = false;
    bool failed = false;
};
template<class Element>
struct SampledVersionAllocator {
    using value_type = Element;
    const uint64_t* budgetLimit;
    explicit SampledVersionAllocator(const uint64_t* limit) noexcept : budgetLimit(limit) {}
    template<class Other>
    SampledVersionAllocator(const SampledVersionAllocator<Other>& other) noexcept : budgetLimit(other.budgetLimit) {}
    struct alignas(Element) Allocation {
        std::shared_ptr<void> budget;
    };
    Element* allocate(size_t count) {
        constexpr auto budgetBytesPerElement = sizeof(Element) + sizeof(SampledTextureVersion) +
            2 * sizeof(std::shared_ptr<void>);
        if (count > (SIZE_MAX - sizeof(Allocation)) / budgetBytesPerElement) throw std::bad_alloc();
        const auto bytes = sizeof(Allocation) + count * sizeof(Element);
        auto budget = mcdr::oit_budget::reserve(sizeof(Allocation) + uint64_t(count) * budgetBytesPerElement, *budgetLimit);
        if (!budget) throw std::bad_alloc();
        auto* allocation = static_cast<Allocation*>(::operator new(bytes));
        new (allocation) Allocation{std::move(budget)};
        return reinterpret_cast<Element*>(allocation + 1);
    }
    void deallocate(Element* elements, size_t) noexcept {
        auto* allocation = reinterpret_cast<Allocation*>(elements) - 1;
        auto budget = std::move(allocation->budget);
        allocation->~Allocation();
        ::operator delete(allocation);
    }
    template<class Other>
    bool operator==(const SampledVersionAllocator<Other>& other) const noexcept {
        return budgetLimit == other.budgetLimit;
    }
};
struct SampledTextureOwner {
    uint64_t generation = 0;
    uint64_t number = 1;
    bool closed = false;
    uint64_t budgetLimit;
    std::shared_ptr<void> budget;
    std::vector<std::weak_ptr<SampledTextureVersion>,
        SampledVersionAllocator<std::weak_ptr<SampledTextureVersion>>> versions;
    explicit SampledTextureOwner(uint64_t totalBudgetBytes)
        : generation(nextSampledTextureOwnerGeneration.fetch_add(1, std::memory_order_relaxed)),
          budgetLimit(totalBudgetBytes),
          versions(SampledVersionAllocator<std::weak_ptr<SampledTextureVersion>>{&budgetLimit}) {}
};
struct SampledTextureRead {
    std::shared_ptr<SampledTextureOwner> owner;
    std::shared_ptr<SampledTextureVersion> version;
    std::shared_ptr<void> sourceViewLease;
    std::shared_ptr<void> budget;
    mutable std::shared_ptr<void> descriptorBudget;
    Dx12Object* sourceView = nullptr;
    D3D12_SHADER_RESOURCE_VIEW_DESC description{};
    uint32_t firstMip = 0, mipCount = 0, layerCount = 0;
    mutable bool registered = false;
    mutable ComPtr<ID3D12DescriptorHeap> descriptors;
    mutable Dx12Object savedView;
};
struct SampledWriteRecording {
    CommandContext* context;
    bool completed = false;
    ~SampledWriteRecording() {
        if (!completed) context->recordingFailed.store(true, std::memory_order_release);
    }
};
void closeSampledTextureOwner(Dx12Object* object) {
    std::lock_guard lock(object->sampledContentMutex);
    auto owner = std::static_pointer_cast<SampledTextureOwner>(object->sampledContentOwner.load(std::memory_order_acquire));
    if (!owner) return;
    owner->closed = true;
}
bool textureWriteRecordingCompleted(const TextureWriteRecording& recording) {
    if (!recording.identity || (recording.discarded && recording.discarded->load(std::memory_order_acquire))) return true;
    if (!recording.fence) return false;
    const auto completed = recording.fence->GetCompletedValue();
    return completed != UINT64_MAX && completed >= recording.submission;
}
void retireTextureWriteRecordings(Dx12Object& texture, const std::shared_ptr<const void>& activeRecording) {
    if (activeRecording && texture.textureWriteRecording.identity == activeRecording &&
        !texture.additionalTextureWriteRecordings) return;
    if (textureWriteRecordingCompleted(texture.textureWriteRecording)) texture.textureWriteRecording = {};
    auto* entry = &texture.additionalTextureWriteRecordings;
    while (*entry) {
        if (textureWriteRecordingCompleted((*entry)->recording)) {
            auto retired = std::move(*entry);
            *entry = std::move(retired->next);
        } else {
            entry = &(*entry)->next;
        }
    }
}
bool hasPriorTextureSubmission(const CommandContext& context, const ID3D12Fence* fence, uint64_t submission) {
    return fence == context.fence.Get() && submission && submission <= context.fenceValue;
}
bool textureWriteOrderingAvailable(const Dx12Object& texture, const CommandContext* context) {
    const auto hasOrdering = [context](const TextureWriteRecording& recording) {
        return !recording.identity || (context && (recording.identity == context->recordingIdentity ||
            hasPriorTextureSubmission(*context, recording.fence.Get(), recording.submission)));
    };
    if (!hasOrdering(texture.textureWriteRecording)) return false;
    for (auto* entry = texture.additionalTextureWriteRecordings.get(); entry; entry = entry->next.get()) {
        if (!hasOrdering(entry->recording)) return false;
    }
    return true;
}
SampledTextureContentStatus recordTextureWrite(Dx12Object& texture, CommandContext& context,
    uint32_t firstSubresource, uint32_t subresourceCount) {
    auto* recording = &texture.textureWriteRecording;
    if (recording->identity && recording->identity != context.recordingIdentity) {
        recording = nullptr;
        for (auto* entry = texture.additionalTextureWriteRecordings.get(); entry; entry = entry->next.get()) {
            if (entry->recording.identity == context.recordingIdentity) {
                recording = &entry->recording;
                break;
            }
        }
        if (!recording) {
            auto budget = mcdr::oit_budget::reserve(sizeof(TextureWriteRecordingEntry));
            if (!budget) return SampledTextureContentStatus::BudgetExceeded;
            auto entry = std::make_unique<TextureWriteRecordingEntry>();
            entry->budget = std::move(budget);
            entry->next = std::move(texture.additionalTextureWriteRecordings);
            recording = &entry->recording;
            texture.additionalTextureWriteRecordings = std::move(entry);
        }
    }
    if (recording->identity) {
        const auto end = std::max(uint64_t(recording->firstSubresource) + recording->subresourceCount,
            uint64_t(firstSubresource) + subresourceCount);
        recording->firstSubresource = std::min(recording->firstSubresource, firstSubresource);
        recording->subresourceCount = static_cast<uint32_t>(end - recording->firstSubresource);
    } else {
        recording->identity = context.recordingIdentity;
        recording->discarded = context.recordingDiscarded;
        recording->fence = context.fence;
        recording->submission = context.fenceValue + 1;
        recording->firstSubresource = firstSubresource;
        recording->subresourceCount = subresourceCount;
    }
    return SampledTextureContentStatus::Ready;
}
struct SampledTextureCpuWriteLease {
    std::shared_ptr<void> budget;
    std::shared_ptr<void> sourceLease;
    std::unique_lock<std::mutex> lock;
};
bool sampledVersionCompleted(const SampledTextureVersion& version) {
    if (version.discarded && version.discarded->load(std::memory_order_acquire)) return true;
    if (!version.fence) return false;
    const auto completed = version.fence->GetCompletedValue();
    return completed != UINT64_MAX && completed >= version.submission;
}
bool sampledCoverageOverlaps(const SampledTextureVersion& version, uint32_t first, uint32_t count) {
    const auto end = std::min<uint64_t>(uint64_t(first) + count, version.coverage.size());
    for (uint64_t subresource = first; subresource < end; ++subresource) {
        if (version.coverage[static_cast<size_t>(subresource)]) return true;
    }
    return false;
}
struct PacketContentObjects {
    ComPtr<ID3D12Fence> fence;
    uint64_t submission = 0;
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    std::shared_ptr<mcdr::oit_budget::Reservation> descriptorBudget;
    std::vector<std::shared_ptr<mcdr::oit_budget::Reservation>> resourceBudgets;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    std::vector<std::unique_ptr<Dx12Object>> objects;
};
bool addPacketContentBudget(uint64_t& total, uint64_t bytes) {
    if (bytes > UINT64_MAX - total) return false;
    total += bytes;
    return true;
}
}

SampledTextureContentCapture captureSampledTextureContent(void* opaqueContext, void* opaqueView,
                                                         uint64_t totalBudgetBytes) {
    SampledTextureContentCapture result;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto* view = static_cast<Dx12Object*>(opaqueView);
    if (context && context->recordingFailed.load(std::memory_order_acquire)) {
        result.status = SampledTextureContentStatus::RecordingFailed;
        return result;
    }
    if (!context || !context->listOpen || !context->commandList || !context->recordingIdentity || !context->fence ||
        context->fenceValue == UINT64_MAX ||
        context->submissionFailed.load() || context->recordingFailed.load()) {
        result.status = SampledTextureContentStatus::StaleRecording;
        return result;
    }
    if (!view || view->closeRequested.load(std::memory_order_acquire) ||
        view->kind != Dx12Object::Kind::TextureView || !view->hasSrvDesc ||
        !view->cpuHandle.ptr || !view->sourceTexture) return result;
    auto* texture = view->sourceTexture;
    if (texture->closeRequested.load(std::memory_order_acquire) ||
        texture->kind != Dx12Object::Kind::Texture || !texture->resource ||
        texture->heapType != D3D12_HEAP_TYPE_DEFAULT) return result;
    std::lock_guard lock(texture->sampledContentMutex);
    const auto description = texture->resource->GetDesc();
    const auto total = sampledTextureSubresourceCount(*texture);
    if (!total || description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        description.SampleDesc.Count != 1 || description.SampleDesc.Quality != 0 ||
        *total != uint32_t(description.MipLevels) * description.DepthOrArraySize ||
        (description.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0 ||
        isDepthStencilFormat(view->srvDesc.Format)) return result;
    const auto& srv = view->srvDesc;
    if ((description.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 ||
        isDepthStencilFormat(texture->dxgiFormat)) {
        if (texture->dxgiFormat != DXGI_FORMAT_D32_FLOAT || description.Format != DXGI_FORMAT_R32_TYPELESS ||
            (description.Flags != D3D12_RESOURCE_FLAG_NONE &&
                description.Flags != D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) || description.DepthOrArraySize != 1 ||
            srv.Format != DXGI_FORMAT_R32_FLOAT || srv.ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D ||
            srv.Texture2D.PlaneSlice != 0) return result;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_R32_FLOAT};
        constexpr auto requiredSupport = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
            D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
        if (FAILED(gCtx.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
            (support.Support1 & requiredSupport) != requiredSupport) return result;
    }
    const bool cube = srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE;
    if (!cube && srv.ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D) return result;
    const auto firstMip = cube ? srv.TextureCube.MostDetailedMip : srv.Texture2D.MostDetailedMip;
    const auto declaredMips = cube ? srv.TextureCube.MipLevels : srv.Texture2D.MipLevels;
    if (firstMip >= description.MipLevels) return result;
    const uint32_t mipCount = declaredMips == UINT32_MAX ? description.MipLevels - firstMip : declaredMips;
    const uint32_t layerCount = cube ? 6u : 1u;
    if (!mipCount || mipCount > description.MipLevels - firstMip ||
        layerCount > description.DepthOrArraySize || (!cube && srv.Texture2D.PlaneSlice != 0)) return result;
    for (const auto& attachment : context->activeAttachmentRanges) {
        if (!attachment.object || attachment.object->resource != texture->resource) continue;
        for (uint32_t layer = 0; layer < layerCount; ++layer) {
            const auto first = firstMip + layer * description.MipLevels;
            if (first < uint64_t(attachment.firstSubresource) + attachment.subresourceCount &&
                attachment.firstSubresource < uint64_t(first) + mipCount) return result;
        }
    }
    bool changed = false;
    try {
        if (texture->closeRequested.load(std::memory_order_acquire)) return result;
        retireTextureWriteRecordings(*texture, context->recordingIdentity);
        if (!textureWriteOrderingAvailable(*texture, context)) {
            result.status = SampledTextureContentStatus::OrderingUnavailable;
            return result;
        }
        auto opaqueOwner = texture->sampledContentOwner.load(std::memory_order_acquire);
        if (!opaqueOwner) {
            auto owner = std::make_shared<SampledTextureOwner>(totalBudgetBytes);
            owner->budget = mcdr::oit_budget::reserve(sizeof(SampledTextureOwner), totalBudgetBytes);
            if (!owner->budget) {
                result.status = SampledTextureContentStatus::BudgetExceeded;
                return result;
            }
            opaqueOwner = owner;
            texture->sampledContentOwner.store(opaqueOwner, std::memory_order_release);
        }
        auto owner = std::static_pointer_cast<SampledTextureOwner>(opaqueOwner);
        if (owner->closed || owner->number == UINT64_MAX) return result;
        owner->budgetLimit = totalBudgetBytes;
        std::shared_ptr<SampledTextureVersion> version;
        for (auto iterator = owner->versions.begin(); iterator != owner->versions.end();) {
            auto existing = iterator->lock();
            if (!existing || sampledVersionCompleted(*existing)) {
                iterator = owner->versions.erase(iterator);
                continue;
            }
            if (existing->number == owner->number) {
                if (existing->recording != context->recordingIdentity) {
                    if (!hasPriorTextureSubmission(*context, existing->fence.Get(), existing->submission)) {
                        result.status = SampledTextureContentStatus::OrderingUnavailable;
                        return result;
                    }
                } else {
                    version = std::move(existing);
                }
            }
            ++iterator;
        }
        if (!version) {
            version = std::make_shared<SampledTextureVersion>();
            version->budget = mcdr::oit_budget::reserve(sizeof(SampledTextureVersion) +
                uint64_t(*total) * (sizeof(uint32_t) + sizeof(uint8_t)), totalBudgetBytes);
            if (!version->budget) {
                result.status = SampledTextureContentStatus::BudgetExceeded;
                return result;
            }
            version->number = owner->number;
            version->budgetLimit = totalBudgetBytes;
            version->recording = context->recordingIdentity;
            version->discarded = context->recordingDiscarded;
            version->fence = context->fence;
            version->submission = context->fenceValue + 1;
            version->source = texture->resource;
            version->sourceLease = retainObjectLease(texture);
            version->coverage.resize(*total);
            version->initialized.resize(*total);
            owner->versions.push_back(version);
        }
        for (uint32_t layer = 0; layer < layerCount; ++layer) {
            for (uint32_t mip = firstMip; mip < firstMip + mipCount; ++mip) {
                if (version->coverage[mip + layer * description.MipLevels] == UINT32_MAX) {
                    result.status = SampledTextureContentStatus::BudgetExceeded;
                    return result;
                }
            }
        }
        auto read = std::make_shared<SampledTextureRead>();
        read->budget = mcdr::oit_budget::reserve(sizeof(SampledTextureRead), totalBudgetBytes);
        if (!read->budget) {
            result.status = SampledTextureContentStatus::BudgetExceeded;
            return result;
        }
        read->owner = owner;
        read->version = version;
        read->sourceView = view;
        read->sourceViewLease = retainObjectLease(view);
        read->description = srv;
        read->firstMip = firstMip;
        read->mipCount = mipCount;
        read->layerCount = layerCount;
        read->savedView.kind = Dx12Object::Kind::TextureView;
        read->savedView.sourceTexture = &version->saved;
        read->savedView.srvDesc = srv;
        read->savedView.hasSrvDesc = true;
        read->savedView.packetContentOwner = read;
        context->recordingLeases[context->currentListSlot].push_back(read);
        changed = true;
        for (uint32_t layer = 0; layer < layerCount; ++layer) {
            for (uint32_t mip = firstMip; mip < firstMip + mipCount; ++mip) {
                ++version->coverage[mip + layer * description.MipLevels];
            }
        }
        read->registered = true;
        version->budgetLimit = std::min(version->budgetLimit, totalBudgetBytes);
        result.status = SampledTextureContentStatus::Ready;
        result.lease = read;
        result.ownerIdentity = owner;
        result.ownerGeneration = owner->generation;
        result.contentVersion = version->number;
        result.firstMip = firstMip;
        result.mipCount = mipCount;
        result.layerCount = layerCount;
        return result;
    } catch (...) {
        if (changed) context->recordingFailed.store(true, std::memory_order_release);
        result.status = changed ? SampledTextureContentStatus::RecordingFailed : SampledTextureContentStatus::BudgetExceeded;
        return result;
    }
}

void cancelSampledTextureContent(const std::shared_ptr<const void>& opaqueRead) {
    const auto read = std::static_pointer_cast<const SampledTextureRead>(opaqueRead);
    if (!read) return;
    std::lock_guard lock(read->sourceView->sourceTexture->sampledContentMutex);
    if (!read->registered) return;
    const auto description = read->version->source->GetDesc();
    for (uint32_t layer = 0; layer < read->layerCount; ++layer) {
        for (uint32_t mip = read->firstMip; mip < read->firstMip + read->mipCount; ++mip) {
            --read->version->coverage[mip + layer * description.MipLevels];
        }
    }
    read->registered = false;
}

SampledTextureContentResolution resolveSampledTextureContent(void* opaqueContext,
    const std::shared_ptr<const void>& opaqueRead) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    const auto read = std::static_pointer_cast<const SampledTextureRead>(opaqueRead);
    if (!context || !read || !context->listOpen || !context->commandList ||
        context->recordingIdentity != read->version->recording || context->submissionFailed.load() ||
        context->recordingFailed.load() || context->fence.Get() != read->version->fence.Get() ||
        context->fenceValue + 1 != read->version->submission) return {};
    std::lock_guard lock(read->sourceView->sourceTexture->sampledContentMutex);
    if (!read->registered) return {};
    auto& version = *read->version;
    if (version.failed) return {SampledTextureContentStatus::RecordingFailed};
    if (!version.savedReady) return {SampledTextureContentStatus::Ready, read->sourceView};
    const auto description = version.source->GetDesc();
    for (uint32_t layer = 0; layer < read->layerCount; ++layer) {
        for (uint32_t mip = read->firstMip; mip < read->firstMip + read->mipCount; ++mip) {
            if (!version.initialized[mip + layer * description.MipLevels])
                return {SampledTextureContentStatus::UnsupportedView};
        }
    }
    if (!read->descriptors) {
        try {
            auto budget = mcdr::oit_budget::reserve(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, version.budgetLimit);
            if (!budget) return {SampledTextureContentStatus::BudgetExceeded};
            D3D12_DESCRIPTOR_HEAP_DESC description{};
            description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            description.NumDescriptors = 1;
            ComPtr<ID3D12DescriptorHeap> descriptors;
            if (FAILED(gCtx.device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&descriptors))))
                return {SampledTextureContentStatus::BudgetExceeded};
            const auto descriptor = descriptors->GetCPUDescriptorHandleForHeapStart();
            gCtx.device->CreateShaderResourceView(version.saved.resource.Get(), &read->description, descriptor);
            read->savedView.cpuHandle = descriptor;
            read->descriptorBudget = std::move(budget);
            read->descriptors = std::move(descriptors);
        } catch (const std::bad_alloc&) {
            return {SampledTextureContentStatus::BudgetExceeded};
        }
    }
    return {SampledTextureContentStatus::Ready, &read->savedView};
}

int captureSampledTextureContentV1(void* context, void* view, uint64_t totalBudgetBytes,
    uint64_t* ownerIdentity, uint64_t* ownerGeneration, uint64_t* contentVersion,
    uint32_t* firstMip, uint32_t* mipCount, uint32_t* firstLayer, uint32_t* layerCount) {
    if (!ownerIdentity || !ownerGeneration || !contentVersion || !firstMip || !mipCount || !firstLayer || !layerCount)
        return -1;
    *ownerIdentity = *ownerGeneration = *contentVersion = 0;
    *firstMip = *mipCount = *firstLayer = *layerCount = 0;
    const auto capture = captureSampledTextureContent(context, view, totalBudgetBytes);
    if (capture.status != SampledTextureContentStatus::Ready || !capture.lease || !capture.ownerIdentity ||
        capture.contentVersion == 0) return static_cast<int>(capture.status) + 1;
    *ownerIdentity = reinterpret_cast<uint64_t>(capture.ownerIdentity.get());
    *ownerGeneration = capture.ownerGeneration;
    *contentVersion = capture.contentVersion;
    *firstMip = capture.firstMip;
    *mipCount = capture.mipCount;
    *firstLayer = capture.firstLayer;
    *layerCount = capture.layerCount;
    return 1;
}

SampledTextureContentStatus prepareSampledTextureWrite(void* opaqueContext, void* opaqueTexture,
    uint32_t firstSubresource, uint32_t subresourceCount) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto* texture = static_cast<Dx12Object*>(opaqueTexture);
    if (!context || !context->listOpen || !context->commandList || !context->recordingIdentity ||
        !context->recordingDiscarded || !context->fence || context->fenceValue == UINT64_MAX ||
        context->submissionFailed.load() || context->recordingFailed.load())
        return SampledTextureContentStatus::StaleRecording;
    if (!texture || texture->closeRequested.load(std::memory_order_acquire) ||
        texture->kind != Dx12Object::Kind::Texture || !texture->resource)
        return SampledTextureContentStatus::UnsupportedView;
    std::lock_guard lock(texture->sampledContentMutex);
    bool changed = false;
    const auto fail = [&](SampledTextureContentStatus status) {
        context->recordingFailed.store(true, std::memory_order_release);
        return changed ? SampledTextureContentStatus::RecordingFailed : status;
    };
    if (texture->closeRequested.load(std::memory_order_acquire)) return fail(SampledTextureContentStatus::StaleRecording);
    const auto total = sampledTextureSubresourceCount(*texture);
    if (!total || !subresourceCount || firstSubresource >= *total ||
        subresourceCount > *total - firstSubresource) return fail(SampledTextureContentStatus::UnsupportedView);
    try {
        retireTextureWriteRecordings(*texture, context->recordingIdentity);
        auto owner = std::static_pointer_cast<SampledTextureOwner>(texture->sampledContentOwner.load(std::memory_order_acquire));
        if (!owner) {
            const auto status = recordTextureWrite(*texture, *context, firstSubresource, subresourceCount);
            return status == SampledTextureContentStatus::Ready ? status : fail(status);
        }
        if (owner->closed || owner->number == UINT64_MAX) return fail(SampledTextureContentStatus::StaleRecording);
        uint64_t temporaryLimit = mcdr::oit_budget::limit;
        size_t conflictCount = 0;
        for (auto iterator = owner->versions.begin(); iterator != owner->versions.end();) {
            auto version = iterator->lock();
            if (!version || sampledVersionCompleted(*version) ||
                hasPriorTextureSubmission(*context, version->fence.Get(), version->submission)) {
                iterator = owner->versions.erase(iterator);
                continue;
            }
            ++iterator;
            if (version->failed) return fail(SampledTextureContentStatus::RecordingFailed);
            if (!sampledCoverageOverlaps(*version, firstSubresource, subresourceCount)) continue;
            if (version->recording != context->recordingIdentity)
                return fail(SampledTextureContentStatus::OrderingUnavailable);
            if (version->savedReady) continue;
            ++conflictCount;
            temporaryLimit = std::min(temporaryLimit, version->budgetLimit);
        }
        auto temporaryBudget = conflictCount == 0 ? std::shared_ptr<mcdr::oit_budget::Reservation>{}
            : mcdr::oit_budget::reserve(uint64_t(conflictCount) * sizeof(std::shared_ptr<SampledTextureVersion>), temporaryLimit);
        if (conflictCount != 0 && !temporaryBudget) return fail(SampledTextureContentStatus::BudgetExceeded);
        std::vector<std::shared_ptr<SampledTextureVersion>> conflicts;
        conflicts.reserve(conflictCount);
        for (const auto& weak : owner->versions) {
            auto version = weak.lock();
            if (version && !version->savedReady && !sampledVersionCompleted(*version) &&
                !hasPriorTextureSubmission(*context, version->fence.Get(), version->submission) &&
                sampledCoverageOverlaps(*version, firstSubresource, subresourceCount))
                conflicts.push_back(std::move(version));
        }
        for (const auto& version : conflicts) {
            auto description = version->source->GetDesc();
            description.Alignment = 0;
            description.Flags = version->source->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            const auto allocation = gCtx.device->GetResourceAllocationInfo(0, 1, &description);
            if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX)
                return fail(SampledTextureContentStatus::UnsupportedView);
            version->savedBudget = mcdr::oit_budget::reserve(allocation.SizeInBytes, version->budgetLimit);
            if (!version->savedBudget) return fail(SampledTextureContentStatus::BudgetExceeded);
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            if (FAILED(gCtx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&version->saved.resource))))
                return fail(SampledTextureContentStatus::BudgetExceeded);
            version->saved.kind = Dx12Object::Kind::Texture;
            version->saved.dxgiFormat = texture->dxgiFormat;
            version->saved.usage = texture->usage;
            version->saved.texturePlanes = 1;
            version->saved.packetContentOwner = version;
        }
        for (const auto& version : conflicts) {
            changed = true;
            for (uint32_t subresource = 0; subresource < version->coverage.size(); ++subresource) {
                if (!version->coverage[subresource]) continue;
                const auto sourceState = resourceSubresourceState(context, version->source.Get(),
                    subresource, D3D12_RESOURCE_STATE_COMMON);
                if (!transitionResourceRange(context, version->source.Get(), subresource, 1,
                        sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE)) {
                    version->failed = true;
                    return fail(SampledTextureContentStatus::RecordingFailed);
                }
                D3D12_TEXTURE_COPY_LOCATION source{};
                source.pResource = version->source.Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                source.SubresourceIndex = subresource;
                D3D12_TEXTURE_COPY_LOCATION destination{};
                destination.pResource = version->saved.resource.Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                destination.SubresourceIndex = subresource;
                context->commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
                if (!transitionResourceRange(context, version->source.Get(), subresource, 1,
                        D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState) ||
                    !transitionResourceRange(context, version->saved.resource.Get(), subresource, 1,
                        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON)) {
                    version->failed = true;
                    return fail(SampledTextureContentStatus::RecordingFailed);
                }
                version->initialized[subresource] = 1;
            }
            version->savedReady = true;
        }
        ++owner->number;
        const auto status = recordTextureWrite(*texture, *context, firstSubresource, subresourceCount);
        if (status != SampledTextureContentStatus::Ready) return fail(status);
        return SampledTextureContentStatus::Ready;
    } catch (...) {
        return fail(SampledTextureContentStatus::RecordingFailed);
    }
}

SampledTextureCpuWrite prepareSampledTextureCpuWrite(void* opaqueTexture,
    uint32_t firstSubresource, uint32_t subresourceCount) {
    auto* texture = static_cast<Dx12Object*>(opaqueTexture);
    if (!texture || texture->closeRequested.load(std::memory_order_acquire) ||
        texture->kind != Dx12Object::Kind::Texture || !texture->resource)
        return {};
    try {
        std::unique_lock lock(texture->sampledContentMutex);
        const auto total = sampledTextureSubresourceCount(*texture);
        if (texture->closeRequested.load(std::memory_order_acquire) || !total || !subresourceCount ||
            firstSubresource >= *total || subresourceCount > *total - firstSubresource) return {};
        retireTextureWriteRecordings(*texture, {});
        if (!textureWriteOrderingAvailable(*texture, nullptr)) return {SampledTextureContentStatus::OrderingUnavailable};
        auto owner = std::static_pointer_cast<SampledTextureOwner>(texture->sampledContentOwner.load(std::memory_order_acquire));
        if (owner) {
            if (owner->closed || owner->number == UINT64_MAX) return {};
            for (const auto& weak : owner->versions) {
                if (const auto version = weak.lock(); version && !sampledVersionCompleted(*version) &&
                    sampledCoverageOverlaps(*version, firstSubresource, subresourceCount))
                    return {SampledTextureContentStatus::OrderingUnavailable};
            }
        }
        auto budget = mcdr::oit_budget::reserve(sizeof(SampledTextureCpuWriteLease));
        if (!budget) return {SampledTextureContentStatus::BudgetExceeded};
        auto lease = std::make_shared<SampledTextureCpuWriteLease>();
        lease->budget = std::move(budget);
        lease->sourceLease = retainObjectLease(texture);
        lease->lock = std::move(lock);
        if (owner) ++owner->number;
        return {SampledTextureContentStatus::Ready, std::move(lease)};
    } catch (...) {
        return {SampledTextureContentStatus::BudgetExceeded};
    }
}

SampledTextureContentStatus prepareSampledRenderPassWrites(void* opaqueContext) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->listOpen || !context->commandList || !context->inRenderPass ||
        context->submissionFailed.load() || context->recordingFailed.load())
        return SampledTextureContentStatus::StaleRecording;
    for (auto& range : context->activeAttachmentRanges) {
        if (range.sampledWritePrepared) continue;
        if (range.state == D3D12_RESOURCE_STATE_DEPTH_WRITE) {
            if (range.object != context->activeDepthTarget || range.object->dxgiFormat != DXGI_FORMAT_D32_FLOAT) continue;
        } else {
            if (range.state != D3D12_RESOURCE_STATE_RENDER_TARGET) continue;
            if (range.object != context->additionalColorTarget && range.object != context->activeMotionTarget &&
                std::find(context->activeColorTargets.begin(), context->activeColorTargets.end(), range.object) ==
                    context->activeColorTargets.end()) continue;
            if (context->currentPipeline && context->currentPipeline->colorTargetCount == 0) continue;
            if (range.object == context->activeMotionTarget && !context->rasterMotionSelected) continue;
            if (range.object != context->activeMotionTarget && context->currentPipeline &&
                !context->currentPipeline->rasterMotionWritesWorldColor && range.object != context->additionalColorTarget) continue;
        }
        const auto status = prepareSampledTextureWrite(context, range.object, range.firstSubresource, range.subresourceCount);
        if (status != SampledTextureContentStatus::Ready) return status;
        range.sampledWritePrepared = true;
    }
    return SampledTextureContentStatus::Ready;
}

std::shared_ptr<const void> retainPacketContent(void* opaqueContext, void* opaqueRanges, uint32_t count,
    uint64_t budgetLimit) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->listOpen || !context->commandList || context->submissionFailed.load() || context->recordingFailed.load() ||
        !context->recordingIdentity || !context->fence || context->fenceValue == UINT64_MAX || !opaqueRanges || !count) return {};
    uint64_t contentBudgetBytes = sizeof(PacketContentLease) + sizeof(mcdr::oit_budget::Reservation);
    if (!addPacketContentBudget(contentBudgetBytes, uint64_t(count) * sizeof(PacketContentRange)) ||
        !addPacketContentBudget(contentBudgetBytes, uint64_t(count) * sizeof(PacketContentRange)) ||
        !addPacketContentBudget(contentBudgetBytes, uint64_t(count) * sizeof(PacketContentTextureView))) return {};
    auto contentBudget = mcdr::oit_budget::reserve(contentBudgetBytes, budgetLimit);
    if (!contentBudget) return {};
    std::shared_ptr<PacketContentLease> content;
    try {
        content = std::make_shared<PacketContentLease>();
        content->budget = std::move(contentBudget);
        content->reads.reserve(count);
        content->frozenReads.reserve(count);
        content->frozenViews.reserve(count);
    } catch (...) {
        return {};
    }
    const auto* ranges = static_cast<const PacketContentRange*>(opaqueRanges);
    try {
        content->reads.assign(ranges, ranges + count);
    } catch (...) {
        return {};
    }
    content->fence = context->fence;
    content->submission = context->fenceValue + 1;
    content->recordingIdentity = context->recordingIdentity;
    for (const auto& range : content->reads) {
        if (!range.resource || !range.subresourceCount) {
            content->status = PacketContentStatus::UnsupportedContent;
            return content;
        }
        const auto subresources = resourceSubresourceCount(range.resource.Get());
        if (!subresources || range.firstSubresource >= *subresources ||
            range.subresourceCount > *subresources - range.firstSubresource) {
            content->status = PacketContentStatus::UnsupportedContent;
            return content;
        }
        const auto description = range.resource->GetDesc();
        if (description.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            (range.firstSubresource != 0 || range.subresourceCount != 1 || !range.byteLength ||
                range.byteOffset > description.Width || range.byteLength > description.Width - range.byteOffset ||
                range.byteLength > UINT64_MAX - 255u || range.byteLength > SIZE_MAX)) {
            content->status = PacketContentStatus::UnsupportedContent;
            return content;
        }
        if (std::any_of(context->activeAttachmentRanges.begin(), context->activeAttachmentRanges.end(),
                [&](const auto& attachment) {
                    return attachment.object && attachment.object->resource == range.resource;
                })) {
            content->status = PacketContentStatus::UnsupportedContent;
            return content;
        }
        if (description.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
            (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.SampleDesc.Count != 1 ||
                *subresources != uint32_t(description.MipLevels) * description.DepthOrArraySize ||
                !range.object || range.object->kind != Dx12Object::Kind::Texture ||
                range.object->resource != range.resource || range.cpuWritable || range.byteOffset ||
                range.byteLength ||
                (description.Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                    D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) != 0)) {
            content->status = PacketContentStatus::UnsupportedContent;
            return content;
        }
        if (!range.immutableOwner) {
            D3D12_HEAP_PROPERTIES properties{};
            D3D12_HEAP_FLAGS flags{};
            if (FAILED(range.resource->GetHeapProperties(&properties, &flags)) ||
                properties.Type != (range.cpuWritable ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT)) {
                content->status = PacketContentStatus::UnsupportedContent;
                return content;
            }
        }
        if (range.view) {
            const auto* view = range.view;
            if (view->kind != Dx12Object::Kind::TextureView || !view->hasSrvDesc ||
                !view->sourceTexture || view->sourceTexture->resource != range.resource) {
                content->status = PacketContentStatus::UnsupportedContent;
                return content;
            }
            const auto& srv = view->srvDesc;
            const bool cube = srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE;
            if (!cube && srv.ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D) {
                content->status = PacketContentStatus::UnsupportedContent;
                return content;
            }
            const uint32_t firstMip = cube ? srv.TextureCube.MostDetailedMip : srv.Texture2D.MostDetailedMip;
            const uint32_t declaredMipCount = cube ? srv.TextureCube.MipLevels : srv.Texture2D.MipLevels;
            const uint32_t layerCount = cube ? 6u : 1u;
            const uint32_t mipCount = declaredMipCount == UINT32_MAX && firstMip < description.MipLevels
                ? description.MipLevels - firstMip : declaredMipCount;
            if (firstMip >= description.MipLevels || mipCount == 0 ||
                mipCount > description.MipLevels - firstMip || layerCount > description.DepthOrArraySize ||
                (!cube && srv.Texture2D.PlaneSlice != 0)) {
                content->status = PacketContentStatus::UnsupportedContent;
                return content;
            }
            for (uint32_t layer = 0; layer < layerCount; ++layer) {
                for (uint32_t mip = firstMip; mip < firstMip + mipCount; ++mip) {
                    const uint32_t subresource = mip + layer * description.MipLevels;
                    if (std::none_of(content->reads.begin(), content->reads.end(), [&](const auto& read) {
                            return read.resource == range.resource && read.view == view &&
                                read.firstSubresource <= subresource &&
                                subresource - read.firstSubresource < read.subresourceCount;
                        })) {
                        content->status = PacketContentStatus::UnsupportedContent;
                        return content;
                    }
                }
            }
        }
    }
    uint64_t ownerBudgetBytes = sizeof(PacketContentObjects);
    const uint64_t objectCapacity = uint64_t(count) * 2u;
    if (objectCapacity > SIZE_MAX ||
        !addPacketContentBudget(ownerBudgetBytes, objectCapacity * sizeof(std::unique_ptr<Dx12Object>)) ||
        objectCapacity > UINT64_MAX / sizeof(Dx12Object) ||
        !addPacketContentBudget(ownerBudgetBytes, objectCapacity * sizeof(Dx12Object)) ||
        !addPacketContentBudget(ownerBudgetBytes,
            uint64_t(count) * sizeof(std::shared_ptr<mcdr::oit_budget::Reservation>)) ||
        !addPacketContentBudget(ownerBudgetBytes, uint64_t(count) * sizeof(Dx12Object*)) ||
        !addPacketContentBudget(ownerBudgetBytes,
            (uint64_t(count) + 1u) * sizeof(mcdr::oit_budget::Reservation))) {
        content->status = PacketContentStatus::UnsupportedContent;
        return content;
    }
    std::shared_ptr<PacketContentObjects> frozenOwner;
    try {
        frozenOwner = std::make_shared<PacketContentObjects>();
        frozenOwner->budget = mcdr::oit_budget::reserve(ownerBudgetBytes, budgetLimit);
        if (!frozenOwner->budget) {
            content->status = PacketContentStatus::UnsupportedContent;
            return content;
        }
        frozenOwner->fence = context->fence;
        frozenOwner->submission = content->submission;
        frozenOwner->resourceBudgets.reserve(content->reads.size());
        frozenOwner->objects.reserve(static_cast<size_t>(objectCapacity));
    } catch (...) {
        content->status = PacketContentStatus::UnsupportedContent;
        return content;
    }
    content->frozenOwner = frozenOwner;
    const auto makeFrozenObject = [&]() -> Dx12Object* {
        try {
            auto object = std::make_unique<Dx12Object>();
            object->packetContentOwner = frozenOwner;
            auto* retained = object.get();
            frozenOwner->objects.push_back(std::move(object));
            return retained;
        } catch (...) {
            return nullptr;
        }
    };
    for (const auto& range : content->reads) {
        const auto existing = std::find_if(content->frozenReads.begin(), content->frozenReads.end(),
            [&](const auto& frozen) {
                return frozen.sourceResource == range.resource && frozen.sourceByteOffset == range.byteOffset &&
                    frozen.byteLength == range.byteLength && frozen.firstSubresource == range.firstSubresource &&
                    frozen.subresourceCount == range.subresourceCount && frozen.cpuWritable == range.cpuWritable &&
                    frozen.immutableOwner == range.immutableOwner;
            });
        if (existing != content->frozenReads.end()) {
            auto frozen = *existing;
            frozen.view = range.view;
            content->frozenReads.push_back(std::move(frozen));
            continue;
        }
        auto frozenSource = range.resource;
        if (range.immutableOwner) {
            auto frozen = range;
            frozen.sourceResource = frozenSource;
            frozen.sourceByteOffset = range.byteOffset;
            content->frozenReads.push_back(std::move(frozen));
            continue;
        }
        ComPtr<ID3D12Resource> frozenResource;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = range.cpuWritable ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
        const auto description = range.resource->GetDesc();
        const bool texture = description.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER;
        auto frozenDescription = description;
        frozenDescription.Alignment = 0;
        frozenDescription.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (texture) {
            const auto shared = std::find_if(content->frozenReads.begin(), content->frozenReads.end(),
                [&](const auto& frozen) {
                    return frozen.sourceResource == range.resource && !frozen.immutableOwner;
                });
            if (shared != content->frozenReads.end()) {
                auto frozen = range;
                frozen.resource = shared->resource;
                frozen.sourceResource = frozenSource;
                frozen.object = shared->object;
                frozen.initialState = D3D12_RESOURCE_STATE_COMMON;
                content->frozenReads.push_back(std::move(frozen));
                continue;
            }
            frozenDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        } else {
            frozenDescription.Width = (range.byteLength + 255ull) & ~255ull;
        }
        const auto allocation = gCtx.device->GetResourceAllocationInfo(0, 1, &frozenDescription);
        if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX) {
            content->status = PacketContentStatus::UnsupportedContent;
            return content;
        }
        auto resourceBudget = mcdr::oit_budget::reserve(allocation.SizeInBytes, budgetLimit);
        if (!resourceBudget) {
            content->status = PacketContentStatus::UnsupportedContent;
            return content;
        }
        const auto frozenInitial = range.cpuWritable ? D3D12_RESOURCE_STATE_GENERIC_READ
            : D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(gCtx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &frozenDescription,
                frozenInitial, nullptr, IID_PPV_ARGS(&frozenResource)))) {
            content->status = PacketContentStatus::UnsupportedContent;
            return content;
        }
        try {
            frozenOwner->resourceBudgets.push_back(std::move(resourceBudget));
        } catch (...) {
            content->status = PacketContentStatus::UnsupportedContent;
            return content;
        }
        auto frozen = range;
        frozen.resource = frozenResource;
        frozen.sourceResource = frozenSource;
        frozen.sourceByteOffset = range.byteOffset;
        frozen.byteOffset = 0;
        frozen.initialState = range.cpuWritable ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON;
        if (range.cpuWritable) {
            void* sourceMapped = range.object && range.object->mappedPtr ? range.object->mappedPtr : nullptr;
            bool unmapSource = false;
            if (!sourceMapped && SUCCEEDED(range.resource->Map(0, nullptr, &sourceMapped))) unmapSource = true;
            if (!sourceMapped) {
                content->status = PacketContentStatus::UnsupportedContent;
                return content;
            }
            void* destinationMapped = nullptr;
            const D3D12_RANGE readRange{0, 0};
            if (FAILED(frozenResource->Map(0, &readRange, &destinationMapped))) {
                if (unmapSource) range.resource->Unmap(0, nullptr);
                content->status = PacketContentStatus::UnsupportedContent;
                return content;
            }
            std::memcpy(destinationMapped, static_cast<const std::byte*>(sourceMapped) + range.byteOffset,
                static_cast<size_t>(range.byteLength));
            frozenResource->Unmap(0, nullptr);
            if (unmapSource) range.resource->Unmap(0, nullptr);
        }
        auto object = makeFrozenObject();
        if (!object) {
            content->status = PacketContentStatus::UnsupportedContent;
            return content;
        }
        object->kind = texture ? Dx12Object::Kind::Texture : Dx12Object::Kind::Buffer;
        object->resource = frozenResource;
        object->heapType = heap.Type;
        object->size = static_cast<long long>(range.byteLength);
        if (texture) {
            object->dxgiFormat = range.object->dxgiFormat;
            object->usage = range.object->usage;
            object->texturePlanes = 1;
        }
        frozen.object = object;
        content->frozenReads.push_back(std::move(frozen));
    }
    std::vector<Dx12Object*> views;
    try {
        views.reserve(content->frozenReads.size());
    } catch (...) {
        content->status = PacketContentStatus::UnsupportedContent;
        return content;
    }
    for (const auto& range : content->frozenReads) {
        if (range.view && std::find(views.begin(), views.end(), range.view) == views.end())
            views.push_back(range.view);
    }
    if (!views.empty()) {
        const uint64_t descriptorBytes = std::max<uint64_t>(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
            uint64_t(views.size()) * gCtx.srvInc);
        frozenOwner->descriptorBudget = mcdr::oit_budget::reserve(descriptorBytes, budgetLimit);
        if (!frozenOwner->descriptorBudget) {
            content->status = PacketContentStatus::UnsupportedContent;
            return content;
        }
        D3D12_DESCRIPTOR_HEAP_DESC description{};
        description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        description.NumDescriptors = static_cast<UINT>(views.size());
        if (FAILED(gCtx.device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&frozenOwner->descriptors)))) {
            content->status = PacketContentStatus::UnsupportedContent;
            return content;
        }
        auto descriptor = frozenOwner->descriptors->GetCPUDescriptorHandleForHeapStart();
        for (auto* sourceView : views) {
            const auto read = std::find_if(content->frozenReads.begin(), content->frozenReads.end(),
                [&](const auto& range) { return range.view == sourceView; });
            if (read == content->frozenReads.end() || !read->object) {
                content->status = PacketContentStatus::UnsupportedContent;
                return content;
            }
            auto view = makeFrozenObject();
            if (!view) {
                content->status = PacketContentStatus::UnsupportedContent;
                return content;
            }
            view->kind = Dx12Object::Kind::TextureView;
            view->sourceTexture = read->object;
            view->srvDesc = sourceView->srvDesc;
            if (view->srvDesc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE) {
                if (view->srvDesc.TextureCube.MipLevels == UINT32_MAX)
                    view->srvDesc.TextureCube.MipLevels = read->resource->GetDesc().MipLevels -
                        view->srvDesc.TextureCube.MostDetailedMip;
            } else if (view->srvDesc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D &&
                view->srvDesc.Texture2D.MipLevels == UINT32_MAX) {
                view->srvDesc.Texture2D.MipLevels = read->resource->GetDesc().MipLevels -
                    view->srvDesc.Texture2D.MostDetailedMip;
            }
            view->hasSrvDesc = true;
            view->cpuHandle = descriptor;
            gCtx.device->CreateShaderResourceView(read->resource.Get(), &view->srvDesc, descriptor);
            content->frozenViews.push_back({sourceView, view});
            descriptor.ptr += gCtx.srvInc;
        }
    }
    context->recordingLeases[context->currentListSlot].push_back(content);
    try {
        uint64_t copiedCapacity = 0;
        for (const auto& frozen : content->frozenReads) {
            if (!frozen.immutableOwner && !frozen.cpuWritable &&
                !addPacketContentBudget(copiedCapacity, frozen.subresourceCount)) {
                content->status = PacketContentStatus::UnsupportedContent;
                return content;
            }
        }
        uint64_t copiedBudgetBytes = 0;
        if (copiedCapacity > UINT64_MAX / sizeof(std::pair<ID3D12Resource*, uint32_t>) ||
            !addPacketContentBudget(copiedBudgetBytes,
                copiedCapacity * sizeof(std::pair<ID3D12Resource*, uint32_t>))) {
            content->status = PacketContentStatus::UnsupportedContent;
            return content;
        }
        auto copiedBudget = mcdr::oit_budget::reserve(copiedBudgetBytes, budgetLimit);
        if (copiedCapacity && !copiedBudget) {
            content->status = PacketContentStatus::UnsupportedContent;
            return content;
        }
        std::vector<std::pair<ID3D12Resource*, uint32_t>> copied;
        copied.reserve(static_cast<size_t>(copiedCapacity));
        for (const auto& frozen : content->frozenReads) {
            if (frozen.immutableOwner || frozen.cpuWritable) continue;
            const bool texture = frozen.resource->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER;
            for (uint32_t subresource = frozen.firstSubresource;
                subresource < frozen.firstSubresource + frozen.subresourceCount; ++subresource) {
                const auto key = std::pair{frozen.resource.Get(), subresource};
                if (std::find(copied.begin(), copied.end(), key) != copied.end()) continue;
                copied.push_back(key);
                const auto& source = content->reads[static_cast<size_t>(&frozen - content->frozenReads.data())];
                const uint32_t sourceState = resourceSubresourceState(context, frozen.sourceResource.Get(),
                    subresource, source.initialState);
                if (!transitionResourceRange(context, frozen.sourceResource.Get(), subresource, 1,
                        sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE)) {
                    content->status = PacketContentStatus::StaleRecording;
                    context->recordingFailed.store(true, std::memory_order_release);
                    return content;
                }
                if (texture) {
                    D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
                    sourceLocation.pResource = frozen.sourceResource.Get();
                    sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    sourceLocation.SubresourceIndex = subresource;
                    D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
                    destinationLocation.pResource = frozen.resource.Get();
                    destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    destinationLocation.SubresourceIndex = subresource;
                    context->commandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
                } else {
                    context->commandList->CopyBufferRegion(frozen.resource.Get(), frozen.byteOffset,
                        frozen.sourceResource.Get(), frozen.sourceByteOffset, frozen.byteLength);
                }
                if (!transitionResourceRange(context, frozen.resource.Get(), subresource, 1,
                        D3D12_RESOURCE_STATE_COPY_DEST, frozen.initialState) ||
                    !transitionResourceRange(context, frozen.sourceResource.Get(), subresource, 1,
                        D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState) || context->submissionFailed.load()) {
                    content->status = PacketContentStatus::StaleRecording;
                    context->recordingFailed.store(true, std::memory_order_release);
                    return content;
                }
            }
        }
    } catch (const std::bad_alloc&) {
        content->status = PacketContentStatus::UnsupportedContent;
        return content;
    } catch (...) {
        content->status = PacketContentStatus::StaleRecording;
        context->recordingFailed.store(true, std::memory_order_release);
        return content;
    }
    content->status = PacketContentStatus::Frozen;
    return content;
}

PacketContentStatus packetContentStatus(void* opaqueContext, const std::shared_ptr<const void>& opaqueContent) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto content = std::static_pointer_cast<const PacketContentLease>(opaqueContent);
    if (!context || !content || !context->listOpen || context->submissionFailed.load() || context->recordingFailed.load() ||
        content->recordingIdentity != context->recordingIdentity ||
        content->fence.Get() != context->fence.Get() || content->submission != context->fenceValue + 1)
        return PacketContentStatus::StaleRecording;
    return content->status;
}

mcdr::Result<void*> mapBuffer(Dx12Object* buffer, long long offset, long long length, bool read,
                              bool write) {
    if (!buffer || buffer->kind != Dx12Object::Kind::Buffer) {
        return mcdr::err("mapBuffer", "mapBuffer: invalid buffer handle");
    }
    if (read && !(buffer->usage & 1)) {
        return mcdr::err("mapBuffer", "mapBuffer: buffer lacks MAP_READ");
    }
    if (write && !(buffer->usage & 2)) {
        return mcdr::err("mapBuffer", "mapBuffer: buffer lacks MAP_WRITE");
    }
    if (offset < 0 || length < 0 || offset + length > buffer->size) {
        return mcdr::err("mapBuffer", "mapBuffer: range out of bounds");
    }
    if (buffer->heapType != D3D12_HEAP_TYPE_UPLOAD && buffer->heapType != D3D12_HEAP_TYPE_READBACK) {
        return mcdr::err("mapBuffer",
                         "mapBuffer: buffer is not host-visible (use MAP_WRITE/MAP_READ usage)");
    }

    if (!buffer->mappedPtr) {
        void* base = nullptr;
        HRESULT hr = buffer->resource->Map(0, nullptr, &base);
        if (FAILED(hr)) {
            return mcdr::err("mapBuffer", "Map: " + formatBackendResult(hr));
        }
        buffer->mappedPtr = base;
    }
    return static_cast<char*>(buffer->mappedPtr) + offset;
}

void unmapBuffer(Dx12Object* buffer) {
    if (!buffer || buffer->kind != Dx12Object::Kind::Buffer) return;
    if (buffer->mappedPtr) {
        buffer->resource->Unmap(0, nullptr);
        buffer->mappedPtr = nullptr;
    }
}

namespace {

bool needTransition(D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return false;
    return true;
}

static const char* stateNameFor(D3D12_RESOURCE_STATES s) {
    switch (s) {
        case D3D12_RESOURCE_STATE_COMMON: return "COMMON";
        case D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER: return "VB_CB";
        case D3D12_RESOURCE_STATE_INDEX_BUFFER: return "IB";
        case D3D12_RESOURCE_STATE_RENDER_TARGET: return "RT";
        case D3D12_RESOURCE_STATE_UNORDERED_ACCESS: return "UA";
        case D3D12_RESOURCE_STATE_DEPTH_WRITE: return "DEPTH_W";
        case D3D12_RESOURCE_STATE_DEPTH_READ: return "DEPTH_R";
        case D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE: return "NPSR";
        case D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE: return "PSR";
        case D3D12_RESOURCE_STATE_STREAM_OUT: return "SO";
        case D3D12_RESOURCE_STATE_COPY_DEST: return "COPY_D";
        case D3D12_RESOURCE_STATE_COPY_SOURCE: return "COPY_S";
        case D3D12_RESOURCE_STATE_RESOLVE_DEST: return "RESOLVE_D";
        case D3D12_RESOURCE_STATE_RESOLVE_SOURCE: return "RESOLVE_S";
        case D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT: return "INDIRECT";
        default: return "???";
    }
}

void resourceBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
                     D3D12_RESOURCE_STATES to) {
    if (res == nullptr) {
        dbgLogError("resourceBarrier: null resource (%s -> %s) ignored", stateNameFor(from),
                    stateNameFor(to));
        return;
    }
    if (from == to) {
        DBG_LOG_DEBUG("resourceBarrier: SKIP (from==to=%s) res=%p", stateNameFor(from), (void*)res);
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    DBG_LOG_DEBUG("resourceBarrier: %p %s -> %s", (void*)res, stateNameFor(from), stateNameFor(to));
    list->ResourceBarrier(1, &b);
}

UINT blockSizeFor(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT: return 16;
        case DXGI_FORMAT_R32G32B32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R32G32B32_SINT: return 12;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT: return 8;
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_D32_FLOAT: return 4;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R11G11B10_FLOAT: return 4;
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R8G8_SINT: return 2;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return 8;
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_A8_UNORM: return 1;
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM: return 8;
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB: return 16;
        default: return 4;
    }
}

bool isDepthStencilFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_D16_UNORM || format == DXGI_FORMAT_D24_UNORM_S8_UINT
        || format == DXGI_FORMAT_D32_FLOAT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

UINT alignedPitch(UINT bytes) {
    return (bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u)
        & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
}

UINT blockWidthFor(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB: return 4;
        default: return 1;
    }
}

UINT64 compactRowBytesFor(DXGI_FORMAT format, UINT width) {
    const UINT blockWidth = blockWidthFor(format);
    return ((static_cast<UINT64>(width) + blockWidth - 1) / blockWidth) * blockSizeFor(format);
}

UINT compactRowsFor(DXGI_FORMAT format, UINT height) {
    const UINT blockHeight = blockWidthFor(format);
    return (height + blockHeight - 1) / blockHeight;
}

mcdr::Result<void> flushAndWait(ID3D12CommandList* list, const char* where) {
    dbgLogInfo("flushAndWait: enter");
    ID3D12CommandList* lists[] = {list};
    gCtx.queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    if (FAILED(gCtx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        return mcdr::err(where, "CreateFence failed");
    }
    UINT64 fv = 1;
    if (FAILED(gCtx.queue->Signal(fence.Get(), fv))) {
        return mcdr::err(where, "Signal failed");
    }
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!evt) {
        return mcdr::err(where, "CreateEvent failed");
    }
    while (fence->GetCompletedValue() < fv) {
        fence->SetEventOnCompletion(fv, evt);
        if (WaitForSingleObject(evt, 5000) != WAIT_OBJECT_0) {
            CloseHandle(evt);
            dbgLogError("flushAndWait: TIMEOUT (queue stalled)");
            return mcdr::err(where, "flushAndWait: timed out waiting for queue completion");
        }
    }
    CloseHandle(evt);
    dbgLogInfo("flushAndWait: done");
    return {};
}

}

void transitionTo(CommandContext* ctx, ID3D12Resource* res, D3D12_RESOURCE_STATES fromInitial,
                  D3D12_RESOURCE_STATES to) {
    if (!ctx || !res) return;
    auto& m = ctx->resourceState;
    auto split = ctx->subresourceState.find(res);
    if (split != ctx->subresourceState.end()) {
        const auto baseline = m.contains(res) ? m.at(res) : fromInitial;
        const auto total = resourceSubresourceCount(res);
        if (!total) {
            ctx->submissionFailed.store(true, std::memory_order_release);
            dbgLogError("resource range state reconciliation failed: %s", total.error().message.c_str());
            return;
        }
        for (uint32_t subresource = 0; subresource < *total; ++subresource) {
            const auto state = split->second.contains(subresource) ? split->second.at(subresource) : baseline;
            if (state == to) continue;
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = res;
            barrier.Transition.Subresource = subresource;
            barrier.Transition.StateBefore = state;
            barrier.Transition.StateAfter = to;
            ctx->commandList->ResourceBarrier(1, &barrier);
        }
        ctx->subresourceState.erase(split);
        m[res] = to;
        return;
    }
    D3D12_RESOURCE_STATES from;
    auto it = m.find(res);
    if (it != m.end()) {
        from = it->second;
    } else {
        from = fromInitial;
        if (!needTransition(from, to)) return;
    }
    if (from == to) return;
    resourceBarrier(ctx->commandList, res, from, to);
    m[res] = to;
}

namespace {
mcdr::Result<uint32_t> resourceSubresourceCount(ID3D12Resource* resource) {
    const auto desc = resource->GetDesc();
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return 1u;
    D3D12_FEATURE_DATA_FORMAT_INFO format{desc.Format, 0};
    const auto result = gCtx.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &format, sizeof(format));
    if (FAILED(result) || format.PlaneCount == 0) {
        return mcdr::err("resourceRange", "could not query texture plane count");
    }
    const uint32_t layers = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : desc.DepthOrArraySize;
    const uint64_t count = uint64_t(desc.MipLevels) * layers * format.PlaneCount;
    if (count == 0 || count > UINT32_MAX) return mcdr::err("resourceRange", "invalid subresource count");
    return static_cast<uint32_t>(count);
}

mcdr::Result<void> applyResourceRange(void* rawContext, void* rawResource, uint32_t first,
                                     uint32_t count, uint32_t initialState, uint32_t targetState,
                                     bool adopt) {
    auto* context = static_cast<CommandContext*>(rawContext);
    auto* resource = static_cast<ID3D12Resource*>(rawResource);
    if (!context || !context->listOpen || !context->commandList || !resource || !gCtx.device) {
        return mcdr::err("resourceRange", "an open recording and resource are required");
    }
    const auto total = resourceSubresourceCount(resource);
    if (!total) return std::unexpected(total.error());
    if (count == 0 || first >= *total || count > *total - first) {
        return mcdr::err("resourceRange", "subresource range is out of bounds");
    }
    const auto initial = static_cast<D3D12_RESOURCE_STATES>(initialState);
    const auto target = static_cast<D3D12_RESOURCE_STATES>(targetState);
    auto& uniform = context->resourceState;
    if (first == 0 && count == *total) {
        if (adopt) {
            context->subresourceState.erase(resource);
            uniform[resource] = target;
        } else {
            transitionTo(context, resource, initial, target);
        }
        return {};
    }
    const auto baseline = uniform.contains(resource) ? uniform.at(resource) : initial;
    uniform.try_emplace(resource, baseline);
    auto& states = context->subresourceState[resource];
    for (uint32_t subresource = first; subresource < first + count; ++subresource) {
        const auto before = states.contains(subresource) ? states.at(subresource) : baseline;
        if (!adopt && before != target) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = subresource;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = target;
            context->commandList->ResourceBarrier(1, &barrier);
        }
        if (target == baseline) states.erase(subresource);
        else states[subresource] = target;
    }
    if (states.empty()) context->subresourceState.erase(resource);
    return {};
}
}

mcdr::Result<void> transitionResourceRange(void* context, void* resource, uint32_t first,
                                          uint32_t count, uint32_t initialState, uint32_t state) {
    return applyResourceRange(context, resource, first, count, initialState, state, false);
}

mcdr::Result<void> adoptResourceRangeState(void* context, void* resource, uint32_t first,
                                          uint32_t count, uint32_t initialState, uint32_t state) {
    return applyResourceRange(context, resource, first, count, initialState, state, true);
}

uint32_t resourceSubresourceState(void* rawContext, void* rawResource, uint32_t subresource,
                                 uint32_t initialState) {
    auto* context = static_cast<CommandContext*>(rawContext);
    auto* resource = static_cast<ID3D12Resource*>(rawResource);
    if (!context || !resource) return initialState;
    const auto split = context->subresourceState.find(resource);
    if (split != context->subresourceState.end()) {
        const auto state = split->second.find(subresource);
        if (state != split->second.end()) return static_cast<uint32_t>(state->second);
    }
    const auto uniform = context->resourceState.find(resource);
    return uniform == context->resourceState.end() ? initialState : static_cast<uint32_t>(uniform->second);
}

void transitionBufferTo(CommandContext* ctx, Dx12Object* buf, D3D12_RESOURCE_STATES to) {
    if (!buf || !buf->resource) return;
    if (buf->heapType == D3D12_HEAP_TYPE_UPLOAD || buf->heapType == D3D12_HEAP_TYPE_READBACK) return;
    transitionTo(ctx, buf->resource.Get(), initialStateFor(buf->heapType), to);
}

void transitionTextureTo(CommandContext* ctx, Dx12Object* tex, D3D12_RESOURCE_STATES to) {
    if (!tex || !tex->resource) return;
    transitionTo(ctx, tex->resource.Get(), D3D12_RESOURCE_STATE_COMMON, to);
}

namespace {

mcdr::Result<void> textureSubresourceRanges(std::vector<GraphicsResourceRange>& ranges,
    Dx12Object* texture, uint32_t firstMip, uint32_t mipCount, uint32_t firstLayer,
    uint32_t layerCount, uint32_t firstPlane, uint32_t planeCount, uint32_t state) {
    if (!texture || texture->kind != Dx12Object::Kind::Texture || !texture->resource) {
        return mcdr::err("graphicsRange", "texture resource is invalid");
    }
    const auto description = texture->resource->GetDesc();
    const uint32_t layers = description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
        ? 1u : description.DepthOrArraySize;
    if (texture->texturePlanes == 0) {
        const auto total = resourceSubresourceCount(texture->resource.Get());
        if (!total) return std::unexpected(total.error());
        texture->texturePlanes = *total / (uint32_t(description.MipLevels) * layers);
    }
    const uint32_t planes = texture->texturePlanes;
    if (mipCount == 0 || firstMip >= description.MipLevels || mipCount > description.MipLevels - firstMip ||
        layerCount == 0 || firstLayer >= layers || layerCount > layers - firstLayer ||
        planeCount == 0 || firstPlane >= planes || planeCount > planes - firstPlane) {
        return mcdr::err("graphicsRange", "texture subresource range is invalid");
    }
    for (uint32_t plane = firstPlane; plane < firstPlane + planeCount; ++plane) {
        for (uint32_t layer = firstLayer; layer < firstLayer + layerCount; ++layer) {
            ranges.push_back({texture, firstMip + layer * description.MipLevels +
                plane * description.MipLevels * layers, mipCount, state});
        }
    }
    return {};
}

mcdr::Result<void> addAttachmentRanges(CommandContext* context, Dx12Object* texture, int mip,
                                      uint32_t state, bool depth) {
    const auto description = texture->resource->GetDesc();
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.SampleDesc.Count != 1) {
        return mcdr::err("graphicsRange", "attachment dimension is unsupported");
    }
    const auto total = resourceSubresourceCount(texture->resource.Get());
    if (!total) return std::unexpected(total.error());
    const uint32_t planes = depth ? *total / (uint32_t(description.MipLevels) * description.DepthOrArraySize) : 1u;
    return textureSubresourceRanges(context->activeAttachmentRanges, texture,
        static_cast<uint32_t>(std::clamp(mip, 0, int(description.MipLevels) - 1)), 1,
        0, description.DepthOrArraySize, 0, planes, state);
}

mcdr::Result<void> transitionAttachmentRanges(CommandContext* context, Dx12Object* only,
                                              bool finish) {
    for (const auto& range : context->activeAttachmentRanges) {
        if (only && range.object != only) continue;
        const auto result = transitionResourceRange(context, range.object->resource.Get(),
            range.firstSubresource, range.subresourceCount, D3D12_RESOURCE_STATE_COMMON,
            finish ? D3D12_RESOURCE_STATE_COMMON : range.state);
        if (!result) return result;
    }
    return {};
}

}

mcdr::Result<void> deviceWaitIdle() {
    if (!gCtx.device || !gCtx.queue) {
        return mcdr::err("deviceWaitIdle", "deviceWaitIdle: device not initialized");
    }
    UINT64 fenceValue = 0;
    {
        std::lock_guard<std::mutex> queueLock(gQueueSubmissionMutex);
        const UINT64 completed = gCtx.queueFence->GetCompletedValue();
        if (completed == UINT64_MAX || gCtx.queueFenceValue >= UINT64_MAX - 1) {
            return mcdr::err("deviceWaitIdle", "deviceWaitIdle: device removed or fence exhausted");
        }
        fenceValue = (std::max)(completed, gCtx.queueFenceValue) + 1;
        gCtx.queueFenceValue = fenceValue;
        if (FAILED(gCtx.queue->Signal(gCtx.queueFence.Get(), fenceValue))) {
            return mcdr::err("deviceWaitIdle", "deviceWaitIdle: Signal failed");
        }
    }
    return waitForQueueFenceValue(fenceValue, 5'000'000'000ULL);
}

mcdr::Result<void*> createCommandEncoder() {
    if (auto d = ensureDevice(); !d) return mcdr::err("createCommandEncoder", errorMessage(d.error()));
    DBG_LOG_DEBUG("createCommandEncoder: enter");
    auto ctx = std::make_unique<CommandContext>();
    for (int i = 0; i < 3; ++i) {
        ++gStatEncoderCreate;
        if (FAILED(gCtx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&ctx->allocators[i])))) {
            return mcdr::err("createCommandEncoder", "createCommandEncoder: CreateCommandAllocator failed");
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (FAILED(gCtx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ctx->allocators[i].Get(),
                                                  nullptr, IID_PPV_ARGS(&ctx->commandLists[i])))) {
            return mcdr::err("createCommandEncoder", "createCommandEncoder: CreateCommandList failed");
        }
        const HRESULT workGraphInterface = ctx->commandLists[i].As(&ctx->workGraphCommandLists[i]);
        if (FAILED(workGraphInterface) && (workGraphInterface != E_NOINTERFACE ||
            gCtx.caps.workGraphsTier >= D3D12_WORK_GRAPHS_TIER_1_0)) {
            return mcdr::err("createCommandEncoder", "Work Graph command list interface is unavailable");
        }
        ctx->commandLists[i]->Close();
    }
    ctx->currentListSlot = 0;
    ctx->commandList = ctx->commandLists[0].Get();
    ctx->workGraphCommandList = ctx->workGraphCommandLists[0].Get();
    if (FAILED(gCtx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ctx->fence)))) {
        return mcdr::err("createCommandEncoder", "createCommandEncoder: CreateFence failed");
    }
    ctx->fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ctx->fenceEvent) {
        return mcdr::err("createCommandEncoder", "createCommandEncoder: CreateEvent failed");
    }
    {
        std::lock_guard<std::mutex> lock(gRecordingContextsMutex);
        for (const auto& list : ctx->commandLists) gRecordingContexts.emplace(list.Get(), ctx.get());
    }
    dbgLogInfo("createCommandEncoder: done ctx=%p", reinterpret_cast<void*>(ctx.get()));
    return static_cast<void*>(ctx.release());
}

int validateCommandContextControl(void* opaqueContext) noexcept {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->commandList) return static_cast<int>(CommandContextControlStatus::Invalid);
    if (context->submissionFailed.load()) return static_cast<int>(CommandContextControlStatus::SubmissionUnknown);
    if (context->inRenderPass || gAsyncRenderCtx.load(std::memory_order_acquire) == context)
        return static_cast<int>(CommandContextControlStatus::Busy);
    return static_cast<int>(CommandContextControlStatus::Ready);
}

int destroyCommandEncoderChecked(CommandContext* ctx) {
    if (!ctx) return MCDR_COMMAND_DESTROY_INVALID;
    std::lock_guard submissionLock(gAsyncSubmissionMutex);
    if (ctx->submissionFailed.load(std::memory_order_acquire)) return MCDR_COMMAND_DESTROY_SUBMISSION_UNKNOWN;
    if (gAsyncRenderCtx.load(std::memory_order_acquire) == ctx) return MCDR_COMMAND_DESTROY_OWNERSHIP_PENDING;
    if (fgDisplayFrameQueued(ctx) && !waitForPendingPresents(2000))
        return MCDR_COMMAND_DESTROY_OWNERSHIP_PENDING;
    fgAbortDisplayFrame(ctx);
    DBG_LOG_DEBUG("destroyCommandEncoder: enter fenceValue=%llu", (unsigned long long)ctx->fenceValue);
    if (ctx->fenceValue > 0) {
        auto w = waitForFenceValue(ctx, ctx->fenceValue, 5000000000ULL);
        if (!w) {
            dbgLogError("destroyCommandEncoder: wait FAILED: %s", errorMessage(w.error()).c_str());
            return gCtx.device && SUCCEEDED(gCtx.device->GetDeviceRemovedReason())
                ? MCDR_COMMAND_DESTROY_OWNERSHIP_PENDING : MCDR_COMMAND_DESTROY_SUBMISSION_UNKNOWN;
        }
    }
    if (ctx->listOpen) {
        ctx->listOpen = 0;
        if (gOpenListCount > 0) --gOpenListCount;
        dbgLog("destroyCommandEncoder: openList now=%d", gOpenListCount);
        if (gOpenListCount == 0) flushPendingDeletes();
    }
    if (ctx->fenceEvent) CloseHandle(ctx->fenceEvent);
    {
        std::lock_guard<std::mutex> lock(gRecordingContextsMutex);
        for (const auto& list : ctx->commandLists) gRecordingContexts.erase(list.Get());
    }
    ctx->commandList = nullptr;
    ctx->workGraphCommandList = nullptr;
    for (auto& list : ctx->commandLists) list.Reset();
    for (auto& list : ctx->workGraphCommandLists) list.Reset();
    if (ctx->recordingDiscarded) ctx->recordingDiscarded->store(true, std::memory_order_release);
    delete ctx;
    dbgLogInfo("destroyCommandEncoder: done ctx=%p", reinterpret_cast<void*>(ctx));
    return MCDR_COMMAND_DESTROY_DESTROYED;
}

void destroyCommandEncoder(CommandContext* ctx) {
    if (!ctx) return;
    const int result = destroyCommandEncoderChecked(ctx);
    if (result != MCDR_COMMAND_DESTROY_DESTROYED)
        dbgLogError("destroyCommandEncoder: retained context, outcome=%d", result);
}

int commandRecordingStatus(void* opaqueContext) noexcept {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->commandList) return MCDR_COMMAND_OUTCOME_INVALID;
    if (context->submissionFailed.load(std::memory_order_acquire)) return MCDR_COMMAND_OUTCOME_SUBMISSION_UNKNOWN;
    if (gAsyncRenderCtx.load(std::memory_order_acquire) == context) return MCDR_COMMAND_OUTCOME_OWNERSHIP_PENDING;
    return context->recordingFailed.load(std::memory_order_acquire)
        ? MCDR_COMMAND_OUTCOME_RECORDING_REJECTED : MCDR_COMMAND_OUTCOME_READY;
}

mcdr::Result<void> beginCommandList(CommandContext* ctx) {
    if (!ctx) {
        return mcdr::err("beginCommandList", "beginCommandList: null ctx");
    }
    if (gAsyncRenderCtx.load(std::memory_order_acquire) == ctx || ctx->submissionFailed) {
        return mcdr::err("beginCommandList", "beginCommandList: command ownership remains active");
    }
    if (ctx->listOpen) return {};
    UINT slot = static_cast<UINT>(ctx->fenceValue % 3u);
    ctx->currentListSlot = slot;
    ctx->commandList = ctx->commandLists[slot].Get();
    ctx->workGraphCommandList = ctx->workGraphCommandLists[slot].Get();
    static const bool sCmdRing = [] {
        const char* v = getenv("DX12_CMD_RING");
        return !(v && v[0] == '0');
    }();
    UINT64 waitVal = sCmdRing ? ctx->slotFence[slot] : ctx->fenceValue;
    return beginCommandListWithWait(ctx, waitVal);
}

namespace {

const char* shadingRateName(int rate) {
    switch (rate) {
        case static_cast<int>(D3D12_SHADING_RATE_1X1): return "1X1";
        case static_cast<int>(D3D12_SHADING_RATE_1X2): return "1X2";
        case static_cast<int>(D3D12_SHADING_RATE_2X1): return "2X1";
        case static_cast<int>(D3D12_SHADING_RATE_2X2): return "2X2";
        case static_cast<int>(D3D12_SHADING_RATE_2X4): return "2X4";
        case static_cast<int>(D3D12_SHADING_RATE_4X2): return "4X2";
        case static_cast<int>(D3D12_SHADING_RATE_4X4): return "4X4";
        default: return "unknown";
    }
}

bool isKnownShadingRate(int rate) {
    switch (rate) {
        case static_cast<int>(D3D12_SHADING_RATE_1X1):
        case static_cast<int>(D3D12_SHADING_RATE_1X2):
        case static_cast<int>(D3D12_SHADING_RATE_2X1):
        case static_cast<int>(D3D12_SHADING_RATE_2X2):
        case static_cast<int>(D3D12_SHADING_RATE_2X4):
        case static_cast<int>(D3D12_SHADING_RATE_4X2):
        case static_cast<int>(D3D12_SHADING_RATE_4X4):
            return true;
        default:
            return false;
    }
}

const char* shadingRateUnsupportedReason(int rate) {
    if (!gCtx.caps.shadingRateSupported()) {
        return "the device reports VRS tier 0";
    }
    if (!gCtx.caps.additionalShadingRates &&
        (rate == static_cast<int>(D3D12_SHADING_RATE_2X4) ||
         rate == static_cast<int>(D3D12_SHADING_RATE_4X2) ||
         rate == static_cast<int>(D3D12_SHADING_RATE_4X4))) {
        return "the device reports AdditionalShadingRatesSupported = false";
    }
    return nullptr;
}

const char* shadingRateCombinerName(int combiner) {
    switch (combiner) {
        case static_cast<int>(D3D12_SHADING_RATE_COMBINER_PASSTHROUGH): return "PASSTHROUGH";
        case static_cast<int>(D3D12_SHADING_RATE_COMBINER_OVERRIDE): return "OVERRIDE";
        case static_cast<int>(D3D12_SHADING_RATE_COMBINER_MIN): return "MIN";
        case static_cast<int>(D3D12_SHADING_RATE_COMBINER_MAX): return "MAX";
        case static_cast<int>(D3D12_SHADING_RATE_COMBINER_SUM): return "SUM";
        default: return "unknown";
    }
}

bool issueShadingRate(ID3D12GraphicsCommandList* list, int rate) {
    if (!list || rate < 0) {
        return false;
    }
    if (const char* why = shadingRateUnsupportedReason(rate)) {
        if (!gCtx.shadingRateUnsupportedLogged) {
            gCtx.shadingRateUnsupportedLogged = true;
            dbgLogWarn("VRS: shading rate %s not applied: %s; staying at 1X1", shadingRateName(rate), why);
        }
        return false;
    }
    ComPtr<ID3D12GraphicsCommandList5> list5;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&list5))) || !list5) {
        if (!gCtx.shadingRateList5Logged) {
            gCtx.shadingRateList5Logged = true;
            dbgLogWarn("VRS: ID3D12GraphicsCommandList5 unavailable; shading rate %s not applied",
                       shadingRateName(rate));
        }
        return false;
    }
    const D3D12_SHADING_RATE_COMBINER combiners[2] = {
        static_cast<D3D12_SHADING_RATE_COMBINER>(gCtx.shadingRateCombiner0),
        static_cast<D3D12_SHADING_RATE_COMBINER>(gCtx.shadingRateCombiner1)};
    list5->RSSetShadingRate(static_cast<D3D12_SHADING_RATE>(rate), combiners);
    ++gCtx.shadingRateAppliedLists;
    if (gCtx.shadingRateAppliedLists == 1) {
        dbgLogInfo("VRS: shading rate %s applied via RSSetShadingRate (combiners %s/%s)", shadingRateName(rate),
                   shadingRateCombinerName(gCtx.shadingRateCombiner0),
                   shadingRateCombinerName(gCtx.shadingRateCombiner1));
    }
    return true;
}

}

int setShadingRate(int rate) {
    if (rate >= 0 && !isKnownShadingRate(rate)) {
        dbgLogError("VRS: unknown shading rate 0x%X (expected 0x0/0x1/0x4/0x5/0x6/0x9/0xA, or -1 off)",
                    rate);
        return -1;
    }
    if (rate >= 0 && !gCtx.caps.queried) {
        gCtx.requestedShadingRate = rate;
        dbgLogInfo("VRS: shading rate %s requested before the device exists; it will be gated on the first "
                   "scoped draw batch",
                   shadingRateName(rate));
        return 1;
    }
    if (rate >= 0) {
        if (const char* why = shadingRateUnsupportedReason(rate)) {
            dbgLogWarn("VRS: shading rate %s refused: %s; keeping the default 1X1", shadingRateName(rate), why);
            return 0;
        }
    }
    gCtx.requestedShadingRate = (rate < 0) ? -1 : rate;
    gCtx.shadingRateUnsupportedLogged = false;
    if (gCtx.requestedShadingRate < 0) {
        dbgLogInfo("VRS: shading rate control off; device keeps its default 1X1");
        return 1;
    }
    dbgLogInfo("VRS: shading rate %s set (tier=%d imageTileSize=%u additionalRates=%d) — it is the coarse "
               "rate ceiling for the whitelisted draw batches (no longer replayed per list)",
               shadingRateName(rate), static_cast<int>(gCtx.caps.shadingRateTier),
               gCtx.caps.shadingRateImageTileSize, gCtx.caps.additionalShadingRates ? 1 : 0);
    return 1;
}

int setShadingRateNow(CommandContext* ctx, int rate) {
    if (!isKnownShadingRate(rate)) {
        dbgLogError("VRS: setShadingRateNow got unknown rate 0x%X (expected 0x0/0x1/0x4/0x5/0x6/0x9/0xA)",
                    rate);
        return -1;
    }
    if (!ctx || !ctx->commandList || !ctx->listOpen) {
        dbgLogWarn("VRS: setShadingRateNow(%s) without an open command list; ignored", shadingRateName(rate));
        return -2;
    }
    if (!issueShadingRate(ctx->commandList, rate)) return 0;
    ctx->recordedShadingRate = static_cast<D3D12_SHADING_RATE>(rate);
    ctx->recordedShadingCombiners[0] = static_cast<D3D12_SHADING_RATE_COMBINER>(gCtx.shadingRateCombiner0);
    ctx->recordedShadingCombiners[1] = static_cast<D3D12_SHADING_RATE_COMBINER>(gCtx.shadingRateCombiner1);
    return 1;
}

int setShadingRateCombiners(int first, int second) {
    const auto valid = [](int combiner) {
        return combiner >= static_cast<int>(D3D12_SHADING_RATE_COMBINER_PASSTHROUGH) &&
               combiner <= static_cast<int>(D3D12_SHADING_RATE_COMBINER_SUM);
    };
    if (!valid(first) || !valid(second)) {
        dbgLogError("VRS: bad shading-rate combiners %d/%d (expected 0..4, PASSTHROUGH=0 OVERRIDE=1 MIN=2 "
                    "MAX=3 SUM=4)",
                    first, second);
        return -1;
    }
    gCtx.shadingRateCombiner0 = first;
    gCtx.shadingRateCombiner1 = second;
    dbgLogInfo("VRS: combiners set to %s / %s", shadingRateCombinerName(first), shadingRateCombinerName(second));
    return 1;
}

mcdr::Result<void> beginCommandListWithWait(CommandContext* ctx, UINT64 waitForValue) {
    if (!ctx) {
        return mcdr::err("beginCommandListWithWait", "beginCommandListWithWait: null ctx");
    }
    if (gAsyncRenderCtx.load(std::memory_order_acquire) == ctx || ctx->submissionFailed) {
        return mcdr::err("beginCommandListWithWait", "beginCommandListWithWait: command ownership remains active");
    }
    waitForValue = (std::max)(waitForValue, ctx->slotFence[ctx->currentListSlot]);
    if (nativeLogLevel() >= 3) dumpInfoQueueMessages();
    DBG_LOG_DEBUG("beginCommandListWithWait: fenceValue=%llu waitValue=%llu",
                  (unsigned long long)ctx->fenceValue, (unsigned long long)waitForValue);
    if (waitForValue > 0) {
        UINT64 cv = 0ULL;
        if (gCtx.queueFence) cv = gCtx.queueFence->GetCompletedValue();
        dbgLogDebug("beginCommandListWithWait: checking qf=%llu cv=%llu qfVal=%llu",
                    static_cast<unsigned long long>(waitForValue), cv,
                    static_cast<unsigned long long>(gCtx.queueFenceValue));
    }
    if (waitForValue > 0) {
        auto wl = waitForFenceValue(ctx, waitForValue, 5'000'000'000ULL);
        if (!wl) {
            dbgLogError("beginCommandListWithWait: per-list fence wait FAILED: %s",
                        errorMessage(wl.error()).c_str());
            return mcdr::err("beginCommandListWithWait", errorMessage(wl.error()));
        }
        dbgLogDebug("beginCommandListWithWait: per-list fence wait done val=%llu",
                    static_cast<unsigned long long>(waitForValue));
    }
    auto recordingDiscarded = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<const void> recordingIdentity = recordingDiscarded;
    dbgLogDebug("beginCommandListWithWait: begin allocator reset");
    ctx->controlIdentity.reset();
    HRESULT hr = ctx->currentAllocator()->Reset();
    if (FAILED(hr)) {
        return mcdr::err("beginCommandListWithWait", "beginCommandListWithWait: allocator Reset " + formatBackendResult(hr));
    }
    dbgLogDebug("beginCommandListWithWait: allocator reset ok");
    dbgLogDebug("beginCommandListWithWait: begin list reset");
    hr = ctx->commandList->Reset(ctx->currentAllocator().Get(), nullptr);
    if (FAILED(hr)) {
        return mcdr::err("beginCommandListWithWait", "beginCommandListWithWait: list Reset " + formatBackendResult(hr));
    }
    dbgLogDebug("beginCommandListWithWait: list reset ok");
    if (ctx->recordingDiscarded) ctx->recordingDiscarded->store(true, std::memory_order_release);
    const auto completed = ctx->fence->GetCompletedValue();
    for (UINT slot = 0; slot < std::size(ctx->recordingLeases); ++slot) {
        if (slot == ctx->currentListSlot ||
            (completed != UINT64_MAX && ctx->slotFence[slot] && ctx->slotFence[slot] <= completed))
            ctx->recordingLeases[slot].clear();
    }
    ctx->recordingIdentity = std::move(recordingIdentity);
    ctx->recordingDiscarded = std::move(recordingDiscarded);
    ctx->recordingFailed.store(false, std::memory_order_release);
    ctx->graphicsCommandObserver = {};
    ctx->retainedGraphicsPipelines.clear();
    ctx->retainedRecordingObjects.clear();
    ctx->resourceDrawHeap.Reset();
    ctx->resourceDrawCapacity = 0;
    ctx->resourceComputeHeap.Reset();
    ctx->resourceComputeCapacity = 0;
    ctx->resourceComputeNext = 0;
    dbgLogDebug("beginCommandListWithWait: pre-increment ctx=%p", reinterpret_cast<void*>(ctx));
    ctx->listOpen = 1;
    ctx->inRenderPass = 0;
    ctx->activePassRtvs.clear();
    ctx->activePassDsv = {};
    ctx->activeMotionRtv = {};
    ctx->activeMotionTarget = nullptr;
    ctx->rasterMotionSelected = false;
    ctx->activePassBoundColorCount = UINT_MAX;
    ctx->rasterMotionWorldColor = nullptr;
    ctx->rasterMotionWorldDepth = nullptr;
    ctx->rasterMotionTarget = nullptr;
    ctx->rasterMotionLeases.clear();
    ctx->colorTargetsWritten = false;
    ++gOpenListCount;
    dbgLogDebug("beginCommandListWithWait: post-increment openList=%d ctx=%p", gOpenListCount,
                reinterpret_cast<void*>(ctx));
    ctx->resourceState.clear();
    ctx->subresourceState.clear();
    ctx->drawHeapSlotBase = static_cast<UINT>(ctx->fenceValue % kDrawHeapSections) * kDrawHeapPerFrame;
    ctx->nextDrawSlot = 0;
    ctx->lastSrvSlotOffset = 0;
    ctx->lastSrvCount = 0;
    ctx->lastSrvCbvCount = 0;
    ctx->lastSrvViews.clear();
    gNextFrameRtv = 0;
    gNextDsv = 0;
    ++gFrameViewGeneration;
    ctx->rtvCache.clear();
    ctx->dsvCache.clear();
    ctx->cachedPso = nullptr;
    ctx->currentPipeline = nullptr;
    invalidateGraphicsInputBindings(ctx, true);
    ctx->graphicsDrawState = {};
    ctx->graphicsInputRanges.clear();
    ctx->recordedShadingRate = D3D12_SHADING_RATE_1X1;
    ctx->recordedShadingCombiners[0] = D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
    ctx->recordedShadingCombiners[1] = D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
    ctx->shadingRateImageBound = nullptr;
    ctx->captureShadingRate = {};
    ctx->graphicsCapture = {};
    ctx->oitFrameState.reset();
    ctx->activeAttachmentRanges.clear();
    ID3D12DescriptorHeap* drawHeaps[] = {ctx->resourceDrawHeap ? ctx->resourceDrawHeap.Get() : gCtx.drawHeap.Get()};
    ctx->commandList->SetDescriptorHeaps(1, drawHeaps);
    return {};
}

void restoreDrawHeap(CommandContext* ctx) {
    if (ctx == nullptr || ctx->commandList == nullptr || (!ctx->resourceDrawHeap && !gCtx.drawHeap)) {
        return;
    }
    ID3D12DescriptorHeap* drawHeaps[] = {ctx->resourceDrawHeap ? ctx->resourceDrawHeap.Get() : gCtx.drawHeap.Get()};
    ctx->commandList->SetDescriptorHeaps(1, drawHeaps);
    ctx->cachedPso = nullptr;
    ctx->lastSrvCount = 0;
    ctx->lastSrvCbvCount = 0;
    ctx->lastSrvViews.clear();
    invalidateGraphicsInputBindings(ctx, false);
}

mcdr::Result<void> endCommandList(CommandContext* ctx) {
    if (!ctx) {
        return mcdr::err("endCommandList", "endCommandList: null ctx");
    }
    if (!ctx->listOpen) return {};
    while (!ctx->subresourceState.empty()) {
        auto* resource = ctx->subresourceState.begin()->first;
        const auto baseline = ctx->resourceState.at(resource);
        transitionTo(ctx, resource, baseline, baseline);
    }
    for (auto it = ctx->resourceState.begin(); it != ctx->resourceState.end();) {
        if (it->second
            & (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) {
            if (it->second != D3D12_RESOURCE_STATE_COMMON)
                resourceBarrier(ctx->commandList, it->first, it->second, D3D12_RESOURCE_STATE_COMMON);
            it = ctx->resourceState.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = ctx->resourceState.begin(); it != ctx->resourceState.end();) {
        D3D12_RESOURCE_STATES target = D3D12_RESOURCE_STATE_COMMON;
        if (it->second == target) {
            it = ctx->resourceState.erase(it);
        } else {
            resourceBarrier(ctx->commandList, it->first, it->second, target);
            it = ctx->resourceState.erase(it);
        }
    }
    HRESULT hr = ctx->commandList->Close();
    if (FAILED(hr)) {
        fgAbortDisplayFrame(ctx);
        dumpInfoQueueMessages();
        return mcdr::err("endCommandList",
                         "endCommandList: Close " + formatBackendResult(hr) + " — " + deviceStatusText());
    }
    ctx->listOpen = 0;
    if (gOpenListCount > 0) --gOpenListCount;
    dbgLogDebug("endCommandList: openList now=%d ctx=%p", gOpenListCount, reinterpret_cast<void*>(ctx));
    return {};
}

static void logMemoryClass(uint64_t frame) {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        return;
    }
    const unsigned long long privMB = static_cast<unsigned long long>(pmc.PrivateUsage) >> 20;
    const unsigned long long wsMB = static_cast<unsigned long long>(pmc.WorkingSetSize) >> 20;
    DXGI_QUERY_VIDEO_MEMORY_INFO localInfo{};
    DXGI_QUERY_VIDEO_MEMORY_INFO nonLocalInfo{};
    bool haveVid = false;
    if (gCtx.adapter) {
        ComPtr<IDXGIAdapter3> adapter3;
        if (SUCCEEDED(gCtx.adapter.As(&adapter3)) && adapter3) {
            haveVid =
                SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localInfo)) &&
                SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocalInfo));
        }
    }
    if (haveVid) {
        dbgLogInfo("frame=%llu private=%lluMB ws=%lluMB local=%lluMB/%lluMB nonLocal=%lluMB/%lluMB liveTex=%lld liveBuf=%lld bufTotal=%llu upMB=%lld defMB=%lld rbMB=%lld pd=%lld",
                   static_cast<unsigned long long>(frame), privMB, wsMB,
                   static_cast<unsigned long long>(localInfo.CurrentUsage >> 20),
                   static_cast<unsigned long long>(localInfo.Budget >> 20),
                   static_cast<unsigned long long>(nonLocalInfo.CurrentUsage >> 20),
                   static_cast<unsigned long long>(nonLocalInfo.Budget >> 20),
                   static_cast<long long>(gLiveTex.load()), static_cast<long long>(gLiveBuf.load()),
                   static_cast<unsigned long long>(gStatBufCreate.load()),
                   static_cast<long long>(gLiveUploadBytes.load() >> 20),
                   static_cast<long long>(gLiveDefaultBytes.load() >> 20),
                   static_cast<long long>(gLiveReadbackBytes.load() >> 20),
                   static_cast<long long>(pendingDeletesCount()));
    } else {
        dbgLogInfo("frame=%llu private=%lluMB ws=%lluMB (video memory query unavailable)",
                   static_cast<unsigned long long>(frame), privMB, wsMB);
    }
}
namespace {
mcdr::Result<UINT64> submitRecordedCommandList(CommandContext* ctx) {
    if (!ctx) {
        return mcdr::err("submitCommandList", "submitCommandList: null ctx");
    }
    if (ctx->submissionFailed) {
        invalidatePreparedReconstructionHistory();
        fgAbortDisplayFrame(ctx);
        return mcdr::err("submitCommandList", "submitCommandList: previous submission failed");
    }
    if (ctx->recordingFailed.load(std::memory_order_acquire)) {
        invalidatePreparedReconstructionHistory();
        fgAbortDisplayFrame(ctx);
        return mcdr::err("submitCommandList", "submitCommandList: recording failed; discard the command list");
    }
    if (!ctx->listOpen) {
        fgAbortDisplayFrame(ctx);
        dbgLogDebug("submitCommandList: no open list — skip execute (empty frame) ctx=%p",
                    reinterpret_cast<void*>(ctx));
        return ctx->fenceValue;
    }
    if (auto e = endCommandList(ctx); !e) {
        if (gCtx.device && FAILED(gCtx.device->GetDeviceRemovedReason()))
            ctx->submissionFailed.store(true, std::memory_order_release);
        else
            ctx->recordingFailed.store(true, std::memory_order_release);
        invalidatePreparedReconstructionHistory();
        fgAbortDisplayFrame(ctx);
        return mcdr::err("submitCommandList", errorMessage(e.error()));
    }
    UINT64 value = ctx->fenceValue + 1;
    if ((value % 300ULL) == 1) {
        logMemoryClass(value);
    }
    DBG_LOG_DEBUG("submit: ExecuteCommandLists enter (v->%llu)", (unsigned long long)value);
    ID3D12CommandList* lists[] = {ctx->commandList};
    ctx->submissionFailed = true;
    ctx->recordingDiscarded.reset();
    gCtx.queue->ExecuteCommandLists(1, lists);
    value = ++ctx->fenceValue;
    ctx->slotFence[ctx->currentListSlot] = value;
    DBG_LOG_DEBUG("submit: executed, Signal v=%llu", (unsigned long long)value);
    if (FAILED(gCtx.queue->Signal(ctx->fence.Get(), value))) {
        invalidatePreparedReconstructionHistory();
        fgAbortDisplayFrame(ctx);
        return mcdr::err("submitCommandList", "submitCommandList: Signal failed");
    }
    {
        UINT64 qv = ++gCtx.queueFenceValue;
        if (FAILED(gCtx.queue->Signal(gCtx.queueFence.Get(), qv))) {
            invalidatePreparedReconstructionHistory();
            fgAbortDisplayFrame(ctx);
            return mcdr::err("submitCommandList", "submitCommandList: Signal(queue) failed");
        }
        ctx->lastSubmitQueueFence = qv;
        ctx->prevFenceValue = value;
        dbgLogDebug("renderThread: submitCommandList done ctx=%p value=%llu qf=%llu lastQF=%llu",
                    reinterpret_cast<void*>(ctx), static_cast<unsigned long long>(value),
                    static_cast<unsigned long long>(gCtx.queueFenceValue),
                    static_cast<unsigned long long>(ctx->lastSubmitQueueFence));
    }
    DBG_LOG_DEBUG("submit: done v=%llu qf=%llu", (unsigned long long)value,
                  (unsigned long long)gCtx.queueFenceValue);
    ctx->submissionFailed = false;
    fgNoteRecordingSubmitted(ctx, value);
    return value;
}
}

mcdr::Result<UINT64> submitCommandList(CommandContext* ctx) {
    std::lock_guard<std::mutex> queueLock(gQueueSubmissionMutex);
    if (!ctx || !ctx->listOpen) return submitRecordedCommandList(ctx);
    const auto submitted = submitRecordedCommandList(ctx);
    if (!submitted) return submitted;
    auto* surface = getActiveSurface();
    if (surface) {
        const int index = surface->currentImageIndex;
        if (index >= 0 && index < static_cast<int>(kSurfaceBufferCount)) {
            if (surface->surfaceFences.size() < static_cast<size_t>(kSurfaceBufferCount))
                surface->surfaceFences.resize(kSurfaceBufferCount, 0);
            surface->surfaceFences[static_cast<size_t>(index)] = ctx->lastSubmitQueueFence;
        }
    }
    const auto queued = fgQueueDisplayFrame(ctx, *submitted);
    if (!queued) {
        fgAbortDisplayFrame(ctx);
        return mcdr::err("submitCommandList", errorMessage(queued.error()));
    }
    const auto ticket = *queued;
    if (surface && surface->blittedThisFrame.load(std::memory_order_acquire) && ticket.serial != 0)
        surface->frameGenerationTicket = ticket;
    return submitted;
}

int submitCommandListV1(CommandContext* context, uint64_t* submittedFence) {
    if (!submittedFence) return MCDR_COMMAND_OUTCOME_INVALID;
    *submittedFence = 0;
    std::lock_guard submissionLock(gAsyncSubmissionMutex);
    const int status = commandRecordingStatus(context);
    if (status != MCDR_COMMAND_OUTCOME_READY) return status;
    if (!context->listOpen) return MCDR_COMMAND_OUTCOME_READY;
    const auto submitted = submitCommandList(context);
    if (!submitted) {
        dbgLogError("submitCommandListV1: %s", errorMessage(submitted.error()).c_str());
        if (context->recordingFailed.load(std::memory_order_acquire) &&
            !context->submissionFailed.load(std::memory_order_acquire))
            return MCDR_COMMAND_OUTCOME_RECORDING_REJECTED;
        context->submissionFailed.store(true, std::memory_order_release);
        return MCDR_COMMAND_OUTCOME_SUBMISSION_UNKNOWN;
    }
    *submittedFence = *submitted;
    return MCDR_COMMAND_OUTCOME_SUBMITTED;
}

int submitDisplayGuidesAndResume(void* opaqueContext, uint64_t* guideFence) {
    if (guideFence == nullptr) return -1;
    *guideFence = 0;
    auto* ctx = static_cast<CommandContext*>(opaqueContext);
    if (!ctx || !ctx->listOpen || !ctx->commandList || ctx->inRenderPass ||
        ctx->submissionFailed.load() || gAsyncRenderCtx.load(std::memory_order_acquire) == ctx) return -1;
    const auto ticket = fgDisplayGuideTicket(ctx);
    if (ticket.serial == 0) return -1;
    const bool colorTargetsWritten = ctx->colorTargetsWritten;
    const auto submitted = [&] {
        std::lock_guard<std::mutex> queueLock(gQueueSubmissionMutex);
        return submitRecordedCommandList(ctx);
    }();
    if (!submitted) return -1;
    *guideFence = *submitted;
    if (auto resumed = beginCommandList(ctx); !resumed) {
        dbgLogError("display guides: recording resume failed: %s", errorMessage(resumed.error()).c_str());
        invalidatePreparedReconstructionHistory();
        fgAbortDisplayFrame(ctx);
        return -1;
    }
    ctx->colorTargetsWritten = colorTargetsWritten;
    if (fgDisplayGuideTicket(ctx) != ticket) {
        invalidatePreparedReconstructionHistory();
        fgAbortDisplayFrame(ctx);
        return -1;
    }
    return 1;
}

mcdr::Result<void> waitForFenceValue(CommandContext* ctx, UINT64 value, UINT64 timeoutNs) {
    return waitForContextFenceValue(ctx, value, timeoutNs);
}

UINT64 currentFenceValue(CommandContext* ctx) { return ctx ? ctx->fenceValue : 0; }

mcdr::Result<void> waitForQueueFenceValue(UINT64 value, UINT64 timeoutNs) {
    if (!gCtx.queueFence) {
        return mcdr::err("waitForQueueFenceValue", "waitForQueueFenceValue: queue fence not initialized");
    }
    UINT64 cv = gCtx.queueFence->GetCompletedValue();
    if (cv == UINT64_MAX) {
        return mcdr::err("waitForQueueFenceValue", "waitForQueueFenceValue: device removed");
    }
    dbgLogDebug("waitQFence: value=%llu completed=%llu", static_cast<unsigned long long>(value),
                static_cast<unsigned long long>(cv));
    if (cv >= value) return {};
    if (timeoutNs == 0) {
        return mcdr::err("waitForQueueFenceValue", "waitForQueueFenceValue: timed out after 0ns");
    }
    static thread_local HANDLE t_queueFenceEvent = nullptr;
    if (!t_queueFenceEvent) {
        t_queueFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!t_queueFenceEvent) {
            return mcdr::err("waitForQueueFenceValue", "waitForQueueFenceValue: CreateEvent failed");
        }
    }
    UINT64 ms64 = timeoutNs / 1000000ULL + (timeoutNs % 1000000ULL != 0);
    DWORD ms = ms64 > 0xFFFFFFF0ULL ? 0xFFFFFFF0ULL : static_cast<DWORD>(ms64);
    const ULONGLONG waitStarted = GetTickCount64();
    for (;;) {
        HRESULT hr = gCtx.queueFence->SetEventOnCompletion(value, t_queueFenceEvent);
        if (FAILED(hr)) {
            return mcdr::err("waitForQueueFenceValue",
                             "waitForQueueFenceValue: SetEventOnCompletion " + formatBackendResult(hr));
        }
        const ULONGLONG elapsed = GetTickCount64() - waitStarted;
        DWORD remaining = elapsed >= ms ? 0 : ms - static_cast<DWORD>(elapsed);
        DWORD wr = WaitForSingleObject(t_queueFenceEvent, remaining);
        cv = gCtx.queueFence->GetCompletedValue();
        if (cv == UINT64_MAX) {
            return mcdr::err("waitForQueueFenceValue", "waitForQueueFenceValue: device removed");
        }
        if (cv >= value) {
            DBG_LOG_DEBUG("waitQFence: OK value=%llu", (unsigned long long)value);
            return {};
        }
        if (wr != WAIT_OBJECT_0 || GetTickCount64() - waitStarted >= ms) {
            dbgLogError("waitQFence: TIMEOUT value=%llu completed=%llu", static_cast<unsigned long long>(value),
                   static_cast<unsigned long long>(gCtx.queueFence->GetCompletedValue()));
            return mcdr::err("waitForQueueFenceValue",
                             "waitForQueueFenceValue: timed out after " + std::to_string(timeoutNs) + "ns");
        }
    }
}

mcdr::Result<void> waitForContextFenceValue(CommandContext* ctx, UINT64 value, UINT64 timeoutNs) {
    if (!ctx || !ctx->fence) {
        return mcdr::err("waitForContextFenceValue", "waitForContextFenceValue: null ctx or fence");
    }
    UINT64 cv = ctx->fence->GetCompletedValue();
    if (cv == UINT64_MAX || ctx->submissionFailed) {
        return mcdr::err("waitForContextFenceValue", "waitForContextFenceValue: submission failed or device removed");
    }
    dbgLogDebug("waitCtxFence: value=%llu completed=%llu ctx=%p", static_cast<unsigned long long>(value),
                static_cast<unsigned long long>(cv), reinterpret_cast<void*>(ctx));
    if (cv >= value) return {};
    if (timeoutNs == 0) {
        return mcdr::err("waitForContextFenceValue", "waitForContextFenceValue: timed out after 0ns");
    }
    static thread_local HANDLE t_ctxFenceEvent = nullptr;
    if (!t_ctxFenceEvent) {
        t_ctxFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!t_ctxFenceEvent) {
            return mcdr::err("waitForContextFenceValue", "waitForContextFenceValue: CreateEvent failed");
        }
    }
    UINT64 ms64 = timeoutNs / 1000000ULL + (timeoutNs % 1000000ULL != 0);
    DWORD ms = ms64 > 0xFFFFFFF0ULL ? 0xFFFFFFF0ULL : static_cast<DWORD>(ms64);
    const ULONGLONG waitStarted = GetTickCount64();
    for (;;) {
        HRESULT hr = ctx->fence->SetEventOnCompletion(value, t_ctxFenceEvent);
        if (FAILED(hr)) {
            return mcdr::err("waitForContextFenceValue",
                             "waitForContextFenceValue: SetEventOnCompletion " + formatBackendResult(hr));
        }
        const ULONGLONG elapsed = GetTickCount64() - waitStarted;
        DWORD remaining = elapsed >= ms ? 0 : ms - static_cast<DWORD>(elapsed);
        DWORD wr = WaitForSingleObject(t_ctxFenceEvent, remaining);
        cv = ctx->fence->GetCompletedValue();
        if (cv == UINT64_MAX) {
            return mcdr::err("waitForContextFenceValue", "waitForContextFenceValue: device removed");
        }
        if (cv >= value) {
            DBG_LOG_DEBUG("waitCtxFence: OK value=%llu", (unsigned long long)value);
            return {};
        }
        if (wr != WAIT_OBJECT_0 || GetTickCount64() - waitStarted >= ms) {
            dbgLogError("waitCtxFence: TIMEOUT value=%llu completed=%llu ctx=%p",
                        static_cast<unsigned long long>(value),
                        static_cast<unsigned long long>(ctx->fence->GetCompletedValue()),
                        reinterpret_cast<void*>(ctx));
            return mcdr::err("waitForContextFenceValue",
                             "waitForContextFenceValue: timed out after " + std::to_string(timeoutNs) + "ns");
        }
    }
}

UINT64 currentQueueFenceValue() { return gCtx.queueFenceValue; }

static mcdr::Result<void> resolveQuery(QueryPool* pool, int start, int count, long long* out) {
    if (!pool) {
        return mcdr::err("resolveQuery", "resolveQuery: null pool");
    }
    if (start < 0 || count <= 0 || start + count > pool->size) {
        return mcdr::err("resolveQuery", "resolveQuery: invalid range");
    }
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(gCtx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))
        || FAILED(gCtx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                 IID_PPV_ARGS(&list)))) {
        return mcdr::err("resolveQuery", "resolveQuery: create command list failed");
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(count) * sizeof(long long);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(gCtx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&readback)))) {
        return mcdr::err("resolveQuery", "resolveQuery: readback alloc failed");
    }
    list->ResolveQueryData(pool->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(start),
                           static_cast<UINT>(count), readback.Get(), 0);
    list->Close();
    if (auto f = flushAndWait(list.Get(), "resolveQuery"); !f) {
        return mcdr::err("resolveQuery", errorMessage(f.error()));
    }
    void* p = nullptr;
    readback->Map(0, nullptr, &p);
    memcpy(out, p, static_cast<size_t>(count) * sizeof(long long));
    readback->Unmap(0, nullptr);
    return {};
}

mcdr::Result<long long> getTimestampNow(CommandContext* ctx) {
    (void)ctx;
    if (auto d = ensureDevice(); !d) return mcdr::err("getTimestampNow", errorMessage(d.error()));
    auto poolRes = createQueryPool(1);
    if (!poolRes) return mcdr::err("getTimestampNow", errorMessage(poolRes.error()));
    QueryPool* pool = static_cast<QueryPool*>(*poolRes);
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(gCtx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))
        || FAILED(gCtx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                 IID_PPV_ARGS(&list)))) {
        destroyQueryPool(pool);
        return mcdr::err("getTimestampNow", "getTimestampNow: create command list failed");
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeof(long long);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(gCtx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&readback)))) {
        destroyQueryPool(pool);
        return mcdr::err("getTimestampNow", "getTimestampNow: readback alloc failed");
    }
    list->EndQuery(pool->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    list->ResolveQueryData(pool->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, readback.Get(), 0);
    list->Close();
    if (auto f = flushAndWait(list.Get(), "getTimestampNow"); !f) {
        destroyQueryPool(pool);
        return mcdr::err("getTimestampNow", errorMessage(f.error()));
    }
    long long ts = 0;
    void* p = nullptr;
    readback->Map(0, nullptr, &p);
    memcpy(&ts, p, sizeof(ts));
    readback->Unmap(0, nullptr);
    destroyQueryPool(pool);
    return ts;
}

mcdr::Result<void> copyBufferToBuffer(CommandContext* ctx, Dx12Object* src, long long srcOffset,
                                      Dx12Object* dst, long long dstOffset, long long size) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("copyBufferToBuffer", "copyBufferToBuffer: no open command list");
    }
    if (!src || !dst || src->kind != Dx12Object::Kind::Buffer || dst->kind != Dx12Object::Kind::Buffer) {
        return mcdr::err("copyBufferToBuffer", "copyBufferToBuffer: invalid buffer handle");
    }
    if (srcOffset < 0 || dstOffset < 0 || size < 0 || srcOffset > src->size || dstOffset > dst->size
        || size > src->size - srcOffset || size > dst->size - dstOffset) {
        return mcdr::err("copyBufferToBuffer", "copyBufferToBuffer: range out of bounds");
    }
    retainObjectForRecording(ctx, src);
    retainObjectForRecording(ctx, dst);
    transitionBufferTo(ctx, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transitionBufferTo(ctx, dst, D3D12_RESOURCE_STATE_COPY_DEST);
    ctx->commandList->CopyBufferRegion(dst->resource.Get(), static_cast<UINT64>(dstOffset),
                                       src->resource.Get(), static_cast<UINT64>(srcOffset),
                                       static_cast<UINT64>(size));
    return {};
}

static D3D12_RENDER_TARGET_VIEW_DESC makeRtvDesc(Dx12Object* tex, int mip) {
    D3D12_RENDER_TARGET_VIEW_DESC d{};
    d.Format = tex->dxgiFormat;
    auto rd = tex->resource->GetDesc();
    int mips = static_cast<int>(rd.MipLevels);
    UINT mipU = static_cast<UINT>(std::max(0, std::min(mip, mips - 1)));
    if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.DepthOrArraySize > 1) {
        d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray.MipSlice = mipU;
        d.Texture2DArray.FirstArraySlice = 0;
        d.Texture2DArray.ArraySize = rd.DepthOrArraySize;
        d.Texture2DArray.PlaneSlice = 0;
    } else {
        d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = mipU;
        d.Texture2D.PlaneSlice = 0;
    }
    return d;
}

static D3D12_DEPTH_STENCIL_VIEW_DESC makeDsvDesc(Dx12Object* tex, int mip) {
    D3D12_DEPTH_STENCIL_VIEW_DESC d{};
    d.Format = tex->dxgiFormat;
    auto rd = tex->resource->GetDesc();
    int mips = static_cast<int>(rd.MipLevels);
    UINT mipU = static_cast<UINT>(std::max(0, std::min(mip, mips - 1)));
    if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.DepthOrArraySize > 1) {
        d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray.MipSlice = mipU;
        d.Texture2DArray.FirstArraySlice = 0;
        d.Texture2DArray.ArraySize = rd.DepthOrArraySize;
    } else {
        d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = mipU;
    }
    return d;
}

static D3D12_CLEAR_FLAGS depthClearFlags(DXGI_FORMAT format) {
    if (format == DXGI_FORMAT_D32_FLOAT || format == DXGI_FORMAT_D16_UNORM) return D3D12_CLEAR_FLAG_DEPTH;
    return D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL;
}

static SampledTextureContentStatus prepareDepthClearWrites(CommandContext* context, Dx12Object* texture) {
    if (texture->dxgiFormat != DXGI_FORMAT_D32_FLOAT) return SampledTextureContentStatus::Ready;
    const auto description = texture->resource->GetDesc();
    if (description.MipLevels == 1)
        return prepareSampledTextureWrite(context, texture, 0, description.DepthOrArraySize);
    for (uint32_t layer = 0; layer < description.DepthOrArraySize; ++layer) {
        const auto status = prepareSampledTextureWrite(context, texture, layer * description.MipLevels, 1);
        if (status != SampledTextureContentStatus::Ready) return status;
    }
    return SampledTextureContentStatus::Ready;
}

static mcdr::Result<D3D12_CPU_DESCRIPTOR_HANDLE> frameRtvFor(CommandContext* ctx, Dx12Object* tex,
                                                             int mip) {
    if (ctx->frameViewGeneration != gFrameViewGeneration) {
        ctx->rtvCache.clear();
        ctx->dsvCache.clear();
        ctx->frameViewGeneration = gFrameViewGeneration;
    }
    RtvDsvKey key{tex->resource.Get(), static_cast<UINT>(mip)};
    auto it = ctx->rtvCache.find(key);
    if (it != ctx->rtvCache.end()) {
        return it->second;
    }
    if (gNextFrameRtv >= kFrameRtvHeapSize) {
        return mcdr::err("frameRtvFor", "frameRtvFor: frame rtv heap exhausted");
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = gCtx.frameRtvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(gNextFrameRtv) * gCtx.rtvInc;
    ++gNextFrameRtv;
    D3D12_RENDER_TARGET_VIEW_DESC desc = makeRtvDesc(tex, mip);
    gCtx.device->CreateRenderTargetView(tex->resource.Get(), &desc, cpu);
    ctx->rtvCache.emplace(key, cpu);
    return cpu;
}

mcdr::Result<D3D12_CPU_DESCRIPTOR_HANDLE> acquireTextureRtv(CommandContext* ctx, Dx12Object* tex) {
    if (!ctx || !tex || !tex->resource) {
        return mcdr::err("acquireTextureRtv", "acquireTextureRtv: null ctx/texture/resource");
    }
    return frameRtvFor(ctx, tex, 0);
}

static mcdr::Result<D3D12_CPU_DESCRIPTOR_HANDLE> frameDsvFor(CommandContext* ctx, Dx12Object* tex,
                                                             int mip) {
    if (ctx->frameViewGeneration != gFrameViewGeneration) {
        ctx->rtvCache.clear();
        ctx->dsvCache.clear();
        ctx->frameViewGeneration = gFrameViewGeneration;
    }
    RtvDsvKey key{tex->resource.Get(), static_cast<UINT>(mip)};
    auto it = ctx->dsvCache.find(key);
    if (it != ctx->dsvCache.end()) {
        return it->second;
    }
    if (gNextDsv >= kDsvHeapSize) {
        return mcdr::err("frameDsvFor", "frameDsvFor: dsv heap exhausted");
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = gCtx.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(gNextDsv) * gCtx.dsvInc;
    ++gNextDsv;
    D3D12_DEPTH_STENCIL_VIEW_DESC desc = makeDsvDesc(tex, mip);
    gCtx.device->CreateDepthStencilView(tex->resource.Get(), &desc, cpu);
    ctx->dsvCache.emplace(key, cpu);
    return cpu;
}

mcdr::Result<void> clearColorTexture(CommandContext* ctx, Dx12Object* tex, float r, float g, float b,
                                     float a) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("clearColorTexture", "clearColorTexture: no open command list");
    }
    if (!tex || tex->kind != Dx12Object::Kind::Texture) {
        return mcdr::err("clearColorTexture", "clearColorTexture: invalid texture");
    }
    if (!(tex->usage & 8)) {
        return mcdr::err("clearColorTexture", "clearColorTexture: texture lacks RENDER_ATTACHMENT");
    }
    auto rtvRes = frameRtvFor(ctx, tex, 0);
    if (!rtvRes) return mcdr::err("clearColorTexture", errorMessage(rtvRes.error()));
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = *rtvRes;
    const auto description = tex->resource->GetDesc();
    for (uint32_t layer = 0; layer < description.DepthOrArraySize; ++layer) {
        if (prepareSampledTextureWrite(ctx, tex, layer * description.MipLevels, 1) != SampledTextureContentStatus::Ready)
            return mcdr::err("clearColorTexture", "sampled texture write ordering failed");
    }
    transitionTextureTo(ctx, tex, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float color[4] = {r, g, b, a};
    ctx->commandList->ClearRenderTargetView(cpu, color, 0, nullptr);
    return {};
}

mcdr::Result<void> clearDepthTexture(CommandContext* ctx, Dx12Object* tex, double depth) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("clearDepthTexture", "clearDepthTexture: no open command list");
    }
    if (!tex || tex->kind != Dx12Object::Kind::Texture) {
        return mcdr::err("clearDepthTexture", "clearDepthTexture: invalid texture");
    }
    auto dsvRes = frameDsvFor(ctx, tex, 0);
    if (!dsvRes) return mcdr::err("clearDepthTexture", errorMessage(dsvRes.error()));
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = *dsvRes;
    if (prepareDepthClearWrites(ctx, tex) != SampledTextureContentStatus::Ready)
        return mcdr::err("clearDepthTexture", "sampled depth write ordering failed");
    transitionTextureTo(ctx, tex, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ctx->commandList->ClearDepthStencilView(cpu, depthClearFlags(tex->dxgiFormat),
                                            static_cast<FLOAT>(depth), 0, 0, nullptr);
    if (ctx->vanillaDepthSeedClear)
        ctx->vanillaDepthSeedClear(ctx->vanillaDepthSeedState, ctx, tex, 0, depth, nullptr, cpu);
    return {};
}

mcdr::Result<void> clearStencilTexture(CommandContext* ctx, Dx12Object* tex, int value) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("clearStencilTexture", "clearStencilTexture: no open command list");
    }
    if (!tex || tex->kind != Dx12Object::Kind::Texture) {
        return mcdr::err("clearStencilTexture", "clearStencilTexture: invalid texture");
    }
    auto dsvRes = frameDsvFor(ctx, tex, 0);
    if (!dsvRes) return mcdr::err("clearStencilTexture", errorMessage(dsvRes.error()));
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = *dsvRes;
    transitionTextureTo(ctx, tex, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ctx->commandList->ClearDepthStencilView(cpu, D3D12_CLEAR_FLAG_STENCIL, 1.0f, static_cast<UINT8>(value), 0,
                                            nullptr);
    return {};
}

mcdr::Result<void> clearColorTextureRegion(CommandContext* ctx, Dx12Object* tex, float r, float g, float b,
                                           float a, int x, int y, int w, int h) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("clearColorTextureRegion", "clearColorTextureRegion: no open command list");
    }
    if (!tex || tex->kind != Dx12Object::Kind::Texture) {
        return mcdr::err("clearColorTextureRegion", "clearColorTextureRegion: invalid texture");
    }
    if (!(tex->usage & 8)) {
        return mcdr::err("clearColorTextureRegion",
                         "clearColorTextureRegion: texture lacks RENDER_ATTACHMENT");
    }
    if (w <= 0 || h <= 0) return {};
    auto rtvRes = frameRtvFor(ctx, tex, 0);
    if (!rtvRes) return mcdr::err("clearColorTextureRegion", errorMessage(rtvRes.error()));
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = *rtvRes;
    const auto description = tex->resource->GetDesc();
    for (uint32_t layer = 0; layer < description.DepthOrArraySize; ++layer) {
        if (prepareSampledTextureWrite(ctx, tex, layer * description.MipLevels, 1) != SampledTextureContentStatus::Ready)
            return mcdr::err("clearColorTextureRegion", "sampled texture write ordering failed");
    }
    transitionTextureTo(ctx, tex, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float color[4] = {r, g, b, a};
    D3D12_RECT rc{x, y, x + w, y + h};
    ctx->commandList->ClearRenderTargetView(cpu, color, 1, &rc);
    return {};
}

mcdr::Result<void> clearDepthTextureRegion(CommandContext* ctx, Dx12Object* tex, double depth, int x, int y,
                                           int w, int h) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("clearDepthTextureRegion", "clearDepthTextureRegion: no open command list");
    }
    if (!tex || tex->kind != Dx12Object::Kind::Texture) {
        return mcdr::err("clearDepthTextureRegion", "clearDepthTextureRegion: invalid texture");
    }
    if (w <= 0 || h <= 0) return {};
    auto dsvRes = frameDsvFor(ctx, tex, 0);
    if (!dsvRes) return mcdr::err("clearDepthTextureRegion", errorMessage(dsvRes.error()));
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = *dsvRes;
    if (prepareDepthClearWrites(ctx, tex) != SampledTextureContentStatus::Ready)
        return mcdr::err("clearDepthTextureRegion", "sampled depth write ordering failed");
    transitionTextureTo(ctx, tex, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_RECT rc{x, y, x + w, y + h};
    ctx->commandList->ClearDepthStencilView(cpu, depthClearFlags(tex->dxgiFormat),
                                            static_cast<FLOAT>(depth), 0, 1, &rc);
    if (ctx->vanillaDepthSeedClear)
        ctx->vanillaDepthSeedClear(ctx->vanillaDepthSeedState, ctx, tex, 0, depth, &rc, cpu);
    return {};
}

mcdr::Result<void> copyBufferToTexture(CommandContext* ctx, Dx12Object* srcBuf, long long srcOffset,
                                       int srcWidth, int srcHeight, Dx12Object* dstTex, int mip, int layer,
                                       int dstX, int dstY, int w, int h) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("copyBufferToTexture", "copyBufferToTexture: no open command list");
    }
    if (!srcBuf || srcBuf->kind != Dx12Object::Kind::Buffer || !dstTex
        || dstTex->kind != Dx12Object::Kind::Texture) {
        return mcdr::err("copyBufferToTexture", "copyBufferToTexture: invalid handle");
    }
    if (w <= 0 || h <= 0 || srcWidth <= 0 || srcHeight <= 0 || srcOffset < 0) {
        return mcdr::err("copyBufferToTexture", "copyBufferToTexture: non-positive size");
    }
    const auto textureDesc = dstTex->resource->GetDesc();
    D3D12_FEATURE_DATA_FORMAT_INFO formatInfo{textureDesc.Format};
    if (FAILED(gCtx.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &formatInfo, sizeof(formatInfo)))
        || formatInfo.PlaneCount != 1 || textureDesc.SampleDesc.Count != 1) {
        return mcdr::err("copyBufferToTexture", "buffer upload requires a single-plane, single-sample texture");
    }
    if (mip < 0 || mip >= textureDesc.MipLevels || layer < 0 ||
        layer >= textureDesc.DepthOrArraySize) {
        return mcdr::err("copyBufferToTexture", "copyBufferToTexture: subresource is out of range");
    }
    if (dstX < 0 || dstY < 0) {
        return mcdr::err("copyBufferToTexture", "copyBufferToTexture: negative destination offset");
    }
    const UINT mipWidth = static_cast<UINT>(std::max<UINT64>(1, textureDesc.Width >> mip));
    const UINT mipHeight = std::max<UINT>(1, textureDesc.Height >> mip);
    if (static_cast<UINT64>(dstX) + static_cast<UINT64>(w) > mipWidth
        || static_cast<UINT64>(dstY) + static_cast<UINT64>(h) > mipHeight) {
        return mcdr::err("copyBufferToTexture", "copyBufferToTexture: destination region is out of range");
    }
    const UINT blockWidth = blockWidthFor(dstTex->dxgiFormat);
    if (blockWidth > 1 && ((dstX % static_cast<int>(blockWidth)) != 0 || (dstY % static_cast<int>(blockWidth)) != 0
        || (w % static_cast<int>(blockWidth)) != 0 && dstX + w != static_cast<int>(mipWidth)
        || (h % static_cast<int>(blockWidth)) != 0 && dstY + h != static_cast<int>(mipHeight))) {
        return mcdr::err("copyBufferToTexture", "copyBufferToTexture: compressed region is not block aligned");
    }
    const UINT64 rowBytes64 = compactRowBytesFor(dstTex->dxgiFormat, static_cast<UINT>(w));
    const UINT64 srcRowBytes64 = compactRowBytesFor(dstTex->dxgiFormat, static_cast<UINT>(srcWidth));
    const UINT rows = compactRowsFor(dstTex->dxgiFormat, static_cast<UINT>(h));
    if (rowBytes64 > UINT_MAX || srcRowBytes64 > UINT_MAX || w > srcWidth || h > srcHeight
        || static_cast<UINT64>(srcOffset) > static_cast<UINT64>(srcBuf->size)) {
        return mcdr::err("copyBufferToTexture", "source buffer does not contain the requested footprint");
    }
    const UINT64 sourceRemaining = static_cast<UINT64>(srcBuf->size) - static_cast<UINT64>(srcOffset);
    if (rowBytes64 > sourceRemaining
        || (rows > 1 && srcRowBytes64 > (sourceRemaining - rowBytes64) / (rows - 1))) {
        return mcdr::err("copyBufferToTexture", "source buffer does not contain the requested footprint");
    }
    if (isDepthStencilFormat(dstTex->dxgiFormat)
        && (dstX != 0 || dstY != 0 || w != static_cast<int>(mipWidth) || h != static_cast<int>(mipHeight))) {
        return mcdr::err("copyBufferToTexture", "depth/stencil uploads require a complete subresource");
    }

    constexpr UINT kPitchAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    const UINT rowBytes = static_cast<UINT>(rowBytes64);
    const UINT srcRowBytes = static_cast<UINT>(srcRowBytes64);
    bool aligned = (srcRowBytes % kPitchAlign) == 0 && srcWidth == w && (srcOffset % 512) == 0;

    UINT subresource = static_cast<UINT>(mip + layer * dstTex->resource->GetDesc().MipLevels);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT copyFootprint{};
    UINT footprintRows = 0;
    UINT64 footprintRowBytes = 0;
    UINT64 footprintBytes = 0;
    gCtx.device->GetCopyableFootprints(&textureDesc, subresource, 1, 0, &copyFootprint,
                                       &footprintRows, &footprintRowBytes, &footprintBytes);
    if (footprintBytes == UINT64_MAX || footprintRows == 0 || footprintRowBytes == 0
        || (w == static_cast<int>(mipWidth) && footprintRowBytes != rowBytes64)) {
        return mcdr::err("copyBufferToTexture", "texture footprint does not match the compact buffer layout");
    }
    if (prepareSampledTextureWrite(ctx, dstTex, subresource, 1) != SampledTextureContentStatus::Ready)
        return mcdr::err("copyBufferToTexture", "sampled texture write ordering failed");
    SampledWriteRecording writeRecording{ctx};
    transitionBufferTo(ctx, srcBuf, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transitionTextureTo(ctx, dstTex, D3D12_RESOURCE_STATE_COPY_DEST);
    if (dstTex->usage & 16) {
        DBG_LOG_DEBUG("copyBufferToTexture: CUBE dst=%p mip=%d layer=%d subres=%u srcOff=%lld srcW=%d "
                      "srcH=%d w=%d h=%d rowBytes=%u aligned=%d",
                      (void*)dstTex, mip, layer, subresource, (long long)srcOffset, srcWidth, srcHeight, w, h,
                      srcRowBytes, (int)aligned);
    }
    if (aligned) {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = srcBuf->resource.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = static_cast<UINT64>(srcOffset);
        src.PlacedFootprint.Footprint.Format = copyFootprint.Footprint.Format;
        src.PlacedFootprint.Footprint.Width = static_cast<UINT>(w);
        src.PlacedFootprint.Footprint.Height = static_cast<UINT>(h);
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch =
            (w == static_cast<int>(mipWidth)) ? copyFootprint.Footprint.RowPitch : alignedPitch(rowBytes);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = dstTex->resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = subresource;
        ctx->commandList->CopyTextureRegion(&dst, dstX, dstY, 0, &src, nullptr);
    } else {
        const UINT pitch = alignedPitch(rowBytes);
        const UINT64 scratchSize = static_cast<UINT64>(pitch) * rows;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = scratchSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> scratch;
        if (FAILED(gCtx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&scratch)))) {
            ctx->recordingFailed.store(true, std::memory_order_release);
            return mcdr::err("copyBufferToTexture", "aligned upload footprint allocation failed");
        }
        ctx->recordingLeases[ctx->currentListSlot].push_back(
            std::make_shared<ComPtr<ID3D12Resource>>(scratch));
        for (UINT row = 0; row < rows; ++row) {
            ctx->commandList->CopyBufferRegion(scratch.Get(), static_cast<UINT64>(row) * pitch,
                                               srcBuf->resource.Get(),
                                               static_cast<UINT64>(srcOffset) + static_cast<UINT64>(row) * srcRowBytes,
                                               rowBytes);
        }
        D3D12_RESOURCE_BARRIER scratchBarrier{};
        scratchBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        scratchBarrier.Transition.pResource = scratch.Get();
        scratchBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        scratchBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        scratchBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ctx->commandList->ResourceBarrier(1, &scratchBarrier);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = scratch.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 0;
        src.PlacedFootprint.Footprint.Format = copyFootprint.Footprint.Format;
        src.PlacedFootprint.Footprint.Width = static_cast<UINT>(w);
        src.PlacedFootprint.Footprint.Height = static_cast<UINT>(h);
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = pitch;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = dstTex->resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = subresource;
        ctx->commandList->CopyTextureRegion(&dst, dstX, dstY, 0, &src, nullptr);
    }
    transitionTextureTo(ctx, dstTex, D3D12_RESOURCE_STATE_COMMON);
    writeRecording.completed = true;
    return {};
}

mcdr::Result<void> copyTextureToBuffer(CommandContext* ctx, Dx12Object* srcTex, int mip, int layer,
                                       int srcX, int srcY, int w, int h, Dx12Object* dstBuf,
                                       long long dstOffset) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("copyTextureToBuffer", "copyTextureToBuffer: no open command list");
    }
    if (!srcTex || srcTex->kind != Dx12Object::Kind::Texture || !dstBuf
        || dstBuf->kind != Dx12Object::Kind::Buffer) {
        return mcdr::err("copyTextureToBuffer", "copyTextureToBuffer: invalid handle");
    }
    if (w <= 0 || h <= 0 || dstOffset < 0) {
        return mcdr::err("copyTextureToBuffer", "copyTextureToBuffer: non-positive size");
    }
    const auto textureDesc = srcTex->resource->GetDesc();
    D3D12_FEATURE_DATA_FORMAT_INFO formatInfo{textureDesc.Format};
    if (FAILED(gCtx.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &formatInfo, sizeof(formatInfo)))
        || formatInfo.PlaneCount != 1 || textureDesc.SampleDesc.Count != 1) {
        return mcdr::err("copyTextureToBuffer", "buffer readback requires a single-plane, single-sample texture");
    }
    if (mip < 0 || mip >= textureDesc.MipLevels || layer < 0 ||
        layer >= textureDesc.DepthOrArraySize) {
        return mcdr::err("copyTextureToBuffer", "copyTextureToBuffer: subresource is out of range");
    }
    const UINT mipWidth = static_cast<UINT>(std::max<UINT64>(1, textureDesc.Width >> mip));
    const UINT mipHeight = std::max<UINT>(1, textureDesc.Height >> mip);
    if (srcX < 0 || srcY < 0 || static_cast<UINT64>(srcX) + static_cast<UINT64>(w) > mipWidth
        || static_cast<UINT64>(srcY) + static_cast<UINT64>(h) > mipHeight) {
        return mcdr::err("copyTextureToBuffer", "copyTextureToBuffer: source region is out of range");
    }
    const UINT blockWidth = blockWidthFor(srcTex->dxgiFormat);
    if (blockWidth > 1 && ((srcX % static_cast<int>(blockWidth)) != 0 || (srcY % static_cast<int>(blockWidth)) != 0
        || (w % static_cast<int>(blockWidth)) != 0 && srcX + w != static_cast<int>(mipWidth)
        || (h % static_cast<int>(blockWidth)) != 0 && srcY + h != static_cast<int>(mipHeight))) {
        return mcdr::err("copyTextureToBuffer", "copyTextureToBuffer: compressed region is not block aligned");
    }
    const UINT64 rowBytes64 = compactRowBytesFor(srcTex->dxgiFormat, static_cast<UINT>(w));
    const UINT rows = compactRowsFor(srcTex->dxgiFormat, static_cast<UINT>(h));
    if (rowBytes64 > UINT_MAX) {
        return mcdr::err("copyTextureToBuffer", "copyTextureToBuffer: row footprint is too large");
    }
    const UINT rowBytes = static_cast<UINT>(rowBytes64);
    if (isDepthStencilFormat(srcTex->dxgiFormat)
        && (srcX != 0 || srcY != 0 || w != static_cast<int>(mipWidth) || h != static_cast<int>(mipHeight))) {
        return mcdr::err("copyTextureToBuffer", "depth/stencil readback requires a complete subresource");
    }

    transitionTextureTo(ctx, srcTex, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transitionBufferTo(ctx, dstBuf, D3D12_RESOURCE_STATE_COPY_DEST);

    UINT subresource = static_cast<UINT>(mip + layer * srcTex->resource->GetDesc().MipLevels);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT copyFootprint{};
    UINT footprintRows = 0;
    UINT64 footprintRowBytes = 0;
    UINT64 footprintBytes = 0;
    gCtx.device->GetCopyableFootprints(&textureDesc, subresource, 1, 0, &copyFootprint,
                                       &footprintRows, &footprintRowBytes, &footprintBytes);
    if (footprintBytes == UINT64_MAX || footprintRows == 0 || footprintRowBytes == 0
        || (w == static_cast<int>(mipWidth) && footprintRowBytes != rowBytes64)) {
        return mcdr::err("copyTextureToBuffer", "texture footprint does not match the compact buffer layout");
    }
    bool aligned = (rowBytes % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) == 0 && (dstOffset % 512) == 0;
    if (static_cast<UINT64>(dstOffset) > static_cast<UINT64>(dstBuf->size)
        || static_cast<UINT64>(rowBytes) * rows > static_cast<UINT64>(dstBuf->size) - static_cast<UINT64>(dstOffset)) {
        return mcdr::err("copyTextureToBuffer", "copyTextureToBuffer: destination buffer too small");
    }

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = srcTex->resource.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = subresource;
    if (aligned) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = dstBuf->resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = static_cast<UINT64>(dstOffset);
        dst.PlacedFootprint.Footprint.Format = copyFootprint.Footprint.Format;
        dst.PlacedFootprint.Footprint.Width = static_cast<UINT>(w);
        dst.PlacedFootprint.Footprint.Height = static_cast<UINT>(h);
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch =
            (w == static_cast<int>(mipWidth)) ? copyFootprint.Footprint.RowPitch : alignedPitch(rowBytes);
        D3D12_BOX srcBox{static_cast<UINT>(srcX),     static_cast<UINT>(srcY),     0,
                         static_cast<UINT>(srcX + w), static_cast<UINT>(srcY + h), 1};
        ctx->commandList->CopyTextureRegion(&dst, 0, 0, 0, &src,
            isDepthStencilFormat(srcTex->dxgiFormat) ? nullptr : &srcBox);
    } else {
        const UINT pitch = alignedPitch(rowBytes);
        const UINT64 scratchSize = static_cast<UINT64>(pitch) * rows;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = scratchSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> scratch;
        if (FAILED(gCtx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&scratch)))) {
            return mcdr::err("copyTextureToBuffer", "aligned readback footprint allocation failed");
        }
        ctx->recordingLeases[ctx->currentListSlot].push_back(
            std::make_shared<ComPtr<ID3D12Resource>>(scratch));
        D3D12_TEXTURE_COPY_LOCATION scratchLocation{};
        scratchLocation.pResource = scratch.Get();
        scratchLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        scratchLocation.PlacedFootprint.Offset = 0;
        scratchLocation.PlacedFootprint.Footprint.Format = copyFootprint.Footprint.Format;
        scratchLocation.PlacedFootprint.Footprint.Width = static_cast<UINT>(w);
        scratchLocation.PlacedFootprint.Footprint.Height = static_cast<UINT>(h);
        scratchLocation.PlacedFootprint.Footprint.Depth = 1;
        scratchLocation.PlacedFootprint.Footprint.RowPitch = pitch;
        D3D12_BOX srcBox{static_cast<UINT>(srcX), static_cast<UINT>(srcY), 0,
                         static_cast<UINT>(srcX + w), static_cast<UINT>(srcY + h), 1};
        ctx->commandList->CopyTextureRegion(&scratchLocation, 0, 0, 0, &src,
            isDepthStencilFormat(srcTex->dxgiFormat) ? nullptr : &srcBox);
        D3D12_RESOURCE_BARRIER scratchBarrier{};
        scratchBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        scratchBarrier.Transition.pResource = scratch.Get();
        scratchBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        scratchBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        scratchBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ctx->commandList->ResourceBarrier(1, &scratchBarrier);
        for (UINT row = 0; row < rows; ++row) {
            ctx->commandList->CopyBufferRegion(dstBuf->resource.Get(),
                                               static_cast<UINT64>(dstOffset) + static_cast<UINT64>(row) * rowBytes,
                                               scratch.Get(), static_cast<UINT64>(row) * pitch, rowBytes);
        }
    }
    transitionTextureTo(ctx, srcTex, D3D12_RESOURCE_STATE_COMMON);
    return {};
}

mcdr::Result<void> copyTextureToTexture(CommandContext* ctx, Dx12Object* srcTex, Dx12Object* dstTex, int mip,
                                        int layer, int srcX, int srcY, int dstX, int dstY, int w, int h) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("copyTextureToTexture", "copyTextureToTexture: no open command list");
    }
    if (!srcTex || srcTex->kind != Dx12Object::Kind::Texture || !dstTex
        || dstTex->kind != Dx12Object::Kind::Texture) {
        return mcdr::err("copyTextureToTexture", "copyTextureToTexture: invalid handle");
    }
    if (w <= 0 || h <= 0) {
        return mcdr::err("copyTextureToTexture", "copyTextureToTexture: non-positive size");
    }
    if (srcTex->dxgiFormat != dstTex->dxgiFormat) {
        return mcdr::err("copyTextureToTexture", "copyTextureToTexture: format mismatch");
    }
    const auto sourceDesc = srcTex->resource->GetDesc();
    const auto destinationDesc = dstTex->resource->GetDesc();
    D3D12_FEATURE_DATA_FORMAT_INFO sourceFormatInfo{sourceDesc.Format};
    if (FAILED(gCtx.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &sourceFormatInfo, sizeof(sourceFormatInfo))) ||
        sourceFormatInfo.PlaneCount == 0) {
        return mcdr::err("copyTextureToTexture", "cannot determine texture plane count");
    }
    if (mip < 0 || mip >= sourceDesc.MipLevels || mip >= destinationDesc.MipLevels || layer < 0
        || layer >= sourceDesc.DepthOrArraySize || layer >= destinationDesc.DepthOrArraySize) {
        return mcdr::err("copyTextureToTexture", "copyTextureToTexture: subresource is out of range");
    }
    const UINT sourceWidth = static_cast<UINT>(std::max<UINT64>(1, sourceDesc.Width >> mip));
    const UINT sourceHeight = std::max<UINT>(1, sourceDesc.Height >> mip);
    const UINT destinationWidth = static_cast<UINT>(std::max<UINT64>(1, destinationDesc.Width >> mip));
    const UINT destinationHeight = std::max<UINT>(1, destinationDesc.Height >> mip);
    if (srcX < 0 || srcY < 0 || dstX < 0 || dstY < 0
        || static_cast<UINT64>(srcX) + static_cast<UINT64>(w) > sourceWidth
        || static_cast<UINT64>(srcY) + static_cast<UINT64>(h) > sourceHeight
        || static_cast<UINT64>(dstX) + static_cast<UINT64>(w) > destinationWidth
        || static_cast<UINT64>(dstY) + static_cast<UINT64>(h) > destinationHeight) {
        return mcdr::err("copyTextureToTexture", "copyTextureToTexture: copy region is out of range");
    }
    const UINT blockWidth = blockWidthFor(srcTex->dxgiFormat);
    if (blockWidth > 1 && ((srcX % static_cast<int>(blockWidth)) != 0 || (srcY % static_cast<int>(blockWidth)) != 0
        || (dstX % static_cast<int>(blockWidth)) != 0 || (dstY % static_cast<int>(blockWidth)) != 0
        || (w % static_cast<int>(blockWidth)) != 0
            && (srcX + w != static_cast<int>(sourceWidth) || dstX + w != static_cast<int>(destinationWidth))
        || (h % static_cast<int>(blockWidth)) != 0
            && (srcY + h != static_cast<int>(sourceHeight) || dstY + h != static_cast<int>(destinationHeight)))) {
        return mcdr::err("copyTextureToTexture", "copyTextureToTexture: compressed region is not block aligned");
    }
    const bool wholeSubresource =
        ((sourceDesc.Flags | destinationDesc.Flags) & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 ||
        isDepthStencilFormat(srcTex->dxgiFormat) ||
        sourceDesc.SampleDesc.Count > 1 || destinationDesc.SampleDesc.Count > 1;
    if (wholeSubresource &&
        (mip < 0 || mip >= sourceDesc.MipLevels || mip >= destinationDesc.MipLevels ||
         srcX != 0 || srcY != 0 || dstX != 0 || dstY != 0 ||
         static_cast<UINT64>(w) != sourceWidth ||
         static_cast<UINT>(h) != sourceHeight ||
         sourceDesc.Width != destinationDesc.Width || sourceDesc.Height != destinationDesc.Height ||
         sourceDesc.SampleDesc.Count != destinationDesc.SampleDesc.Count)) {
        return mcdr::err("copyTextureToTexture", "depth/MSAA copy requires a complete matching subresource");
    }
    UINT srcSub = static_cast<UINT>(mip + layer * srcTex->resource->GetDesc().MipLevels);
    UINT dstSub = static_cast<UINT>(mip + layer * dstTex->resource->GetDesc().MipLevels);
    for (UINT plane = 0; plane < sourceFormatInfo.PlaneCount; ++plane) {
        if (prepareSampledTextureWrite(ctx, dstTex,
                dstSub + plane * destinationDesc.MipLevels * destinationDesc.DepthOrArraySize, 1) != SampledTextureContentStatus::Ready)
            return mcdr::err("copyTextureToTexture", "sampled texture write ordering failed");
    }
    transitionTextureTo(ctx, srcTex, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transitionTextureTo(ctx, dstTex, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = srcTex->resource.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = srcSub;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = dstTex->resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = dstSub;
    D3D12_BOX srcBox{static_cast<UINT>(srcX),     static_cast<UINT>(srcY),     0,
                     static_cast<UINT>(srcX + w), static_cast<UINT>(srcY + h), 1};
    for (UINT plane = 0; plane < sourceFormatInfo.PlaneCount; ++plane) {
        src.SubresourceIndex = srcSub + plane * sourceDesc.MipLevels * sourceDesc.DepthOrArraySize;
        dst.SubresourceIndex = dstSub + plane * destinationDesc.MipLevels * destinationDesc.DepthOrArraySize;
        ctx->commandList->CopyTextureRegion(&dst, dstX, dstY, 0, &src, wholeSubresource ? nullptr : &srcBox);
    }
    transitionTextureTo(ctx, srcTex, D3D12_RESOURCE_STATE_COMMON);
    transitionTextureTo(ctx, dstTex, D3D12_RESOURCE_STATE_COMMON);
    return {};
}

mcdr::Result<void> setRasterMotionTarget(void* rawContext, void* rawWorldColor, void* rawWorldDepth,
                                        void* rawMotion) {
    auto* context = static_cast<CommandContext*>(rawContext);
    if (!context) return mcdr::err("setRasterMotionTarget", "null command context");
    if (!rawWorldColor && !rawWorldDepth && !rawMotion) {
        if (context->activeMotionTarget) {
            if (!context->listOpen || !context->commandList) {
                return mcdr::err("setRasterMotionTarget", "active motion attachment has no recording context");
            }
            auto targets = context->activePassRtvs;
            if (context->additionalColorTarget) targets.push_back(context->additionalColorRtv);
            context->commandList->OMSetRenderTargets(static_cast<UINT>(targets.size()),
                targets.empty() ? nullptr : targets.data(), FALSE,
                context->activeDepthTarget ? &context->activePassDsv : nullptr);
            if (const auto result = transitionAttachmentRanges(context, context->activeMotionTarget, true); !result) return result;
            context->activeMotionTarget = nullptr;
            context->activeMotionRtv = {};
            context->rasterMotionSelected = false;
            context->activePassBoundColorCount = static_cast<UINT>(targets.size());
            context->cachedPso = nullptr;
            context->currentPipeline = nullptr;
            invalidateGraphicsInputBindings(context, false);
        }
        context->rasterMotionWorldColor = nullptr;
        context->rasterMotionWorldDepth = nullptr;
        context->rasterMotionTarget = nullptr;
        context->rasterMotionLeases.clear();
        return {};
    }
    if (!context->listOpen || !context->commandList || context->submissionFailed || context->inRenderPass ||
        !rawWorldColor || !rawWorldDepth || !rawMotion) {
        return mcdr::err("setRasterMotionTarget", "registration requires a complete target and idle recording pass");
    }
    auto* worldColor = static_cast<Dx12Object*>(rawWorldColor);
    auto* worldDepth = static_cast<Dx12Object*>(rawWorldDepth);
    auto* motion = static_cast<Dx12Object*>(rawMotion);
    auto valid = [](Dx12Object* texture) {
        if (texture->kind != Dx12Object::Kind::Texture || !texture->resource || !(texture->usage & 8)) return false;
        const auto description = texture->resource->GetDesc();
        return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && description.Width > 0 &&
            description.Height > 0 && description.MipLevels == 1 && description.DepthOrArraySize == 1 &&
            description.SampleDesc.Count == 1;
    };
    if (!valid(worldColor) || !valid(worldDepth) || !valid(motion) ||
        worldDepth->dxgiFormat != DXGI_FORMAT_D32_FLOAT || isDepthStencilFormat(worldColor->dxgiFormat) ||
        motion->dxgiFormat != DXGI_FORMAT_R32G32B32A32_FLOAT ||
        worldColor->resource.Get() == worldDepth->resource.Get() ||
        worldColor->resource.Get() == motion->resource.Get() ||
        worldDepth->resource.Get() == motion->resource.Get()) {
        return mcdr::err("setRasterMotionTarget", "invalid texture format, usage, shape or alias");
    }
    const auto colorDescription = worldColor->resource->GetDesc();
    const auto depthDescription = worldDepth->resource->GetDesc();
    const auto motionDescription = motion->resource->GetDesc();
    if (colorDescription.Width != depthDescription.Width || colorDescription.Height != depthDescription.Height ||
        colorDescription.Width != motionDescription.Width || colorDescription.Height != motionDescription.Height ||
        !(colorDescription.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
        !(depthDescription.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ||
        !(motionDescription.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
        return mcdr::err("setRasterMotionTarget", "motion and world attachments have incompatible dimensions or flags");
    }
    std::vector<std::shared_ptr<void>> leases;
    leases.push_back(retainObjectLease(worldColor));
    leases.push_back(retainObjectLease(worldDepth));
    leases.push_back(retainObjectLease(motion));
    context->rasterMotionLeases = std::move(leases);
    context->rasterMotionWorldColor = worldColor;
    context->rasterMotionWorldDepth = worldDepth;
    context->rasterMotionTarget = motion;
    return {};
}

mcdr::Result<void> setAdditionalColorTarget(void* rawContext, void* rawTarget) {
    auto* context = static_cast<CommandContext*>(rawContext);
    auto* target = static_cast<Dx12Object*>(rawTarget);
    if (!context || !context->listOpen || !context->inRenderPass)
        return mcdr::err("setAdditionalColorTarget", "an open render pass is required");
    if (context->additionalColorTarget == target) return {};
    invalidateGraphicsBindingReadCache(context);
    auto targets = context->activePassRtvs;
    D3D12_CPU_DESCRIPTOR_HANDLE additional{};
    if (target) {
        if (targets.empty() || targets.size() + 1u + (context->rasterMotionSelected ? 1u : 0u) >
                D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT ||
            target->kind != Dx12Object::Kind::Texture || !(target->usage & 8))
            return mcdr::err("setAdditionalColorTarget", "additional color attachment is incompatible");
        const auto source = context->activeColorTargets.front()->resource->GetDesc();
        const auto destination = target->resource->GetDesc();
        if (source.Width != destination.Width || source.Height != destination.Height ||
            source.SampleDesc.Count != destination.SampleDesc.Count ||
            destination.DepthOrArraySize != 1 ||
            (context->activeMotionTarget && target->resource.Get() == context->activeMotionTarget->resource.Get()) ||
            (context->activeDepthTarget && target->resource.Get() == context->activeDepthTarget->resource.Get()) ||
            std::any_of(context->activeColorTargets.begin(), context->activeColorTargets.end(),
                [&](const auto* color) { return color->resource.Get() == target->resource.Get(); }))
            return mcdr::err("setAdditionalColorTarget", "additional color extent or alias is invalid");
        auto rtv = frameRtvFor(context, target, 0);
        if (!rtv) return std::unexpected<mcdr::Error>(std::move(rtv.error()));
        additional = *rtv;
        if (const auto result = addAttachmentRanges(context, target, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, false); !result)
            return result;
        if (const auto result = transitionAttachmentRanges(context, target, false); !result) return result;
        retainObjectForRecording(context, target);
        targets.push_back(additional);
    }
    if (context->rasterMotionSelected) targets.push_back(context->activeMotionRtv);
    context->additionalColorTarget = target;
    context->additionalColorRtv = additional;
    context->commandList->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), FALSE,
        context->activeDepthTarget ? &context->activePassDsv : nullptr);
    context->activePassBoundColorCount = static_cast<UINT>(targets.size());
    return {};
}

mcdr::Result<void> beginRenderPass(CommandContext* ctx, Dx12Object* const* colorViews, int colorCount,
                                   const int* colorMips, const int* colorClearFlags,
                                   const float* clearColors, Dx12Object* depthView, int depthMip,
                                   int depthClearFlag, double depthClearValue, int x, int y, int w, int h) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("beginRenderPass", "beginRenderPass: no open command list");
    }
    if (ctx->inRenderPass) {
        return mcdr::err("beginRenderPass", "beginRenderPass: render pass already open");
    }
    if (colorCount < 0 || colorCount > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT ||
        (colorCount > 0 && !colorViews)) {
        return mcdr::err("beginRenderPass", "beginRenderPass: negative color count");
    }
    SampledWriteRecording writeRecording{ctx};

    ctx->activeColorTargets.clear();
    ctx->activeColorTargetsTouched.clear();
    ctx->activeDepthTarget = nullptr;
    ctx->activeAttachmentRanges.clear();
    invalidateGraphicsBindingReadCache(ctx);
    ctx->activePassRtvs.clear();
    ctx->additionalColorTarget = nullptr;
    ctx->additionalColorRtv = {};
    ctx->activePassDsv = {};
    ctx->activeMotionRtv = {};
    ctx->activeMotionTarget = nullptr;
    ctx->rasterMotionSelected = false;

    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs;
    rtvs.reserve(static_cast<size_t>(colorCount));
    for (int i = 0; i < colorCount; ++i) {
        Dx12Object* tex = colorViews[i];
        if (!tex) continue;
        if (tex->kind != Dx12Object::Kind::Texture || !(tex->usage & 8)) {
            return mcdr::err("beginRenderPass",
                             "beginRenderPass: invalid color attachment (needs RENDER_ATTACHMENT)");
        }
        int mip = colorMips ? colorMips[i] : 0;
        auto rtvRes = frameRtvFor(ctx, tex, mip);
        if (!rtvRes) return std::unexpected<mcdr::Error>(std::move(rtvRes.error()));
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = *rtvRes;
        rtvs.push_back(cpu);
        ctx->activeColorTargets.push_back(tex);
        DBG_LOG_DEBUG("beginRenderPass color[%d] tex=%p rtv=%p mip=%d dims=%ux%u fmt=%d", i, (void*)tex,
                      (void*)cpu.ptr, mip, (UINT)tex->resource->GetDesc().Width,
                      (UINT)tex->resource->GetDesc().Height, (int)tex->dxgiFormat);
        if (const auto result = addAttachmentRanges(ctx, tex, mip, D3D12_RESOURCE_STATE_RENDER_TARGET, false); !result) return result;
        if (const auto result = transitionAttachmentRanges(ctx, tex, false); !result) return result;
        retainObjectForRecording(ctx, tex);
        if (colorClearFlags && colorClearFlags[i] && clearColors) {
            for (const auto& range : ctx->activeAttachmentRanges) {
                if (range.object == tex && prepareSampledTextureWrite(ctx, tex, range.firstSubresource,
                        range.subresourceCount) != SampledTextureContentStatus::Ready)
                    return mcdr::err("beginRenderPass", "sampled texture load clear ordering failed");
            }
            const float* c = clearColors + i * 4;
            ctx->commandList->ClearRenderTargetView(cpu, c, 0, nullptr);
            static int rpDbg = 0;
            if ((++rpDbg % 60) == 1) {
                dbgLogDebug("beginRenderPass clear[%d] = (%.3f, %.3f, %.3f, %.3f)", i, static_cast<double>(c[0]),
                       static_cast<double>(c[1]), static_cast<double>(c[2]), static_cast<double>(c[3]));
            }
        }
    }

    ctx->activeColorTargetsTouched.assign(ctx->activeColorTargets.size(), false);

    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    bool hasDsv = false;
    if (depthView) {
        if (depthView->kind != Dx12Object::Kind::Texture) {
            return mcdr::err("beginRenderPass", "beginRenderPass: invalid depth attachment");
        }
        auto dsvRes = frameDsvFor(ctx, depthView, depthMip);
        if (!dsvRes) return std::unexpected<mcdr::Error>(std::move(dsvRes.error()));
        dsv = *dsvRes;
        hasDsv = true;
        ctx->activeDepthTarget = depthView;
        if (const auto result = addAttachmentRanges(ctx, depthView, depthMip, D3D12_RESOURCE_STATE_DEPTH_WRITE, true); !result) return result;
        if (const auto result = transitionAttachmentRanges(ctx, depthView, false); !result) return result;
        retainObjectForRecording(ctx, depthView);
        if (depthClearFlag) {
            if (depthView->dxgiFormat == DXGI_FORMAT_D32_FLOAT) {
                for (const auto& range : ctx->activeAttachmentRanges) {
                    if (range.object == depthView && prepareSampledTextureWrite(ctx, depthView,
                            range.firstSubresource, range.subresourceCount) != SampledTextureContentStatus::Ready)
                        return mcdr::err("beginRenderPass", "sampled depth load clear ordering failed");
                }
            }
            ctx->commandList->ClearDepthStencilView(dsv, depthClearFlags(depthView->dxgiFormat),
                                                    static_cast<FLOAT>(depthClearValue), 0, 0, nullptr);
            if (ctx->vanillaDepthSeedClear)
                ctx->vanillaDepthSeedClear(ctx->vanillaDepthSeedState, ctx, depthView,
                                           static_cast<UINT>(depthMip), depthClearValue, nullptr, dsv);
            DBG_LOG_DEBUG("beginRenderPass depth clear=explicit val=%.3f", (FLOAT)depthClearValue);
        } else {
            DBG_LOG_DEBUG("beginRenderPass depth load (no clear)");
        }
    }

    if (colorCount == 1 && rtvs.size() == 1 && colorViews[0] == ctx->rasterMotionWorldColor &&
        depthView == ctx->rasterMotionWorldDepth && ctx->rasterMotionTarget &&
        (!colorMips || colorMips[0] == 0) && depthMip == 0) {
        auto motionRtv = frameRtvFor(ctx, ctx->rasterMotionTarget, 0);
        if (!motionRtv) return std::unexpected<mcdr::Error>(std::move(motionRtv.error()));
        ctx->activeMotionRtv = *motionRtv;
        ctx->activeMotionTarget = ctx->rasterMotionTarget;
        if (const auto result = addAttachmentRanges(ctx, ctx->activeMotionTarget, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, false); !result) return result;
        if (const auto result = transitionAttachmentRanges(ctx, ctx->activeMotionTarget, false); !result) return result;
        retainObjectForRecording(ctx, ctx->rasterMotionWorldColor);
        retainObjectForRecording(ctx, ctx->rasterMotionWorldDepth);
        retainObjectForRecording(ctx, ctx->activeMotionTarget);
    }
    ctx->activePassRtvs = rtvs;
    ctx->activePassDsv = dsv;
    ctx->activePassBoundColorCount = static_cast<UINT>(rtvs.size());
    ctx->commandList->OMSetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.empty() ? nullptr : rtvs.data(),
                                         false, hasDsv ? &dsv : nullptr);
    D3D12_VIEWPORT vp{static_cast<FLOAT>(x),
                      static_cast<FLOAT>(y),
                      static_cast<FLOAT>(w),
                      static_cast<FLOAT>(h),
                      0.0f,
                      1.0f};
    ctx->commandList->RSSetViewports(1, &vp);
    D3D12_RECT scissor{x, y, x + w, y + h};
    ctx->commandList->RSSetScissorRects(1, &scissor);
    ctx->graphicsDrawState.viewport = vp;
    ctx->graphicsDrawState.scissor = scissor;
    ctx->graphicsDrawState.viewportValid = true;
    ctx->graphicsDrawState.scissorValid = true;
    ctx->inRenderPass = 1;
    DBG_LOG_DEBUG("beginRenderPass: ctx=%p colorCount=%d area=%d,%d %dx%d depth=%s", (void*)ctx, colorCount,
                  x, y, w, h, depthView ? "yes" : "no");
    for (int i = 0; i < colorCount; ++i) {
        if (colorViews[i]) {
#ifdef DX12_DEBUG_VERBOSE
            auto* tex = colorViews[i];
            auto desc = tex->resource->GetDesc();
            const float* cc = clearColors ? (clearColors + i * 4) : nullptr;
            DBG_LOG_DEBUG("beginRenderPass color[%d] tex=%p dims=%ux%u fmt=%d clear=(%.3f,%.3f,%.3f,%.3f)", i,
                          (void*)tex, (UINT)desc.Width, (UINT)desc.Height, (int)tex->dxgiFormat,
                          cc ? (double)cc[0] : 0.0, cc ? (double)cc[1] : 0.0, cc ? (double)cc[2] : 0.0,
                          cc ? (double)cc[3] : 0.0);
#endif
        }
    }
    writeRecording.completed = true;
    return {};
}

mcdr::Result<void> endRenderPass(CommandContext* ctx) {
    if (!ctx) {
        return mcdr::err("endRenderPass", "endRenderPass: null ctx");
    }
    if (!ctx->inRenderPass) return {};
    DBG_LOG_DEBUG("endRenderPass: ctx=%p", (void*)ctx);
    if (const auto result = transitionAttachmentRanges(ctx, nullptr, true); !result) return result;
    if (ctx->activeColorTargets.size() > 0) {
        for (Dx12Object* tex : ctx->activeColorTargets) {
            if (tex && tex->resource) {
                auto desc = tex->resource->GetDesc();
                DBG_LOG_DEBUG("endRenderPass colorAttach tex=%p dims=%ux%u fmt=%d touched=%d", (void*)tex,
                              (UINT)desc.Width, (UINT)desc.Height, (int)tex->dxgiFormat,
                              (int)std::distance(ctx->activeColorTargets.begin(),
                                                 std::find(ctx->activeColorTargets.begin(),
                                                           ctx->activeColorTargets.end(), tex)));
            }
        }
    }
    ctx->activeColorTargets.clear();
    ctx->activeColorTargetsTouched.clear();
    ctx->activeDepthTarget = nullptr;
    ctx->activeAttachmentRanges.clear();
    ctx->activePassRtvs.clear();
    ctx->additionalColorTarget = nullptr;
    ctx->additionalColorRtv = {};
    ctx->activePassDsv = {};
    ctx->activeMotionRtv = {};
    ctx->activeMotionTarget = nullptr;
    ctx->rasterMotionSelected = false;
    ctx->activePassBoundColorCount = UINT_MAX;
    ctx->inRenderPass = 0;
    invalidateGraphicsBindingReadCache(ctx);
    return {};
}

mcdr::Result<void*> createQueryPool(int size) {
    if (auto d = ensureDevice(); !d) return mcdr::err("createQueryPool", errorMessage(d.error()));
    if (size <= 0) {
        return mcdr::err("createQueryPool", "createQueryPool: size must be positive");
    }
    D3D12_QUERY_HEAP_DESC desc{};
    desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    desc.Count = static_cast<UINT>(size);
    auto pool = std::make_unique<QueryPool>();
    if (FAILED(gCtx.device->CreateQueryHeap(&desc, IID_PPV_ARGS(&pool->heap)))) {
        return mcdr::err("createQueryPool", "createQueryPool: CreateQueryHeap failed");
    }
    pool->size = size;
    return static_cast<void*>(pool.release());
}

void destroyQueryPool(QueryPool* pool) { delete pool; }

mcdr::Result<void> writeTimestampToPool(CommandContext* ctx, QueryPool* pool, int index) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("writeTimestampToPool", "writeTimestampToPool: no open command list");
    }
    if (!pool || index < 0 || index >= pool->size) {
        return mcdr::err("writeTimestampToPool", "writeTimestampToPool: invalid index");
    }
    ctx->commandList->EndQuery(pool->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(index));
    return {};
}

mcdr::Result<void> readQueryValue(QueryPool* pool, int index, long long& out) {
    return resolveQuery(pool, index, 1, &out);
}

mcdr::Result<void> readQueryValues(QueryPool* pool, int start, int count, long long* out) {
    return resolveQuery(pool, start, count, out);
}

unsigned long long getTimestampFrequency() { return gCtx.timestampFrequency; }

namespace {

D3D12_BLEND toD3d12BlendFactor(uint8_t f) {
    switch (f) {
        case 0: return D3D12_BLEND_BLEND_FACTOR;
        case 1: return D3D12_BLEND_BLEND_FACTOR;
        case 2: return D3D12_BLEND_DEST_ALPHA;
        case 3: return D3D12_BLEND_DEST_COLOR;
        case 4: return D3D12_BLEND_ONE;
        case 5: return D3D12_BLEND_INV_BLEND_FACTOR;
        case 6: return D3D12_BLEND_INV_BLEND_FACTOR;
        case 7: return D3D12_BLEND_INV_DEST_ALPHA;
        case 8: return D3D12_BLEND_INV_DEST_COLOR;
        case 9: return D3D12_BLEND_INV_SRC_ALPHA;
        case 10: return D3D12_BLEND_INV_SRC_COLOR;
        case 11: return D3D12_BLEND_SRC_ALPHA;
        case 12: return D3D12_BLEND_SRC_ALPHA_SAT;
        case 13: return D3D12_BLEND_SRC_COLOR;
        case 14: return D3D12_BLEND_ZERO;
        default: return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND_OP toD3d12BlendOp(uint8_t op) {
    switch (op) {
        case 0: return D3D12_BLEND_OP_ADD;
        case 1: return D3D12_BLEND_OP_SUBTRACT;
        case 2: return D3D12_BLEND_OP_REV_SUBTRACT;
        case 3: return D3D12_BLEND_OP_MIN;
        case 4: return D3D12_BLEND_OP_MAX;
        default: return D3D12_BLEND_OP_ADD;
    }
}

D3D12_COMPARISON_FUNC toD3d12Compare(uint8_t c) {
    switch (c) {
        case 0: return D3D12_COMPARISON_FUNC_ALWAYS;
        case 1: return D3D12_COMPARISON_FUNC_LESS;
        case 2: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case 3: return D3D12_COMPARISON_FUNC_EQUAL;
        case 4: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case 5: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case 6: return D3D12_COMPARISON_FUNC_GREATER;
        case 7: return D3D12_COMPARISON_FUNC_NEVER;
        default: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE toTopologyType(int t) {
    switch (t) {
        case 0:
        case 1:
        case 2: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case 3: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

D3D12_PRIMITIVE_TOPOLOGY toPrimitiveTopology(int t) {
    switch (t) {
        case 0:
        case 1: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case 2: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case 3: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        case 4: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case 5: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case 6: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLEFAN;
        case 7: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

}

bool ensureComputeHeap(CommandContext* ctx, UINT required) {
    if (!ctx || required == 0) return false;
    if (ctx->resourceComputeHeap && ctx->resourceComputeNext <= ctx->resourceComputeCapacity
        && required <= ctx->resourceComputeCapacity - ctx->resourceComputeNext) {
        return true;
    }
    const UINT oldCapacity = ctx->resourceComputeCapacity;
    if (required > UINT_MAX - ctx->resourceComputeNext) return false;
    const UINT requested = ctx->resourceComputeNext + required;
    const UINT doubled = oldCapacity <= UINT_MAX / 2 ? oldCapacity * 2u : UINT_MAX;
    const UINT capacity = std::max(requested, std::max(64u, doubled));
    if (capacity == UINT_MAX) return false;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = capacity;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    auto page = std::make_shared<ComPtr<ID3D12DescriptorHeap>>();
    if (FAILED(gCtx.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(page->GetAddressOf())))) {
        return false;
    }
    ctx->recordingLeases[ctx->currentListSlot].push_back(page);
    ctx->resourceComputeHeap = *page;
    ctx->resourceComputeCapacity = capacity;
    ctx->resourceComputeNext = 0;
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE computeHeapCpu(CommandContext* ctx, UINT slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = ctx->resourceComputeHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot)
        * gCtx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE computeHeapGpu(CommandContext* ctx, UINT slot) {
    D3D12_GPU_DESCRIPTOR_HANDLE h = ctx->resourceComputeHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(slot)
        * gCtx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return h;
}

mcdr::Result<void*> createComputePipeline(const void* dxil, int32_t dxilSize) {
    if (auto d = ensureDevice(); !d) return mcdr::err("createComputePipeline", errorMessage(d.error()));
    if (dxil == nullptr || dxilSize <= 0) {
        return mcdr::err("createComputePipeline", "createComputePipeline: empty DXIL bytecode");
    }

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &uavRange;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.RegisterSpace = 0;
    params[1].Constants.Num32BitValues = 8;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 0;
    rsDesc.pStaticSamplers = nullptr;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> rsBlob, rsErr;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &rsErr);
    if (FAILED(hr)) {
        return mcdr::err("createComputePipeline",
                         "createComputePipeline: D3D12SerializeRootSignature hr=0x" + formatBackendResult(hr));
    }

    auto obj = std::make_unique<Dx12Object>();
    obj->kind = Dx12Object::Kind::ComputePipeline;
    obj->size = dxilSize;
    hr = gCtx.device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                          IID_PPV_ARGS(&obj->computeRootSignature));
    if (FAILED(hr)) {
        return mcdr::err("createComputePipeline",
                         "createComputePipeline: CreateRootSignature hr=0x" + formatBackendResult(hr));
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = obj->computeRootSignature.Get();
    psoDesc.CS.pShaderBytecode = dxil;
    psoDesc.CS.BytecodeLength = static_cast<SIZE_T>(dxilSize);
    psoDesc.NodeMask = 0;
    psoDesc.CachedPSO.pCachedBlob = nullptr;
    psoDesc.CachedPSO.CachedBlobSizeInBytes = 0;
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    hr = gCtx.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&obj->computePipelineState));
    if (FAILED(hr)) {
        std::string text = "createComputePipeline: CreateComputePipelineState hr=" + formatBackendResult(hr);
        char magic[5] = {0, 0, 0, 0, 0};
        if (dxilSize >= 4) {
            std::memcpy(magic, dxil, 4);
        }
        char headText[128];
        std::snprintf(headText, sizeof(headText),
                      " dxil=%d magic=[%c%c%c%c] rootSig=%p", dxilSize, magic[0], magic[1], magic[2],
                      magic[3], static_cast<void*>(psoDesc.pRootSignature));
        text += headText;
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET
            || hr == DXGI_ERROR_DEVICE_HUNG) {
            text += " — " + deviceStatusText();
        }
        dumpInfoQueueMessages();
        return mcdr::err("createComputePipeline", text);
    }

    dbgLogInfo("createComputePipeline: PSO handle=%p dxil=%d bytes (descriptor table u0 + 8 root constants b0)",
               reinterpret_cast<void*>(obj.get()), dxilSize);
    return static_cast<void*>(obj.release());
}

mcdr::Result<void*> createTerrainTileBinningPipeline(const void* dxil, int32_t dxilSize) {
    if (auto d = ensureDevice(); !d) return mcdr::err("createTerrainTileBinningPipeline", errorMessage(d.error()));
    if (dxil == nullptr || dxilSize <= 0)
        return mcdr::err("createTerrainTileBinningPipeline", "empty DXIL bytecode");

    D3D12_ROOT_PARAMETER params[8]{};
    const auto setCbv = [](D3D12_ROOT_PARAMETER& parameter, UINT shaderRegister) {
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameter.Descriptor.ShaderRegister = shaderRegister;
        parameter.Descriptor.RegisterSpace = 3;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    };
    const auto setSrv = [](D3D12_ROOT_PARAMETER& parameter, UINT shaderRegister) {
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameter.Descriptor.ShaderRegister = shaderRegister;
        parameter.Descriptor.RegisterSpace = 3;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    };
    const auto setUav = [](D3D12_ROOT_PARAMETER& parameter, UINT shaderRegister) {
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameter.Descriptor.ShaderRegister = shaderRegister;
        parameter.Descriptor.RegisterSpace = 3;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    };
    setCbv(params[0], 0);
    setCbv(params[1], 1);
    setCbv(params[2], 2);
    setSrv(params[3], 8);
    setUav(params[4], 0);
    setUav(params[5], 1);
    setUav(params[6], 2);
    setUav(params[7], 3);

    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = static_cast<UINT>(std::size(params));
    rootDescription.pParameters = params;
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    HRESULT result = D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &errors);
    if (FAILED(result))
        return mcdr::err("createTerrainTileBinningPipeline", "Root signature serialization failed hr=0x" +
            formatBackendResult(result));

    auto object = std::make_unique<Dx12Object>();
    object->kind = Dx12Object::Kind::ComputePipeline;
    object->size = dxilSize;
    object->terrainTileBinning = true;
    result = gCtx.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&object->computeRootSignature));
    if (FAILED(result))
        return mcdr::err("createTerrainTileBinningPipeline", "Root signature creation failed hr=0x" +
            formatBackendResult(result));

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = object->computeRootSignature.Get();
    pipeline.CS.pShaderBytecode = dxil;
    pipeline.CS.BytecodeLength = static_cast<SIZE_T>(dxilSize);
    result = gCtx.device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&object->computePipelineState));
    if (FAILED(result)) {
        dumpInfoQueueMessages();
        return mcdr::err("createTerrainTileBinningPipeline", "Compute PSO creation failed hr=0x" +
            formatBackendResult(result));
    }
    dbgLogInfo("createTerrainTileBinningPipeline: PSO handle=%p dxil=%d bytes (space3 b0/b1/b2 + t8 + u0..u3)",
        static_cast<void*>(object.get()), dxilSize);
    return static_cast<void*>(object.release());
}

int dispatchCompute(CommandContext* ctx, Dx12Object* pipeline, Dx12Object* uavTexture,
                    const int32_t* constants, int32_t constantCount, int32_t x, int32_t y, int32_t z) {
    if (ctx == nullptr || !ctx->commandList || !ctx->listOpen) {
        dbgLogError("dispatchCompute: called without an open command list");
        return -2;
    }
    if (pipeline == nullptr || pipeline->kind != Dx12Object::Kind::ComputePipeline
        || !pipeline->computePipelineState || !pipeline->computeRootSignature) {
        dbgLogError("dispatchCompute: handle is not a compute pipeline");
        return 0;
    }
    if (uavTexture == nullptr || uavTexture->kind != Dx12Object::Kind::Texture
        || !uavTexture->resource) {
        dbgLogError("dispatchCompute: UAV target is not a texture");
        return 0;
    }
    if ((uavTexture->usage & 32) == 0) {
        dbgLogError("dispatchCompute: texture lacks usage bit 32 (ALLOW_UNORDERED_ACCESS), usage=0x%x",
                    uavTexture->usage);
        return 0;
    }
    if (x <= 0 || y <= 0 || z <= 0) {
        dbgLogError("dispatchCompute: invalid thread-group counts %d,%d,%d", x, y, z);
        return 0;
    }
    constexpr int32_t kMaxConstants = 8;
    if (constantCount < 0 || constantCount > kMaxConstants
        || (constantCount > 0 && constants == nullptr)) {
        dbgLogError("dispatchCompute: constantCount %d out of range (0..%d)", constantCount, kMaxConstants);
        return 0;
    }

    ID3D12GraphicsCommandList* list = ctx->commandList;
    if (!ensureComputeHeap(ctx, 1)) {
        return 0;
    }
    if (prepareSampledTextureWrite(ctx, uavTexture, 0, 1) != SampledTextureContentStatus::Ready) return 0;
    const UINT uavSlot = ctx->resourceComputeNext++;
    const D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = computeHeapCpu(ctx, uavSlot);
    const D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = computeHeapGpu(ctx, uavSlot);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = uavTexture->dxgiFormat;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    uavDesc.Texture2D.PlaneSlice = 0;
    gCtx.device->CreateUnorderedAccessView(uavTexture->resource.Get(), nullptr, &uavDesc, uavCpu);

    transitionTextureTo(ctx, uavTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetPipelineState(pipeline->computePipelineState.Get());
    list->SetComputeRootSignature(pipeline->computeRootSignature.Get());
    ID3D12DescriptorHeap* heaps[1] = {ctx->resourceComputeHeap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootDescriptorTable(0, uavGpu);
    if (constantCount > 0) {
        list->SetComputeRoot32BitConstants(1, static_cast<UINT>(constantCount), constants, 0);
    }
    list->Dispatch(static_cast<UINT>(x), static_cast<UINT>(y), static_cast<UINT>(z));
    transitionTextureTo(ctx, uavTexture, D3D12_RESOURCE_STATE_COMMON);
    restoreDrawHeap(ctx);
    return 1;
}

mcdr::Result<void*> createComputePipelineShadingRate(const void* dxil, int32_t dxilSize) {
    if (auto d = ensureDevice(); !d) return mcdr::err("createComputePipelineShadingRate", errorMessage(d.error()));
    if (dxil == nullptr || dxilSize <= 0) {
        return mcdr::err("createComputePipelineShadingRate",
                         "createComputePipelineShadingRate: empty DXIL bytecode");
    }

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 3;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &srvRange;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &uavRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 0;
    params[2].Constants.RegisterSpace = 0;
    params[2].Constants.Num32BitValues = 8;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 0;
    rsDesc.pStaticSamplers = nullptr;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> rsBlob, rsErr;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &rsErr);
    if (FAILED(hr)) {
        return mcdr::err("createComputePipelineShadingRate",
                         "createComputePipelineShadingRate: D3D12SerializeRootSignature hr=0x" + formatBackendResult(hr));
    }

    auto obj = std::make_unique<Dx12Object>();
    obj->kind = Dx12Object::Kind::ComputePipeline;
    obj->size = dxilSize;
    hr = gCtx.device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                          IID_PPV_ARGS(&obj->computeRootSignature));
    if (FAILED(hr)) {
        return mcdr::err("createComputePipelineShadingRate",
                         "createComputePipelineShadingRate: CreateRootSignature hr=0x" + formatBackendResult(hr));
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = obj->computeRootSignature.Get();
    psoDesc.CS.pShaderBytecode = dxil;
    psoDesc.CS.BytecodeLength = static_cast<SIZE_T>(dxilSize);
    psoDesc.NodeMask = 0;
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    hr = gCtx.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&obj->computePipelineState));
    if (FAILED(hr)) {
        std::string text =
            "createComputePipelineShadingRate: CreateComputePipelineState hr=0x" + formatBackendResult(hr);
        char head[96];
        std::snprintf(head, sizeof(head), " dxil=%d", dxilSize);
        text += head;
        dumpInfoQueueMessages();
        return mcdr::err("createComputePipelineShadingRate", text);
    }

    dbgLogInfo("createComputePipelineShadingRate: PSO handle=%p dxil=%d bytes (srv t0/t1/t2 + uav u0 + b0)",
               static_cast<void*>(obj.get()), dxilSize);
    return static_cast<void*>(obj.release());
}

int dispatchComputeShadingRate(CommandContext* ctx, Dx12Object* pipeline, Dx12Object* srvDepth,
                              Dx12Object* srvMotion, Dx12Object* srvPrevious, Dx12Object* image,
                              const int32_t* constants, int32_t constantCount, int32_t x, int32_t y,
                              int32_t z) {
    if (ctx == nullptr || !ctx->commandList || !ctx->listOpen) {
        dbgLogError("dispatchComputeShadingRate: called without an open command list");
        return -2;
    }
    if (pipeline == nullptr || pipeline->kind != Dx12Object::Kind::ComputePipeline
        || !pipeline->computePipelineState || !pipeline->computeRootSignature) {
        dbgLogError("dispatchComputeShadingRate: handle is not a compute pipeline");
        return 0;
    }
    if (srvDepth == nullptr || srvMotion == nullptr || srvPrevious == nullptr || image == nullptr
        || !image->resource || image->kind != Dx12Object::Kind::Texture) {
        dbgLogError("dispatchComputeShadingRate: depth/motion/previous view or image missing");
        return 0;
    }
    if ((image->usage & 32) == 0) {
        dbgLogError("dispatchComputeShadingRate: image lacks usage bit 32, usage=0x%x", image->usage);
        return 0;
    }
    constexpr int32_t kMaxConstants = 8;
    if (constantCount < 0 || constantCount > kMaxConstants
        || (constantCount > 0 && constants == nullptr)) {
        dbgLogError("dispatchComputeShadingRate: constantCount %d out of range (0..%d)", constantCount,
                    kMaxConstants);
        return 0;
    }
    if (x <= 0 || y <= 0 || z <= 0) {
        dbgLogError("dispatchComputeShadingRate: invalid thread-group counts %d,%d,%d", x, y, z);
        return 0;
    }

    if (!ensureComputeHeap(ctx, 4)) return 0;
    ID3D12GraphicsCommandList* list = ctx->commandList;

    if (prepareSampledTextureWrite(ctx, image, 0, 1) != SampledTextureContentStatus::Ready) return 0;
    const UINT srvSlot = ctx->resourceComputeNext;
    const UINT uavSlot = srvSlot + 3;
    ctx->resourceComputeNext = srvSlot + 4;
    const D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = computeHeapCpu(ctx, srvSlot);
    const D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = computeHeapCpu(ctx, uavSlot);
    if (srvDepth->cpuHandle.ptr != 0) {
        gCtx.device->CopyDescriptorsSimple(1, srvCpu, srvDepth->cpuHandle,
                                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    if (srvMotion->cpuHandle.ptr != 0) {
        D3D12_CPU_DESCRIPTOR_HANDLE second{};
        second = computeHeapCpu(ctx, srvSlot + 1);
        gCtx.device->CopyDescriptorsSimple(1, second, srvMotion->cpuHandle,
                                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    if (srvPrevious->cpuHandle.ptr != 0) {
        D3D12_CPU_DESCRIPTOR_HANDLE third{};
        third = computeHeapCpu(ctx, srvSlot + 2);
        gCtx.device->CopyDescriptorsSimple(1, third, srvPrevious->cpuHandle,
                                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    } else {
        dbgLogError("dispatchComputeShadingRate: previous image view has no CPU handle");
        ctx->recordingFailed.store(true, std::memory_order_release);
        return 0;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = image->dxgiFormat;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    uavDesc.Texture2D.PlaneSlice = 0;
    gCtx.device->CreateUnorderedAccessView(image->resource.Get(), nullptr, &uavDesc, uavCpu);

    if (srvDepth->sourceTexture && srvDepth->sourceTexture->resource) {
        transitionTo(ctx, srvDepth->sourceTexture->resource.Get(), D3D12_RESOURCE_STATE_COMMON,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (srvMotion->sourceTexture && srvMotion->sourceTexture->resource) {
        transitionTo(ctx, srvMotion->sourceTexture->resource.Get(), D3D12_RESOURCE_STATE_COMMON,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (srvPrevious->sourceTexture && srvPrevious->sourceTexture->resource) {
        transitionTo(ctx, srvPrevious->sourceTexture->resource.Get(), D3D12_RESOURCE_STATE_COMMON,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    transitionTextureTo(ctx, image, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    list->SetPipelineState(pipeline->computePipelineState.Get());
    list->SetComputeRootSignature(pipeline->computeRootSignature.Get());
    ID3D12DescriptorHeap* heaps[1] = {ctx->resourceComputeHeap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootDescriptorTable(0, computeHeapGpu(ctx, srvSlot));
    list->SetComputeRootDescriptorTable(1, computeHeapGpu(ctx, uavSlot));
    if (constantCount > 0) {
        list->SetComputeRoot32BitConstants(2, static_cast<UINT>(constantCount), constants, 0);
    }
    list->Dispatch(static_cast<UINT>(x), static_cast<UINT>(y), static_cast<UINT>(z));
    transitionTextureTo(ctx, image, D3D12_RESOURCE_STATE_COMMON);
    restoreDrawHeap(ctx);
    return 1;
}

mcdr::Result<void*> createShadingRateImage(int32_t width, int32_t height) {
    if (auto d = ensureDevice(); !d) return mcdr::err("createShadingRateImage", errorMessage(d.error()));
    if (!gCtx.caps.shadingRateImageSupported()) {
        return mcdr::err("createShadingRateImage", "createShadingRateImage: device is below VRS tier 2");
    }
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return mcdr::err("createShadingRateImage", "createShadingRateImage: bad tile dimensions");
    }
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = static_cast<UINT64>(width);
    desc.Height = static_cast<UINT>(height);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8_UINT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto obj = std::make_unique<Dx12Object>();
    obj->kind = Dx12Object::Kind::Texture;
    obj->usage = 38;
    obj->dxgiFormat = DXGI_FORMAT_R8_UINT;
    obj->size = static_cast<long long>(width) * height;
    HRESULT hr = gCtx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(&obj->resource));
    if (FAILED(hr)) {
        return mcdr::err("createShadingRateImage",
                         "createShadingRateImage: CreateCommittedResource hr=0x" + formatBackendResult(hr));
    }
    dbgLogInfo("createShadingRateImage: handle=%p %dx%d tiles (tile=%u)", static_cast<void*>(obj.get()),
               width, height, gCtx.caps.shadingRateImageTileSize);
    return static_cast<void*>(obj.release());
}

namespace {
void setCommandShadingRateImage(CommandContext* ctx, ID3D12GraphicsCommandList5* list5, Dx12Object* image) {
    if (image == nullptr) {
        list5->RSSetShadingRateImage(nullptr);
        if (ctx->shadingRateImageBound != nullptr && ctx->shadingRateImageBound->resource) {
            transitionTo(ctx, ctx->shadingRateImageBound->resource.Get(),
                         D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            ctx->shadingRateImageBound = nullptr;
        }
        return;
    }
    if (ctx->shadingRateImageBound != nullptr && ctx->shadingRateImageBound != image
        && ctx->shadingRateImageBound->resource) {
        transitionTo(ctx, ctx->shadingRateImageBound->resource.Get(),
                     D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        ctx->shadingRateImageBound = nullptr;
    }
    retainObjectForRecording(ctx, image);
    transitionTo(ctx, image->resource.Get(), D3D12_RESOURCE_STATE_COMMON,
                 D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
    ctx->shadingRateImageBound = image;
    list5->RSSetShadingRateImage(image->resource.Get());
    static bool logged = false;
    if (!logged) {
        logged = true;
        dbgLogInfo("setShadingRateImage: RSSetShadingRateImage bound");
    }
}
}

int setShadingRateImage(CommandContext* ctx, Dx12Object* image) {
    if (ctx == nullptr || !ctx->commandList || !ctx->listOpen) {
        dbgLogError("setShadingRateImage: called without an open command list");
        return -2;
    }
    if (image && (image->kind!=Dx12Object::Kind::Texture || !image->resource)) {
        dbgLogError("setShadingRateImage: handle is not a texture");
        return -1;
    }
    if (!gCtx.caps.shadingRateImageSupported()) return 0;
    ComPtr<ID3D12GraphicsCommandList5> list5;
    if (FAILED(ctx->commandList->QueryInterface(IID_PPV_ARGS(&list5))) || !list5) {
        dbgLogWarn("setShadingRateImage: command list is not ID3D12GraphicsCommandList5");
        return 0;
    }
    setCommandShadingRateImage(ctx,list5.Get(),image);
    return 1;
}

mcdr::Result<void*> createGraphicsPipeline(const PipelineDesc& desc) {
    if (auto d = ensureDevice(); !d) return mcdr::err("createGraphicsPipeline", errorMessage(d.error()));
    if (desc.captureHash) {
        if (const auto supported = requireCaptureBarycentrics(); !supported)
            return std::unexpected(supported.error());
    }
    if (desc.colorCount < 0 || desc.colorCount > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT ||
        desc.colorTargets.size() < static_cast<size_t>(desc.colorCount)) {
        return mcdr::err("createGraphicsPipeline", "invalid color target count");
    }
    if (desc.rasterMotion) {
        if (desc.colorCount < 2 ||
            toDxgiFormat(desc.colorTargets[desc.colorCount - 1].format) != DXGI_FORMAT_R32G32B32A32_FLOAT ||
            desc.colorTargets[desc.colorCount - 1].writeMask != D3D12_COLOR_WRITE_ENABLE_ALL)
            return mcdr::err("createGraphicsPipeline", "raster motion requires a world target and a final full RGBA32_FLOAT MRT");
        const auto& motion = desc.colorTargets[desc.colorCount - 1];
        if (motion.blendEnabled) {
            if (motion.srcColor != 11 || motion.dstColor != 9 || motion.colorOp != 0 ||
                motion.srcAlpha != 11 || motion.dstAlpha != 9 || motion.alphaOp != 0)
                return mcdr::err("createGraphicsPipeline", "raster motion only supports source-alpha gated blending");
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
            support.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            if (FAILED(gCtx.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
                (support.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE) == 0)
                return mcdr::err("createGraphicsPipeline", "RGBA32_FLOAT motion blending is unavailable on this device");
        }
    }

    std::vector<uint8_t> vsBytes = desc.vsBytes;
    std::vector<uint8_t> psBytes = desc.psBytes;
    {
        static int hlslDump = 0;
        if (hlslDump < 2) {
            ++hlslDump;
            std::string vsStr(reinterpret_cast<const char*>(vsBytes.data()), vsBytes.size());
            std::string psStr(reinterpret_cast<const char*>(psBytes.data()), psBytes.size());
            dbgLogDebug("=== HLSL #%d vs (%zuB) ===\n%s", hlslDump, vsBytes.size(), vsStr.c_str());
            dbgLogDebug("=== HLSL #%d ps (%zuB) ===\n%s", hlslDump, psBytes.size(), psStr.c_str());
        }
    }
    const std::string smProfile = graphicsShaderModelProfile();
    if (smProfile.empty()) {
        return mcdr::err("createGraphicsPipeline", "createGraphicsPipeline: " + sm69RequirementText());
    }
    std::vector<uint8_t> vsDxil, psDxil;
    const std::string vsSrc(reinterpret_cast<const char*>(vsBytes.data()), vsBytes.size());
    const std::string psSrc(reinterpret_cast<const char*>(psBytes.data()), psBytes.size());
    auto vsDxc = compileHlslToDxil(vsSrc, "main", "vs_" + smProfile, vsDxil);
    if (!vsDxc) {
        return mcdr::err("createGraphicsPipeline",
                         "vs_" + smProfile + " (DXC): " + firstLine(vsDxc.error().message));
    }
    auto psDxc = compileHlslToDxil(psSrc, "main", "ps_" + smProfile, psDxil);
    if (!psDxc) {
        return mcdr::err("createGraphicsPipeline",
                         "ps_" + smProfile + " (DXC): " + firstLine(psDxc.error().message));
    }
    const void* vsCode = vsDxil.data();
    SIZE_T vsCodeSize = vsDxil.size();
    const void* psCode = psDxil.data();
    SIZE_T psCodeSize = psDxil.size();
    static bool loggedDxcGraphics = false;
    if (!loggedDxcGraphics) {
        loggedDxcGraphics = true;
        dbgLogInfo("createGraphicsPipeline: main graphics pipeline compiled with DXC SM %s (DXIL vs=%zuB ps=%zuB)",
                   smProfile.c_str(), vsDxil.size(), psDxil.size());
    }

    if (desc.captureHash) {
        if (const auto capture=validateCapturePixelShader(psDxil.data(),static_cast<uint32_t>(psDxil.size())); !capture)
            return std::unexpected(capture.error());
    }
    std::vector<uint8_t> cbvRegs;
    std::vector<D3D12_DESCRIPTOR_RANGE> srvRanges;
    std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;
    cbvRegs.reserve(desc.bindings.size());
    srvRanges.reserve(desc.bindings.size());
    for (const PipelineDesc::Binding& b : desc.bindings) {
        if (b.type == 0) {
            cbvRegs.push_back(b.reg);
            continue;
        }
        D3D12_DESCRIPTOR_RANGE r{};
        r.NumDescriptors = 1;
        r.RegisterSpace = 0;
        r.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        r.BaseShaderRegister = b.reg;
        r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges.push_back(r);
        if (b.type == 1) {
            D3D12_STATIC_SAMPLER_DESC s{};
            s.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            s.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            s.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            s.MipLODBias = 0.0f;
            s.MaxAnisotropy = 1;
            s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            s.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
            s.MinLOD = 0.0f;
            s.MaxLOD = D3D12_FLOAT32_MAX;
            s.ShaderRegister = b.reg;
            s.RegisterSpace = 0;
            s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            staticSamplers.push_back(s);
        }
    }
    std::vector<D3D12_ROOT_PARAMETER> params;
    params.reserve(cbvRegs.size() + 1);
    for (uint8_t reg : cbvRegs) {
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p.Descriptor.ShaderRegister = reg;
        p.Descriptor.RegisterSpace = 0;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    if (!srvRanges.empty()) {
        D3D12_ROOT_DESCRIPTOR_TABLE table{};
        table.NumDescriptorRanges = static_cast<UINT>(srvRanges.size());
        table.pDescriptorRanges = srvRanges.data();
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable = table;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = static_cast<UINT>(params.size());
    rsDesc.pParameters = params.empty() ? nullptr : params.data();
    rsDesc.NumStaticSamplers = static_cast<UINT>(staticSamplers.size());
    rsDesc.pStaticSamplers = staticSamplers.empty() ? nullptr : staticSamplers.data();
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (desc.pushConstantBytes > 128 || desc.pushConstantBytes % 4 != 0 ||
        cbvRegs.size() * 2 + (srvRanges.empty() ? 0 : 1) + desc.pushConstantBytes / 4 + (desc.captureHash ? 4 : 0) > 64) {
        return mcdr::err("createGraphicsPipeline", "root constant size or root DWORD budget exceeded");
    }
    const UINT constantParam = static_cast<UINT>(params.size());
    if (desc.pushConstantBytes != 0) {
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants = {0, 1, desc.pushConstantBytes / 4};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(parameter);
        rsDesc.NumParameters = static_cast<UINT>(params.size());
        rsDesc.pParameters = params.data();
    }

    D3D12_DESCRIPTOR_RANGE captureRanges[2]{};
    UINT captureParam = UINT_MAX;
    if (desc.captureHash != 0) {
        if (desc.captureHash != mcdr::oit_shader::contractHash || !desc.hasDepth || desc.depthWrite ||
            desc.psBytes.empty() || desc.rasterMotion || desc.captureGeneration == 0)
            return mcdr::err("createGraphicsPipeline", "Capture requires a matching shader contract and read-only depth");
        captureParam = mcdr::graphics_capture::appendParameters(params, captureRanges);
        rsDesc.NumParameters = static_cast<UINT>(params.size());
        rsDesc.pParameters = params.data();
    }
    ComPtr<ID3DBlob> rsBlob, rsErr;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &rsErr);
    if (FAILED(hr)) {
        return mcdr::err("createGraphicsPipeline",
                         "createGraphicsPipeline: D3D12SerializeRootSignature hr=0x" + formatBackendResult(hr));
    }
    ComPtr<ID3D12RootSignature> rootSig;
    hr = gCtx.device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                          IID_PPV_ARGS(&rootSig));
    if (FAILED(hr)) {
        return mcdr::err("createGraphicsPipeline",
                         "createGraphicsPipeline: CreateRootSignature hr=0x" + formatBackendResult(hr));
    }

    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;
    inputLayout.reserve(desc.inputElements.size());
    std::vector<std::string> semanticStore;
    semanticStore.reserve(desc.inputElements.size());
    std::string inputDescStr;
    for (const PipelineDesc::InputElement& el : desc.inputElements) {
        D3D12_INPUT_ELEMENT_DESC ie{};
        std::string baseName;
        UINT semanticIndex = 0;
        if (!el.semanticName.empty()) {
            const std::string& sn = el.semanticName;
            size_t lastNonDigit = sn.find_last_not_of("0123456789");
            if (lastNonDigit == std::string::npos) {
                baseName = "";
            } else {
                baseName = sn.substr(0, lastNonDigit + 1);
                if (lastNonDigit + 1 < sn.size()) {
                    semanticIndex = static_cast<UINT>(std::stoi(sn.substr(lastNonDigit + 1)));
                }
            }
        } else {
            baseName = "TEXCOORD";
            semanticIndex = static_cast<UINT>(el.location);
        }
        semanticStore.push_back(baseName);
        ie.SemanticName = semanticStore.back().c_str();
        ie.SemanticIndex = semanticIndex;
        ie.Format = toDxgiVertexFormat(el.format);
        auto dxgiFmtName = [](DXGI_FORMAT f) -> const char* {
            switch (f) {
                case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
                case DXGI_FORMAT_R8_SNORM: return "R8_SNORM";
                case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
                case DXGI_FORMAT_R8G8_SNORM: return "R8G8_SNORM";
                case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
                case DXGI_FORMAT_R8G8B8A8_SNORM: return "R8G8B8A8_SNORM";
                case DXGI_FORMAT_R16_UNORM: return "R16_UNORM";
                case DXGI_FORMAT_R16G16_UNORM: return "R16G16_UNORM";
                case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
                case DXGI_FORMAT_R16G16B16A16_UINT: return "R16G16B16A16_UINT";
                case DXGI_FORMAT_R32_UINT: return "R32_UINT";
                case DXGI_FORMAT_R32G32_UINT: return "R32G32_UINT";
                case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
                case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
                case DXGI_FORMAT_R32G32B32_FLOAT: return "R32G32B32_FLOAT";
                case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
                case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
                case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
                case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
                case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
                case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
                case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
                case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
                case DXGI_FORMAT_R8G8B8A8_UINT: return "R8G8B8A8_UINT";
                case DXGI_FORMAT_R8G8B8A8_SINT: return "R8G8B8A8_SINT";
                default: return "UNKNOWN";
            }
        };
        dbgLogDebug("ord=%d -> dxgi_fmt=%u (%s) semantic=%s", el.format,
                     static_cast<unsigned>(ie.Format), dxgiFmtName(ie.Format), ie.SemanticName);
        ie.InputSlot = static_cast<UINT>(el.binding);
        ie.AlignedByteOffset = static_cast<UINT>(el.offset);
        if (el.stepRate > 0) {
            ie.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
            ie.InstanceDataStepRate = static_cast<UINT>(el.stepRate);
        } else {
            ie.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            ie.InstanceDataStepRate = 0;
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "  loc=%d bind=%d fmt=%d off=%d stride=%d step=%d semantic=%s idx=%u\n",
                 el.location, el.binding, el.format, el.offset, el.stride, el.stepRate,
                 el.semanticName.empty() ? "(fallback TEXCOORD)" : el.semanticName.c_str(), semanticIndex);
        inputDescStr += buf;
        inputLayout.push_back(ie);
    }
    auto ordinalByteSize = [](int ord) -> UINT {
        switch (ord) {
            case 36: return 12u;
            case 37: return 12u;
            case 45: return 8u;
            case 46: return 12u;
            case 47: return 16u;
            case 44: return 4u;
            case 40: return 2u;
            case 41: return 4u;
            case 42: return 8u;
            case 43: return 8u;
            case 48: return 4u;
            case 49: return 4u;
            case 50: return 4u;
            case 0: return 1u;
            case 1: return 1u;
            case 2: return 2u;
            case 3: return 2u;
            case 4: return 4u;
            case 5: return 4u;
            case 6: return 4u;
            case 7: return 4u;
            case 8: return 2u;
            case 9: return 2u;
            case 10: return 4u;
            case 11: return 4u;
            case 12: return 8u;
            case 13: return 8u;
            case 14: return 8u;
            case 15: return 8u;
            case 16: return 1u;
            case 17: return 1u;
            case 18: return 2u;
            case 19: return 2u;
            case 20: return 4u;
            case 21: return 4u;
            case 22: return 4u;
            case 23: return 4u;
            case 24: return 2u;
            case 25: return 2u;
            case 26: return 4u;
            case 27: return 4u;
            case 28: return 8u;
            case 29: return 8u;
            case 30: return 8u;
            case 31: return 8u;
            case 32: return 4u;
            case 33: return 4u;
            case 34: return 8u;
            case 35: return 8u;
            case 38: return 16u;
            case 39: return 16u;
            case 51: return 4u;
            case 52: return 4u;
            case 53: return 4u;
            case 54: return 2u;
            default: return 4u;
        }
    };
    UINT slotStride[16] = {};
    bool anyInputSlot = false;
    for (const PipelineDesc::InputElement& el : desc.inputElements) {
        if (el.binding < 0 || el.binding >= 16) continue;
        const UINT end = static_cast<UINT>(el.offset) + ordinalByteSize(el.format);
        const UINT candidate = el.stride > 0 ? static_cast<UINT>(el.stride) : end;
        if (candidate > slotStride[el.binding]) slotStride[el.binding] = candidate;
        anyInputSlot = true;
    }
    if (anyInputSlot) {
        semanticStore.clear();
        semanticStore.reserve(desc.inputElements.size());
        inputDescStr.clear();
        inputLayout.clear();
        for (size_t k = 0; k < desc.inputElements.size(); ++k) {
            const PipelineDesc::InputElement& el = desc.inputElements[k];
            D3D12_INPUT_ELEMENT_DESC ie{};
            std::string baseName;
            UINT semanticIndex = 0;
            if (!el.semanticName.empty()) {
                const std::string& sn = el.semanticName;
                size_t lastNonDigit = sn.find_last_not_of("0123456789");
                if (lastNonDigit == std::string::npos) {
                    baseName = "";
                } else {
                    baseName = sn.substr(0, lastNonDigit + 1);
                    if (lastNonDigit + 1 < sn.size()) {
                        semanticIndex = static_cast<UINT>(std::stoi(sn.substr(lastNonDigit + 1)));
                    }
                }
            } else {
                baseName = "TEXCOORD";
                semanticIndex = static_cast<UINT>(el.location);
            }
            semanticStore.push_back(baseName);
            ie.SemanticName = semanticStore.back().c_str();
            ie.SemanticIndex = semanticIndex;
            ie.Format = toDxgiVertexFormat(el.format);
            dbgLogDebug("ord=%d -> dxgi_fmt=%u semantic=%s", el.format,
                         static_cast<unsigned>(ie.Format), ie.SemanticName);
            ie.InputSlot = static_cast<UINT>(el.binding);
            ie.AlignedByteOffset = static_cast<UINT>(el.offset);
            if (el.stepRate > 0) {
                ie.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
                ie.InstanceDataStepRate = static_cast<UINT>(el.stepRate);
            } else {
                ie.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                ie.InstanceDataStepRate = 0;
            }
            char buf[128];
            snprintf(
                buf, sizeof(buf), "  loc=%d bind=%d fmt=%d off=%d stride=%d step=%d semantic=%s idx=%u\n",
                el.location, el.binding, el.format, el.offset,
                (el.binding >= 0 && el.binding < 16) ? slotStride[el.binding] : 0u, el.stepRate,
                el.semanticName.empty() ? "(fallback TEXCOORD)" : el.semanticName.c_str(), semanticIndex);
            inputDescStr += buf;
            inputLayout.push_back(ie);
        }
    }
    dbgLogInfo("createGraphicsPipeline: inputElements=%zu\n%s", desc.inputElements.size(),
               inputDescStr.c_str());

    D3D12_BLEND_DESC blend{};
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = TRUE;
    int numRT = 0;
    for (int i = 0; i < desc.colorCount; ++i) {
        const PipelineDesc::ColorTarget& ct = desc.colorTargets[i];
        if (ct.format < 0) continue;
        numRT = i + 1;
        D3D12_RENDER_TARGET_BLEND_DESC& rt = blend.RenderTarget[i];
        rt.BlendEnable = ct.blendEnabled;
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend = toD3d12BlendFactor(ct.srcColor);
        rt.DestBlend = toD3d12BlendFactor(ct.dstColor);
        rt.BlendOp = toD3d12BlendOp(ct.colorOp);
        rt.SrcBlendAlpha = toD3d12BlendFactor(ct.srcAlpha);
        rt.DestBlendAlpha = toD3d12BlendFactor(ct.dstAlpha);
        rt.BlendOpAlpha = toD3d12BlendOp(ct.alphaOp);
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = ct.writeMask;
    }

    D3D12_RASTERIZER_DESC rs{};
    rs.FillMode = (desc.polygonMode == 1) ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    rs.CullMode = desc.cullEnabled ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    rs.FrontCounterClockwise = TRUE;
    rs.DepthBias = 0;
    rs.DepthBiasClamp = 0.0f;
    rs.SlopeScaledDepthBias = 0.0f;
    rs.DepthClipEnable = TRUE;
    rs.MultisampleEnable = FALSE;
    rs.AntialiasedLineEnable = FALSE;
    rs.ForcedSampleCount = 0;
    rs.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    auto pipeline = std::make_unique<Dx12Pipeline>();
    pipeline->rootSignature = rootSig;
    pipeline->shaderGeneration = allocateGraphicsPipelineGeneration();
    if (!pipeline->shaderGeneration)
        return mcdr::err("createGraphicsPipeline", "Graphics shader generation exhausted");
    pipeline->graphicsBindingLayoutKnown = true;
    for (const auto& binding : desc.bindings) {
        pipeline->graphicsBindingTypes.push_back(binding.type);
        pipeline->graphicsBindingRegisters.push_back(binding.reg);
    }
    pipeline->pushConstantBytes = desc.pushConstantBytes;
    pipeline->pushConstantParam = desc.pushConstantBytes ? constantParam : UINT_MAX;
    pipeline->captureParam = captureParam;
    pipeline->captureHash = desc.captureHash;
    pipeline->captureGeneration = desc.captureGeneration;
    pipeline->normalizationProvenance = desc.normalizationProvenance;
    if (desc.captureHash) {
        pipeline->captureBudget = mcdr::oit_budget::reserve(1024 * 1024);
        if (!pipeline->captureBudget) return mcdr::err("createGraphicsPipeline", "Capture pipeline budget exceeded");
    }
    pipeline->topology = desc.topology;
    pipeline->rasterMotion = desc.rasterMotion;
    pipeline->depthWrite = desc.hasDepth && desc.depthWrite;
    pipeline->rasterMotionWritesWorldColor = false;
    const int worldColorCount = desc.colorCount - (desc.rasterMotion ? 1 : 0);
    for (int targetIndex = 0; targetIndex < worldColorCount; ++targetIndex) {
        const auto& target = desc.colorTargets[targetIndex];
        if (target.format >= 0 && target.writeMask != 0) pipeline->rasterMotionWritesWorldColor = true;
    }
    pipeline->colorTargetCount = static_cast<UINT>(numRT);
    pipeline->colorTargetFormats.reserve(numRT);
    for (int index = 0; index < numRT; ++index) {
        const auto format = desc.colorTargets[index].format;
        pipeline->colorTargetFormats.push_back(format < 0 ? DXGI_FORMAT_UNKNOWN : toDxgiFormat(format));
    }
    pipeline->depthTargetFormat = desc.hasDepth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;

    for (UINT slot = 0; slot < 16; ++slot) {
        if (slotStride[slot] != 0) pipeline->vertexStrides[slot] = slotStride[slot];
    }

    auto buildPso = [&](DXGI_FORMAT dsvFormat, bool depthEnable, D3D12_DEPTH_WRITE_MASK depthWrite,
                        D3D12_COMPARISON_FUNC depthFunc,
                        ComPtr<ID3D12PipelineState>& out) -> mcdr::Result<void> {
        D3D12_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = depthEnable;
        ds.DepthWriteMask = depthWrite;
        ds.DepthFunc = depthFunc;
        ds.StencilEnable = FALSE;
        ds.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        ds.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = rootSig.Get();
        pso.VS.pShaderBytecode = vsCode;
        pso.VS.BytecodeLength = vsCodeSize;
        pso.PS.pShaderBytecode = psCode;
        pso.PS.BytecodeLength = psCodeSize;
        pso.InputLayout.NumElements = static_cast<UINT>(inputLayout.size());
        pso.InputLayout.pInputElementDescs = inputLayout.empty() ? nullptr : inputLayout.data();
        pso.BlendState = blend;
        pso.RasterizerState = rs;
        pso.DepthStencilState = ds;
        pso.PrimitiveTopologyType = toTopologyType(desc.topology);
        pso.NumRenderTargets = static_cast<UINT>(numRT);
        for (int i = 0; i < desc.colorCount; ++i) {
            const PipelineDesc::ColorTarget& ct = desc.colorTargets[i];
            pso.RTVFormats[i] = ct.format < 0 ? DXGI_FORMAT_UNKNOWN : toDxgiFormat(ct.format);
        }
        pso.DSVFormat = dsvFormat;
        pso.SampleDesc.Count = 1;
        pso.SampleDesc.Quality = 0;
        pso.SampleMask = UINT_MAX;
        {
            std::string descInfo =
                "inputElements=" + std::to_string(inputLayout.size()) + " numRT=" + std::to_string(numRT)
                + " colorCount=" + std::to_string(desc.colorCount)
                + " topology=" + std::to_string(desc.topology) + " hasDepth=" + std::to_string(desc.hasDepth)
                + " dsv=" + std::to_string(static_cast<int>(dsvFormat));
            for (int i = 0; i < desc.colorCount; ++i) {
                descInfo +=
                    " rt" + std::to_string(i) + "=" + std::to_string(static_cast<int>(pso.RTVFormats[i]));
            }
            dbgLogDebug("createGraphicsPipeline: PSO=[%s]\n", descInfo.c_str());
        }
        {
            std::string vsStr(reinterpret_cast<const char*>(vsBytes.data()), vsBytes.size());
            std::string psStr(reinterpret_cast<const char*>(psBytes.data()), psBytes.size());
            int vsLen = std::min(static_cast<int>(vsStr.size()), 400);
            int psLen = std::min(static_cast<int>(psStr.size()), 400);
            dbgLogDebug("createGraphicsPipeline: vs(%zuB)=[%.*s...]\nps(%zuB)=[%.*s...]", vsBytes.size(), vsLen,
                   vsStr.c_str(), psBytes.size(), psLen, psStr.c_str());
        }
        {
            char buf[512];
            snprintf(buf, sizeof(buf),
                     "  pso_fields: rootSig=%p vsPtr=%p vsSize=%u psPtr=%p psSize=%u "
                     "inputElts=%u rtCount=%u dsvFmt=%d topType=%d "
                     "blendIndep=%d depthEnable=%d depthWrite=%d depthFunc=%d",
                     reinterpret_cast<void*>(pso.pRootSignature), const_cast<void*>(pso.VS.pShaderBytecode),
                     static_cast<UINT>(pso.VS.BytecodeLength), const_cast<void*>(pso.PS.pShaderBytecode),
                     static_cast<UINT>(pso.PS.BytecodeLength), pso.InputLayout.NumElements,
                     pso.NumRenderTargets, static_cast<int>(pso.DSVFormat),
                     static_cast<int>(pso.PrimitiveTopologyType),
                     static_cast<int>(pso.BlendState.IndependentBlendEnable),
                     static_cast<int>(pso.DepthStencilState.DepthEnable),
                     static_cast<int>(pso.DepthStencilState.DepthWriteMask),
                     static_cast<int>(pso.DepthStencilState.DepthFunc));
            dbgLogDebug(buf);
            for (UINT i = 0; i < pso.InputLayout.NumElements; ++i) {
                const D3D12_INPUT_ELEMENT_DESC& ie = inputLayout[i];
                snprintf(buf, sizeof(buf),
                         "  inputEl[%u]: fmt=%d slot=%u offset=%u class=%u step=%u semantic=%s", i,
                         static_cast<int>(ie.Format), ie.InputSlot, ie.AlignedByteOffset, ie.InputSlotClass,
                         ie.InstanceDataStepRate, ie.SemanticName);
                dbgLogDebug(buf);
            }
        }
        ++gStatPsoCreate;
        HRESULT h = gCtx.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&out));
        if (FAILED(h)) {
            dbgLogDebug("D3D12 validation messages:");
            if (gCtx.infoQueue) {
                UINT64 count = gCtx.infoQueue->GetNumStoredMessages();
                dbgLogDebug("  InfoQueue stored=%llu", static_cast<unsigned long long>(count));
                UINT64 start = count > 64 ? count - 64 : 0;
                for (UINT64 i = start; i < count; ++i) {
                    SIZE_T len = 0;
                    if (FAILED(gCtx.infoQueue->GetMessage(static_cast<UINT>(i), nullptr, &len))) continue;
                    std::vector<char> buf(len > 0 ? len : 1);
                    D3D12_MESSAGE* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                    if (SUCCEEDED(gCtx.infoQueue->GetMessage(static_cast<UINT>(i), msg, &len))) {
                        const char* sev = "?";
                        switch (msg->Severity) {
                            case D3D12_MESSAGE_SEVERITY_CORRUPTION: sev = "CORRUPTION"; break;
                            case D3D12_MESSAGE_SEVERITY_ERROR: sev = "ERROR"; break;
                            case D3D12_MESSAGE_SEVERITY_WARNING: sev = "WARNING"; break;
                            default: break;
                        }
                        dbgLogDebug("  [%s] %s", sev, msg->pDescription ? msg->pDescription : "");
                    }
                }
            }
            return mcdr::err("createGraphicsPipeline",
                             "createGraphicsPipeline: CreateGraphicsPipelineState hr=" + formatBackendResult(h));
        }
        return {};
    };

    D3D12_DEPTH_WRITE_MASK depthWrite =
        (desc.hasDepth && desc.depthWrite) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;

    bool wDepthEnable = desc.hasDepth;
    D3D12_COMPARISON_FUNC wDepthFunc = desc.hasDepth
                                           ? toD3d12Compare(static_cast<uint8_t>(desc.depthCompareOp))
                                           : D3D12_COMPARISON_FUNC_ALWAYS;
    auto wDepthPso = buildPso(DXGI_FORMAT_D32_FLOAT, wDepthEnable, depthWrite, wDepthFunc,
                              pipeline->withDepth);
    if (!wDepthPso) return std::unexpected<mcdr::Error>(std::move(wDepthPso.error()));
    auto noDepthPso = buildPso(DXGI_FORMAT_UNKNOWN, false, D3D12_DEPTH_WRITE_MASK_ZERO,
                               D3D12_COMPARISON_FUNC_ALWAYS, pipeline->withoutDepth);
    if (!noDepthPso) return std::unexpected<mcdr::Error>(std::move(noDepthPso.error()));
    if (desc.opaqueSeed.programGeneration) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        if (SUCCEEDED(gCtx.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
            options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation) {
            pipeline->tileRoute = std::make_shared<Dx12Pipeline::TileRoute>();
            auto& seed = *pipeline->tileRoute;
            seed.tileStage = desc.opaqueSeed.tileStage;
            seed.laneRegisters = desc.opaqueSeed.laneRegisters;
            seed.laneBindingSlots.fill(UINT32_MAX);
            seed.bindingCount = static_cast<uint32_t>(desc.bindings.size());
            seed.pushConstantParam = pipeline->pushConstantParam;
            if (desc.rasterMotion) {
                if (seed.bindingCount < 2 || desc.bindings[seed.bindingCount - 2].type != 0 ||
                    desc.bindings.back().type != 2)
                    return mcdr::err("opaque.seed.pipeline", "Missing motion binding suffix");
                seed.bindingCount -= 2;
                const auto constants = desc.pushConstantBytes ? params[constantParam] : D3D12_ROOT_PARAMETER{};
                params.resize(cbvRegs.size() - 1);
                cbvRegs.pop_back();
                srvRanges.pop_back();
                if (!srvRanges.empty()) {
                    D3D12_ROOT_PARAMETER table{};
                    table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                    table.DescriptorTable = {static_cast<UINT>(srvRanges.size()), srvRanges.data()};
                    table.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                    params.push_back(table);
                }
                if (desc.pushConstantBytes) {
                    seed.pushConstantParam = static_cast<UINT>(params.size());
                    params.push_back(constants);
                }
            }
            const bool tileOit = seed.tileStage != UINT32_MAX;
            if (cbvRegs.size() * 2 + (srvRanges.empty() ? 0 : 1) + desc.pushConstantBytes / 4 +
                    (tileOit ? 7 : 6) > 64 ||
                toTopologyType(desc.topology) != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE)
                return mcdr::err("oit.tile.ia", "Incompatible root size or topology");
            D3D12_DESCRIPTOR_RANGE laneReads{};
            if (tileOit) {
                std::array<bool, 256> seen{};
                for (size_t index = 0; index < seed.laneRegisters.size(); ++index) {
                    const auto reg = seed.laneRegisters[index];
                    if (reg == UINT32_MAX) continue;
                    const auto binding = std::ranges::find_if(desc.bindings, [reg](const auto& candidate) {
                        return candidate.type == 1 && candidate.reg == reg;
                    });
                    if (reg >= seen.size() || seen[reg] || binding == desc.bindings.end())
                        return mcdr::err("oit.tile.ia", "Invalid lane-local SRV");
                    seen[reg] = true;
                    seed.laneBindingSlots[index] = static_cast<uint32_t>(binding - desc.bindings.begin());
                }
                if (std::ranges::any_of(seen, [](bool used) { return used; })) {
                    D3D12_DESCRIPTOR_HEAP_DESC heap{};
                    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                    heap.NumDescriptors = 1;
                    if (FAILED(gCtx.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&seed.lanePlaceholderHeap))))
                        return mcdr::err("oit.tile.ia", "Lane descriptor allocation failed");
                    seed.lanePlaceholder.kind = Dx12Object::Kind::TextureView;
                    seed.lanePlaceholder.cpuHandle = seed.lanePlaceholderHeap->GetCPUDescriptorHandleForHeapStart();
                    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
                    view.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    view.Texture2D.MipLevels = 1;
                    gCtx.device->CreateShaderResourceView(nullptr, &view, seed.lanePlaceholder.cpuHandle);
                }
                laneReads.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                laneReads.NumDescriptors = static_cast<UINT>(seed.laneRegisters.size());
                laneReads.RegisterSpace = 3;
                D3D12_ROOT_PARAMETER table{};
                table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                table.DescriptorTable = {1, &laneReads};
                table.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                seed.laneDescriptorParam = static_cast<UINT>(params.size());
                params.push_back(table);
            }
            for (const auto& element : inputLayout)
                seed.vertexStepRates[element.InputSlot] = element.InstanceDataStepRate;
            constexpr std::string_view instanceBatchContract = "#define MCDR_TILE_IA_INSTANCE_BATCH_V1 1\n";
            seed.instanceTileBatch = tileOit && desc.opaqueSeed.vertex.starts_with(instanceBatchContract) &&
                desc.opaqueSeed.fragment.starts_with(instanceBatchContract) &&
                std::ranges::all_of(inputLayout, [](const auto& element) {
                    return element.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA &&
                        element.InstanceDataStepRate == 0;
                });
            seed.drawParam = static_cast<UINT>(params.size());
            D3D12_ROOT_PARAMETER draw{};
            draw.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            draw.Constants = {0, 3, 4};
            draw.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params.push_back(draw);
            seed.batchParam = static_cast<UINT>(params.size());
            D3D12_ROOT_PARAMETER batch{};
            batch.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            batch.Descriptor = {1, 3};
            batch.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params.push_back(batch);
            rsDesc.NumParameters = static_cast<UINT>(params.size());
            rsDesc.pParameters = params.data();
            rsBlob.Reset();
            rsErr.Reset();
            if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &rsErr)) ||
                FAILED(gCtx.device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                    IID_PPV_ARGS(&seed.root)))) return mcdr::err("opaque.seed.pipeline", "Root signature creation failed");
            std::vector<uint8_t> tileVertex, tileFragment;
            if (auto compiled = compileHlslToDxil(std::string(desc.opaqueSeed.vertex), "main", "vs_" + smProfile, tileVertex); !compiled)
                return std::unexpected(compiled.error());
            if (auto compiled = compileHlslToDxil(std::string(desc.opaqueSeed.fragment), "main", "ps_" + smProfile, tileFragment); !compiled)
                return std::unexpected(compiled.error());
            D3D12_GRAPHICS_PIPELINE_STATE_DESC depthOnly{};
            depthOnly.pRootSignature = seed.root.Get();
            depthOnly.VS = {tileVertex.data(), tileVertex.size()};
            depthOnly.PS = {tileFragment.data(), tileFragment.size()};
            depthOnly.InputLayout = {inputLayout.data(), static_cast<UINT>(inputLayout.size())};
            depthOnly.RasterizerState = rs;
            if (tileOit) {
                depthOnly.BlendState = blend;
                depthOnly.NumRenderTargets = static_cast<UINT>(numRT);
                for (int index = 0; index < desc.colorCount; ++index) {
                    const auto format = desc.colorTargets[index].format;
                    depthOnly.RTVFormats[index] = format < 0 ? DXGI_FORMAT_UNKNOWN : toDxgiFormat(format);
                }
            }
            depthOnly.DepthStencilState.DepthEnable = TRUE;
            depthOnly.DepthStencilState.DepthWriteMask = depthWrite;
            depthOnly.DepthStencilState.DepthFunc = wDepthFunc;
            depthOnly.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            depthOnly.DSVFormat = DXGI_FORMAT_D32_FLOAT;
            depthOnly.SampleDesc.Count = 1;
            depthOnly.SampleMask = UINT_MAX;
            const auto created = gCtx.device->CreateGraphicsPipelineState(&depthOnly, IID_PPV_ARGS(&seed.pso));
            if (FAILED(created))
                return mcdr::err("opaque.seed.pipeline", "Depth PSO creation failed: " + formatBackendResult(created));
            seed.programGeneration = desc.opaqueSeed.programGeneration;
        }
    }
    return static_cast<void*>(pipeline.release());
}

mcdr::Result<void> pushGraphicsConstants(void* rawContext, const void* data, uint32_t byteSize) {
    auto* context = static_cast<CommandContext*>(rawContext);
    if (!context || !context->listOpen || !context->currentPipeline || !data || byteSize == 0 ||
        byteSize != context->currentPipeline->pushConstantBytes || byteSize > 128 || byteSize % 4 != 0 ||
        context->currentPipeline->pushConstantParam == UINT_MAX) {
        return mcdr::err("pushGraphicsConstants", "invalid context, active pipeline, or constant byte size");
    }
    context->commandList->SetGraphicsRoot32BitConstants(context->currentPipeline->pushConstantParam,
        byteSize / 4, data, 0);
    std::memcpy(context->graphicsDrawState.pushConstants.data(), data, byteSize);
    context->graphicsDrawState.pushConstantBytes = byteSize;
    context->graphicsDrawState.pushRoot = context->currentPipeline->rootSignature.Get();
    context->graphicsDrawState.pushConstantsValid = true;
    return {};
}

void destroyPipeline(Dx12Pipeline* pipeline) {
    DBG_LOG_DEBUG("destroyPipeline: pso=%p", (void*)pipeline);
    if (pipeline && pipeline->lifetimeReferences.fetch_sub(1, std::memory_order_acq_rel) == 1) delete pipeline;
    DBG_LOG_DEBUG("destroyPipeline: done");
}

uint64_t allocateGraphicsPipelineGeneration() {
    static std::atomic<uint64_t> nextGeneration{1};
    auto generation = nextGeneration.load(std::memory_order_relaxed);
    do {
        if (generation == UINT64_MAX) return 0;
    } while (!nextGeneration.compare_exchange_weak(generation, generation + 1, std::memory_order_relaxed));
    return generation;
}

mcdr::Result<void> setPipeline(CommandContext* ctx, Dx12Pipeline* pipeline, bool hasDepth) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("setPipeline", "setPipeline: no open command list");
    }
    if (!pipeline) {
        return mcdr::err("setPipeline", "setPipeline: null pipeline");
    }
    ID3D12PipelineState* pso = hasDepth ? pipeline->withDepth.Get() : pipeline->withoutDepth.Get();
    if (!pso) pso = pipeline->withDepth.Get();
    if (!pipeline->rootSignature) {
        return mcdr::err("setPipeline", "setPipeline: null root signature");
    }
    if (pipeline->rasterMotion && (!ctx->inRenderPass || !ctx->activeMotionTarget || !hasDepth)) {
        return mcdr::err("setPipeline", "raster motion pipeline requires its registered world pass");
    }
    if (ctx->activeMotionTarget || ctx->additionalColorTarget) {
        const UINT expected = static_cast<UINT>(ctx->activePassRtvs.size()) +
            (pipeline->rasterMotion ? 1u : 0u) + (ctx->additionalColorTarget ? 1u : 0u);
        const bool depthOnly = !pipeline->rasterMotion && pipeline->colorTargetCount == 0;
        if (pipeline->colorTargetCount != UINT_MAX && pipeline->colorTargetCount != expected && !depthOnly) {
            return mcdr::err("setPipeline", "pipeline color target count differs from the active world attachments");
        }
        const UINT targetCount = depthOnly ? 0u : expected;
        if (ctx->cachedPso == nullptr || ctx->rasterMotionSelected != pipeline->rasterMotion ||
            ctx->activePassBoundColorCount != targetCount) {
            auto targets = depthOnly ? std::vector<D3D12_CPU_DESCRIPTOR_HANDLE>{} : ctx->activePassRtvs;
            if (!depthOnly && ctx->additionalColorTarget) {
                if (const auto result = transitionAttachmentRanges(ctx, ctx->additionalColorTarget, false); !result) return result;
                targets.push_back(ctx->additionalColorRtv);
            }
            if (pipeline->rasterMotion) {
                if (const auto result = transitionAttachmentRanges(ctx, ctx->activeMotionTarget, false); !result) return result;
                targets.push_back(ctx->activeMotionRtv);
            }
            ctx->commandList->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.empty() ? nullptr : targets.data(), FALSE,
                ctx->activeDepthTarget ? &ctx->activePassDsv : nullptr);
            ctx->rasterMotionSelected = pipeline->rasterMotion;
            ctx->activePassBoundColorCount = targetCount;
        }
    }
    if (ctx->retainedGraphicsPipelines.emplace(pipeline, true).second) {
        pipeline->lifetimeReferences.fetch_add(1, std::memory_order_relaxed);
        ctx->recordingLeases[ctx->currentListSlot].push_back(std::shared_ptr<void>(pipeline, [](void* retained) {
            destroyPipeline(static_cast<Dx12Pipeline*>(retained));
        }));
    }
    if (pso == ctx->cachedPso) {
        ctx->graphicsHasDepth = hasDepth;
        ctx->currentPipeline = pipeline;
        return {};
    }
    if (ctx->graphicsBindingRoot != pipeline->rootSignature.Get()) {
        invalidateGraphicsInputBindings(ctx, false);
    }
    ctx->commandList->SetGraphicsRootSignature(pipeline->rootSignature.Get());
    ctx->graphicsBindingRoot = pipeline->rootSignature.Get();
    if (pipeline->pushConstantBytes == 0) {
        ctx->graphicsDrawState.pushConstantBytes = 0;
        ctx->graphicsDrawState.pushRoot = pipeline->rootSignature.Get();
        ctx->graphicsDrawState.pushConstantsValid = true;
    }
    ctx->commandList->SetPipelineState(pso);
    ctx->currentPipeline = pipeline;
    ctx->cachedPso = pso;
    ctx->graphicsHasDepth = hasDepth;
    D3D12_PRIMITIVE_TOPOLOGY topo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    if (!pipeline->isMesh) {
        topo = toPrimitiveTopology(pipeline->topology);
        ctx->commandList->IASetPrimitiveTopology(topo);
    }
    DBG_LOG_DEBUG("setPipeline rootSig=%p pso=%p hasDepth=%d topoOrdinal=%d topo=%d",
                  (void*)pipeline->rootSignature.Get(), (void*)pso, (int)hasDepth, (int)pipeline->topology,
                  (int)topo);
    return {};
}

mcdr::Result<void> setScissor(CommandContext* ctx, int x, int y, int w, int h) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("setScissor", "setScissor: no open command list");
    }
    D3D12_RECT rect{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x) + w,
                    static_cast<LONG>(y) + h};
    ctx->commandList->RSSetScissorRects(1, &rect);
    ctx->graphicsDrawState.scissor = rect;
    ctx->graphicsDrawState.scissorValid = true;
    return {};
}

mcdr::Result<void> setViewport(CommandContext* ctx, int x, int y, int w, int h) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("setViewport", "setViewport: no open command list");
    }
    if (w <= 0 || h <= 0) {
        return mcdr::err("setViewport", "setViewport: empty viewport");
    }
    D3D12_VIEWPORT vp{static_cast<FLOAT>(x),
                      static_cast<FLOAT>(y),
                      static_cast<FLOAT>(w),
                      static_cast<FLOAT>(h),
                      0.0f,
                      1.0f};
    ctx->commandList->RSSetViewports(1, &vp);
    ctx->graphicsDrawState.viewport = vp;
    ctx->graphicsDrawState.viewportValid = true;
    return {};
}

mcdr::Result<void> setVertexBuffer(CommandContext* ctx, int slot, Dx12Object* buffer, long long offset,
                                   int stride, long long length) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("setVertexBuffer", "setVertexBuffer: no open command list");
    }
    if (slot < 0 || slot >= 16) {
        return mcdr::err("setVertexBuffer", "setVertexBuffer: slot out of range");
    }
    if (!buffer || buffer->kind != Dx12Object::Kind::Buffer || !buffer->resource ||
        buffer->heapType == D3D12_HEAP_TYPE_READBACK) {
        return mcdr::err("setVertexBuffer", "setVertexBuffer: invalid buffer handle");
    }
    if (offset < 0 || offset >= buffer->size) {
        return mcdr::err("setVertexBuffer", "setVertexBuffer: offset out of bounds");
    }
    const auto bytes = length ? length : buffer->size - offset;
    if (bytes <= 0 || bytes > UINT_MAX || bytes > buffer->size - offset)
        return mcdr::err("setVertexBuffer", "Vertex range is out of bounds");
    transitionBufferTo(ctx, buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    UINT effectiveStride = static_cast<UINT>(stride);
    if (ctx->currentPipeline && ctx->currentPipeline->vertexStrides.count(slot))
        effectiveStride = ctx->currentPipeline->vertexStrides[slot];
    D3D12_VERTEX_BUFFER_VIEW vb;
    vb.BufferLocation = buffer->resource->GetGPUVirtualAddress() + static_cast<UINT64>(offset);
    vb.SizeInBytes = static_cast<UINT>(bytes);
    vb.StrideInBytes = effectiveStride;
    ctx->commandList->IASetVertexBuffers(static_cast<UINT>(slot), 1, &vb);
    ctx->graphicsVertexBuffers[slot] = buffer;
    ctx->graphicsVertexViews[slot] = vb;
    ctx->graphicsVertexSlices[slot] = length != 0;
    retainObjectForRecording(ctx, buffer);
    DBG_LOG_DEBUG("setVertexBuffer: slot=%d buf=%p size=%lld off=%lld javaStride=%d effStride=%d heap=%d",
                  slot, (void*)buffer, (long long)buffer->size, (long long)offset, (int)stride,
                  (int)effectiveStride, (int)buffer->heapType);
    return {};
}

mcdr::Result<void> setIndexBuffer(CommandContext* ctx, Dx12Object* buffer, int indexType) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("setIndexBuffer", "setIndexBuffer: no open command list");
    }
    if (!buffer || buffer->kind != Dx12Object::Kind::Buffer || !buffer->resource ||
        buffer->heapType == D3D12_HEAP_TYPE_READBACK) {
        return mcdr::err("setIndexBuffer", "setIndexBuffer: invalid buffer handle");
    }
    transitionBufferTo(ctx, buffer, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    D3D12_INDEX_BUFFER_VIEW ib{};
    ib.BufferLocation = buffer->resource->GetGPUVirtualAddress();
    ib.SizeInBytes = static_cast<UINT>(buffer->size);
    ib.Format = indexType == 1 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    ctx->commandList->IASetIndexBuffer(&ib);
    ctx->graphicsIndexBuffer = buffer;
    ctx->graphicsIndexView = ib;
    retainObjectForRecording(ctx, buffer);
    DBG_LOG_DEBUG("setIndexBuffer: buf=%p size=%lld idxWidth=%d heap=%d", (void*)buffer,
                  (long long)buffer->size, indexType == 1 ? 4 : 2, (int)buffer->heapType);
    return {};
}

namespace {

mcdr::Result<D3D12_SHADER_RESOURCE_VIEW_DESC> bufferSrvDescription(
    const Dx12Object* buffer, long long offset, long long length, int texelFormat) {
    if (offset < 0 || length <= 0 || offset > buffer->size || length > buffer->size - offset) {
        return mcdr::err("bufferSrvDescription", "Buffer SRV range is invalid");
    }
    const bool raw = texelFormat == -1;
    D3D12_SHADER_RESOURCE_VIEW_DESC description{};
    description.Format = raw ? DXGI_FORMAT_R32_TYPELESS : toDxgiFormat(texelFormat);
    if (description.Format == DXGI_FORMAT_UNKNOWN) {
        return mcdr::err("bufferSrvDescription", "Buffer SRV format is unsupported");
    }
    const UINT elementBytes = raw ? 4u : std::max<UINT>(1u, blockSizeFor(description.Format));
    if (offset % elementBytes != 0 || (!raw && length % elementBytes != 0)) {
        return mcdr::err("bufferSrvDescription", "Buffer SRV offset or length is unaligned");
    }
    const UINT64 elementCount = (static_cast<UINT64>(length) + elementBytes - 1) / elementBytes;
    if (elementCount > UINT_MAX || static_cast<UINT64>(offset) + elementCount * elementBytes
            > buffer->resource->GetDesc().Width) {
        return mcdr::err("bufferSrvDescription", "Buffer SRV exceeds its allocation");
    }
    description.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    description.Buffer.FirstElement = static_cast<UINT64>(offset) / elementBytes;
    description.Buffer.NumElements = static_cast<UINT>(elementCount);
    description.Buffer.Flags = raw ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
    return description;
}

}

namespace {

ID3D12DescriptorHeap* drawHeapFor(CommandContext* ctx) {
    return ctx->resourceDrawHeap ? ctx->resourceDrawHeap.Get() : gCtx.drawHeap.Get();
}

UINT drawHeapSlotFor(CommandContext* ctx, UINT offset) {
    return ctx->resourceDrawHeap ? offset : ctx->drawHeapSlotBase + offset;
}

UINT drawHeapCapacityFor(CommandContext* ctx) {
    return ctx->resourceDrawHeap ? ctx->resourceDrawCapacity : syncRingCapacity();
}

bool growDrawHeap(CommandContext* ctx, UINT required) {
    if (!ctx || required == 0) return false;
    if (required > UINT_MAX - ctx->nextDrawSlot) return false;
    const UINT needed = ctx->nextDrawSlot + required;
    const UINT current = ctx->resourceDrawHeap ? ctx->resourceDrawCapacity : 0;
    if (ctx->resourceDrawHeap && needed <= current) return true;
    if (ctx->resourceDrawHeap && ctx->currentPipeline && ctx->currentPipeline->isMesh &&
        !ctx->currentPipeline->meshVisibleWork) return false;
    const UINT doubled = current > 0 && current <= UINT_MAX / 2 ? current * 2 : 0;
    const UINT capacity = std::max(needed, std::max(256u, doubled));
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = capacity;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    auto page = std::make_shared<ComPtr<ID3D12DescriptorHeap>>();
    if (FAILED(gCtx.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(page->GetAddressOf())))) return false;
    ctx->recordingLeases[ctx->currentListSlot].push_back(page);
    ctx->resourceDrawHeap = *page;
    ctx->resourceDrawCapacity = capacity;
    ctx->nextDrawSlot = 0;
    ctx->lastSrvSlotOffset = 0;
    ctx->lastSrvCount = 0;
    ctx->lastSrvCbvCount = 0;
    ctx->lastSrvViews.clear();
    ID3D12DescriptorHeap* heaps[] = {ctx->resourceDrawHeap.Get()};
    ctx->commandList->SetDescriptorHeaps(1, heaps);
    return true;
}

}

mcdr::Result<DrawDescriptorRange> allocateDrawDescriptors(CommandContext* context, UINT count) {
    if (!context || !context->listOpen || !context->commandList || count == 0) {
        return mcdr::err("allocateDrawDescriptors", "Draw descriptor allocation requires an open recording and a nonempty range");
    }
    if (!growDrawHeap(context, count)) {
        return mcdr::err("allocateDrawDescriptors", "Draw descriptor heap growth failed");
    }
    const UINT offset = context->nextDrawSlot;
    const SIZE_T byteOffset = static_cast<SIZE_T>(drawHeapSlotFor(context, offset)) * gCtx.drawInc;
    DrawDescriptorRange range{drawHeapFor(context)->GetCPUDescriptorHandleForHeapStart(),
                              drawHeapFor(context)->GetGPUDescriptorHandleForHeapStart(), offset};
    range.cpu.ptr += byteOffset;
    range.gpu.ptr += byteOffset;
    context->nextDrawSlot += count;
    return range;
}

namespace {

mcdr::Result<void> mergeGraphicsReads(CommandContext* context,
    std::vector<GraphicsResourceRange>& destination, const std::vector<GraphicsResourceRange>& source) {
    for (const auto& range : source) {
        for (const auto& attachment : context->activeAttachmentRanges) {
            if (range.object->resource.Get() == attachment.object->resource.Get() &&
                uint64_t(range.firstSubresource) < uint64_t(attachment.firstSubresource) + attachment.subresourceCount &&
                uint64_t(attachment.firstSubresource) < uint64_t(range.firstSubresource) + range.subresourceCount) {
                return mcdr::err("graphicsInputs", "graphics input overlaps an active color, depth or motion attachment");
            }
        }
    }
    for (const auto& range : source) {
        for (uint32_t subresource = range.firstSubresource;
             subresource < range.firstSubresource + range.subresourceCount; ++subresource) {
            auto found = std::find_if(destination.begin(), destination.end(), [&](const auto& existing) {
                return existing.object->resource.Get() == range.object->resource.Get() &&
                    existing.firstSubresource == subresource;
            });
            if (found == destination.end()) destination.push_back({range.object, subresource, 1, range.state});
            else found->state |= range.state;
        }
    }
    return {};
}

mcdr::Result<void> addBufferRead(std::vector<GraphicsResourceRange>& ranges,
                                Dx12Object* object, uint32_t state) {
    if (!object || object->kind != Dx12Object::Kind::Buffer || !object->resource) {
        return mcdr::err("graphicsInputs", "graphics input buffer is invalid");
    }
    if (object->heapType == D3D12_HEAP_TYPE_READBACK) {
        return mcdr::err("graphicsInputs", "READBACK buffers cannot be graphics inputs");
    }
    ranges.push_back({object, 0, 1, object->heapType == D3D12_HEAP_TYPE_UPLOAD
        ? uint32_t(D3D12_RESOURCE_STATE_GENERIC_READ) : state});
    return {};
}

mcdr::Result<void> bindingReadRanges(CommandContext* context, const DrawBinding* bindings,
    uint32_t count, std::vector<GraphicsResourceRange>& ranges, bool complete) {
    const auto* pipeline = context->currentPipeline;
    if (complete && pipeline && pipeline->graphicsBindingLayoutKnown) {
        if (count != pipeline->graphicsBindingTypes.size()) {
            return mcdr::err("graphicsInputs", "graphics binding packet does not match the current root layout");
        }
        for (uint32_t index = 0; index < count; ++index) {
            if (bindings[index].type != pipeline->graphicsBindingTypes[index]) {
                return mcdr::err("graphicsInputs", "graphics binding type differs from the current root layout");
            }
        }
    }
    auto& pending = context->graphicsBindingPendingScratch;
    pending.clear();
    for (uint32_t index = 0; index < count; ++index) {
        const auto& binding = bindings[index];
        if (binding.type == 0 || binding.type == 2) {
            const auto state = binding.type == 0 ? D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
                : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            if (const auto result = addBufferRead(pending, binding.buffer, state); !result) return result;
            if (binding.type == 0) {
                if (binding.offset < 0 || binding.offset >= binding.buffer->size || binding.length < 0 ||
                    binding.length > binding.buffer->size - binding.offset ||
                    (binding.buffer->resource->GetGPUVirtualAddress() + static_cast<UINT64>(binding.offset)) % 256 != 0) {
                    return mcdr::err("graphicsInputs", "graphics CBV offset, length or alignment is invalid");
                }
            } else if (const auto description = bufferSrvDescription(binding.buffer, binding.offset,
                binding.length, binding.texelFormat); !description) return std::unexpected(description.error());
        } else if (binding.type == 1) {
            const auto* view = binding.view;
            if (!view || view->kind != Dx12Object::Kind::TextureView || !view->hasSrvDesc ||
                !view->cpuHandle.ptr || !view->sourceTexture) {
                return mcdr::err("graphicsInputs", "graphics texture view has no valid SRV metadata");
            }
            const auto& description = view->srvDesc;
            mcdr::Result<void> result;
            constexpr uint32_t state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            if (description.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
                result = textureSubresourceRanges(pending, view->sourceTexture,
                    description.Texture2D.MostDetailedMip, description.Texture2D.MipLevels,
                    0, 1, description.Texture2D.PlaneSlice, 1, state);
            } else if (description.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE) {
                result = textureSubresourceRanges(pending, view->sourceTexture,
                    description.TextureCube.MostDetailedMip, description.TextureCube.MipLevels,
                    0, 6, 0, 1, state);
            } else return mcdr::err("graphicsInputs", "graphics SRV dimension is unsupported");
            if (!result) return result;
        } else return mcdr::err("graphicsInputs", "unknown graphics binding type");
    }
    return mergeGraphicsReads(context, ranges, pending);
}

mcdr::Result<void> applyGraphicsRanges(CommandContext* context,
                                      const std::vector<GraphicsResourceRange>& ranges) {
    for (const auto& range : ranges) {
        if (!range.owner) retainObjectForRecording(context, range.object);
        if (range.object->heapType == D3D12_HEAP_TYPE_UPLOAD) continue;
        if (resourceSubresourceState(context, range.object->resource.Get(), range.firstSubresource,
            D3D12_RESOURCE_STATE_COMMON) == range.state && range.subresourceCount == 1) continue;
        if (const auto result = transitionResourceRange(context, range.object->resource.Get(),
            range.firstSubresource, range.subresourceCount, D3D12_RESOURCE_STATE_COMMON, range.state); !result) return result;
    }
    return {};
}

}

void invalidateGraphicsInputBindings(void* rawContext, bool invalidateIa) {
    auto* context = static_cast<CommandContext*>(rawContext);
    if (!context) return;
    context->graphicsBindings.clear();
    invalidateGraphicsBindingReadCache(context);
    context->graphicsBindingsValid = false;
    context->graphicsBindingRoot = nullptr;
    context->graphicsBindingTable = {};
    context->graphicsDrawState.pushConstantsValid = false;
    context->cachedPso = nullptr;
    if (invalidateIa) {
        context->graphicsDrawState.viewportValid = false;
        context->graphicsDrawState.scissorValid = false;
        for (auto& buffer : context->graphicsVertexBuffers) buffer = nullptr;
        context->graphicsIndexBuffer = nullptr;
        for (auto& view : context->graphicsVertexViews) view = {};
        context->graphicsVertexSlices.fill(false);
        context->graphicsIndexView = {};
    }
}

void restoreGraphicsInputAssembly(void* rawContext) {
    auto* context = static_cast<CommandContext*>(rawContext);
    if (!context || !context->listOpen || !context->commandList) return;
    for (UINT slot = 0; slot < 16; ++slot) {
        if (context->graphicsVertexBuffers[slot]) {
            context->commandList->IASetVertexBuffers(slot, 1, &context->graphicsVertexViews[slot]);
        }
    }
    if (context->graphicsIndexBuffer) context->commandList->IASetIndexBuffer(&context->graphicsIndexView);
}

mcdr::Result<void> collectGraphicsInputs(void* rawContext, bool indexed, void* indirectCommands) {
    auto* context = static_cast<CommandContext*>(rawContext);
    if (!context || !context->listOpen || !context->commandList || !context->currentPipeline) {
        return mcdr::err("graphicsInputs", "graphics input collection requires an active pipeline and recording");
    }
    context->graphicsInputRanges.clear();
    const auto* pipeline = context->currentPipeline;
    if (!pipeline->graphicsBindingLayoutKnown) {
        return mcdr::err("graphicsInputs", "graphics pipeline has no declared binding layout");
    }
    if (pipeline->isMesh && indexed) {
        return mcdr::err("graphicsInputs", "mesh pipelines cannot consume IA index input");
    }
    if (!pipeline->graphicsBindingTypes.empty() && (!context->graphicsBindingsValid ||
        context->graphicsBindingRoot != pipeline->rootSignature.Get())) {
        return mcdr::err("graphicsInputs", "effective graphics bindings were invalidated");
    }
    auto& pending = context->graphicsCollectScratch;
    const bool bindingLayoutMatches = context->graphicsBindings.size() == pipeline->graphicsBindingTypes.size() &&
        std::equal(context->graphicsBindings.begin(), context->graphicsBindings.end(),
            pipeline->graphicsBindingTypes.begin(), [](const auto& binding, const auto type) {
                return binding.type == type;
            });
    const bool reuseBindingReads = context->graphicsBindingReadCacheValid &&
        context->graphicsBindingsValid && context->graphicsBindingRoot == pipeline->rootSignature.Get() &&
        bindingLayoutMatches;
    if (reuseBindingReads) {
        context->graphicsInputRanges = context->graphicsBindingReadCache;
        pending.clear();
    } else {
        pending.clear();
        if (const auto result = bindingReadRanges(context, context->graphicsBindings.data(),
            static_cast<uint32_t>(context->graphicsBindings.size()), pending, true); !result) return result;
    }
    if (!pipeline->isMesh) {
        for (const auto& input : pipeline->vertexStrides) {
            const int slot = input.first;
            if (slot < 0 || slot >= 16 || !context->graphicsVertexBuffers[slot]) {
                return mcdr::err("graphicsInputs", "effective graphics vertex input is missing");
            }
            if (const auto result = addBufferRead(pending, context->graphicsVertexBuffers[slot],
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER); !result) return result;
        }
        if (indexed) {
            if (const auto result = addBufferRead(pending, context->graphicsIndexBuffer,
                D3D12_RESOURCE_STATE_INDEX_BUFFER); !result) return result;
        }
    }
    if (indirectCommands) {
        if (const auto result = addBufferRead(pending, static_cast<Dx12Object*>(indirectCommands),
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT); !result) return result;
    }
    if (reuseBindingReads) {
        if (!pending.empty()) {
            if (const auto result = mergeGraphicsReads(context, context->graphicsInputRanges, pending); !result)
                return result;
        }
    } else {
        if (const auto result = mergeGraphicsReads(context, context->graphicsInputRanges, pending); !result)
            return result;
    }
    if (pipeline->captureHash) {
        const auto& capture = context->graphicsCapture;
        if (!capture.lease || capture.contractHash != pipeline->captureHash || capture.generation != pipeline->captureGeneration)
            return mcdr::err("graphicsInputs", "Capture pipeline has no matching immutable draw lease");
        for (const auto& range : capture.resources) {
            for (const auto& attachment : context->activeAttachmentRanges)
                if (range.object->resource == attachment.object->resource)
                    return mcdr::err("graphicsInputs", "Capture resources alias a raster attachment");
            context->graphicsInputRanges.push_back(range);
        }
    }
    return {};
}

mcdr::Result<void> appendGraphicsInputBindings(void* rawContext, const void* rawBindings, uint32_t count) {
    auto* context = static_cast<CommandContext*>(rawContext);
    if (!context || !context->listOpen || !context->commandList || (count != 0 && !rawBindings)) {
        return mcdr::err("graphicsInputs", "additional graphics binding input is invalid");
    }
    invalidateGraphicsBindingReadCache(context);
    return bindingReadRanges(context, static_cast<const DrawBinding*>(rawBindings), count,
        context->graphicsInputRanges, false);
}

mcdr::Result<void> applyGraphicsInputReads(void* rawContext) {
    auto* context = static_cast<CommandContext*>(rawContext);
    if (!context || !context->listOpen || !context->commandList) {
        return mcdr::err("graphicsInputs", "graphics input reads require an open recording");
    }
    if (prepareSampledRenderPassWrites(context) != SampledTextureContentStatus::Ready)
        return mcdr::err("graphicsInputs", "sampled render attachment write ordering failed");
    if (const auto result = applyGraphicsRanges(context, context->graphicsInputRanges); !result) return result;
    return applyGraphicsCapture(rawContext);
}

void restoreGraphicsCaptureRate(void* rawContext) {
    auto* context = static_cast<CommandContext*>(rawContext);
    if (!context || !context->listOpen || !context->commandList) return;
    auto& saved=context->captureShadingRate;
    if (!saved.commandList || saved.recording!=context->commandList) return;
    if (gCtx.caps.shadingRateImageSupported())
        setCommandShadingRateImage(context,saved.commandList.Get(),saved.image);
    saved.commandList->RSSetShadingRate(saved.rate,saved.combiners);
    context->recordedShadingRate=saved.rate;
    context->recordedShadingCombiners[0]=saved.combiners[0];
    context->recordedShadingCombiners[1]=saved.combiners[1];
    saved={};
}

mcdr::Result<void> applyGraphicsCapture(void* rawContext) {
    auto* context = static_cast<CommandContext*>(rawContext);
    if (!context || !context->currentPipeline) return mcdr::err("capture.bind", "Missing graphics pipeline");
    auto* pipeline = context->currentPipeline;
    if (!pipeline->captureHash) return {};
    auto& capture = context->graphicsCapture;
    if (!capture.lease || !capture.remainingDraws || capture.contractHash != pipeline->captureHash || capture.generation != pipeline->captureGeneration)
        return mcdr::err("capture.bind", "Capture draw lease does not match the selected pipeline");
    auto* oldHeap = context->resourceDrawHeap.Get();
    const auto ordinarySrvCount = std::count_if(context->graphicsBindings.begin(), context->graphicsBindings.end(),
        [](const DrawBinding& binding) { return binding.type != 0; });
    const uint64_t requiredDescriptors = 14u + static_cast<uint64_t>(ordinarySrvCount);
    if (requiredDescriptors > UINT_MAX || !growDrawHeap(context, static_cast<UINT>(requiredDescriptors)))
        return mcdr::err("capture.bind", "Capture and graphics descriptor reservation failed");
    auto allocated = allocateDrawDescriptors(context, 14);
    if (!allocated) return std::unexpected(allocated.error());
    auto range = *allocated;
    if (oldHeap != context->resourceDrawHeap.Get() && !context->graphicsBindings.empty()) {
        const auto bindings = context->graphicsBindings;
        if (const auto restored = pushDescriptors(context, bindings); !restored) return restored;
    }
    auto destination = range.cpu;
    auto source = capture.descriptors;
    for (UINT index = 0; index != 14; ++index) {
        gCtx.device->CopyDescriptorsSimple(1, destination, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        destination.ptr += gCtx.srvInc;
        source.ptr += gCtx.srvInc;
    }
    context->commandList->SetGraphicsRootConstantBufferView(pipeline->captureParam, capture.constants);
    context->commandList->SetGraphicsRootDescriptorTable(pipeline->captureParam + 1, range.gpu);
    auto uav = range.gpu;
    uav.ptr += static_cast<uint64_t>(gCtx.srvInc) * 11;
    context->commandList->SetGraphicsRootDescriptorTable(pipeline->captureParam + 2, uav);
    if (gCtx.caps.shadingRateSupported()) {
        if (context->captureShadingRate.commandList)
            return mcdr::err("capture.bind", "A previous capture shading scope is still active");
        ComPtr<ID3D12GraphicsCommandList5> list;
        if (FAILED(context->commandList->QueryInterface(IID_PPV_ARGS(&list))))
            return mcdr::err("capture.bind", "Capture requires a shading-rate-capable command list");
        auto& saved=context->captureShadingRate;
        saved.commandList=list;
        saved.recording=context->commandList;
        saved.image=context->shadingRateImageBound;
        saved.rate=context->recordedShadingRate;
        saved.combiners[0]=context->recordedShadingCombiners[0];
        saved.combiners[1]=context->recordedShadingCombiners[1];
        if (gCtx.caps.shadingRateImageSupported())
            setCommandShadingRateImage(context,list.Get(),nullptr);
        const D3D12_SHADING_RATE_COMBINER combiners[2] = {D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
        list->RSSetShadingRate(D3D12_SHADING_RATE_1X1, combiners);
        context->recordedShadingRate=D3D12_SHADING_RATE_1X1;
        context->recordedShadingCombiners[0]=combiners[0];
        context->recordedShadingCombiners[1]=combiners[1];
    }
    --capture.remainingDraws;
    return {};
}

mcdr::Result<void> pushDescriptors(CommandContext* ctx, const std::vector<DrawBinding>& bindings,
    DescriptorBindPoint bindPoint) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("pushDescriptors", "pushDescriptors: no open command list");
    }
    UINT count = static_cast<UINT>(bindings.size());
    auto& bindingReads = ctx->graphicsBindingReadScratch;
    const bool graphics = bindPoint != DescriptorBindPoint::Compute;
    const bool trackReads = bindPoint == DescriptorBindPoint::Graphics;
    if (trackReads) {
        invalidateGraphicsBindingReadCache(ctx);
        bindingReads.clear();
        if (const auto result = bindingReadRanges(ctx, bindings.data(), count, bindingReads, true); !result)
            return result;
        if (const auto result = applyGraphicsRanges(ctx, bindingReads); !result) return result;
    }
    if (count == 0) {
        if (!trackReads) return {};
        ctx->graphicsBindings.clear();
        ctx->graphicsBindingReadCacheValid = true;
        ctx->graphicsBindingsValid = true;
        ctx->graphicsBindingTable = {};
        return {};
    }
    UINT cbvCount = 0;
    UINT srvCount = 0;
    for (const DrawBinding& b : bindings) {
        if (b.type == 0) {
            ++cbvCount;
        } else {
            ++srvCount;
        }
    }
    bool reuseSrvTable = false;
    if (srvCount > 0 && ctx->lastSrvCount == srvCount && ctx->lastSrvCbvCount == cbvCount) {
        reuseSrvTable = true;
        size_t viewIndex = 0;
        for (const DrawBinding& b : bindings) {
            if (b.type == 0) {
                continue;
            }
            if (b.type != 1 || viewIndex >= ctx->lastSrvViews.size()
                || ctx->lastSrvViews[viewIndex].first != b.view
                || !b.view || ctx->lastSrvViews[viewIndex].second != b.view->cpuHandle.ptr) {
                reuseSrvTable = false;
                break;
            }
            ++viewIndex;
        }
    }
    DrawDescriptorRange descriptors{};
    if (srvCount > 0) {
        if (reuseSrvTable) {
            descriptors.offset = ctx->lastSrvSlotOffset;
            const SIZE_T byteOffset = static_cast<SIZE_T>(drawHeapSlotFor(ctx, descriptors.offset)) * gCtx.drawInc;
            descriptors.cpu = drawHeapFor(ctx)->GetCPUDescriptorHandleForHeapStart();
            descriptors.gpu = drawHeapFor(ctx)->GetGPUDescriptorHandleForHeapStart();
            descriptors.cpu.ptr += byteOffset;
            descriptors.gpu.ptr += byteOffset;
        } else {
            auto allocated = allocateDrawDescriptors(ctx, srvCount);
            if (!allocated) return std::unexpected(allocated.error());
            descriptors = allocated.value();
        }
    }
    static int pdFrameCount = 0;
    if (static_cast<int>(ctx->fenceValue) != pdFrameCount) {
        pdFrameCount = static_cast<int>(ctx->fenceValue);
        dbgLogDebug("pushDescriptors[%llu]: count=%u firstBuf=%p firstView=%p",
                    static_cast<unsigned long long>(ctx->fenceValue), count,
                    (count > 0 ? reinterpret_cast<void*>(bindings[0].buffer) : nullptr),
                    (count > 0 ? reinterpret_cast<void*>(bindings[0].view) : nullptr));
        for (UINT j = 0; j < count && j < 8; ++j) {
            const DrawBinding& bj = bindings[j];
            const char* tname = bj.type == 0 ? "CBV" : bj.type == 1 ? "SRV" : bj.type == 2 ? "BUF" : "?";
            dbgLogDebug("pushDesc BIND[%u]: type=%s buf=%p off=%lld len=%lld view=%p heap=%d",
                        static_cast<unsigned>(j), tname,
                        reinterpret_cast<void*>(bj.buffer ? bj.buffer : nullptr), bj.offset, bj.length,
                        reinterpret_cast<void*>(bj.view ? bj.view : nullptr),
                        (bj.buffer ? bj.buffer->heapType : -1));
        }
    }
    const UINT srvSlotOffset = descriptors.offset;
    if (trackReads) ctx->graphicsBindingTable = srvCount ? descriptors.gpu : D3D12_GPU_DESCRIPTOR_HANDLE{};
    const SIZE_T base = static_cast<SIZE_T>(drawHeapSlotFor(ctx, srvSlotOffset)) * gCtx.drawInc;
    const D3D12_CPU_DESCRIPTOR_HANDLE cpu = descriptors.cpu;
    UINT cbvIdx = 0;
    UINT srvIdx = 0;
    for (UINT i = 0; i < count; ++i) {
        const DrawBinding& b = bindings[i];
        DBG_LOG_DEBUG("pushDesc[%u] type=%d buf=%p view=%p off=%lld len=%lld texel=%d", (unsigned)i,
                      (int)b.type, (void*)b.buffer, (void*)b.view, (long long)b.offset, (long long)b.length,
                      b.texelFormat);
        switch (b.type) {
            case 0: {
                if (!b.buffer || b.buffer->kind != Dx12Object::Kind::Buffer || !b.buffer->resource) {
                    dbgLogError("pushDesc[%u] INVALID CBV buffer", static_cast<unsigned>(i));
                    return mcdr::err("pushDescriptors",
                                     "pushDescriptors: invalid buffer for CBV entry " + std::to_string(i));
                }
                D3D12_GPU_VIRTUAL_ADDRESS va =
                    b.buffer->resource->GetGPUVirtualAddress() + static_cast<UINT64>(b.offset);
                constexpr UINT64 kCbvAlignment = 256;
                if (va % kCbvAlignment != 0) {
                    dbgLogError("pushDesc[%u] CBV not 256B aligned: off=%lld va=%llx", static_cast<unsigned>(i),
                                b.offset, static_cast<unsigned long long>(va));
                    return mcdr::err("pushDescriptors",
                                     "pushDescriptors: CBV entry " + std::to_string(i) + " is not 256-byte aligned"
                                     " (buffer offset=" + std::to_string(b.offset) +
                                         "; D3D12 requires the address to be a multiple of 256)");
                }
                static UINT64 lastPdFrame = 0;
                if (ctx->fenceValue != lastPdFrame) {
                    lastPdFrame = ctx->fenceValue;
                    dbgLogDebug("pushDesc CBV[%u]: bufGVA=%llx off=%lld rootVA=%llx heap=%d",
                                static_cast<unsigned>(i),
                                static_cast<unsigned long long>(b.buffer->resource->GetGPUVirtualAddress()),
                                b.offset, static_cast<unsigned long long>(va),
                                static_cast<int>(b.buffer->heapType));
                    if (i == 0 && count > 1) {
                        const DrawBinding& b1 = bindings[1];
                        dbgLogDebug("pushDesc UBO_BIND[1]: bufGVA=%llx off=%lld len=%lld heap=%d",
                                    b1.buffer ? static_cast<unsigned long long>(
                                                    b1.buffer->resource->GetGPUVirtualAddress())
                                              : 0ULL,
                                    b1.offset, b1.length, (b1.buffer ? b1.buffer->heapType : -1));
                    }
                }
                if (graphics) ctx->commandList->SetGraphicsRootConstantBufferView(cbvIdx, va);
                else ctx->commandList->SetComputeRootConstantBufferView(cbvIdx, va);
                ++cbvIdx;
                break;
            }
            case 1: {
                D3D12_CPU_DESCRIPTOR_HANDLE dst{cpu.ptr + static_cast<SIZE_T>(srvIdx) * gCtx.drawInc};
                if (!b.view || b.view->cpuHandle.ptr == 0) {
                    dbgLogError("pushDesc[%u] INVALID view", static_cast<unsigned>(i));
                    return mcdr::err("pushDescriptors",
                                     "pushDescriptors: missing view for SRV entry " + std::to_string(i));
                }
                if (!reuseSrvTable) {
                    gCtx.device->CopyDescriptorsSimple(1, dst, b.view->cpuHandle,
                                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                }
                ++srvIdx;
                break;
            }
            case 2: {
                D3D12_CPU_DESCRIPTOR_HANDLE dst{cpu.ptr + static_cast<SIZE_T>(srvIdx) * gCtx.drawInc};
                if (!b.buffer || b.buffer->kind != Dx12Object::Kind::Buffer || !b.buffer->resource) {
                    dbgLogError("pushDesc[%u] INVALID texel buffer", static_cast<unsigned>(i));
                    return mcdr::err("pushDescriptors", "pushDescriptors: invalid texel buffer handle");
                }
                auto description = bufferSrvDescription(b.buffer, b.offset, b.length, b.texelFormat);
                if (!description) return std::unexpected(description.error());
                gCtx.device->CreateShaderResourceView(b.buffer->resource.Get(), &description.value(), dst);
                ++srvIdx;
                break;
            }
            default:
                return mcdr::err("pushDescriptors",
                                 "pushDescriptors: unknown binding type " + std::to_string(b.type));
        }
    }
    if (srvCount > 0 && !reuseSrvTable) {
        ctx->lastSrvSlotOffset = srvSlotOffset;
        ctx->lastSrvCount = srvCount;
        ctx->lastSrvCbvCount = cbvCount;
        ctx->lastSrvViews.clear();
        ctx->lastSrvViews.reserve(srvCount);
        for (const DrawBinding& b : bindings) {
            if (b.type == 1) {
                ctx->lastSrvViews.emplace_back(b.view, b.view ? b.view->cpuHandle.ptr : 0);
            }
        }
    }
    if (srvCount > 0) {
        const D3D12_GPU_DESCRIPTOR_HANDLE gpuRoot = descriptors.gpu;
        static UINT64 lastGpuFrame = 0;
        if (ctx->fenceValue != lastGpuFrame) {
            lastGpuFrame = ctx->fenceValue;
            dbgLogDebug(
                "pushDesc SET_ROOT_TABLE[%u]: heapBase=%llx writeBase=%llx slotCount=%u", cbvCount,
                 static_cast<unsigned long long>(drawHeapFor(ctx)->GetGPUDescriptorHandleForHeapStart().ptr),
                static_cast<unsigned long long>(base), static_cast<unsigned>(srvCount));
        }
        if (graphics) ctx->commandList->SetGraphicsRootDescriptorTable(cbvCount, gpuRoot);
        else ctx->commandList->SetComputeRootDescriptorTable(cbvCount, gpuRoot);
    }
    if (trackReads) {
        ctx->graphicsBindings = bindings;
        ctx->graphicsBindingReadCache.swap(bindingReads);
        ctx->graphicsBindingReadCacheValid = true;
        ctx->graphicsBindingsValid = true;
    }
    if (bindPoint != DescriptorBindPoint::GraphicsDeclared)
        for (const auto& binding : bindings)
            retainObjectForRecording(ctx, binding.type == 1 ? binding.view : binding.buffer);
    return {};
}

namespace {

D3D12_CPU_DESCRIPTOR_HANDLE drawHeapCpuSlot(UINT slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = gCtx.drawHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * gCtx.drawInc;
    return h;
}

mcdr::Result<void> drawHeapSlotInRange(UINT slot, const char* where) {
    if (!gCtx.drawHeap || !gCtx.device) {
        return mcdr::err(where, std::string(where) + ": drawHeap/device not ready");
    }
    if (slot >= gCtx.drawHeap->GetDesc().NumDescriptors) {
        return mcdr::err(where, std::string(where) + ": slot out of range");
    }
    return {};
}

}

mcdr::Result<void> writeCBVToDrawHeap(const UINT& slot, const Dx12Object*& buf, const long long& offset,
                                      const long long& size) {
    if (auto r = drawHeapSlotInRange(slot, "writeCBVToDrawHeap"); !r) return r;
    if (!buf || buf->kind != Dx12Object::Kind::Buffer || !buf->resource) {
        return mcdr::err("writeCBVToDrawHeap", "writeCBVToDrawHeap: invalid buffer");
    }
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv;
    cbv.BufferLocation = buf->resource->GetGPUVirtualAddress() + static_cast<UINT64>(offset);
    if (cbv.BufferLocation % 256ULL != 0) {
        return mcdr::err("writeCBVToDrawHeap", std::string("writeCBVToDrawHeap: CBV is not 256-byte aligned"
                                                           " (buffer offset=") +
                                                    std::to_string(offset) + ")");
    }
    UINT64 cbvSize = static_cast<UINT64>(size);
    cbvSize = (cbvSize + 255) & ~255ULL;
    if (cbvSize == 0) cbvSize = 256;
    cbv.SizeInBytes = static_cast<UINT>(cbvSize);
    gCtx.device->CreateConstantBufferView(&cbv, drawHeapCpuSlot(slot));
    return {};
}

mcdr::Result<void> writeTextureSRVToDrawHeap(const UINT& slot, const Dx12Object*& view) {
    if (auto r = drawHeapSlotInRange(slot, "writeTextureSRVToDrawHeap"); !r) return r;
    if (!view || view->cpuHandle.ptr == 0) {
        return mcdr::err("writeTextureSRVToDrawHeap", "writeTextureSRVToDrawHeap: invalid texture view");
    }
    gCtx.device->CopyDescriptorsSimple(1, drawHeapCpuSlot(slot), view->cpuHandle,
                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return {};
}

mcdr::Result<void> writeBufferSRVToDrawHeap(UINT slot, Dx12Object* buf, long long offset, long long length,
                                            int texelFormat) {
    if (auto r = drawHeapSlotInRange(slot, "writeBufferSRVToDrawHeap"); !r) return r;
    if (!buf || buf->kind != Dx12Object::Kind::Buffer || !buf->resource) {
        return mcdr::err("writeBufferSRVToDrawHeap", "writeBufferSRVToDrawHeap: invalid buffer");
    }
    auto description = bufferSrvDescription(buf, offset, length, texelFormat);
    if (!description) return std::unexpected(description.error());
    gCtx.device->CreateShaderResourceView(buf->resource.Get(), &description.value(), drawHeapCpuSlot(slot));
    return {};
}

namespace {

void writeRawBufferSRV(const D3D12_CPU_DESCRIPTOR_HANDLE& dst, Dx12Object* buf, long long offset,
                       long long length) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = static_cast<UINT>(static_cast<UINT64>(offset) / 4ull);
    srv.Buffer.NumElements = static_cast<UINT>(static_cast<UINT64>(length) / 4ull);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    gCtx.device->CreateShaderResourceView(buf->resource.Get(), &srv, dst);
}

long long remainingToEnd(const Dx12Object* buf, long long offset) {
    if (!buf || buf->size <= offset) return 0;
    return buf->size - offset;
}

}

namespace {
struct GraphicsCommandRead {
    ComPtr<ID3D12Resource> resource;
    std::shared_ptr<void> owner;
    uint64_t offset = 0, bytes = 0;
    uint32_t firstSubresource = 0, subresourceCount = 1, state = 0, access = 1;
    D3D12_SHADER_RESOURCE_VIEW_DESC textureView{};
    bool hasTextureView = false;
};

struct GraphicsCommandPacket {
    mcdr::graphics_recording::PacketFacts facts;
    bool confirmed = false;
    GraphicsDrawState state;
    std::shared_ptr<const void> recordingIdentity;
    ComPtr<ID3D12Fence> fence;
    uint64_t submission = 0;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12DescriptorHeap> colorDescriptors, depthDescriptors;
    ComPtr<ID3D12CommandSignature> commandSignature;
    std::shared_ptr<void> pipelineOwner, captureOwner, routeOwner;
    std::shared_ptr<const void> laneProjection;
    std::weak_ptr<void> observerOwner;
    const void* depthWriteTarget = nullptr;
    int (*confirm)(void*, void*, const void*) = nullptr;
    std::shared_ptr<mcdr::oit_budget::Reservation> budget;
    std::vector<DrawBinding> bindings;
    std::vector<DrawBinding> contentBindings;
    std::shared_ptr<const void> opaqueSeedBuffers;
    mcdr::graphics_recording::Command opaqueSeedDraw;
    uint64_t opaqueSeedProgramGeneration = 0;
    std::vector<std::shared_ptr<const void>> sampledReads;
    std::vector<std::unique_ptr<Dx12Object>> frozenBuffers;
    std::vector<std::shared_ptr<mcdr::oit_budget::Reservation>> contentBudgets;
    std::vector<GraphicsCommandRead> reads;
    std::vector<GraphicsResourceRange> attachments;
    std::vector<std::shared_ptr<void>> objectOwners;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> renderTargets;
    std::array<const Dx12Object*, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> colorObjects{};
    D3D12_CPU_DESCRIPTOR_HANDLE depthTarget{};
    D3D12_VERTEX_BUFFER_VIEW vertices[16]{};
    D3D12_INDEX_BUFFER_VIEW indices{};
    std::array<UINT, 16> vertexStrides{};
    std::vector<uint8_t> bindingTypes;
    D3D12_GPU_VIRTUAL_ADDRESS captureConstants = 0, meshGlobals = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE captureDescriptors{};
    D3D12_GPU_DESCRIPTOR_HANDLE meshGeometry{};
    D3D12_GPU_DESCRIPTOR_HANDLE bindingTable{};
    uint64_t shaderContract = 0, shaderGeneration = 0, pipelineGeneration = 0;
    int topology = 0;
    UINT pushConstantParam = UINT_MAX, geometryParam = 0, segmentParam = 0;
    D3D12_SHADING_RATE shadingRate = D3D12_SHADING_RATE_1X1;
    D3D12_SHADING_RATE_COMBINER shadingCombiners[2]{};
    ~GraphicsCommandPacket() {
        for (const auto& read : sampledReads) if (read) cancelSampledTextureContent(read);
    }
};

mcdr::Result<void> freezeGraphicsCommandContent(CommandContext* context,
    GraphicsCommandPacket& packet, uint64_t budgetLimit) {
    using mcdr::graphics_recording::CommandFamily;
    using mcdr::graphics_recording::ContentState;
    packet.facts.content = ContentState::Unsupported;
    if (packet.facts.command.family != CommandFamily::Draw || context->currentPipeline->isMesh ||
        !context->currentPipeline->vertexStrides.empty() || context->currentPipeline->captureHash ||
        context->shadingRateImageBound) return {};
    for (const auto& binding : packet.bindings) {
        if (binding.type == 1) continue;
        if (binding.type != 0 && binding.type != 2) return {};
        const auto* buffer = binding.buffer;
        if (!buffer || !buffer->resource || buffer->kind != Dx12Object::Kind::Buffer ||
            (buffer->heapType != D3D12_HEAP_TYPE_UPLOAD && buffer->heapType != D3D12_HEAP_TYPE_DEFAULT) ||
            binding.offset < 0 || binding.length <= 0 || binding.offset > buffer->size ||
            binding.length > buffer->size - binding.offset ||
            uint64_t(binding.length) > UINT64_MAX - 255u || uint64_t(binding.length) > SIZE_MAX) return {};
    }
    packet.contentBindings = packet.bindings;
    packet.sampledReads.resize(packet.bindings.size());
    packet.frozenBuffers.reserve(packet.bindings.size());
    packet.contentBudgets.reserve(packet.bindings.size());
    for (size_t index = 0; index < packet.bindings.size(); ++index) {
        const auto& source = packet.bindings[index];
        if (source.type == 1) {
            auto captured = captureSampledTextureContent(context, source.view, budgetLimit);
            if (captured.status != SampledTextureContentStatus::Ready) {
                for (const auto& read : packet.sampledReads) if (read) cancelSampledTextureContent(read);
                packet.sampledReads.clear();
                packet.contentBindings.clear();
                if (captured.status == SampledTextureContentStatus::BudgetExceeded ||
                    captured.status == SampledTextureContentStatus::RecordingFailed)
                    return mcdr::err("graphics.content", "Sampled content capture failed or exceeded its budget");
                return {};
            }
            packet.sampledReads[index] = std::move(captured.lease);
            continue;
        }
        auto object = std::make_unique<Dx12Object>();
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = (uint64_t(source.length) + 255u) & ~255ull;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const auto allocation = gCtx.device->GetResourceAllocationInfo(0, 1, &description);
        if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX)
            return mcdr::err("graphics.content", "Frozen buffer allocation size is invalid");
        auto budget = mcdr::oit_budget::reserve(allocation.SizeInBytes, budgetLimit);
        if (!budget) return mcdr::err("graphics.content", "Frozen buffer budget exceeded");
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = source.buffer->heapType;
        const auto state = heap.Type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
            : D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(gCtx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                state, nullptr, IID_PPV_ARGS(&object->resource))))
            return mcdr::err("graphics.content", "Frozen buffer allocation failed");
        object->kind = Dx12Object::Kind::Buffer;
        object->heapType = heap.Type;
        object->size = source.length;
        if (heap.Type == D3D12_HEAP_TYPE_UPLOAD) {
            void* mappedSource = source.buffer->mappedPtr;
            bool unmapSource = false;
            if (!mappedSource) {
                if (FAILED(source.buffer->resource->Map(0, nullptr, &mappedSource)))
                    return mcdr::err("graphics.content", "Source upload mapping failed");
                unmapSource = true;
            }
            void* mappedDestination = nullptr;
            const D3D12_RANGE noRead{0, 0};
            const auto mapped = object->resource->Map(0, &noRead, &mappedDestination);
            if (FAILED(mapped)) {
                if (unmapSource) source.buffer->resource->Unmap(0, nullptr);
                return mcdr::err("graphics.content", "Frozen upload mapping failed");
            }
            std::memcpy(mappedDestination, static_cast<const std::byte*>(mappedSource) + source.offset,
                static_cast<size_t>(source.length));
            object->resource->Unmap(0, nullptr);
            if (unmapSource) source.buffer->resource->Unmap(0, nullptr);
        }
        auto& destination = packet.contentBindings[index];
        destination.buffer = object.get();
        destination.offset = 0;
        packet.contentBudgets.push_back(std::move(budget));
        packet.frozenBuffers.push_back(std::move(object));
    }
    for (size_t index = 0; index < packet.bindings.size(); ++index) {
        const auto& source = packet.bindings[index];
        if (source.type == 1 || source.buffer->heapType == D3D12_HEAP_TYPE_UPLOAD) continue;
        const auto& destination = packet.contentBindings[index];
        const auto previous = resourceSubresourceState(context, source.buffer->resource.Get(), 0,
            D3D12_RESOURCE_STATE_COMMON);
        if (const auto result = transitionResourceRange(context, source.buffer->resource.Get(), 0, 1,
                previous, D3D12_RESOURCE_STATE_COPY_SOURCE); !result) return result;
        context->commandList->CopyBufferRegion(destination.buffer->resource.Get(), 0,
            source.buffer->resource.Get(), static_cast<uint64_t>(source.offset), static_cast<uint64_t>(source.length));
        if (const auto result = transitionResourceRange(context, destination.buffer->resource.Get(), 0, 1,
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON); !result) return result;
        if (const auto result = transitionResourceRange(context, source.buffer->resource.Get(), 0, 1,
                D3D12_RESOURCE_STATE_COPY_SOURCE, previous); !result) return result;
    }
    packet.facts.content = ContentState::Frozen;
    return {};
}

mcdr::Result<void> freezeTileDraw(CommandContext* context, GraphicsCommandPacket& packet,
    uint64_t budgetLimit, mcdr::graphics_recording::TileDrawScope scope) {
    using mcdr::graphics_recording::CommandFamily;
    using mcdr::graphics_recording::ContentState;
    auto* pipeline = static_cast<Dx12Pipeline*>(packet.pipelineOwner.get());
    if (!pipeline->tileRoute) return {};
    auto& route = *pipeline->tileRoute;
    const uint32_t stage = scope == mcdr::graphics_recording::TileDrawScope::Seed
        ? UINT32_MAX : static_cast<uint32_t>(scope);
    auto draw = packet.facts.command;
    const bool indexed = draw.family == CommandFamily::DrawIndexed;
    packet.facts.content = ContentState::Unsupported;
    static thread_local std::unordered_set<const Dx12Pipeline*> reportedTileInputs;
    const auto explain = [&](const char* reason) -> mcdr::Result<void> {
        if (reportedTileInputs.insert(pipeline).second)
            dbgLogDebug("OIT tile draw input: %s (%u, %u)", reason,
                static_cast<uint32_t>(scope), static_cast<uint32_t>(draw.family));
        return {};
    };
    if ((draw.family != CommandFamily::Draw && !indexed) || !route.pso || route.tileStage != stage ||
        route.programGeneration != context->frameProgramGeneration || packet.pso != pipeline->withDepth ||
        packet.bindings.size() < route.bindingCount ||
        !packet.state.viewportValid || !packet.state.scissorValid ||
        (pipeline->pushConstantBytes &&
            (!packet.state.pushConstantsValid || packet.state.pushRoot != pipeline->rootSignature.Get() ||
                packet.state.pushConstantBytes != pipeline->pushConstantBytes))) {
        if (reportedTileInputs.insert(pipeline).second)
            dbgLogDebug("OIT tile state: stage=%u/%u generation=%llu/%llu depth=%d bindings=%zu/%u vp=%d sc=%d pc=%d",
                route.tileStage, stage, route.programGeneration, context->frameProgramGeneration,
                packet.pso == pipeline->withDepth, packet.bindings.size(), route.bindingCount,
                packet.state.viewportValid, packet.state.scissorValid, packet.state.pushConstantsValid);
        return {};
    }
    packet.opaqueSeedProgramGeneration = route.programGeneration;
    const auto bindings = std::span(packet.bindings).first(route.bindingCount);
    std::vector<PacketContentRange> ranges;
    ranges.reserve(pipeline->vertexStrides.size() + indexed + bindings.size());
    const auto appendBuffer = [&](Dx12Object* buffer, uint64_t offset, uint64_t length,
                                  D3D12_RESOURCE_STATES state) {
        if (!buffer || !buffer->resource || buffer->kind != Dx12Object::Kind::Buffer ||
            (buffer->heapType != D3D12_HEAP_TYPE_DEFAULT && buffer->heapType != D3D12_HEAP_TYPE_UPLOAD) ||
            buffer->size <= 0 || !length || offset > uint64_t(buffer->size) ||
            length > uint64_t(buffer->size) - offset) return false;
        const bool upload = buffer->heapType == D3D12_HEAP_TYPE_UPLOAD;
        ranges.push_back({buffer->resource, 0, 1, offset, length, upload,
            static_cast<uint32_t>(upload ? D3D12_RESOURCE_STATE_GENERIC_READ : state), buffer});
        return true;
    };
    for (const auto& [slot, stride] : pipeline->vertexStrides) {
        auto* buffer = context->graphicsVertexBuffers[slot];
        const auto& view = packet.vertices[slot];
        if (!stride || !view.SizeInBytes || stride != view.StrideInBytes) return explain("vertex stride");
        const auto stepRate = route.vertexStepRates[slot];
        uint64_t firstByte = 0;
        uint64_t bytes = view.SizeInBytes;
        if (stepRate) {
            const uint64_t lastElement = uint64_t(draw.firstInstance) + (draw.instanceCount - 1) / stepRate;
            if (lastElement >= bytes / stride) return explain("instance range");
            bytes = (lastElement + 1) * stride;
        } else if (!indexed) {
            if (uint64_t(draw.firstVertexOrIndex) + draw.count > bytes / stride) return explain("vertex range");
            firstByte = uint64_t(draw.firstVertexOrIndex) * stride;
            bytes = uint64_t(draw.count) * stride;
        } else if (!context->graphicsVertexSlices[slot]) {
            return explain("missing vertex range");
        }
        const auto offset = view.BufferLocation - buffer->resource->GetGPUVirtualAddress();
        if (!appendBuffer(buffer, offset + firstByte, bytes, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER))
            return explain("vertex buffer");
    }
    if (indexed) {
        auto* buffer = context->graphicsIndexBuffer;
        const auto& view = packet.indices;
        const uint32_t stride = view.Format == DXGI_FORMAT_R32_UINT ? 4 : 2;
        if (uint64_t(draw.firstVertexOrIndex) + draw.count > view.SizeInBytes / stride) return explain("index range");
        const auto offset = view.BufferLocation - buffer->resource->GetGPUVirtualAddress();
        if (!appendBuffer(buffer, offset + uint64_t(draw.firstVertexOrIndex) * stride,
                uint64_t(draw.count) * stride, D3D12_RESOURCE_STATE_INDEX_BUFFER)) return explain("index buffer");
    }
    for (const auto& binding : bindings) {
        if (binding.type == 1) continue;
        if (binding.offset < 0 || binding.length <= 0 ||
            (binding.type != 0 && binding.type != 2) ||
            (binding.type == 0 && binding.offset % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT != 0))
            return explain("shader buffer binding");
        const auto state = binding.type == 0 ? D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
            : D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        if (!appendBuffer(binding.buffer, binding.offset, binding.length, state)) return explain("shader buffer range");
    }
    packet.sampledReads.resize(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
        if (bindings[i].type != 1 || std::ranges::contains(route.laneBindingSlots, i)) continue;
        auto captured = captureSampledTextureContent(context, bindings[i].view, budgetLimit);
        if (captured.status != SampledTextureContentStatus::Ready) {
            for (const auto& read : packet.sampledReads) cancelSampledTextureContent(read);
            if (captured.status == SampledTextureContentStatus::RecordingFailed)
                return mcdr::err("opaque.seed.content", "Sampled input capture failed during recording");
            if (reportedTileInputs.insert(pipeline).second)
                dbgLogDebug("OIT sampled input: stage=%u slot=%zu status=%u", stage, i,
                    static_cast<uint32_t>(captured.status));
            return {};
        }
        packet.sampledReads[i] = std::move(captured.lease);
    }
    if (!ranges.empty()) {
        packet.opaqueSeedBuffers = retainPacketContent(context, ranges.data(), static_cast<uint32_t>(ranges.size()),
            budgetLimit);
        if (packetContentStatus(context, packet.opaqueSeedBuffers) != PacketContentStatus::Frozen) {
            for (const auto& read : packet.sampledReads) cancelSampledTextureContent(read);
            if (context->recordingFailed.load() || context->submissionFailed.load())
                return mcdr::err("opaque.seed.content", "Buffer capture failed during recording");
            packet.opaqueSeedBuffers.reset();
            return explain("buffer content");
        }
    }
    packet.contentBindings.assign(bindings.begin(), bindings.end());
    for (const auto slot : route.laneBindingSlots)
        if (slot != UINT32_MAX) packet.contentBindings[slot].view = &route.lanePlaceholder;
    if (const auto content = std::static_pointer_cast<const PacketContentLease>(packet.opaqueSeedBuffers)) {
        auto input = content->frozenReads.begin();
        for (const auto& [slot, stride] : pipeline->vertexStrides) {
            auto& view = packet.vertices[slot];
            view.BufferLocation = input->resource->GetGPUVirtualAddress() + input->byteOffset;
            view.SizeInBytes = static_cast<UINT>(input->byteLength);
            ++input;
        }
        if (indexed) {
            packet.indices.BufferLocation = input->resource->GetGPUVirtualAddress() + input->byteOffset;
            packet.indices.SizeInBytes = static_cast<UINT>(input->byteLength);
            ++input;
        }
        for (auto& binding : packet.contentBindings) {
            if (binding.type == 1) continue;
            binding.buffer = input->object;
            binding.offset = static_cast<long long>(input->byteOffset);
            ++input;
        }
    }
    if (indexed || !pipeline->vertexStrides.empty()) draw.firstVertexOrIndex = 0;
    packet.opaqueSeedDraw = draw;
    packet.facts.content = ContentState::Frozen;
    return {};
}

mcdr::Result<std::shared_ptr<GraphicsCommandPacket>> prepareGraphicsCommand(CommandContext* context,
    const mcdr::graphics_recording::Command& command, const DrawBinding* meshBindings = nullptr,
    uint32_t meshBindingCount = 0, D3D12_GPU_DESCRIPTOR_HANDLE meshGeometry = {},
    D3D12_GPU_VIRTUAL_ADDRESS meshGlobals = 0,
    const mcdr::graphics_recording::AdapterFacts& adapter = {}) {
    using mcdr::graphics_recording::CommandFamily;
    const auto observer = context->graphicsCommandObserver;
    if (!observer.prepare) return std::shared_ptr<GraphicsCommandPacket>{};
    if (observer.depthWriteTarget &&
        (observer.depthWriteTarget != context->activeDepthTarget || !context->graphicsHasDepth ||
            !context->currentPipeline || !context->currentPipeline->depthWrite))
        return std::shared_ptr<GraphicsCommandPacket>{};
    auto owner = observer.owner.lock();
    if (!owner || !observer.confirm || !observer.complete || !context->recordingIdentity || !context->fence ||
        context->submissionFailed.load() || context->recordingFailed.load() ||
        !context->currentPipeline || !context->cachedPso)
        return mcdr::err("graphics.record", "Graphics command observer or recording is stale");
    const bool indexed = command.family == CommandFamily::DrawIndexed || command.family == CommandFamily::DrawIndexedIndirect;
    if (const auto result = collectGraphicsInputs(context, indexed, command.indirectBuffer); !result)
        return std::unexpected(result.error());
    auto* pipeline = context->currentPipeline;
    const uint64_t bindingCount = context->graphicsBindings.size() + meshBindingCount;
    const uint64_t readCapacity = context->graphicsInputRanges.size() + bindingCount + 18;
    const uint64_t ownerCapacity = bindingCount + readCapacity + context->activeAttachmentRanges.size() + 20;
    const uint64_t budgetBytes = sizeof(GraphicsCommandPacket) + 512 +
        bindingCount * (2 * sizeof(DrawBinding) + sizeof(std::shared_ptr<const void>) +
            sizeof(std::unique_ptr<Dx12Object>) + sizeof(std::shared_ptr<mcdr::oit_budget::Reservation>) + sizeof(Dx12Object)) +
        readCapacity * sizeof(GraphicsCommandRead) +
        pipeline->graphicsBindingTypes.size() * sizeof(uint8_t) +
        ownerCapacity * (sizeof(std::shared_ptr<void>) + 128) +
        context->activeAttachmentRanges.size() * sizeof(GraphicsResourceRange) +
        (context->activePassRtvs.size() + 2) * sizeof(D3D12_CPU_DESCRIPTOR_HANDLE);
    auto budget = mcdr::oit_budget::reserve(budgetBytes, observer.totalBudgetBytes);
    if (!budget) {
        if (!observer.budgetExceeded) return mcdr::err("graphics.record", "Graphics command packet budget exceeded");
        observer.budgetExceeded(owner.get(), context);
        return std::shared_ptr<GraphicsCommandPacket>{};
    }
    auto packet = std::make_shared<GraphicsCommandPacket>();
    packet->budget = std::move(budget);
    packet->reads.reserve(static_cast<size_t>(readCapacity));
    packet->objectOwners.reserve(static_cast<size_t>(ownerCapacity));
    packet->bindings.reserve(static_cast<size_t>(bindingCount));
    packet->attachments.reserve(context->activeAttachmentRanges.size());
    packet->renderTargets.reserve(context->activePassRtvs.size() + 2);
    packet->bindings.assign(context->graphicsBindings.begin(), context->graphicsBindings.end());
    for (uint32_t index = 0; index < meshBindingCount; ++index) packet->bindings.push_back(meshBindings[index]);
    packet->state = context->graphicsDrawState;
    packet->recordingIdentity = context->recordingIdentity;
    packet->fence = context->fence;
    packet->submission = context->fenceValue + 1;
    packet->root = pipeline->rootSignature;
    packet->pso = context->cachedPso;
    packet->descriptors = drawHeapFor(context);
    packet->colorDescriptors = gCtx.frameRtvHeap;
    packet->depthDescriptors = gCtx.dsvHeap;
    packet->bindingTable = context->graphicsBindingTable;
    packet->captureOwner = context->graphicsCapture.lease;
    packet->routeOwner = pipeline->terrainTileRoute;
    packet->observerOwner = observer.owner;
    packet->depthWriteTarget = observer.depthWriteTarget;
    packet->confirm = observer.confirm;
    packet->captureConstants = context->graphicsCapture.constants;
    packet->captureDescriptors = context->graphicsCapture.descriptors;
    packet->meshGeometry = meshGeometry;
    packet->meshGlobals = meshGlobals;
    packet->shaderContract = pipeline->captureHash;
    packet->shaderGeneration = pipeline->captureGeneration;
    packet->pipelineGeneration = pipeline->shaderGeneration;
    packet->pushConstantParam = pipeline->pushConstantParam;
    packet->geometryParam = pipeline->geometryParam;
    packet->segmentParam = pipeline->segmentParam;
    packet->topology = pipeline->topology;
    packet->bindingTypes = pipeline->graphicsBindingTypes;
    for (const auto& input : pipeline->vertexStrides)
        if (input.first >= 0 && input.first < 16) packet->vertexStrides[input.first] = input.second;
    packet->shadingRate = context->recordedShadingRate;
    std::copy_n(context->recordedShadingCombiners, 2, packet->shadingCombiners);
    std::copy_n(context->graphicsVertexViews, 16, packet->vertices);
    packet->indices = context->graphicsIndexView;
    if (pipeline->colorTargetCount != 0) {
        packet->renderTargets = context->activePassRtvs;
        if (context->additionalColorTarget) packet->renderTargets.push_back(context->additionalColorRtv);
        if (context->rasterMotionSelected) packet->renderTargets.push_back(context->activeMotionRtv);
        std::copy_n(context->activeColorTargets.begin(),
            std::min(context->activeColorTargets.size(), packet->colorObjects.size()), packet->colorObjects.begin());
    }
    packet->depthTarget = context->activePassDsv;
    pipeline->lifetimeReferences.fetch_add(1, std::memory_order_relaxed);
    packet->pipelineOwner = std::shared_ptr<void>(pipeline, [](void* retained) {
        destroyPipeline(static_cast<Dx12Pipeline*>(retained));
    });
    for (const auto& range : context->graphicsInputRanges) {
        GraphicsCommandRead read;
        read.resource = range.object->resource;
        read.owner = range.owner ? range.owner : retainObjectLease(range.object);
        read.firstSubresource = range.firstSubresource;
        read.subresourceCount = range.subresourceCount;
        read.state = range.state;
        read.access = range.access;
        packet->reads.push_back(std::move(read));
    }
    for (const auto& binding : packet->bindings) {
        auto* object = binding.type == 1 ? binding.view : binding.buffer;
        if (!object || (binding.type == 1 && !object->sourceTexture))
            return mcdr::err("graphics.record", "Graphics command binding is invalid");
        packet->objectOwners.push_back(retainObjectLease(object));
        GraphicsCommandRead read;
        if (binding.type == 1) {
            read.resource = object->sourceTexture->resource;
            read.textureView = object->srvDesc;
            read.hasTextureView = object->hasSrvDesc;
        } else {
            if (binding.offset < 0 || binding.length < 0 || binding.offset > object->size ||
                binding.length > object->size - binding.offset)
                return mcdr::err("graphics.record", "Graphics command buffer range is invalid");
            read.resource = object->resource;
            read.offset = static_cast<uint64_t>(binding.offset);
            read.bytes = static_cast<uint64_t>(binding.length ? binding.length : object->size - binding.offset);
        }
        packet->reads.push_back(std::move(read));
    }
    if (!pipeline->isMesh) {
        for (const auto& input : pipeline->vertexStrides) {
            const auto slot = input.first;
            auto* buffer = context->graphicsVertexBuffers[slot];
            GraphicsCommandRead read;
            read.resource = buffer->resource;
            read.offset = packet->vertices[slot].BufferLocation - buffer->resource->GetGPUVirtualAddress();
            read.bytes = packet->vertices[slot].SizeInBytes;
            packet->reads.push_back(std::move(read));
        }
        if (indexed) {
            auto* buffer = context->graphicsIndexBuffer;
            GraphicsCommandRead read;
            read.resource = buffer->resource;
            read.offset = packet->indices.BufferLocation - buffer->resource->GetGPUVirtualAddress();
            read.bytes = packet->indices.SizeInBytes;
            packet->reads.push_back(std::move(read));
        }
    }
    if (command.indirectBuffer) {
        auto* buffer = static_cast<Dx12Object*>(command.indirectBuffer);
        GraphicsCommandRead read;
        read.resource = buffer->resource;
        read.offset = command.indirectOffset;
        read.bytes = uint64_t(command.count) * (indexed ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS));
        if (read.offset > static_cast<uint64_t>(buffer->size) || read.bytes > static_cast<uint64_t>(buffer->size) - read.offset)
            return mcdr::err("graphics.record", "Graphics indirect command range is invalid");
        packet->reads.push_back(std::move(read));
        packet->commandSignature = indexed ? gCtx.cmdSigIndexed : gCtx.cmdSigNonIndexed;
    }
    packet->attachments = context->activeAttachmentRanges;
    for (auto& attachment : packet->attachments)
        if (!attachment.owner) attachment.owner = retainObjectLease(attachment.object);
    for (auto* buffer : context->graphicsVertexBuffers)
        if (buffer) packet->objectOwners.push_back(retainObjectLease(buffer));
    if (context->graphicsIndexBuffer) packet->objectOwners.push_back(retainObjectLease(context->graphicsIndexBuffer));
    if (context->shadingRateImageBound) packet->objectOwners.push_back(retainObjectLease(context->shadingRateImageBound));
    static std::atomic<uint64_t> nextIdentity{1};
    auto identity = nextIdentity.load(std::memory_order_relaxed);
    do {
        if (identity == UINT64_MAX) return mcdr::err("graphics.record", "Graphics command packet identity exhausted");
    } while (!nextIdentity.compare_exchange_weak(identity, identity + 1, std::memory_order_relaxed));
    packet->facts.command = command;
    packet->facts.adapterFacts = adapter;
    packet->facts.adapter = adapter.state;
    packet->facts.pipelineIdentity = reinterpret_cast<uintptr_t>(pipeline);
    packet->facts.packetIdentity = identity;
    packet->facts.readSetIdentity = identity;
    packet->facts.empty = command.family == CommandFamily::Mesh
        ? command.meshGroups[0] == 0 || command.meshGroups[1] == 0 || command.meshGroups[2] == 0
        : command.count == 0 || ((command.family == CommandFamily::Draw || command.family == CommandFamily::DrawIndexed) && command.instanceCount == 0);
    auto& leases = context->recordingLeases[context->currentListSlot];
    leases.reserve(leases.size() + 1);
    leases.push_back(packet);
    const auto& operation = pipeline->oitLaneOperation;
    if (operation.kind == MCDR_OIT_LANE_BOUNDS_CULL && !packet->facts.empty &&
        operation.projectionBinding < packet->bindings.size()) {
        constexpr uint64_t projectionBytes = 64;
        const auto& binding = packet->bindings[operation.projectionBinding];
        auto* buffer = binding.buffer;
        if (binding.type == 0 && buffer && buffer->resource && binding.offset >= 0 &&
            binding.offset <= buffer->size && buffer->size - binding.offset >= projectionBytes &&
            (binding.length == 0 || binding.length >= projectionBytes)) {
            const bool cpuWritable = buffer->heapType == D3D12_HEAP_TYPE_UPLOAD;
            PacketContentRange range{buffer->resource, 0, 1, static_cast<uint64_t>(binding.offset),
                projectionBytes, cpuWritable, static_cast<uint32_t>(cpuWritable ? D3D12_RESOURCE_STATE_GENERIC_READ :
                    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER), buffer};
            packet->laneProjection = retainPacketContent(context, &range, 1);
            if (context->recordingFailed.load() || context->submissionFailed.load())
                return mcdr::err("graphics.record", "Lane projection freeze failed during recording");
        }
    }
    if (observer.contentPolicy == mcdr::graphics_recording::ContentPolicy::FreezeBindings) {
        if (const auto frozen = freezeGraphicsCommandContent(context, *packet, observer.totalBudgetBytes); !frozen)
            return std::unexpected(frozen.error());
    } else if (!packet->facts.empty) {
        using mcdr::graphics_recording::TileDrawScope;
        const auto scope = observer.depthWriteTarget ? TileDrawScope::Seed
            : observer.tileDrawScope ? observer.tileDrawScope(owner.get()) : TileDrawScope::None;
        if (scope != TileDrawScope::None)
            if (const auto frozen = freezeTileDraw(context, *packet, observer.totalBudgetBytes, scope); !frozen)
                return std::unexpected(frozen.error());
    }
    if (observer.prepare(owner.get(), context, packet->facts, packet) != 1)
        return mcdr::err("graphics.record", "Graphics command observer preparation failed");
    return packet;
}

}

int bindGraphicsCommandObserver(void* opaqueContext, const mcdr::graphics_recording::Observer& observer) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !context->listOpen || !context->recordingIdentity || context->graphicsCommandObserver.prepare ||
        !context->commandList || !context->fence ||
        !observer.prepare || !observer.confirm || !observer.complete || observer.owner.expired() || !observer.totalBudgetBytes ||
        context->submissionFailed.load() || context->recordingFailed.load()) return -1;
    context->graphicsCommandObserver = observer;
    return 1;
}

void unbindGraphicsCommandObserver(void* opaqueContext) {
    if (auto* context = static_cast<CommandContext*>(opaqueContext)) context->graphicsCommandObserver = {};
}

int prepareGraphicsCommandCapture(void* opaqueContext, const mcdr::graphics_recording::Command& command,
    std::shared_ptr<const void>* output) {
    return prepareGraphicsCommandCaptureWithInputs(opaqueContext, command, nullptr, 0, {}, 0, {}, output);
}

int prepareGraphicsCommandCaptureWithInputs(void* opaqueContext, const mcdr::graphics_recording::Command& command,
    const DrawBinding* bindings, uint32_t bindingCount, D3D12_GPU_DESCRIPTOR_HANDLE geometry,
    D3D12_GPU_VIRTUAL_ADDRESS globals, const mcdr::graphics_recording::AdapterFacts& adapter,
    std::shared_ptr<const void>* output) {
    if (output) output->reset();
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !output || !context->listOpen || !context->commandList || !context->inRenderPass) return -1;
    try {
        auto prepared = prepareGraphicsCommand(context, command, bindings, bindingCount, geometry, globals, adapter);
        if (prepared) {
            *output = std::move(*prepared);
            return 1;
        }
    } catch (...) {
        context->recordingFailed.store(true, std::memory_order_release);
        return -1;
    }
    context->recordingFailed.store(true, std::memory_order_release);
    return -1;
}

int confirmGraphicsCommandCapture(void* opaqueContext, const std::shared_ptr<const void>& opaquePacket) {
    if (!opaquePacket) return 1;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context) return -1;
    try {
        auto packet = std::static_pointer_cast<GraphicsCommandPacket>(std::const_pointer_cast<void>(opaquePacket));
        auto owner = packet->observerOwner.lock();
        if (owner && packet->confirm && packet->recordingIdentity == context->recordingIdentity &&
            packet->fence == context->fence && packet->submission == context->fenceValue + 1 &&
            !context->submissionFailed.load() && !context->recordingFailed.load() &&
            packet->confirm(owner.get(), context, packet.get()) == 1) {
            packet->confirmed = true;
            return 1;
        }
    } catch (...) {
        context->recordingFailed.store(true, std::memory_order_release);
        return -1;
    }
    context->recordingFailed.store(true, std::memory_order_release);
    return -1;
}

bool graphicsCommandPacketMatchesObserver(void* opaqueContext, const std::shared_ptr<const void>& opaquePacket,
    const mcdr::graphics_recording::Observer& observer, uint32_t commandFamily) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !opaquePacket || !context->currentPipeline || !context->cachedPso) return false;
    const auto& bound = context->graphicsCommandObserver;
    auto expectedOwner = observer.owner.lock();
    auto boundOwner = bound.owner.lock();
    if (!expectedOwner || expectedOwner != boundOwner || bound.prepare != observer.prepare ||
        bound.confirm != observer.confirm || bound.complete != observer.complete ||
        bound.totalBudgetBytes != observer.totalBudgetBytes || bound.contentPolicy != observer.contentPolicy ||
        bound.depthWriteTarget != observer.depthWriteTarget ||
        bound.budgetExceeded != observer.budgetExceeded) return false;
    const auto packet = std::static_pointer_cast<const GraphicsCommandPacket>(opaquePacket);
    auto packetOwner = packet->observerOwner.lock();
    return packetOwner == expectedOwner && packet->confirm == observer.confirm &&
        packet->depthWriteTarget == observer.depthWriteTarget &&
        packet->recordingIdentity == context->recordingIdentity && packet->fence == context->fence &&
        packet->submission == context->fenceValue + 1 && packet->pso.Get() == context->cachedPso &&
        packet->pipelineGeneration == context->currentPipeline->shaderGeneration &&
        packet->facts.pipelineIdentity == reinterpret_cast<uintptr_t>(context->currentPipeline) &&
        static_cast<uint32_t>(packet->facts.command.family) == commandFamily &&
        !context->recordingFailed.load(std::memory_order_acquire) &&
        !context->submissionFailed.load(std::memory_order_acquire);
}

GraphicsPacketInspection inspectGraphicsCommandPacket(void* opaqueContext,
    const std::shared_ptr<const void>& opaquePacket) {
    GraphicsPacketInspection result;
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !opaquePacket || !context->listOpen || !context->commandList ||
        context->recordingFailed.load(std::memory_order_acquire) ||
        context->submissionFailed.load(std::memory_order_acquire)) {
        result.status = context && context->recordingFailed.load(std::memory_order_acquire)
            ? GraphicsPacketInspectionStatus::RecordingFailed : GraphicsPacketInspectionStatus::StaleRecording;
        return result;
    }
    try {
        const auto packet = std::static_pointer_cast<const GraphicsCommandPacket>(opaquePacket);
        if (packet->recordingIdentity != context->recordingIdentity || packet->fence != context->fence ||
            packet->submission != context->fenceValue + 1) {
            result.status = GraphicsPacketInspectionStatus::StaleRecording;
            return result;
        }
        result.confirmed = packet->confirmed;
        result.content = packet->facts.content;
        result.adapter = packet->facts.adapterFacts;
        result.depthWrite = packet->pipelineOwner &&
            static_cast<const Dx12Pipeline*>(packet->pipelineOwner.get())->depthWrite;
        const auto depthAttachment = std::find_if(packet->attachments.begin(), packet->attachments.end(),
            [&](const auto& range) {
                return range.object && range.object->resource && range.firstSubresource == 0 &&
                    range.subresourceCount == 1 && range.state == D3D12_RESOURCE_STATE_DEPTH_WRITE;
            });
        result.depthTargetBound = packet->depthTarget.ptr != 0 && depthAttachment != packet->attachments.end();
        result.depthObject = result.depthTargetBound ? depthAttachment->object : nullptr;
        result.routeBound = static_cast<bool>(packet->routeOwner);
        result.opaqueSeedProgramGeneration = packet->opaqueSeedProgramGeneration;
        result.pipelineIdentity = packet->facts.pipelineIdentity;
        result.shaderGeneration = packet->pipelineGeneration;
        result.commandFamily = static_cast<uint32_t>(packet->facts.command.family);
        result.depthTarget = packet->depthTarget;
        result.pso = packet->pso;
        result.viewport = packet->state.viewport;
        result.scissor = packet->state.scissor;
        const auto* pipeline = static_cast<const Dx12Pipeline*>(packet->pipelineOwner.get());
        if (pipeline && pipeline->tileRoute && packet->opaqueSeedProgramGeneration)
            result.tileStage = pipeline->tileRoute->tileStage;
        if (pipeline && pipeline->oitLaneOperation.kind) {
            result.oitLaneOperation = pipeline->oitLaneOperation;
            result.laneProjection = packet->laneProjection;
            const auto& command = packet->facts.command;
            result.fullScreenTriangle = packet->state.viewportValid && packet->state.scissorValid &&
                command.family == mcdr::graphics_recording::CommandFamily::Draw &&
                command.count == 3 && command.instanceCount == 1 &&
                command.firstVertexOrIndex == 0 && command.firstInstance == 0;
        }
        result.status = GraphicsPacketInspectionStatus::Ready;
        return result;
    } catch (...) {
        result.status = GraphicsPacketInspectionStatus::Unsupported;
        return result;
    }
}

void cancelOpaqueSeedPacketCapture(const std::shared_ptr<const void>& opaquePacket) {
    const auto packet = std::static_pointer_cast<const GraphicsCommandPacket>(opaquePacket);
    if (!packet || !packet->opaqueSeedProgramGeneration) return;
    for (const auto& read : packet->sampledReads) cancelSampledTextureContent(read);
}

bool matchesGraphicsCommandTexture(const std::shared_ptr<const void>& opaquePacket, const void* opaqueTexture,
    uint32_t subresource, GraphicsCommandTextureBinding binding, uint32_t bindingIndex) {
    if (!opaquePacket || !opaqueTexture) return false;
    const auto packet = std::static_pointer_cast<const GraphicsCommandPacket>(opaquePacket);
    const auto* texture = static_cast<const Dx12Object*>(opaqueTexture);
    if (!packet->confirmed || !texture->resource || texture->kind != Dx12Object::Kind::Texture)
        return false;
    if (binding == GraphicsCommandTextureBinding::Sampled) {
        if (bindingIndex != UINT32_MAX) {
            const auto* pipeline = static_cast<const Dx12Pipeline*>(packet->pipelineOwner.get());
            if (!pipeline || !pipeline->tileRoute || bindingIndex >= pipeline->tileRoute->laneBindingSlots.size())
                return false;
            const auto slot = pipeline->tileRoute->laneBindingSlots[bindingIndex];
            if (slot >= packet->bindings.size()) return false;
            const auto& source = packet->bindings[slot];
            const auto* view = source.view;
            return source.type == 1 && view && view->sourceTexture == texture && view->hasSrvDesc &&
                view->srvDesc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D &&
                view->srvDesc.Texture2D.PlaneSlice == 0 && view->srvDesc.Texture2D.MostDetailedMip == subresource;
        }
        return std::ranges::any_of(packet->reads, [&](const auto& read) {
            constexpr uint32_t pixelRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            return read.resource == texture->resource && (read.state & pixelRead) != 0 &&
                read.access == 1 && subresource >= read.firstSubresource &&
                subresource - read.firstSubresource < read.subresourceCount;
        });
    }
    const auto* pipeline = static_cast<const Dx12Pipeline*>(packet->pipelineOwner.get());
    if (!pipeline) return false;
    const bool depth = binding == GraphicsCommandTextureBinding::DepthTarget ||
        binding == GraphicsCommandTextureBinding::DepthAttachment;
    if (binding == GraphicsCommandTextureBinding::DepthTarget && !pipeline->depthWrite) return false;
    if (depth ? !packet->depthTarget.ptr || packet->pso != pipeline->withDepth :
        !pipeline->colorTargetCount || packet->renderTargets.empty()) return false;
    if (!depth && bindingIndex != UINT32_MAX &&
        (bindingIndex >= packet->colorObjects.size() || packet->colorObjects[bindingIndex] != texture))
        return false;
    const uint32_t state = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    return std::ranges::any_of(packet->attachments, [&](const auto& attachment) {
        return attachment.object == texture && attachment.state == state &&
            subresource >= attachment.firstSubresource &&
            subresource - attachment.firstSubresource < attachment.subresourceCount;
    });
}

mcdr::Result<GraphicsCommandContentView> resolveGraphicsCommandContent(
    void* opaqueContext, const std::shared_ptr<const void>& opaquePacket) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context || !opaquePacket || !context->listOpen || !context->commandList ||
        context->recordingFailed.load(std::memory_order_acquire) ||
        context->submissionFailed.load(std::memory_order_acquire))
        return GraphicsCommandContentView{PacketContentStatus::StaleRecording};
    const auto packet = std::static_pointer_cast<GraphicsCommandPacket>(
        std::const_pointer_cast<void>(opaquePacket));
    if (packet->recordingIdentity != context->recordingIdentity || packet->fence != context->fence ||
        packet->submission != context->fenceValue + 1)
        return GraphicsCommandContentView{PacketContentStatus::StaleRecording};
    if (packet->facts.content != mcdr::graphics_recording::ContentState::Frozen)
        return GraphicsCommandContentView{PacketContentStatus::UnsupportedContent};
    if (packet->contentBindings.size() != packet->sampledReads.size())
        return GraphicsCommandContentView{PacketContentStatus::UnsupportedContent};
    for (size_t index = 0; index < packet->sampledReads.size(); ++index) {
        if (!packet->sampledReads[index]) continue;
        const auto resolved = resolveSampledTextureContent(context, packet->sampledReads[index]);
        if (resolved.status == SampledTextureContentStatus::BudgetExceeded ||
            resolved.status == SampledTextureContentStatus::RecordingFailed)
            return mcdr::err("graphics.content", "Frozen sampled content resolution failed or exceeded its budget");
        if (resolved.status != SampledTextureContentStatus::Ready || !resolved.view)
            return GraphicsCommandContentView{PacketContentStatus::StaleRecording};
        packet->contentBindings[index].view = static_cast<Dx12Object*>(resolved.view);
    }
    return GraphicsCommandContentView{PacketContentStatus::Frozen,
        packet->contentBindings.data(), static_cast<uint32_t>(packet->contentBindings.size())};
}

mcdr::Result<void> recordTileGraphicsPacket(void* opaqueContext, const std::shared_ptr<const void>& opaquePacket,
    std::span<const D3D12_VIEWPORT> viewports, std::span<const D3D12_RECT> scissors,
    D3D12_GPU_VIRTUAL_ADDRESS batchConstants, std::span<Dx12Object* const> laneReads) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    const auto packet = std::static_pointer_cast<const GraphicsCommandPacket>(opaquePacket);
    if (!context || !packet || !packet->confirmed || !packet->opaqueSeedProgramGeneration ||
        packet->opaqueSeedProgramGeneration != context->frameProgramGeneration ||
        !batchConstants || batchConstants % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT ||
        viewports.empty() || viewports.size() > D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE ||
        scissors.size() != viewports.size())
        return mcdr::err("oit.tile.ia", "Tile draw state is incomplete");
    const auto resolved = resolveGraphicsCommandContent(context, opaquePacket);
    if (!resolved) return std::unexpected(resolved.error());
    if (resolved->status != PacketContentStatus::Frozen)
        return mcdr::err("oit.tile.ia", "Tile draw input unavailable");
    const auto& bindings = packet->contentBindings;
    const auto* pipeline = static_cast<const Dx12Pipeline*>(packet->pipelineOwner.get());
    const auto& route = *pipeline->tileRoute;
    const bool sampledLane = route.laneDescriptorParam != UINT_MAX;
    if (sampledLane ? laneReads.size() != route.laneBindingSlots.size() : !laneReads.empty())
        return mcdr::err("oit.tile.ia", "Tile draw lane input list is incomplete");
    std::vector<DrawBinding> materialReads;
    if (sampledLane) {
        materialReads.reserve(bindings.size());
        for (const auto& binding : bindings)
            if (binding.view != &route.lanePlaceholder) materialReads.push_back(binding);
    }
    const auto& effectiveReads = sampledLane ? materialReads : bindings;
    std::vector<GraphicsResourceRange> reads;
    if (const auto result = bindingReadRanges(context, effectiveReads.data(),
            static_cast<uint32_t>(effectiveReads.size()), reads, false); !result) return result;
    if (const auto buffers = std::static_pointer_cast<const PacketContentLease>(packet->opaqueSeedBuffers)) {
        if (packetContentStatus(context, packet->opaqueSeedBuffers) != PacketContentStatus::Frozen)
            return mcdr::err("oit.tile.ia", "Tile buffers are stale");
        auto& pending = context->graphicsCollectScratch;
        pending.clear();
        for (size_t i = 0; i < buffers->frozenReads.size(); ++i) {
            const auto& range = buffers->frozenReads[i];
            if (const auto result = addBufferRead(pending, range.object,
                    buffers->reads[i].initialState); !result) return result;
        }
        if (const auto result = mergeGraphicsReads(context, reads, pending); !result) return result;
    }
    for (auto& read : reads) read.owner = std::const_pointer_cast<void>(opaquePacket);
    if (const auto result = applyGraphicsRanges(context, reads); !result) return result;
    if (sampledLane) {
        const auto materialDescriptors = std::ranges::count_if(bindings, [](const DrawBinding& binding) {
            return binding.type != 0;
        });
        if (!growDrawHeap(context, static_cast<UINT>(materialDescriptors + laneReads.size())))
            return mcdr::err("oit.tile.ia", "Tile draw descriptor reservation failed");
    }
    auto* list = context->commandList;
    list->SetPipelineState(route.pso.Get());
    list->SetGraphicsRootSignature(route.root.Get());
    if (const auto result = pushDescriptors(context, bindings, DescriptorBindPoint::GraphicsDeclared); !result)
        return result;
    if (sampledLane) {
        const auto descriptors = allocateDrawDescriptors(context, static_cast<UINT>(laneReads.size()));
        if (!descriptors) return std::unexpected(descriptors.error());
        auto cpu = descriptors->cpu;
        for (size_t index = 0; index < laneReads.size(); ++index) {
            auto* object = route.laneBindingSlots[index] == UINT32_MAX ? nullptr : laneReads[index];
            if (route.laneBindingSlots[index] != UINT32_MAX && (!object || !object->resource))
                return mcdr::err("oit.tile.ia", "Tile draw lane SRV is missing");
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = object ? object->dxgiFormat : DXGI_FORMAT_R32G32B32A32_FLOAT;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2DArray.MipLevels = 1;
            view.Texture2DArray.ArraySize = static_cast<UINT>(viewports.size());
            gCtx.device->CreateShaderResourceView(object ? object->resource.Get() : nullptr, &view, cpu);
            cpu.ptr += gCtx.srvInc;
        }
        list->SetGraphicsRootDescriptorTable(route.laneDescriptorParam, descriptors->gpu);
    }
    if (pipeline->pushConstantBytes)
        list->SetGraphicsRoot32BitConstants(route.pushConstantParam, pipeline->pushConstantBytes / 4u,
            packet->state.pushConstants.data(), 0);
    list->SetGraphicsRootConstantBufferView(route.batchParam, batchConstants);
    list->IASetPrimitiveTopology(toPrimitiveTopology(packet->topology));
    for (const auto& [slot, stride] : pipeline->vertexStrides)
        list->IASetVertexBuffers(static_cast<UINT>(slot), 1, &packet->vertices[slot]);
    const auto& draw = packet->opaqueSeedDraw;
    const bool indexed = draw.family == mcdr::graphics_recording::CommandFamily::DrawIndexed;
    if (indexed) list->IASetIndexBuffer(&packet->indices);
    const auto tileCount = static_cast<UINT>(viewports.size());
    list->RSSetViewports(tileCount, viewports.data());
    list->RSSetScissorRects(tileCount, scissors.data());
    if (route.instanceTileBatch && draw.instanceCount == 1 && draw.firstInstance == 0) {
        const UINT routeConstants[4]{0, 0, 0, 1};
        list->SetGraphicsRoot32BitConstants(route.drawParam, static_cast<UINT>(std::size(routeConstants)),
            routeConstants, 0);
        if (indexed) list->DrawIndexedInstanced(draw.count, tileCount,
            draw.firstVertexOrIndex, draw.baseVertex, 0);
        else list->DrawInstanced(draw.count, tileCount, draw.firstVertexOrIndex, 0);
        return {};
    }
    for (UINT tile = 0; tile < tileCount; ++tile) {
        if (scissors[tile].left >= scissors[tile].right || scissors[tile].top >= scissors[tile].bottom) continue;
        const UINT routeConstants[4]{tile, 0, 0, 0};
        list->SetGraphicsRoot32BitConstants(route.drawParam, static_cast<UINT>(std::size(routeConstants)),
            routeConstants, 0);
        if (indexed) list->DrawIndexedInstanced(draw.count, draw.instanceCount,
            draw.firstVertexOrIndex, draw.baseVertex, draw.firstInstance);
        else list->DrawInstanced(draw.count, draw.instanceCount, draw.firstVertexOrIndex, draw.firstInstance);
    }
    return {};
}

int completeGraphicsCommandRecording(void* opaqueContext, mcdr::graphics_recording::Completion completion) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context) return -1;
    const auto observer = context->graphicsCommandObserver;
    if (!observer.prepare) return 1;
    try {
        auto owner = observer.owner.lock();
        if (owner && observer.complete && observer.complete(owner.get(), context, completion) == 1) {
            if (completion == mcdr::graphics_recording::Completion::Failed)
                context->recordingFailed.store(true, std::memory_order_release);
            return 1;
        }
    } catch (...) {
        context->recordingFailed.store(true, std::memory_order_release);
        return -1;
    }
    context->recordingFailed.store(true, std::memory_order_release);
    return -1;
}

namespace {

void prepareMeshBuffer(CommandContext* ctx, Dx12Object* obj, D3D12_RESOURCE_STATES state) {
    if (obj->heapType != D3D12_HEAP_TYPE_UPLOAD) transitionBufferTo(ctx, obj, state);
}

void markMeshColorTargets(CommandContext* ctx) {
    if (!ctx->currentPipeline || ctx->currentPipeline->rasterMotionWritesWorldColor) {
        for (size_t index = 0; index < ctx->activeColorTargets.size(); ++index) {
            if (index < ctx->activeColorTargetsTouched.size()) ctx->activeColorTargetsTouched[index] = true;
        }
        if (!ctx->activeColorTargets.empty()) ctx->colorTargetsWritten = true;
    }
}

}

mcdr::Result<void> drawMeshlets(CommandContext* ctx, Dx12Object* vertexBuffer, long long vertexOffset,
                                Dx12Object* sections, long long sectionsOffset, long long sectionsLength,
                                Dx12Object* meshlets, long long meshletsOffset, long long meshletsLength,
                                Dx12Object* globals, long long globalsOffset, int meshletCount) {
    constexpr const char* kApi = "drawMeshlets";
    if (!ctx || !ctx->listOpen) {
        return mcdr::err(kApi, "meshletDispatch: no open command list");
    }
    if (!ctx->inRenderPass) {
        return mcdr::err(kApi, "meshletDispatch: no open render pass");
    }
    if (!ctx->currentPipeline || !ctx->currentPipeline->isMesh || ctx->currentPipeline->geometryParam == 0) {
        return mcdr::err(kApi, "meshletDispatch: current pipeline is not a mesh PSO with mesh extras");
    }
    constexpr UINT maxGroups = 1u << 22;
    constexpr UINT dispatchRowWidth = 256u;
    if (meshletCount <= 0 || static_cast<UINT>(meshletCount) > maxGroups) {
        return mcdr::err(kApi, "meshletDispatch: group count must be in [1, 4194304]");
    }
    for (const Dx12Object* buf : {vertexBuffer, sections, meshlets, globals}) {
        if (!buf || buf->kind != Dx12Object::Kind::Buffer || !buf->resource) {
            return mcdr::err(kApi, "meshletDispatch: invalid buffer handle");
        }
    }
    const long long vbBytes = remainingToEnd(vertexBuffer, vertexOffset);
    const long long secBytes = sectionsLength > 0 ? sectionsLength : remainingToEnd(sections, sectionsOffset);
    const long long meshletBytes = meshletsLength > 0 ? meshletsLength : remainingToEnd(meshlets, meshletsOffset);
    if (vbBytes < 4 || secBytes < 40 || secBytes % 8 != 0 || meshletBytes < 16 ||
        meshletBytes % 16 != 0 || meshletsOffset < 0 || meshletsOffset % 16 != 0) {
        return mcdr::err(kApi, "meshletDispatch: empty vertex/section/meshlet range");
    }
    if (sectionsLength > 0 && sectionsLength > remainingToEnd(sections, sectionsOffset)) {
        return mcdr::err(kApi, "meshletDispatch: sections range overflow");
    }
    if (meshletsLength > 0 && meshletsLength > remainingToEnd(meshlets, meshletsOffset)) {
        return mcdr::err(kApi, "meshletDispatch: meshlet range overflow");
    }

    const UINT slotCount = 3u;
    if (ctx->nextDrawSlot > drawHeapCapacityFor(ctx) ||
        slotCount > drawHeapCapacityFor(ctx) - ctx->nextDrawSlot) {
        return mcdr::err(kApi, "meshletDispatch: draw descriptor heap exhausted for this frame");
    }

    prepareMeshBuffer(ctx, vertexBuffer,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    prepareMeshBuffer(ctx, sections,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    prepareMeshBuffer(ctx, meshlets,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    prepareMeshBuffer(ctx, globals, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    const UINT slot = drawHeapSlotFor(ctx, ctx->nextDrawSlot);
    const SIZE_T base = static_cast<SIZE_T>(slot) * gCtx.drawInc;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuRoot = drawHeapFor(ctx)->GetCPUDescriptorHandleForHeapStart();
    cpuRoot.ptr += base;
    writeRawBufferSRV(cpuRoot, vertexBuffer, vertexOffset, vbBytes);
    cpuRoot.ptr += gCtx.drawInc;
    writeRawBufferSRV(cpuRoot, sections, sectionsOffset, secBytes);
    cpuRoot.ptr += gCtx.drawInc;
    writeRawBufferSRV(cpuRoot, meshlets, meshletsOffset, meshletBytes);
    ctx->nextDrawSlot += slotCount;

    D3D12_GPU_DESCRIPTOR_HANDLE gpuRoot = drawHeapFor(ctx)->GetGPUDescriptorHandleForHeapStart();
    gpuRoot.ptr += base;
    ctx->commandList->SetGraphicsRootDescriptorTable(ctx->currentPipeline->geometryParam, gpuRoot);

    const D3D12_GPU_VIRTUAL_ADDRESS globalsVA =
        globals->resource->GetGPUVirtualAddress() + static_cast<UINT64>(globalsOffset);
    if (globalsVA % 256ULL != 0) {
        return mcdr::err(kApi, std::string("meshletDispatch: globals root CBV is not 256-byte aligned (offset=") +
                                   std::to_string(globalsOffset) + ")");
    }
    ctx->commandList->SetGraphicsRootConstantBufferView(ctx->currentPipeline->segmentParam, globalsVA);

    ComPtr<ID3D12GraphicsCommandList6> list6;
    if (FAILED(ctx->commandList->QueryInterface(IID_PPV_ARGS(&list6))) || !list6) {
        ctx->recordingFailed.store(true, std::memory_order_release);
        return mcdr::err(kApi, "meshletDispatch: ID3D12GraphicsCommandList6 is not available on this command list");
    }
    const UINT groupCount = static_cast<UINT>(meshletCount);
    const UINT groupsX = groupCount <= 65535u ? groupCount : dispatchRowWidth;
    const UINT groupsY = groupCount <= 65535u ? 1u : (groupCount + dispatchRowWidth - 1u) / dispatchRowWidth;
    if (prepareSampledRenderPassWrites(ctx) != SampledTextureContentStatus::Ready)
        return mcdr::err(kApi, "sampled render attachment write ordering failed");
    std::shared_ptr<const void> graphicsPacket;
    if (ctx->graphicsCommandObserver.prepare) {
        mcdr::graphics_recording::Command command;
        command.family = mcdr::graphics_recording::CommandFamily::Mesh;
        command.count = groupCount;
        command.meshGroups[0] = groupsX;
        command.meshGroups[1] = groupsY;
        command.meshGroups[2] = 1;
        const DrawBinding meshBindings[] = {
            {2, vertexBuffer, vertexOffset, vbBytes}, {2, sections, sectionsOffset, secBytes},
            {2, meshlets, meshletsOffset, meshletBytes}, {0, globals, globalsOffset, remainingToEnd(globals, globalsOffset)}
        };
        try {
            auto prepared = prepareGraphicsCommand(ctx, command, meshBindings, 4, gpuRoot, globalsVA);
            if (!prepared) {
                ctx->recordingFailed.store(true, std::memory_order_release);
                return std::unexpected(prepared.error());
            }
            graphicsPacket = std::move(*prepared);
        } catch (...) {
            ctx->recordingFailed.store(true, std::memory_order_release);
            return mcdr::err(kApi, "graphics command capture preparation failed");
        }
    }
    list6->DispatchMesh(groupsX, groupsY, 1);
    if (graphicsPacket) {
        if (confirmGraphicsCommandCapture(ctx, graphicsPacket) != 1)
            return mcdr::err(kApi, "graphics command capture confirmation failed");
        if (completeGraphicsCommandRecording(ctx, mcdr::graphics_recording::Completion::Succeeded) != 1)
            return mcdr::err(kApi, "graphics command capture completion failed");
    }
    if (ctx->vanillaDepthSeedDraw && ctx->activeDepthTarget)
        ctx->vanillaDepthSeedDraw(ctx->vanillaDepthSeedState, ctx, ctx->activeDepthTarget,
            ctx->currentPipeline, static_cast<uint32_t>(mcdr::graphics_recording::CommandFamily::Mesh),
            ++ctx->vanillaDepthSeedOrdinal, graphicsPacket);
    markMeshColorTargets(ctx);
    return {};
}

mcdr::Result<void> drawIndexedInstanced(CommandContext* ctx, UINT indexCount, UINT instanceCount,
                                        INT startIndexLocation, INT baseVertexLocation,
                                        UINT startInstanceLocation) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("drawIndexedInstanced", "drawIndexed: no open command list");
    }
    if (!ctx->inRenderPass) {
        return mcdr::err("drawIndexedInstanced", "drawIndexed: no open render pass");
    }
    static UINT64 lastFence = 0;
    static int passCount = 0;
    if (ctx->fenceValue != lastFence) {
        lastFence = ctx->fenceValue;
        passCount = 0;
    }
    passCount++;
    if (indexCount == 0) {
        DBG_LOG_DEBUG("drawIndexed[%llu] PASS#%d: ZERO-COUNT (skip) topo=%d",
                      (unsigned long long)ctx->fenceValue, passCount,
                      (int)(ctx->currentPipeline ? ctx->currentPipeline->topology : -1));
    } else {
        DBG_LOG_DEBUG("drawIndexed[%llu] PASS#%d: count=%u inst=%u first=%d base=%d topo=%d",
                      (unsigned long long)ctx->fenceValue, passCount, indexCount, instanceCount,
                      startIndexLocation, baseVertexLocation,
                      (int)(ctx->currentPipeline ? ctx->currentPipeline->topology : -1));
    }
    if (!ctx->currentPipeline || ctx->currentPipeline->rasterMotionWritesWorldColor) {
        for (size_t index = 0; index < ctx->activeColorTargets.size(); ++index) {
            if (index < ctx->activeColorTargetsTouched.size()) ctx->activeColorTargetsTouched[index] = true;
        }
        if (!ctx->activeColorTargets.empty()) ctx->colorTargetsWritten = true;
    }
    DBG_LOG_DEBUG("drawIndexed[%llu] colorTargetsWritten -> %d", (unsigned long long)ctx->fenceValue,
                  (int)ctx->colorTargetsWritten);
    if (indexCount && instanceCount && prepareSampledRenderPassWrites(ctx) != SampledTextureContentStatus::Ready)
        return mcdr::err("drawIndexedInstanced", "sampled render attachment write ordering failed");
    std::shared_ptr<const void> graphicsPacket;
    if (ctx->graphicsCommandObserver.prepare) {
        mcdr::graphics_recording::Command command;
        command.family = mcdr::graphics_recording::CommandFamily::DrawIndexed;
        command.count = indexCount;
        command.instanceCount = instanceCount;
        command.firstVertexOrIndex = static_cast<UINT>(startIndexLocation);
        command.baseVertex = baseVertexLocation;
        command.firstInstance = startInstanceLocation;
        if (prepareGraphicsCommandCapture(ctx, command, &graphicsPacket) != 1) return mcdr::err("drawIndexedInstanced", "graphics command capture preparation failed");
    }
    ctx->commandList->DrawIndexedInstanced(indexCount, instanceCount, startIndexLocation, baseVertexLocation,
                                           startInstanceLocation);
    if (graphicsPacket && confirmGraphicsCommandCapture(ctx, graphicsPacket) != 1) return mcdr::err("drawIndexedInstanced", "graphics command capture confirmation failed");
    if (ctx->vanillaDepthSeedDraw && ctx->activeDepthTarget)
        ctx->vanillaDepthSeedDraw(ctx->vanillaDepthSeedState, ctx, ctx->activeDepthTarget,
            ctx->currentPipeline, static_cast<uint32_t>(mcdr::graphics_recording::CommandFamily::DrawIndexed),
            ++ctx->vanillaDepthSeedOrdinal, graphicsPacket);
    return {};
}

mcdr::Result<void> drawInstanced(CommandContext* ctx, UINT vertexCount, UINT instanceCount, UINT firstVertex,
                                 UINT startInstanceLocation) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("drawInstanced", "draw: no open command list");
    }
    if (!ctx->inRenderPass) {
        return mcdr::err("drawInstanced", "draw: no open render pass");
    }
    DBG_LOG_DEBUG("draw: vertexCount=%u instance=%u firstVertex=%u", vertexCount, instanceCount, firstVertex);
    if (vertexCount && instanceCount && prepareSampledRenderPassWrites(ctx) != SampledTextureContentStatus::Ready)
        return mcdr::err("drawInstanced", "sampled render attachment write ordering failed");
    std::shared_ptr<const void> graphicsPacket;
    if (ctx->graphicsCommandObserver.prepare) {
        mcdr::graphics_recording::Command command;
        command.family = mcdr::graphics_recording::CommandFamily::Draw;
        command.count = vertexCount;
        command.instanceCount = instanceCount;
        command.firstVertexOrIndex = firstVertex;
        command.firstInstance = startInstanceLocation;
        if (prepareGraphicsCommandCapture(ctx, command, &graphicsPacket) != 1) return mcdr::err("drawInstanced", "graphics command capture preparation failed");
    }
    ctx->commandList->DrawInstanced(vertexCount, instanceCount, firstVertex, startInstanceLocation);
    if (graphicsPacket && confirmGraphicsCommandCapture(ctx, graphicsPacket) != 1) return mcdr::err("drawInstanced", "graphics command capture confirmation failed");
    if (ctx->vanillaDepthSeedDraw && ctx->activeDepthTarget)
        ctx->vanillaDepthSeedDraw(ctx->vanillaDepthSeedState, ctx, ctx->activeDepthTarget,
            ctx->currentPipeline, static_cast<uint32_t>(mcdr::graphics_recording::CommandFamily::Draw),
            ++ctx->vanillaDepthSeedOrdinal, graphicsPacket);
    return {};
}

mcdr::Result<void> drawIndexedIndirect(CommandContext* ctx, Dx12Object* commands, long long offset,
                                       UINT drawCount) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("drawIndexedIndirect", "drawIndexedIndirect: no open command list");
    }
    if (!ctx->inRenderPass) {
        return mcdr::err("drawIndexedIndirect", "drawIndexedIndirect: no open render pass");
    }
    if (!commands || commands->kind != Dx12Object::Kind::Buffer) {
        return mcdr::err("drawIndexedIndirect", "drawIndexedIndirect: invalid buffer handle");
    }
    if (!gCtx.cmdSigIndexed) {
        return mcdr::err("drawIndexedIndirect", "drawIndexedIndirect: no indexed command signature");
    }
    if (drawCount && prepareSampledRenderPassWrites(ctx) != SampledTextureContentStatus::Ready)
        return mcdr::err("drawIndexedIndirect", "sampled render attachment write ordering failed");
    std::shared_ptr<const void> graphicsPacket;
    if (ctx->graphicsCommandObserver.prepare) {
        mcdr::graphics_recording::Command command;
        command.family = mcdr::graphics_recording::CommandFamily::DrawIndexedIndirect;
        command.count = drawCount;
        command.indirectBuffer = commands;
        command.indirectOffset = static_cast<uint64_t>(offset);
        if (prepareGraphicsCommandCapture(ctx, command, &graphicsPacket) != 1) return mcdr::err("drawIndexedIndirect", "graphics command capture preparation failed");
    }
    ctx->commandList->ExecuteIndirect(gCtx.cmdSigIndexed.Get(), drawCount, commands->resource.Get(),
                                      static_cast<UINT64>(offset), nullptr, 0);
    if (graphicsPacket && confirmGraphicsCommandCapture(ctx, graphicsPacket) != 1) return mcdr::err("drawIndexedIndirect", "graphics command capture confirmation failed");
    if (ctx->vanillaDepthSeedDraw && ctx->activeDepthTarget)
        ctx->vanillaDepthSeedDraw(ctx->vanillaDepthSeedState, ctx, ctx->activeDepthTarget,
            ctx->currentPipeline, static_cast<uint32_t>(mcdr::graphics_recording::CommandFamily::DrawIndexedIndirect),
            ++ctx->vanillaDepthSeedOrdinal, graphicsPacket);
    return {};
}

mcdr::Result<void> drawIndirect(CommandContext* ctx, Dx12Object* commands, long long offset, UINT drawCount) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("drawIndirect", "drawIndirect: no open command list");
    }
    if (!ctx->inRenderPass) {
        return mcdr::err("drawIndirect", "drawIndirect: no open render pass");
    }
    if (!commands || commands->kind != Dx12Object::Kind::Buffer) {
        return mcdr::err("drawIndirect", "drawIndirect: invalid buffer handle");
    }
    if (!gCtx.cmdSigNonIndexed) {
        return mcdr::err("drawIndirect", "drawIndirect: no non-indexed command signature");
    }
    if (drawCount && prepareSampledRenderPassWrites(ctx) != SampledTextureContentStatus::Ready)
        return mcdr::err("drawIndirect", "sampled render attachment write ordering failed");
    std::shared_ptr<const void> graphicsPacket;
    if (ctx->graphicsCommandObserver.prepare) {
        mcdr::graphics_recording::Command command;
        command.family = mcdr::graphics_recording::CommandFamily::DrawIndirect;
        command.count = drawCount;
        command.indirectBuffer = commands;
        command.indirectOffset = static_cast<uint64_t>(offset);
        if (prepareGraphicsCommandCapture(ctx, command, &graphicsPacket) != 1) return mcdr::err("drawIndirect", "graphics command capture preparation failed");
    }
    ctx->commandList->ExecuteIndirect(gCtx.cmdSigNonIndexed.Get(), drawCount, commands->resource.Get(),
                                      static_cast<UINT64>(offset), nullptr, 0);
    if (graphicsPacket && confirmGraphicsCommandCapture(ctx, graphicsPacket) != 1) return mcdr::err("drawIndirect", "graphics command capture confirmation failed");
    if (ctx->vanillaDepthSeedDraw && ctx->activeDepthTarget)
        ctx->vanillaDepthSeedDraw(ctx->vanillaDepthSeedState, ctx, ctx->activeDepthTarget,
            ctx->currentPipeline, static_cast<uint32_t>(mcdr::graphics_recording::CommandFamily::DrawIndirect),
            ++ctx->vanillaDepthSeedOrdinal, graphicsPacket);
    return {};
}

void dbgReadbackTexturePixels(Dx12Object* tex, const char* tag) {
    if (gAsyncRenderCtx != nullptr) return;
    if (!tex || tex->kind != Dx12Object::Kind::Texture || !tex->resource) return;
    if (auto idle = deviceWaitIdle(); !idle) return;
    ID3D12Resource* r = tex->resource.Get();
    D3D12_RESOURCE_DESC td = r->GetDesc();
    UINT w = static_cast<UINT>(td.Width), h = td.Height;
    if (w == 0 || h == 0 || w > 16384 || h > 16384) return;

    DXGI_FORMAT fmt = td.Format;
    UINT bpp = 4;
    DXGI_FORMAT fbFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
            bpp = 4;
            fbFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            bpp = 8;
            fbFmt = DXGI_FORMAT_R16G16B16A16_UNORM;
            break;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
            bpp = 16;
            fbFmt = DXGI_FORMAT_R32G32_FLOAT;
            break;
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT:
            bpp = 1;
            fbFmt = DXGI_FORMAT_R8_UNORM;
            break;
        default:
            bpp = 4;
            fbFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
    }
    fbFmt = fmt;
    UINT64 rowBytes = static_cast<UINT64>(w) * bpp;
    UINT64 pitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                   & ~static_cast<UINT64>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    UINT64 total = pitch * h;

    static ComPtr<ID3D12Resource> staging;
    if (!staging || staging->GetDesc().Width < total) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = total;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        hp.CreationNodeMask = 0;
        hp.VisibleNodeMask = 0;
        if (FAILED(gCtx.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&staging))))
            return;
    }
    static ComPtr<ID3D12CommandAllocator> alloc;
    static ComPtr<ID3D12GraphicsCommandList> cl;
    if (!alloc
        && FAILED(gCtx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))))
        return;
    if (!cl) {
        if (FAILED(gCtx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                  IID_PPV_ARGS(&cl))))
            return;
    } else {
        alloc->Reset();
        cl->Reset(alloc.Get(), nullptr);
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = r;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = staging.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = fbFmt;
    dst.PlacedFootprint.Footprint.Width = w;
    dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(pitch);
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    cl->ResourceBarrier(1, &b);
    cl->Close();

    ID3D12CommandList* lists[] = {cl.Get()};
    gCtx.queue->ExecuteCommandLists(1, lists);
    UINT64 fv = ++gCtx.queueFenceValue;
    if (FAILED(gCtx.queue->Signal(gCtx.queueFence.Get(), fv))) return;
    if (auto waited = waitForQueueFenceValue(fv, 5'000'000'000ULL); !waited) return;

    void* ptr = nullptr;
    if (FAILED(staging->Map(0, nullptr, &ptr))) {
        const HRESULT rr = gCtx.device ? gCtx.device->GetDeviceRemovedReason() : E_FAIL;
        dbgLogError("readback[%s]: staging Map failed (fmt=%d %ux%u pitch=%llu total=%llu removedReason=0x%08X)",
                    tag, static_cast<int>(fmt), w, h, static_cast<unsigned long long>(pitch),
                    static_cast<unsigned long long>(total), static_cast<unsigned>(rr));
        return;
    }
    const uint8_t* base = static_cast<const uint8_t*>(ptr);
    int xs[3] = {0, static_cast<int>(w) / 2, static_cast<int>(w) - 1};
    int ys[3] = {0, static_cast<int>(h) / 2, static_cast<int>(h) - 1};
    for (int yi = 0; yi < 3; ++yi) {
        for (int xi = 0; xi < 3; ++xi) {
            const uint8_t* p = base + static_cast<UINT64>(ys[yi]) * pitch + static_cast<UINT64>(xs[xi]) * bpp;
            dbgLogDebug("rbTex[%s][%ux%u] (%d,%d) = RGBA(%3d,%3d,%3d,%3d)", tag, w, h, xs[xi], ys[yi], p[0], p[1],
                   p[2], p[3]);
        }
    }
    if (nativeLogLevel() >= 3) dbgDumpPixelsToFile(base, w, h, pitch, tag);
    staging->Unmap(0, nullptr);
}

void dbgDumpPixelsToFile(const uint8_t* rgba, UINT w, UINT h, UINT64 pitch, const char* tag) {
    if (!rgba || w == 0 || h == 0 || w > 16384 || h > 16384) return;

    {
        char path[256];
        snprintf(path, sizeof(path), "dx12_dump_%s.bmp", tag);
        FILE* f = fopen(path, "wb");
        if (f) {
            UINT64 rowBytes = static_cast<UINT64>(w) * 4;
            UINT64 padded = (rowBytes + 3) & ~3ULL;
            UINT64 imgSize = padded * h;
            uint32_t bfSize = static_cast<uint32_t>(54 + imgSize);
            uint8_t hdr[54] = {0};
            hdr[0] = 'B';
            hdr[1] = 'M';
            hdr[2] = static_cast<uint8_t>(bfSize & 0xFF);
            hdr[3] = static_cast<uint8_t>((bfSize >> 8) & 0xFF);
            hdr[4] = static_cast<uint8_t>((bfSize >> 16) & 0xFF);
            hdr[5] = static_cast<uint8_t>((bfSize >> 24) & 0xFF);
            hdr[10] = 54;
            hdr[14] = 40;
            hdr[18] = static_cast<uint8_t>(w & 0xFF);
            hdr[19] = static_cast<uint8_t>((w >> 8) & 0xFF);
            hdr[20] = static_cast<uint8_t>((w >> 16) & 0xFF);
            hdr[21] = static_cast<uint8_t>((w >> 24) & 0xFF);
            uint32_t hh = h;
            hdr[22] = static_cast<uint8_t>(hh & 0xFF);
            hdr[23] = static_cast<uint8_t>((hh >> 8) & 0xFF);
            hdr[24] = static_cast<uint8_t>((hh >> 16) & 0xFF);
            hdr[25] = static_cast<uint8_t>((hh >> 24) & 0xFF);
            hdr[26] = 1;
            hdr[28] = 32;
            hdr[34] = static_cast<uint8_t>(imgSize & 0xFF);
            hdr[35] = static_cast<uint8_t>((imgSize >> 8) & 0xFF);
            hdr[36] = static_cast<uint8_t>((imgSize >> 16) & 0xFF);
            hdr[37] = static_cast<uint8_t>((imgSize >> 24) & 0xFF);
            fwrite(hdr, 1, 54, f);
            for (UINT y = h; y > 0; --y) {
                const uint8_t* row = rgba + static_cast<UINT64>(y - 1) * pitch;
                for (UINT x = 0; x < w; ++x) {
                    uint8_t bgr[4];
                    bgr[0] = row[x * 4 + 2];
                    bgr[1] = row[x * 4 + 1];
                    bgr[2] = row[x * 4 + 0];
                    bgr[3] = row[x * 4 + 3];
                    fwrite(bgr, 1, 4, f);
                }
            }
            fclose(f);
            dbgLogDebug("dump[%s] BMP saved %ux%u -> %s (rgba=%p)", tag, w, h, path,
                   reinterpret_cast<const void*>(rgba));
        } else {
            dbgLogDebug("dump[%s] BMP open failed (%s)", tag, path);
        }
    }

    const int cols = 96;
    const char* lumaChars = " .:-=+*#%@";
    int rows = static_cast<int>(static_cast<UINT64>(h) * cols / w);
    if (rows < 1) rows = 1;
    if (rows > 64) rows = 64;
    std::string art;
    art.reserve(static_cast<size_t>(cols) * rows + rows);
    for (int ry = 0; ry < rows; ++ry) {
        UINT y0 = static_cast<UINT>(static_cast<UINT64>(ry) * h / rows);
        UINT y1 = static_cast<UINT>(static_cast<UINT64>(ry + 1) * h / rows);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > h) y1 = h;
        for (int rx = 0; rx < cols; ++rx) {
            UINT x0 = static_cast<UINT>(static_cast<UINT64>(rx) * w / cols);
            UINT x1 = static_cast<UINT>(static_cast<UINT64>(rx + 1) * w / cols);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > w) x1 = w;
            unsigned r = 0, g = 0, b = 0, a = 0;
            UINT cnt = 0;
            for (UINT y = y0; y < y1; ++y) {
                const uint8_t* row = rgba + static_cast<UINT64>(y) * pitch;
                for (UINT x = x0; x < x1; ++x) {
                    r += row[x * 4 + 0];
                    g += row[x * 4 + 1];
                    b += row[x * 4 + 2];
                    a += row[x * 4 + 3];
                    ++cnt;
                }
            }
            if (cnt == 0) {
                art += ' ';
                continue;
            }
            r /= cnt;
            g /= cnt;
            b /= cnt;
            a /= cnt;
            int mx = static_cast<int>(r), mn = static_cast<int>(r);
            if (static_cast<int>(g) > mx) mx = g;
            if (static_cast<int>(b) > mx) mx = b;
            if (static_cast<int>(g) < mn) mn = g;
            if (static_cast<int>(b) < mn) mn = b;
            int luma = (r * 299 + g * 587 + b * 114) / 1000;
            int sat = (mx == 0) ? 0 : (mx - mn) * 255 / mx;
            if (sat > 90 && luma > 40) {
                char hue = '?';
                if (r > g && r > b)
                    hue = 'R';
                else if (g > r && g > b)
                    hue = 'G';
                else if (b > r && b > g)
                    hue = 'B';
                else if (r > 120 && g > 120 && b < 90)
                    hue = 'Y';
                else if (r > 120 && b > 120 && g < 90)
                    hue = 'M';
                else if (g > 120 && b > 120 && r < 90)
                    hue = 'C';
                else if (r > 180 && g > 180 && b > 180)
                    hue = 'W';
                art += hue;
            } else {
                int idx = luma * 9 / 255;
                if (idx > 9) idx = 9;
                art += lumaChars[idx];
            }
        }
        art += '\n';
    }
    dbgLogDebug("dump[%s] ASCII %dx%d (grid %dx%d):\n%s", tag, w, h, cols, rows, art.c_str());
}

void dbgReadbackBufferBytes(Dx12Object* buf, long long offset, int len, const char* tag) {
    if (gAsyncRenderCtx != nullptr) return;
    if (!buf || buf->kind != Dx12Object::Kind::Buffer || !buf->resource) return;
    if (offset < 0 || offset >= buf->size) return;
    long long avail = buf->size - offset;
    if (len <= 0) len = static_cast<int>(std::min(avail, 128LL));
    len = static_cast<int>(std::min(static_cast<long long>(len), avail));
    if (len <= 0) return;

    void* ptr = nullptr;
    ID3D12Resource* unmapRes = nullptr;
    if (buf->heapType == D3D12_HEAP_TYPE_UPLOAD) {
        D3D12_RANGE range{static_cast<SIZE_T>(offset), static_cast<SIZE_T>(offset + len)};
        if (FAILED(buf->resource->Map(0, &range, &ptr))) return;
        unmapRes = buf->resource.Get();
    } else {
        if (auto idle = deviceWaitIdle(); !idle) return;
        UINT64 total = (static_cast<UINT64>(len) + 255) & ~255ULL;
        static ComPtr<ID3D12Resource> staging;
        if (!staging || staging->GetDesc().Width < total) {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = total;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            hp.CreationNodeMask = 0;
            hp.VisibleNodeMask = 0;
            if (FAILED(gCtx.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(&staging))))
                return;
        }
        static ComPtr<ID3D12CommandAllocator> alloc;
        static ComPtr<ID3D12GraphicsCommandList> cl;
        if (!alloc
            && FAILED(
                gCtx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))))
            return;
        if (!cl) {
            if (FAILED(gCtx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                      IID_PPV_ARGS(&cl))))
                return;
        } else {
            alloc->Reset();
            cl->Reset(alloc.Get(), nullptr);
        }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = buf->resource.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
        cl->CopyBufferRegion(staging.Get(), 0, buf->resource.Get(), static_cast<UINT64>(offset),
                             static_cast<UINT64>(len));
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        cl->ResourceBarrier(1, &b);
        cl->Close();

        ID3D12CommandList* lists[] = {cl.Get()};
        gCtx.queue->ExecuteCommandLists(1, lists);
        UINT64 fv = ++gCtx.queueFenceValue;
        if (FAILED(gCtx.queue->Signal(gCtx.queueFence.Get(), fv))) return;
        if (auto waited = waitForQueueFenceValue(fv, 5'000'000'000ULL); !waited) return;
        if (FAILED(staging->Map(0, nullptr, &ptr))) return;
        unmapRes = staging.Get();
    }

    const float* f = static_cast<const float*>(ptr);
    const uint8_t* b8 = static_cast<const uint8_t*>(ptr);
    std::string fs;
    int nf = std::min(len / 4, 12);
    for (int i = 0; i < nf; ++i) {
        char t[32];
        snprintf(t, sizeof(t), "%g ", static_cast<double>(f[i]));
        fs += t;
    }
    std::string hs;
    int nh = std::min(len, 16);
    for (int i = 0; i < nh; ++i) {
        char t[8];
        snprintf(t, sizeof(t), "%02X ", b8[i]);
        hs += t;
    }
    dbgLogDebug("rbBuf[%s] off=%lld len=%d heap=%d floats=[%s] hex=[%s]", tag, offset, len,
           static_cast<int>(buf->heapType), fs.c_str(), hs.c_str());
    unmapRes->Unmap(0, nullptr);
}

static std::string wideToUtf8(const wchar_t* w) {
    if (w == nullptr || w[0] == L'\0') {
        return std::string();
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) {
        return std::string();
    }
    std::string s(static_cast<size_t>(n), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    if (written <= 1) {
        return std::string();
    }
    s.resize(static_cast<size_t>(written - 1));
    return s;
}

std::string enumerateAdaptersJson() {
    loadPixCapturer();
    nlohmann::json list = nlohmann::json::array();
    {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            return "[{\"error\":\"CreateDXGIFactory1 failed\"}]";
        }
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter> adapter;
            if (FAILED(factory->EnumAdapters(i, &adapter))) break;
            DXGI_ADAPTER_DESC desc{};
            if (FAILED(adapter->GetDesc(&desc))) continue;
            const D3D_FEATURE_LEVEL highest = probeHighestFeatureLevel(adapter.Get());
            if (highest < D3D_FEATURE_LEVEL_12_0) continue;
            char luidBuf[32] = {};
            snprintf(luidBuf, sizeof(luidBuf), "%08X%08X", desc.AdapterLuid.LowPart,
                     desc.AdapterLuid.HighPart);
            char vidBuf[16] = {};
            snprintf(vidBuf, sizeof(vidBuf), "0x%08X", desc.VendorId);
            char didBuf[16] = {};
            snprintf(didBuf, sizeof(didBuf), "0x%08X", desc.DeviceId);
            list.push_back(nlohmann::json{
                {"name", wideToUtf8(desc.Description)},
                {"luid", luidBuf},
                {"vid", vidBuf},
                {"did", didBuf},
                {"feature_level", featureLevelName(highest)},
                {"feature_level_value", static_cast<int>(highest)},
                {"vram_gb", static_cast<long long>(desc.DedicatedVideoMemory / (1024LL * 1024 * 1024))},
            });
        }
    }
    return list.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

namespace {

struct DxCoreInfo {
    bool ok = false;
    bool isHardware = false;
    bool isIntegrated = false;
    bool isDetachable = false;
    std::string driverVersion;
    std::string driverDescription;
};

}

static void readDxCoreBool(IDXCoreAdapter* adapter, DXCoreAdapterProperty prop, bool& out) {
    if (!adapter->IsPropertySupported(prop)) {
        return;
    }
    bool value = false;
    if (SUCCEEDED(adapter->GetProperty(prop, sizeof(value), &value))) {
        out = value;
    }
}

static void queryDxCoreInfo(const LUID& luid, DxCoreInfo& out) {
    static HMODULE sDxCore = LoadLibraryW(L"dxcore.dll");
    if (sDxCore == nullptr) {
        return;
    }

    using PfnCreateAdapterFactory = HRESULT(WINAPI*)(REFIID, void**);
    static PfnCreateAdapterFactory sCreate =
        reinterpret_cast<PfnCreateAdapterFactory>(GetProcAddress(sDxCore, "DXCoreCreateAdapterFactory"));
    if (sCreate == nullptr) {
        return;
    }

    ComPtr<IDXCoreAdapterFactory> factory;
    if (FAILED(sCreate(__uuidof(IDXCoreAdapterFactory), reinterpret_cast<void**>(factory.GetAddressOf())))) {
        return;
    }

    ComPtr<IDXCoreAdapter> adapter;
    if (FAILED(factory->GetAdapterByLuid(luid, __uuidof(IDXCoreAdapter),
                                         reinterpret_cast<void**>(adapter.GetAddressOf())))) {
        return;
    }

    out.ok = true;

    if (adapter->IsPropertySupported(DXCoreAdapterProperty::DriverVersion)) {
        uint64_t raw = 0;
        if (SUCCEEDED(adapter->GetProperty(DXCoreAdapterProperty::DriverVersion, sizeof(raw), &raw))) {
            const unsigned vA = static_cast<unsigned>((raw >> 48) & 0xFFFFu);
            const unsigned vB = static_cast<unsigned>((raw >> 32) & 0xFFFFu);
            const unsigned vC = static_cast<unsigned>((raw >> 16) & 0xFFFFu);
            const unsigned vD = static_cast<unsigned>(raw & 0xFFFFu);
            char buf[64] = {};
            snprintf(buf, sizeof(buf), "%u.%u.%u.%u", vA, vB, vC, vD);
            out.driverVersion = buf;
        }
    }

    if (adapter->IsPropertySupported(DXCoreAdapterProperty::DriverDescription)) {
        size_t need = 0;
        if (SUCCEEDED(adapter->GetPropertySize(DXCoreAdapterProperty::DriverDescription, &need))
            && need > 0) {
            std::vector<char> text(need + 1, '\0');
            if (SUCCEEDED(
                    adapter->GetProperty(DXCoreAdapterProperty::DriverDescription, need, text.data()))) {
                text.back() = '\0';
                out.driverDescription = text.data();
            }
        }
    }

    readDxCoreBool(adapter.Get(), DXCoreAdapterProperty::IsHardware, out.isHardware);
    readDxCoreBool(adapter.Get(), DXCoreAdapterProperty::IsIntegrated, out.isIntegrated);
    readDxCoreBool(adapter.Get(), DXCoreAdapterProperty::IsDetachable, out.isDetachable);
}

std::string adapterInfoJson() {
    if (!gCtx.device) {
        return std::string();
    }

    const LUID luid = gCtx.device->GetAdapterLuid();

    DXGI_ADAPTER_DESC desc{};
    UINT descFlags = 0;
    bool haveDesc = false;
    bool haveFlags = false;
    {
        ComPtr<IDXGIFactory4> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) {
                ComPtr<IDXGIAdapter1> adapter1;
                DXGI_ADAPTER_DESC1 desc1{};
                if (SUCCEEDED(adapter.As(&adapter1)) && SUCCEEDED(adapter1->GetDesc1(&desc1))) {
                    std::memcpy(&desc, &desc1, sizeof(desc));
                    descFlags = desc1.Flags;
                    haveFlags = true;
                    haveDesc = true;
                } else if (SUCCEEDED(adapter->GetDesc(&desc))) {
                    haveDesc = true;
                }
            }
        }
    }

    DxCoreInfo core;
    queryDxCoreInfo(luid, core);

    std::string name;
    if (haveDesc) {
        name = wideToUtf8(desc.Description);
    }
    if (name.empty()) {
        name = gCtx.adapterName;
    }

    const UINT vendorId = haveDesc ? desc.VendorId : 0u;
    const UINT deviceId = haveDesc ? desc.DeviceId : 0u;
    const UINT subsysId = haveDesc ? desc.SubSysId : 0u;
    const UINT revision = haveDesc ? desc.Revision : 0u;

    std::string vendor;
    char vendorFallback[32] = {};
    snprintf(vendorFallback, sizeof(vendorFallback), "0x%x", vendorId);
    switch (vendorId) {
        case 4098:
        case 4130: vendor = "AMD"; break;
        case 4318:
        case 4818: vendor = "NVIDIA"; break;
        case 32902: vendor = "INTEL"; break;
        case 5772:
        case 6091:
        case 6505:
        case 20803: vendor = "QUALCOMM"; break;
        case 4112: vendor = "IMGTEC"; break;
        case 5348: vendor = "BROADCOM"; break;
        case 5045: vendor = "ARM"; break;
        case 5140: vendor = "MICROSOFT"; break;
        case 4203: vendor = "APPLE"; break;
        default: vendor = vendorFallback; break;
    }

    const char* featureLevel = featureLevelName(gCtx.featureLevel);

    std::string deviceType;
    if (core.ok) {
        if (!core.isHardware) {
            deviceType = "cpu";
        } else if (core.isIntegrated) {
            deviceType = "integrated";
        } else if (core.isDetachable) {
            deviceType = "virtual";
        } else {
            deviceType = "discrete";
        }
    } else if (haveFlags && (descFlags & static_cast<UINT>(DXGI_ADAPTER_FLAG_SOFTWARE)) != 0u) {
        deviceType = "cpu";
    } else {
        deviceType = "discrete";
    }

    char luidBuf[32] = {};
    snprintf(luidBuf, sizeof(luidBuf), "%08X%08X", luid.LowPart, luid.HighPart);

    nlohmann::json j;
    j["name"] = name;
    j["vendor"] = vendor;
    j["vendor_id"] = vendorId;
    j["device_id"] = deviceId;
    j["subsys_id"] = subsysId;
    j["revision"] = revision;
    j["feature_level"] = featureLevel;
    j["feature_level_value"] = static_cast<int>(gCtx.featureLevel);
    j["driver_version"] = core.driverVersion;
    j["driver_description"] = core.driverDescription;
    j["device_type"] = deviceType;
    j["vram_bytes"] = static_cast<unsigned long long>(haveDesc ? desc.DedicatedVideoMemory : 0);
    j["shared_bytes"] = static_cast<unsigned long long>(haveDesc ? desc.SharedSystemMemory : 0);
    j["dedicated_system_bytes"] =
        static_cast<unsigned long long>(haveDesc ? desc.DedicatedSystemMemory : 0);
    j["luid"] = luidBuf;
    j["is_hardware"] = core.isHardware;
    j["is_detachable"] = core.isDetachable;
    j["dxcore"] = core.ok;
    std::string body = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    appendJsonFields(body, capabilitiesJson(gCtx.caps));
    return body;
}

int setDlssEnabled(int enabled) {
    const bool want = enabled != 0;
    if (want == gCtx.dlssEnabled) {
        return want ? 1 : 0;
    }
    gCtx.dlssEnabled = want;
    dbgLogInfo("DLSS %s (independent of anti-aliasing; aaMode=%d)",
               want ? "enabled" : "disabled", gCtx.aaMode);
    if (want) {
        dbgLogInfo("DLSS gate is now open; the next world frame will try to create/evaluate");
    }
    return want ? 1 : 0;
}

thread_local std::string gDlssLastFailure;

bool dlssIsEnabled() {
    return gCtx.dlssEnabled;
}

std::string dlssLastFailureText() {
    return gDlssLastFailure;
}

void setDlssFailure(const char* stage, int32_t result) {
    char note[512] = {};
    mcdr_dlss_query(note, static_cast<int32_t>(sizeof(note)), nullptr, 0, nullptr, 0, nullptr);
    gDlssLastFailure = std::string("DLSS ") + stage;
    if (result != 0) {
        gDlssLastFailure += " rc=" + std::to_string(result);
    }
    if (note[0] != '\0') {
        gDlssLastFailure += ": ";
        gDlssLastFailure += note;
    }
}

namespace {
int32_t createDlssTexture(CommandContext* ctx, const DlssTextureDesc* desc, void** outObject,
                          void** outResource) {
    if (outObject != nullptr) *outObject = nullptr;
    if (outResource != nullptr) *outResource = nullptr;
    if (desc == nullptr || outObject == nullptr || outResource == nullptr) {
        return 0;
    }
    if (gCtx.device == nullptr || desc->dxgiFormat == 0 || desc->width == 0 ||
        desc->height == 0) {
        return 0;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rdesc{};
    rdesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rdesc.Width = desc->width;
    rdesc.Height = desc->height;
    rdesc.DepthOrArraySize = static_cast<UINT16>((desc->arraySize > 1) ? desc->arraySize : 1);
    rdesc.MipLevels = static_cast<UINT16>((desc->mipLevels > 1) ? desc->mipLevels : 1);
    rdesc.Format = static_cast<DXGI_FORMAT>(desc->dxgiFormat);
    rdesc.SampleDesc.Count = (desc->sampleCount > 0) ? desc->sampleCount : 1;
    rdesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rdesc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(desc->flags);
    auto obj = std::make_unique<Dx12Object>();
    obj->kind = Dx12Object::Kind::Texture;
    obj->heapType = D3D12_HEAP_TYPE_DEFAULT;
    obj->dxgiFormat = rdesc.Format;
    obj->size = static_cast<long long>(desc->width) * desc->height;
    const HRESULT hr = gCtx.device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &rdesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&obj->resource));
    if (FAILED(hr)) {
        dbgLogWarn("B4: DLSS private texture creation failed (%ux%u fmt=%d flags=0x%X): %s",
                   desc->width, desc->height, static_cast<int>(rdesc.Format),
                   static_cast<unsigned>(desc->flags), formatBackendResult(hr).c_str());
        return -1;
    }
    Dx12Object* raw = obj.release();
    if (ctx != nullptr) {
        ctx->resourceState[raw->resource.Get()] = D3D12_RESOURCE_STATE_COMMON;
    }
    *outObject = static_cast<void*>(raw);
    *outResource = static_cast<void*>(raw->resource.Get());
    dbgLogInfo("B4: module created a private DLSS texture: %ux%u fmt=%d mips=%u array=%u samples=%u "
               "flags=0x%X handle=%p resource=%p; registered in the tracked state map (COMMON, ctx=%p)",
               desc->width, desc->height, static_cast<int>(rdesc.Format),
               static_cast<unsigned>(rdesc.MipLevels), static_cast<unsigned>(rdesc.DepthOrArraySize),
               static_cast<unsigned>(rdesc.SampleDesc.Count), static_cast<unsigned>(desc->flags),
               static_cast<void*>(raw), static_cast<void*>(raw->resource.Get()),
               static_cast<void*>(ctx));
    return 1;
}

void destroyDlssTexture(CommandContext* ctx, void* object) {
    auto* obj = static_cast<Dx12Object*>(object);
    if (obj == nullptr) {
        return;
    }
    if (ctx != nullptr && obj->resource) {
        ctx->resourceState.erase(obj->resource.Get());
        ctx->subresourceState.erase(obj->resource.Get());
    }
    dbgLogInfo("B4: module destroys a private DLSS texture: handle=%p resource=%p", object,
               obj->resource ? static_cast<void*>(obj->resource.Get()) : nullptr);
    destroyObject(obj);
}
}

int applyDlssToTexture(CommandContext* ctx, const mcdr_dlss_input& in) {
    gDlssLastFailure.clear();
    auto reject = [](const char* reason, int32_t result = 0) {
        setDlssFailure(reason, result);
        return -1;
    };
    if (!ctx || !ctx->listOpen || ctx->commandList == nullptr) {
        return reject("rejected: command list is not open");
    }
    if (in.pInColor == nullptr || in.pInDepth == nullptr || in.pInMotionVectors == nullptr ||
        in.pInOutput == nullptr) {
        return reject("rejected: missing input texture");
    }
    if (in.renderWidth <= 0 || in.renderHeight <= 0 || in.targetWidth <= 0 || in.targetHeight <= 0) {
        return reject("rejected: non-positive render or target dimensions");
    }
    wireDlssImpl();
    retainObjectForRecording(ctx, in.pInColor);
    retainObjectForRecording(ctx, in.pInDepth);
    retainObjectForRecording(ctx, in.pInMotionVectors);
    retainObjectForRecording(ctx, in.pInOutput);
    if (in.pInExposure) {
        auto* exposure = reinterpret_cast<Dx12Object*>(in.pInExposure);
        if (exposure->kind != Dx12Object::Kind::Texture || !exposure->resource ||
            exposure->dxgiFormat != DXGI_FORMAT_R32_FLOAT || exposure->resource->GetDesc().Width != 1 ||
            exposure->resource->GetDesc().Height != 1) return reject("rejected: incompatible exposure texture");
        retainObjectForRecording(ctx, in.pInExposure);
    }
    auto unwrapResource = [](mcdr_object* handle) -> void* {
        auto* obj = reinterpret_cast<Dx12Object*>(handle);
        return obj != nullptr ? reinterpret_cast<void*>(obj->resource.Get()) : nullptr;
    };
    mcdr_dlss_input resolved = in;
    resolved.pInColor = reinterpret_cast<mcdr_object*>(unwrapResource(in.pInColor));
    resolved.pInDepth = reinterpret_cast<mcdr_object*>(unwrapResource(in.pInDepth));
    resolved.pInMotionVectors = reinterpret_cast<mcdr_object*>(unwrapResource(in.pInMotionVectors));
    resolved.pInOutput = reinterpret_cast<mcdr_object*>(unwrapResource(in.pInOutput));
    resolved.pInExposure = reinterpret_cast<mcdr_object*>(unwrapResource(in.pInExposure));
    if (resolved.pInColor == nullptr || resolved.pInDepth == nullptr || resolved.pInOutput == nullptr) {
        static bool s_loggedMissing = false;
        if (!s_loggedMissing) {
            s_loggedMissing = true;
            dbgLogWarn("DLSS skipped — color/depth/output did not unwrap to a D3D12 resource "
                       "(color=%p depth=%p out=%p); the data plane handed over a handle that is not "
                       "a live texture",
                       resolved.pInColor, resolved.pInDepth, resolved.pInOutput);
        }
        return reject("rejected: input texture did not unwrap to a resource");
    }
    if (resolved.pInMotionVectors == nullptr) {
        static bool s_loggedNoMv = false;
        if (!s_loggedNoMv) {
            s_loggedNoMv = true;
            dbgLogWarn("DLSS skipped — no motion-vector texture this frame (the velocity pass "
                       "is off or did not record); DLSS needs per-pixel motion vectors, so it stays "
                       "off until one exists. Enable it with -Pdxrmc.temporal.velocity=true.");
        }
        return reject("rejected: motion-vector texture did not unwrap to a resource");
    }
    if (mcdr_dlss_can_run(reinterpret_cast<void*>(gCtx.device.Get())) != 1) {
        return reject("initialization unavailable");
    }
    DlssTransitionFn tl = [](void* opaqueCtx, void* resource, int32_t toState) {
        auto* c = static_cast<CommandContext*>(opaqueCtx);
        if (c == nullptr || resource == nullptr) {
            return;
        }
        transitionTo(c, static_cast<ID3D12Resource*>(resource), D3D12_RESOURCE_STATE_COMMON,
                     static_cast<D3D12_RESOURCE_STATES>(toState));
    };
    const auto outputFlags = static_cast<ID3D12Resource*>(
        reinterpret_cast<void*>(resolved.pInOutput))->GetDesc().Flags;
    if ((outputFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        void* overrideRaw = nullptr;
        if (gCtx.dlssOutputOverride != nullptr) {
            auto* oObj = reinterpret_cast<Dx12Object*>(gCtx.dlssOutputOverride);
            if (oObj->kind == Dx12Object::Kind::Texture && oObj->resource) {
                retainObjectForRecording(ctx, oObj);
                overrideRaw = reinterpret_cast<void*>(oObj->resource.Get());
            }
        }
        const int32_t outputResult = mcdr_dlss_set_output_texture(overrideRaw);
        if (outputResult < 0) return reject("output texture registration failed");
    }
    const int32_t created = mcdr_dlss_create(reinterpret_cast<void*>(ctx->commandList), &resolved,
                                             tl, ctx);
    if (created != 1) {
        return reject("feature creation failed", created);
    }
    if (ctx->inRenderPass) {
        dbgLogWarn("a render pass was still open at the DLSS evaluate (inRenderPass=1); ending it "
                "so NGX's compute dispatch is legal. The caller is expected to begin its own pass "
                "afterwards — nothing is restarted here.");
        if (auto ended = endRenderPass(ctx); !ended) {
            gDlssLastFailure = std::string("DLSS endRenderPass failed: ") + ended.error().message;
            return -1;
        }
    }
    const int32_t evaluated = mcdr_dlss_evaluate(reinterpret_cast<void*>(ctx->commandList), &resolved, tl, ctx);
    restoreDrawHeap(ctx);
    if (evaluated <= 0) {
        return reject("evaluate failed", evaluated);
    }
    gDlssLastFailure.clear();
    return evaluated;
}

int32_t invalidateReconstructionInputs(void* rawContext) {
    auto* context = static_cast<CommandContext*>(rawContext);
    if (!context) return 0;
    if (!waitForRenderThreadSubmit()) {
        invalidatePreparedReconstructionHistory();
        return 0;
    }
    invalidatePreparedReconstructionHistory();
    if (context->submissionFailed.load()) return 0;
    return mcdr_dlss_invalidate_inputs();
}

int setDlssOutput(mcdr_object* tex) {
    if (tex == nullptr) {
        if (mcdr_dlss_set_output_texture(nullptr) < 0) return -1;
        gCtx.dlssOutputOverride = nullptr;
        gCtx.dlssOutputOverrideLease.reset();
        dbgLogInfo("DLSS output override cleared (back to the private fallback)");
        return 0;
    }
    auto* obj = reinterpret_cast<Dx12Object*>(tex);
    if (obj->kind != Dx12Object::Kind::Texture || !obj->resource) {
        dbgLogError("mcdr_dx12SetDlssOutput got a handle that is not a live texture "
                    "(kind=%d); ignored",
                    static_cast<int>(obj->kind));
        return -1;
    }
    auto lease = retainObjectLease(obj);
    const int32_t r = mcdr_dlss_set_output_texture(reinterpret_cast<void*>(obj->resource.Get()));
    if (r < 0) return -1;
    gCtx.dlssOutputOverride = tex;
    gCtx.dlssOutputOverrideLease = std::move(lease);
    dbgLogInfo("DLSS output override accepted (raw resource %p); the evaluate will write "
               "there instead of the private UAV texture",
               static_cast<void*>(obj->resource.Get()));
    return r == 1 ? 1 : 0;
}

int setDlssQuality(int tier) {
    if (tier < 0 || tier > 4) {
        dbgLogError("invalid DLSS quality tier %d (valid: 0=Performance 1=Balanced "
                    "2=Quality 3=DLAA)",
                    tier);
        return -1;
    }
    const int prev = gCtx.dlssQualityTier;
    gCtx.dlssQualityTier = tier;
    if (tier != prev) {
        mcdr_dlss_request_quality(tier);
        dbgLogInfo("DLSS quality tier = %d (%s) — the feature will be recreated on the next "
                   "frame",
                   tier, tier == 0 ? "Performance"
                                   : (tier == 1 ? "Balanced" : (tier == 2 ? "Quality" : "DLAA")));
    }
    return tier;
}

int setDlssRenderPreset(int preset) {
    if (preset < 0 || preset > 15) {
        dbgLogError("invalid DLSS render preset %d (valid: 0=Default 5=E 6=F 10=J 11=K "
                    "12=L 13=M)",
                    preset);
        return -1;
    }
    const int prev = gCtx.dlssPreset;
    gCtx.dlssPreset = preset;
    if (preset != prev) {
        mcdr_dlss_request_preset(preset);
        dbgLogInfo("DLSS render preset = %d — the feature will be recreated on the next "
                   "frame",
                   preset);
    }
    return preset;
}

int snapshotDlssDepth(CommandContext* ctx, mcdr_object* depthHandle) {
    if (ctx == nullptr || ctx->commandList == nullptr || depthHandle == nullptr) {
        return 0;
    }
    auto* obj = reinterpret_cast<Dx12Object*>(depthHandle);
    ID3D12Resource* res = (obj != nullptr) ? obj->resource.Get() : nullptr;
    if (res == nullptr) {
        return 0;
    }
    DlssTransitionFn tl = [](void* opaqueCtx, void* resource, int32_t toState) {
        auto* c = static_cast<CommandContext*>(opaqueCtx);
        if (c == nullptr || resource == nullptr) {
            return;
        }
        transitionTo(c, static_cast<ID3D12Resource*>(resource), D3D12_RESOURCE_STATE_COMMON,
                     static_cast<D3D12_RESOURCE_STATES>(toState));
    };
    DlssQueryStateFn queryState = [](void* opaqueCtx, void* resource) -> int32_t {
        auto* c = static_cast<CommandContext*>(opaqueCtx);
        if (c == nullptr || resource == nullptr) {
            return static_cast<int32_t>(D3D12_RESOURCE_STATE_COMMON);
        }
        auto it = c->resourceState.find(static_cast<ID3D12Resource*>(resource));
        return static_cast<int32_t>(it != c->resourceState.end() ? it->second
                                                                 : D3D12_RESOURCE_STATE_COMMON);
    };
    return mcdr_dlss_snapshot_depth(reinterpret_cast<void*>(ctx->commandList),
                                    reinterpret_cast<void*>(res), tl, ctx, queryState);
}

int queryDlss(char* out, int cap) {
    if (cap < 0 || (cap > 0 && out == nullptr)) {
        return -1;
    }
    const auto fail = [out, cap](const char* what) {
        if (out != nullptr && cap > 0) {
            std::snprintf(out, static_cast<size_t>(cap), "%s", what);
        }
        return 0;
    };
    if (auto d = ensureDevice(); !d) {
        return fail("device not created");
    }
    wireDlssImpl();
    if (!gCtx.rtx.ngxRuntime) {
        return fail(gCtx.rtx.ngxNote.empty() ? "ngx runtime not found" : gCtx.rtx.ngxNote.c_str());
    }
    if (mcdr_dlss_can_run(reinterpret_cast<void*>(gCtx.device.Get())) != 1) {
        if (gCtx.rtx.dlssQueried && !gCtx.rtx.dlssSuperResolution) {
            char why[192] = {};
            std::snprintf(why, sizeof(why), "NGX init failed and the adapter reports no DLSS: %s",
                          gCtx.rtx.dlssSuperResolutionWhy.empty()
                              ? "unknown"
                              : gCtx.rtx.dlssSuperResolutionWhy.c_str());
            return fail(why);
        }
        return fail("NGX init failed (see the DLSS WARN line)");
    }
    char note[256] = {};
    char optimal[256] = {};
    char runtime[512] = {};
    uint32_t flags = 0;
    mcdr_dlss_query(note, sizeof(note), optimal, sizeof(optimal), runtime, sizeof(runtime), &flags);
    if (out != nullptr && cap > 0) {
        std::snprintf(out, static_cast<size_t>(cap), "%s | %s | runtime=%s | flags=0x%X", note,
                      optimal[0] != '\0' ? optimal : "(no optimal settings yet)",
                      runtime[0] != '\0' ? runtime : "(unknown)",
                      static_cast<unsigned>(flags));
    }
    return 1;
}

namespace {

HANDLE gEvtBeginFrame = nullptr;
HANDLE gEvtRecordingReady = nullptr;
HANDLE gEvtCommandsReady = nullptr;
HANDLE gEvtSubmitDone = nullptr;

std::atomic<bool> gRenderThreadBusy{false};
std::atomic<int> gAsyncSubmissionResult{0};

std::mutex gPresentMtx;
std::condition_variable gPresentCv;
UINT64 gSubmittedFrames = 0;
UINT64 gPresentedFrames = 0;
std::atomic<bool> gPresentRetirementFailed{false};

static DWORD WINAPI renderThreadFunc(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    dbgLogInfo("renderThread: started tid=%lu", static_cast<unsigned long>(GetCurrentThreadId()));

    while (true) {
        for (;;) {
            if (!gRenderRunning) return 0;
            DWORD r = WaitForSingleObject(gEvtBeginFrame, 500);
            if (r == WAIT_OBJECT_0) break;
            if (r == WAIT_TIMEOUT && !gRenderRunning) return 0;
        }
        if (!gRenderRunning) return 0;
        if (!gAsyncRenderCtx) continue;

        CommandContext* ctx = gAsyncRenderCtx.load(std::memory_order_acquire);
        if (!ctx) continue;

        struct RenderBusyGuard {
            RenderBusyGuard() { gRenderThreadBusy.store(true, std::memory_order_release); }
            ~RenderBusyGuard() { gRenderThreadBusy.store(false, std::memory_order_release); }
        };
        [[maybe_unused]] RenderBusyGuard renderBusyGuard;

        dbgLogDebug("renderThread: wake ctx=%p listOpen=%d", reinterpret_cast<void*>(ctx), ctx->listOpen);

        dbgLogDebug("renderThread: signaling recordingReady ctx=%p", reinterpret_cast<void*>(ctx));
        SetEvent(gEvtRecordingReady);

        dbgLogDebug("renderThread: waiting for gEvtCommandsReady ctx=%p", reinterpret_cast<void*>(ctx));
        static const bool sProf = (getenv("DX12_PROF") != nullptr);
        static UINT64 sProfFrames = 0;
        static double sProfWaitCmdMs = 0.0, sProfSubmitMs = 0.0, sProfPresentMs = 0.0;
        const double tProfWait0 = sProf ? nowMs() : 0.0;
        int commandsWaitLoops = 0;
        bool commandsReady = false;
        for (;;) {
            if (!gRenderRunning) {
                return 0;
            }
            DWORD r = WaitForSingleObject(gEvtCommandsReady, 500);
            if (r == WAIT_OBJECT_0) {
                if (!gRenderRunning) return 0;
                dbgLogDebug("renderThread: commandsReady ctx=%p", reinterpret_cast<void*>(ctx));
                commandsReady = true;
                break;
            }
            if (r == WAIT_TIMEOUT && !gRenderRunning) {
                return 0;
            }
            if (++commandsWaitLoops >= 10) {
                dbgLogError("renderThread: commandsReady timeout after %ds, aborting frame ctx=%p",
                       commandsWaitLoops * 500 / 1000, reinterpret_cast<void*>(ctx));
                fgAbortDisplayFrame(ctx);
                failDisplayPresentSubmission(ctx);
                gAsyncSubmissionResult.store(-1, std::memory_order_release);
                SetEvent(gEvtSubmitDone);
                break;
            }
        }
        if (!commandsReady) continue;

        const double tProfWait1 = sProf ? nowMs() : 0.0;
        auto submitRes = submitCommandList(ctx);
        const double tProfSubmit1 = sProf ? nowMs() : 0.0;
        if (!submitRes) {
            dbgLogError("renderThread: submitCommandList FAILED: %s", errorMessage(submitRes.error()).c_str());
            failDisplayPresentSubmission(ctx);
            gAsyncSubmissionResult.store(ctx->recordingFailed.load(std::memory_order_acquire) &&
                !ctx->submissionFailed.load(std::memory_order_acquire) ? 2 : -1, std::memory_order_release);
            SetEvent(gEvtSubmitDone);
            continue;
        }
        [[maybe_unused]] const UINT64 value = *submitRes;
        const auto frameGenerationTicket = fgDisplayFrameTicket(ctx);

        const UINT64 submittedQueueFence = ctx->lastSubmitQueueFence;

        Dx12Surface* s = getActiveSurface();
        const int bbIdx = (s && s->currentImageIndex >= 0) ? s->currentImageIndex : -1;
        if (s && bbIdx >= 0) {
            if (s->surfaceFences.size() < static_cast<size_t>(kSurfaceBufferCount))
                s->surfaceFences.resize(kSurfaceBufferCount, 0);
            s->surfaceFences[static_cast<size_t>(bbIdx)] = submittedQueueFence;
        }

        DBG_LOG_DEBUG("renderThread: submit done v=%llu qf=%llu", (unsigned long long)value,
                      (unsigned long long)gCtx.queueFenceValue);

        gAsyncOwnsPresent = true;
        {
            std::lock_guard<std::mutex> lk(gPresentMtx);
            ++gSubmittedFrames;
        }
        gAsyncSubmissionResult.store(1, std::memory_order_release);
        SetEvent(gEvtSubmitDone);

        dbgLogDebug("renderThread: about to presentSurface qf=%llu bbIdx=%d",
                    static_cast<unsigned long long>(gCtx.queueFenceValue), bbIdx);
        const auto presentResult = presentSurface(getActiveSurface(), frameGenerationTicket);
        if (sProf) {
            const double tProfPresent1 = nowMs();
            sProfWaitCmdMs += tProfWait1 - tProfWait0;
            sProfSubmitMs += tProfSubmit1 - tProfWait1;
            sProfPresentMs += tProfPresent1 - tProfSubmit1;
            if (++sProfFrames % 300 == 0) {
                const double inv = 1.0 / 300.0;
                dbgLogDebug("P3-native prof n=300: waitCommands=%.2fms close+exec+signal=%.2fms present=%.2fms",
                       sProfWaitCmdMs * inv, sProfSubmitMs * inv, sProfPresentMs * inv);
                sProfWaitCmdMs = sProfSubmitMs = sProfPresentMs = 0.0;
            }
        }
        UINT64 presentQueueFenceValue = 0;
        uint32_t presentRetirementState = MCDR_PRESENT_RETIREMENT_NOT_REQUIRED;
        if (s && bbIdx >= 0) {
            std::lock_guard<std::mutex> queueLock(gQueueSubmissionMutex);
            UINT64 pv = ++gCtx.queueFenceValue;
            const HRESULT presentSignal = gCtx.queue->Signal(gCtx.queueFence.Get(), pv);
            if (FAILED(presentSignal)) {
                presentRetirementState = MCDR_PRESENT_RETIREMENT_UNKNOWN;
                gPresentRetirementFailed.store(true, std::memory_order_release);
                dbgLogError("renderThread: post-Present Signal failed hr=0x%08X", static_cast<unsigned>(presentSignal));
            } else {
                presentQueueFenceValue = pv;
                presentRetirementState = MCDR_PRESENT_RETIREMENT_SIGNALLED;
                if (s->surfacePresentFences.size() < static_cast<size_t>(kSurfaceBufferCount))
                    s->surfacePresentFences.resize(kSurfaceBufferCount, 0);
                s->surfacePresentFences[static_cast<size_t>(bbIdx)] = pv;
            }
        }

        completeDisplayPresentReceipt(frameGenerationTicket, presentResult,
            presentQueueFenceValue, presentRetirementState);
        dbgLogDebug("renderThread: post-present qf=%llu openList=%d",
                    static_cast<unsigned long long>(gCtx.queueFenceValue), gOpenListCount);
        if (gOpenListCount == 0) flushPendingDeletes();

        {
            std::lock_guard<std::mutex> lk(gPresentMtx);
            ++gPresentedFrames;
        }
        gPresentCv.notify_all();

        dbgLogDebug("renderThread: frame done, looping back");
    }

    return 0;
}

}

std::thread gRenderThread;
std::atomic<bool> gRenderRunning{false};
std::atomic<CommandContext*> gAsyncRenderCtx{nullptr};
std::atomic<bool> gAsyncOwnsPresent{false};

bool waitForPendingPresents(unsigned timeoutMs) {
    std::unique_lock<std::mutex> lk(gPresentMtx);
    if (gPresentedFrames >= gSubmittedFrames) return !gPresentRetirementFailed.load(std::memory_order_acquire);
    if (!gPresentCv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                             [] { return gPresentedFrames >= gSubmittedFrames; })) {
        dbgLogError("waitForPendingPresents: TIMEOUT submitted=%llu presented=%llu (render thread stalled)",
               static_cast<unsigned long long>(gSubmittedFrames),
               static_cast<unsigned long long>(gPresentedFrames));
        return false;
    }
    return !gPresentRetirementFailed.load(std::memory_order_acquire);
}

bool waitForRenderThreadSubmit() {
    if (!gRenderRunning || !gRenderThread.joinable()) return waitForPendingPresents();
    if (!gRenderThreadBusy.load(std::memory_order_acquire)) {
        return waitForPendingPresents();
    }
    const double p98t0 = nowMs();
    SetEvent(gEvtBeginFrame);
    DWORD r = WaitForSingleObject(gEvtSubmitDone, 10000);
    const double p98submitDoneMs = nowMs() - p98t0;
    if (r != WAIT_OBJECT_0) {
        dbgLogWarn("waitForRenderThreadSubmit: timeout (render thread may be stuck)");
        return false;
    }
    const double p98t1 = nowMs();
    const bool presentsComplete = waitForPendingPresents();
    const double p98presentsMs = nowMs() - p98t1;
    if (p98submitDoneMs > 30.0 || p98presentsMs > 30.0) {
        dbgLogInfo("waitForRenderThreadSubmit: submitDone=%.1f ms (waitResult=%lu) pendingPresents=%.1f ms",
               p98submitDoneMs, static_cast<unsigned long>(r), p98presentsMs);
    }
    return presentsComplete;
}

mcdr::Result<void> beginDisplayPresentReceipt(const FgPresentTicket& ticket) {
    auto* context = static_cast<CommandContext*>(ticket.context);
    if (!context) return {};
    std::lock_guard lock(gPresentMtx);
    auto& result = context->displayPresentResult;
    if (result.phase == DisplayPresentPhase::Awaiting || result.phase == DisplayPresentPhase::Terminal)
        return mcdr::err("display.receipt", "Previous display result has not been consumed");
    result = {};
    result.phase = DisplayPresentPhase::Awaiting;
    result.receipt.schemaVersion = MCDR_PRESENT_RECEIPT_SCHEMA_VERSION;
    result.receipt.structSize = sizeof(mcdr_present_receipt_v1);
    result.receipt.serial = ticket.serial;
    result.receipt.providerGeneration = ticket.providerGeneration;
    return {};
}

void attachDisplayPresentIdentity(void* opaqueContext, const mcdr_frame_identity& identity,
    uint64_t historyGeneration) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context) return;
    std::lock_guard lock(gPresentMtx);
    auto& result = context->displayPresentResult;
    if (result.phase != DisplayPresentPhase::Awaiting) return;
    result.receipt.identity = identity;
    result.receipt.historyGeneration = historyGeneration;
}

void queueDisplayPresentReceipt(const FgPresentTicket& ticket, uint32_t submissionState) {
    auto* context = static_cast<CommandContext*>(ticket.context);
    if (!context) return;
    std::lock_guard lock(gPresentMtx);
    auto& result = context->displayPresentResult;
    if (result.phase != DisplayPresentPhase::Awaiting || result.receipt.serial != ticket.serial) return;
    result.receipt.submissionState = submissionState;
    if (submissionState == MCDR_SUBMISSION_SUBMITTED) {
        result.receipt.commandFenceValue = ticket.fenceValue;
        result.receipt.submitQueueFenceValue = context->lastSubmitQueueFence;
    }
}

void failDisplayPresentSubmission(void* opaqueContext) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context) return;
    {
        std::lock_guard lock(gPresentMtx);
        auto& result = context->displayPresentResult;
        if (result.phase != DisplayPresentPhase::Awaiting) return;
        const bool rejected = context->recordingFailed.load(std::memory_order_acquire) &&
            !context->submissionFailed.load(std::memory_order_acquire);
        result.receipt.submissionState = rejected ? MCDR_SUBMISSION_FAILED : MCDR_SUBMISSION_UNKNOWN;
        result.receipt.presentOutcome = rejected ? MCDR_PRESENT_CANCELLED : MCDR_PRESENT_UNKNOWN;
        result.receipt.retirementState = rejected ? MCDR_PRESENT_RETIREMENT_NOT_REQUIRED : MCDR_PRESENT_RETIREMENT_UNKNOWN;
        result.phase = DisplayPresentPhase::Terminal;
    }
    gPresentCv.notify_all();
}

void cancelDisplayPresentReceipt(void* opaqueContext) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (!context) return;
    {
        std::lock_guard lock(gPresentMtx);
        auto& result = context->displayPresentResult;
        if (result.phase != DisplayPresentPhase::Awaiting) return;
        result.receipt.presentOutcome = MCDR_PRESENT_CANCELLED;
        result.receipt.retirementState = MCDR_PRESENT_RETIREMENT_NOT_REQUIRED;
        result.phase = DisplayPresentPhase::Terminal;
    }
    gPresentCv.notify_all();
}

void completeDisplayPresentReceipt(const FgPresentTicket& ticket, SurfacePresentResult present,
    uint64_t presentQueueFenceValue, uint32_t retirementState) {
    auto* context = static_cast<CommandContext*>(ticket.context);
    if (!context) return;
    {
        std::lock_guard lock(gPresentMtx);
        auto& result = context->displayPresentResult;
        if (result.phase != DisplayPresentPhase::Awaiting || result.receipt.serial != ticket.serial) return;
        result.receipt.presentOutcome = present.outcome;
        result.receipt.dxgiStatus = present.dxgiStatus;
        result.receipt.flags = present.flags;
        result.receipt.outputFrameCount = present.outputFrameCount;
        result.receipt.presentQueueFenceValue = presentQueueFenceValue;
        result.receipt.retirementState = retirementState;
        result.phase = DisplayPresentPhase::Terminal;
    }
    gPresentCv.notify_all();
}

mcdr::Result<int32_t> settleDisplayFrame(void* opaqueContext, uint64_t timeoutMs,
    void* opaqueReceipt, uint32_t byteCount) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto* output = static_cast<mcdr_present_receipt_v1*>(opaqueReceipt);
    if (!context || !output || byteCount != sizeof(*output))
        return mcdr::err("display.receipt", "Invalid display settlement context or receipt");
    *output = {};
    output->schemaVersion = MCDR_PRESENT_RECEIPT_SCHEMA_VERSION;
    output->structSize = sizeof(*output);
    std::unique_lock lock(gPresentMtx);
    auto& result = context->displayPresentResult;
    if (result.phase == DisplayPresentPhase::Empty || result.phase == DisplayPresentPhase::Consumed)
        return MCDR_DISPLAY_SETTLE_EMPTY;
    const auto timeout = std::chrono::milliseconds(std::min<uint64_t>(timeoutMs, 0xFFFFFFF0ULL));
    if (!gPresentCv.wait_for(lock, timeout, [&] { return result.phase == DisplayPresentPhase::Terminal; })) {
        *output = result.receipt;
        return MCDR_DISPLAY_SETTLE_TIMEOUT;
    }
    *output = result.receipt;
    if (output->submissionState == MCDR_SUBMISSION_UNKNOWN ||
        output->retirementState == MCDR_PRESENT_RETIREMENT_UNKNOWN)
        return MCDR_DISPLAY_SETTLE_SUBMISSION_UNKNOWN;
    result.phase = DisplayPresentPhase::Consumed;
    return MCDR_DISPLAY_SETTLE_COMPLETE;
}

mcdr::Result<void> abortDisplayFrame(void* opaqueContext) {
    auto* context = static_cast<CommandContext*>(opaqueContext);
    if (context == nullptr) return mcdr::err("abortDisplayFrame", "Null command context");
    std::lock_guard<std::mutex> submissionLock(gAsyncSubmissionMutex);
    const auto initialTicket = fgDisplayFrameTicket(context);
    if (gAsyncRenderCtx.load(std::memory_order_acquire) == context) {
        if (!gRenderRunning || !gEvtSubmitDone)
            return mcdr::err("abortDisplayFrame", "Asynchronous renderer still owns the context but is not running");
        if (WaitForSingleObject(gEvtSubmitDone, 10'000) != WAIT_OBJECT_0)
            return mcdr::err("abortDisplayFrame", "Asynchronous command submission did not finish within 10 seconds");
        const int result = gAsyncSubmissionResult.load(std::memory_order_acquire);
        if ((result != 1 && result != 2) || context->submissionFailed.load())
            return mcdr::err("abortDisplayFrame", "Asynchronous command submission failed; context ownership retained");
        gAsyncRenderCtx = nullptr;
    }
    if (!waitForPendingPresents(2'000))
        return mcdr::err("abortDisplayFrame", "Pending Present did not finish or its retirement signal failed");
    auto* surface = getActiveSurface();
    std::unique_lock<std::mutex> presentLock;
    if (surface) presentLock = std::unique_lock<std::mutex>(surface->presentMutex);
    std::lock_guard<std::mutex> queueLock(gQueueSubmissionMutex);
    if (context->submissionFailed.load())
        return mcdr::err("abortDisplayFrame", "Command submission failed; resources retained");
    if (context->inRenderPass)
        return mcdr::err("abortDisplayFrame", "Command context still has an open render pass");
    const bool rejected = context->recordingFailed.load(std::memory_order_acquire);
    if (context->listOpen && !rejected) {
        const auto submitted = submitRecordedCommandList(context);
        if (!submitted) return mcdr::err("abortDisplayFrame", errorMessage(submitted.error()));
    }
    if (context->fenceValue != 0) {
        const auto waited = waitForFenceValue(context, context->fenceValue, 5'000'000'000ULL);
        if (!waited) {
            context->submissionFailed = true;
            return mcdr::err("abortDisplayFrame", errorMessage(waited.error()));
        }
    }
    if (rejected) {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        HRESULT result = gCtx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (SUCCEEDED(result)) result = gCtx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator.Get(), nullptr, IID_PPV_ARGS(&list));
        if (SUCCEEDED(result)) result = list->Close();
        ComPtr<ID3D12GraphicsCommandList10> workGraphList;
        if (SUCCEEDED(result)) {
            const HRESULT workGraphInterface = list.As(&workGraphList);
            if (FAILED(workGraphInterface) && (workGraphInterface != E_NOINTERFACE ||
                gCtx.caps.workGraphsTier >= D3D12_WORK_GRAPHS_TIER_1_0)) result = workGraphInterface;
        }
        if (FAILED(result))
            return mcdr::err("abortDisplayFrame", "Could not replace rejected recording: " + formatBackendResult(result));
        {
            std::lock_guard recordingLock(gRecordingContextsMutex);
            gRecordingContexts.erase(context->commandList);
            context->commandList = nullptr;
            context->workGraphCommandList = nullptr;
            context->commandLists[context->currentListSlot].Reset();
            context->workGraphCommandLists[context->currentListSlot].Reset();
            context->allocators[context->currentListSlot] = std::move(allocator);
            context->commandLists[context->currentListSlot] = std::move(list);
            context->workGraphCommandLists[context->currentListSlot] = std::move(workGraphList);
            context->commandList = context->commandLists[context->currentListSlot].Get();
            context->workGraphCommandList = context->workGraphCommandLists[context->currentListSlot].Get();
            gRecordingContexts.emplace(context->commandList, context);
        }
        if (context->listOpen && gOpenListCount > 0) --gOpenListCount;
        context->listOpen = 0;
        context->recordingLeases[context->currentListSlot].clear();
        context->retainedGraphicsPipelines.clear();
        context->retainedRecordingObjects.clear();
        if (context->recordingDiscarded) context->recordingDiscarded->store(true, std::memory_order_release);
        context->recordingDiscarded.reset();
        context->recordingIdentity.reset();
        context->controlIdentity.reset();
        context->resourceState.clear();
        context->subresourceState.clear();
        context->activeColorTargets.clear();
        context->activeColorTargetsTouched.clear();
        context->activeDepthTarget = nullptr;
        context->activeAttachmentRanges.clear();
        context->activePassRtvs.clear();
        context->activePassDsv = {};
        context->activeMotionRtv = {};
        context->activeMotionTarget = nullptr;
        context->additionalColorRtv = {};
        context->additionalColorTarget = nullptr;
        context->activePassBoundColorCount = UINT_MAX;
        context->rasterMotionWorldColor = nullptr;
        context->rasterMotionWorldDepth = nullptr;
        context->rasterMotionTarget = nullptr;
        context->rasterMotionLeases.clear();
        context->rasterMotionSelected = false;
        context->vanillaDepthSeedState = nullptr;
        context->vanillaDepthSeedClear = nullptr;
        context->vanillaDepthSeedDraw = nullptr;
        context->vanillaDepthSeedOrdinal = 0;
        context->colorTargetsWritten = false;
        context->currentPipeline = nullptr;
        invalidateGraphicsInputBindings(context, true);
        context->graphicsInputRanges.clear();
        context->graphicsCapture = {};
        context->captureShadingRate = {};
        context->shadingRateImageBound = nullptr;
        context->lastSrvViews.clear();
        context->rtvCache.clear();
        context->dsvCache.clear();
        context->vanillaFrameState.reset();
        context->oitFrameState.reset();
        invalidatePreparedReconstructionHistory();
    }
    const auto currentTicket = fgDisplayFrameTicket(context);
    if (currentTicket.serial != 0 && currentTicket.serial != initialTicket.serial)
        return mcdr::err("abortDisplayFrame", "Display frame changed while waiting for cancellation");
    if (currentTicket.serial != 0) fgAbortDisplayFrame(currentTicket);
    if (surface && initialTicket.serial != 0 &&
        ((surface->frameGenerationTicket.serial == 0 && currentTicket.serial == initialTicket.serial) ||
         (surface->frameGenerationTicket.context == context &&
          surface->frameGenerationTicket.serial == initialTicket.serial))) {
        surface->frameGenerationTicket = {};
        surface->blittedThisFrame.store(false, std::memory_order_release);
    }
    if (currentTicket.serial != 0 && mcdr_dlss_inputs_ready_for_present() != 1)
        return mcdr::err("abortDisplayFrame", "Frame generation input invalidation failed; resources retained");
    cancelDisplayPresentReceipt(context);
    if (rejected) context->recordingFailed.store(false, std::memory_order_release);
    return {};
}

bool initAsyncRenderer(UINT) {
    std::lock_guard<std::mutex> submissionLock(gAsyncSubmissionMutex);
    if (auto devRes = ensureDevice(); !devRes) {
        dbgLogError("initAsyncRenderer: ensureDevice failed: %s", errorMessage(devRes.error()).c_str());
        return false;
    }
    if (gRenderRunning) return true;
    if (gRenderThread.joinable() || gEvtBeginFrame || gEvtRecordingReady || gEvtCommandsReady || gEvtSubmitDone) {
        return false;
    }

    gEvtBeginFrame = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    gEvtRecordingReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    gEvtCommandsReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    gEvtSubmitDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!gEvtBeginFrame || !gEvtRecordingReady || !gEvtCommandsReady || !gEvtSubmitDone) {
        dbgLogError("initAsyncRenderer: CreateEvent failed");
        if (gEvtBeginFrame) CloseHandle(gEvtBeginFrame);
        if (gEvtRecordingReady) CloseHandle(gEvtRecordingReady);
        if (gEvtCommandsReady) CloseHandle(gEvtCommandsReady);
        if (gEvtSubmitDone) CloseHandle(gEvtSubmitDone);
        gEvtBeginFrame = nullptr;
        gEvtRecordingReady = nullptr;
        gEvtCommandsReady = nullptr;
        gEvtSubmitDone = nullptr;
        return false;
    }

    gAsyncRenderCtx = nullptr;
    {
        std::lock_guard<std::mutex> lk(gPresentMtx);
        gSubmittedFrames = 0;
        gPresentedFrames = 0;
        gPresentRetirementFailed.store(false, std::memory_order_release);
    }
    gRenderRunning = true;
    gRenderThread = std::thread([]() {
        DWORD ret = renderThreadFunc(nullptr);
        (void)ret;
    });

    dbgLogInfo("initAsyncRenderer: started render thread");
    return true;
}

bool destroyAsyncRenderer() {
    std::lock_guard<std::mutex> submissionLock(gAsyncSubmissionMutex);
    if (gAsyncRenderCtx.load(std::memory_order_acquire) &&
        gAsyncSubmissionResult.load(std::memory_order_acquire) == 0 && gRenderRunning) {
        if (WaitForSingleObject(gEvtSubmitDone, 10000) != WAIT_OBJECT_0) {
            dbgLogError("destroyAsyncRenderer: submission remains pending; retaining renderer");
            return false;
        }
    }

    gRenderRunning = false;

    if (gRenderThread.joinable()) {
        SetEvent(gEvtBeginFrame);
        SetEvent(gEvtRecordingReady);
        SetEvent(gEvtCommandsReady);
        if (WaitForSingleObject(gRenderThread.native_handle(), 10000) != WAIT_OBJECT_0) {
            dbgLogError("destroyAsyncRenderer: worker termination timed out; retaining renderer");
            return false;
        }
        gRenderThread.join();
    } else {
        gRenderRunning = false;
    }

    if (gCtx.device) {
        if (auto idle = deviceWaitIdle(); !idle) {
            dbgLogError("destroyAsyncRenderer: GPU remains active; retaining renderer: %s", errorMessage(idle.error()).c_str());
            return false;
        }
    }

    if (gEvtBeginFrame) {
        CloseHandle(gEvtBeginFrame);
        gEvtBeginFrame = nullptr;
    }
    if (gEvtRecordingReady) {
        CloseHandle(gEvtRecordingReady);
        gEvtRecordingReady = nullptr;
    }
    if (gEvtCommandsReady) {
        CloseHandle(gEvtCommandsReady);
        gEvtCommandsReady = nullptr;
    }
    if (gEvtSubmitDone) {
        CloseHandle(gEvtSubmitDone);
        gEvtSubmitDone = nullptr;
    }
    gAsyncRenderCtx = nullptr;
    dbgLogInfo("destroyAsyncRenderer: done");
    return true;
}

mcdr::Result<void> asyncRenderBeginFrame(CommandContext* ctx) {
    std::lock_guard<std::mutex> submissionLock(gAsyncSubmissionMutex);
    if (!ctx || ctx->submissionFailed || ctx->recordingFailed.load(std::memory_order_acquire)) {
        return mcdr::err("asyncRenderBeginFrame", "asyncRenderBeginFrame: invalid or failed context");
    }
    if (!gRenderRunning) {
        gAsyncOwnsPresent = false;
        return mcdr::err("asyncRenderBeginFrame", "asyncRenderBeginFrame: renderer not running");
    }
    if (getActiveSurface() == nullptr) {
        dbgLogInfo("asyncRenderBeginFrame: no active surface, fallback sync");
        gAsyncOwnsPresent = false;
        return mcdr::err("asyncRenderBeginFrame", "asyncRenderBeginFrame: no active surface, fallback sync");
    }
    if (gAsyncRenderCtx != nullptr) {
        return mcdr::err("asyncRenderBeginFrame", "asyncRenderBeginFrame: previous frame not complete");
    }
    gAsyncRenderCtx = ctx;
    queueDisplayPresentReceipt(fgDisplayFrameTicket(ctx), MCDR_SUBMISSION_QUEUED);
    gAsyncSubmissionResult.store(0, std::memory_order_release);
    gAsyncOwnsPresent = false;
    ResetEvent(gEvtRecordingReady);
    ResetEvent(gEvtCommandsReady);
    ResetEvent(gEvtSubmitDone);
    SetEvent(gEvtBeginFrame);
    dbgLogDebug("asyncRenderBeginFrame: signaled beginFrame ctx=%p prevFence=%llu",
                reinterpret_cast<void*>(ctx), static_cast<unsigned long long>(ctx->prevFenceValue));
    return {};
}

int settleCommandSubmissionV1(CommandContext* ctx, uint64_t timeoutMs, uint64_t* submittedFence) {
    if (!submittedFence) return MCDR_COMMAND_OUTCOME_INVALID;
    *submittedFence = 0;
    std::lock_guard submissionLock(gAsyncSubmissionMutex);
    if (!ctx || gAsyncRenderCtx.load(std::memory_order_acquire) != ctx)
        return MCDR_COMMAND_OUTCOME_INVALID;
    if (!gRenderRunning || !gEvtSubmitDone) return MCDR_COMMAND_OUTCOME_SUBMISSION_UNKNOWN;
    const DWORD waitMs = timeoutMs > 0xFFFFFFF0ULL ? 0xFFFFFFF0UL : static_cast<DWORD>(timeoutMs);
    const DWORD waited = WaitForSingleObject(gEvtSubmitDone, waitMs);
    if (waited == WAIT_TIMEOUT) return MCDR_COMMAND_OUTCOME_OWNERSHIP_PENDING;
    if (waited != WAIT_OBJECT_0 || ctx->submissionFailed.load(std::memory_order_acquire))
        return MCDR_COMMAND_OUTCOME_SUBMISSION_UNKNOWN;
    const int result = gAsyncSubmissionResult.load(std::memory_order_acquire);
    if (result == 2 && ctx->recordingFailed.load(std::memory_order_acquire)) {
        gAsyncRenderCtx = nullptr;
        return MCDR_COMMAND_OUTCOME_RECORDING_REJECTED;
    }
    if (result != 1) return MCDR_COMMAND_OUTCOME_SUBMISSION_UNKNOWN;
    *submittedFence = ctx->fenceValue;
    gAsyncRenderCtx = nullptr;
    return MCDR_COMMAND_OUTCOME_SUBMITTED;
}

mcdr::Result<void> asyncRenderWaitComplete(CommandContext* ctx, UINT64 timeoutMs) {
    std::lock_guard<std::mutex> submissionLock(gAsyncSubmissionMutex);
    if (!ctx || gAsyncRenderCtx.load(std::memory_order_acquire) != ctx) {
        return mcdr::err("asyncRenderWaitComplete", "asyncRenderWaitComplete: context does not own submission");
    }
    if (!gRenderRunning) {
        return mcdr::err("asyncRenderWaitComplete", "asyncRenderWaitComplete: renderer not running");
    }
    DWORD waitMs = timeoutMs > 0xFFFFFFF0ULL ? 0xFFFFFFF0UL : static_cast<DWORD>(timeoutMs);
    DWORD r = WaitForSingleObject(gEvtSubmitDone, waitMs);
    if (r == WAIT_OBJECT_0) {
        if (gAsyncSubmissionResult.load(std::memory_order_acquire) != 1 || ctx->submissionFailed) {
            return mcdr::err("asyncRenderWaitComplete", "asyncRenderWaitComplete: submission failed; ownership retained");
        }
        gAsyncRenderCtx = nullptr;
        dbgLogDebug("asyncRenderWaitComplete: OK timeout=%llu", static_cast<unsigned long long>(timeoutMs));
        return {};
    }
    dbgLogWarn("asyncRenderWaitComplete: TIMEOUT timeout=%llu", static_cast<unsigned long long>(timeoutMs));
    return mcdr::err("asyncRenderWaitComplete", "asyncRenderWaitComplete: timeout");
}

bool asyncRenderIsRecordingReady(CommandContext* ctx) {
    if (!ctx || gAsyncRenderCtx.load(std::memory_order_acquire) != ctx) return false;
    if (!gRenderRunning) return false;
    DWORD r = WaitForSingleObject(gEvtRecordingReady, 0);
    return r == WAIT_OBJECT_0;
}

bool asyncRenderWaitRecordingReady(CommandContext* ctx, UINT64 timeoutMs) {
    if (!ctx || gAsyncRenderCtx.load(std::memory_order_acquire) != ctx) return false;
    if (!gRenderRunning) return false;
    DWORD waitMs = timeoutMs > 0xFFFFFFF0ULL ? 0xFFFFFFF0UL : static_cast<DWORD>(timeoutMs);
    DWORD r = WaitForSingleObject(gEvtRecordingReady, waitMs);
    return r == WAIT_OBJECT_0;
}

bool isListOpen(CommandContext* ctx) { return ctx && ctx->listOpen; }

UINT currentDrawFrameSlot(CommandContext* ctx) {
    if (!ctx) return 0;
    return static_cast<UINT>(ctx->fenceValue % kDrawHeapSections);
}

void asyncSendCommandsReady(CommandContext* ctx) {
    if (!ctx || gAsyncRenderCtx.load(std::memory_order_acquire) != ctx) return;
    if (!gRenderRunning) return;
    SetEvent(gEvtCommandsReady);
}

void clearAsyncRenderCtx() {
    std::lock_guard<std::mutex> submissionLock(gAsyncSubmissionMutex);
    if (gAsyncRenderCtx.load(std::memory_order_acquire)) {
        dbgLogWarn("clearAsyncRenderCtx: refusing to discard pending ownership");
    }
}

}
