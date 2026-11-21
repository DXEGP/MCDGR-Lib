module;

#include "_d3d12_module_prelude.h"

export module mcdr.error;

export namespace mcdr {

struct Error {
    std::string message;
    const char* where = "";

    Error() = default;
    Error(const char* w, std::string m) : message(std::move(m)), where(w) {}
};

template <typename T>
using Result = std::expected<T, Error>;

inline std::unexpected<Error> err(const char* where, std::string message) {
    return std::unexpected(Error{where, std::move(message)});
}

inline std::expected<void, Error> ok() { return {}; }

inline std::string describe(const Error& e) {
    if (e.where == nullptr || e.where[0] == '\0') return e.message;
    return std::string(e.where) + ": " + e.message;
}

}
