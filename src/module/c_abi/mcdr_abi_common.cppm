// mcdr_abi_common.cppm — shared helpers for the MCDR C-ABI adapter layer.
//
// Internal to the DLL: never installed, never included by the Java side.
// Its whole job is to keep every exported wrapper thin; the only real difference
// from JNI is the error channel: a C ABI cannot raise, so every failure lands in
// a thread-local last-error string that the Java side polls with mcdrLastError().
//
// mcdr_abi.h 刻意【不】迁移成模块：它是 extern "C" 的 C ABI（126 个导出函数的声明面），
// 语言链接实体不能从模块导出；且 Java/FFM 侧与 Kotlin 的布局镜像（MCDR_ABI.kt /
// MCDR_Layout.kt）必须能按普通 C 头文件包含它。故它继续以头文件存在，只在本模块的
// 全局模块片段里被 #include。
//
// 实现单元里不要 import 自己所属的模块（冗余）；也不能把 `#include` 留在 module 声明
// 之前而不开 `module;`（C5201），或把 `#include` 放进模块范围内（C5244）：/WX 下均直接失败。见 docs\misc\build-and-modules.md

module;

// MCDR_EXPORTS 控制 mcdr_abi.h 里 MCDR_API 展开为 __declspec(dllexport)：本模块的
// 使用者都是本 DLL 内的实现单元，必须在包含 mcdr_abi.h 之前定义它。
#ifndef MCDR_EXPORTS
#define MCDR_EXPORTS 1
#endif

#include "mcdr_abi.h"

#include "_d3d12_module_prelude.h"

export module mcdr.abi.common;

// mcdr.error 是叶子模块（只依赖标准库），此处 import 不形成环。模块内部用
// mcdr::Result<T> 报错，而 C ABI 只认 int32_t / 指针 + mcdrLastError 文本，
// 翻译点就是下面的 failResult*。
import mcdr.error;

export namespace mcdr::abi {
// ---- Error reporting ----

// Records "<what>: <message>" as the calling thread's last error — thread-local so the
// async render thread and the FFM upcall thread never clobber the main thread's message.
void setLastError(const char* what, const std::string& message);
void setLastErrorf(const char* what, const char* fmt, ...);

// The calling thread's pending message for mcdrLastError(); never cleared on read —
// the Java side only asks after a wrapper has already reported failure.
const std::string& lastErrorText();

// The two shapes every wrapper needs: record + log to stderr + return a failure value.
int32_t failWith(const char* what, const std::string& err);
void failVoid(const char* what, const std::string& err);

template <typename T>
inline T* failWithPtr(const char* what, const std::string& err) {
    failVoid(what, err);
    return nullptr;
}

// ---- Result<T> -> C ABI 的翻译点 ----
// 模块内部一律用 mcdr::Result<T> 报错（失败在类型里，不可能被静默忽略）；C ABI 侧
// 必须保持 int32_t / 指针返回值 + 线程局部 mcdrLastError 文本，JVM/FFM 只认这套。
// 错误文本走 mcdr::describe() 得 "函数名: 说明"，Java 侧看到的字符串不变。

// 用于返回 int32_t 的包装函数：记录错误并返回 0（C ABI 的失败约定）。
inline int32_t failResult(const char* what, const mcdr::Error& e) {
    return failWith(what, mcdr::describe(e));
}

// 用于返回指针的包装函数：记录错误并返回 nullptr。
template <typename T>
inline T* failResultPtr(const char* what, const mcdr::Error& e) {
    return failWithPtr<T>(what, mcdr::describe(e));
}

// ---- Handle <-> pointer ----
// The only reinterpret_casts in the adapter layer. `mcdr_object*` and friends are
// typedefs of the real classes, so typed parameters need no cast; these serve the
// integer-valued handles (HWND, bundle list, GPU descriptor handles, user callbacks).

template <typename T>
inline T* fromHandle(uint64_t h) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(h));
}

template <typename T>
inline uint64_t toHandle(const T* p) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p));
}

// ---- String marshalling ----

// Copies `s` into `buf` as UTF-8 + NUL. Always returns the required size including the
// terminator, even when the buffer is too small, so callers can probe with (nullptr, 0)
// then size exactly; returns 0 only when there is genuinely nothing to write.
int32_t writeString(const std::string& s, char* buf, int32_t cap);
}  // namespace mcdr::abi
