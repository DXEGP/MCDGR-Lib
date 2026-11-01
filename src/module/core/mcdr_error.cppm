// mcdr.error —— 内部 C++ 错误约定的最小词表（Error / Result<T>）
//
// 失败必须在类型里：Result<T> 要么是 T 要么是 Error，取 value() 而未检查 has_value()
// 会抛 std::bad_expected_access，而不是静默给出垃圾值。
//
// 设计约束：
//   * 本模块是叶子：只依赖标准库，不 import 任何 mcdr.* 模块，故谁都能 import 而不成环；
//     标准库经全局模块片段的 #include 取得，不能用 `import std;`（放全局片段会 C5202）。
//   * Error 里只出现 std::string，不出现别的模块拥有的类型 —— 否则踩 v145 的模块私有名
//     修饰问题（`Type@$$_A<module>` 与消费方 `@1` 对不上）。
//   * 不强制错误码，只保留文本 + 出错的函数名。
//
// C ABI 边界（src\c_abi\*.cpp）不走 std::expected：那里必须继续是 `extern "C" int32_t` +
// 线程局部的 mcdrLastError 文本，JVM/FFM 侧只认这套。见 docs\misc\build-and-modules.md

module;

#include "_d3d12_module_prelude.h"

export module mcdr.error;

export namespace mcdr {

// message 给人看（日志、mcdrLastError）；where 是出错的函数名（字面量，生命周期不归 Error 管）。
struct Error {
    std::string message;
    const char* where = "";

    Error() = default;
    Error(const char* w, std::string m) : message(std::move(m)), where(w) {}
};

template <typename T>
using Result = std::expected<T, Error>;

// 刻意**不叫** fail()：避免和从 std::string& err 时代沿用下来的局部变量名相撞。
inline std::unexpected<Error> err(const char* where, std::string message) {
    return std::unexpected(Error{where, std::move(message)});
}

inline std::expected<void, Error> ok() { return {}; }

// 摊平成 "where: message" 一行，供 C ABI 的 setLastError / failWith 使用。
inline std::string describe(const Error& e) {
    if (e.where == nullptr || e.where[0] == '\0') return e.message;
    return std::string(e.where) + ": " + e.message;
}

}  // namespace mcdr
