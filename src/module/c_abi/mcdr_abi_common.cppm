module;

#ifndef MCDR_EXPORTS
#define MCDR_EXPORTS 1
#endif

#include "mcdr_abi.h"

#include "_d3d12_module_prelude.h"

export module mcdr.abi.common;

import mcdr.error;

export namespace mcdr::abi {

void setLastError(const char* what, const std::string& message);
void setLastErrorf(const char* what, const char* fmt, ...);

const std::string& lastErrorText();

int32_t failWith(const char* what, const std::string& err);
void failVoid(const char* what, const std::string& err);

template <typename T>
inline T* failWithPtr(const char* what, const std::string& err) {
    failVoid(what, err);
    return nullptr;
}

inline int32_t failResult(const char* what, const mcdr::Error& e) {
    return failWith(what, mcdr::describe(e));
}

template <typename T>
inline T* failResultPtr(const char* what, const mcdr::Error& e) {
    return failWithPtr<T>(what, mcdr::describe(e));
}

template <typename T>
inline T* fromHandle(uint64_t h) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(h));
}

template <typename T>
inline uint64_t toHandle(const T* p) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p));
}

int32_t writeString(const std::string& s, char* buf, int32_t cap);
}

