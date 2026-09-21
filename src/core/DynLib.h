#pragma once
#include <string>
#include <utility>

namespace oss {

// RAII handle to a shared library opened at runtime (dlopen / LoadLibrary). GL-free.
// The platform headers live in DynLib.cpp so <windows.h> never leaks into a header.
class DynLib {
public:
    DynLib() = default;
    ~DynLib() { close(); }
    DynLib(const DynLib&) = delete;
    DynLib& operator=(const DynLib&) = delete;
    DynLib(DynLib&& o) noexcept : handle_(o.handle_), error_(std::move(o.error_)) { o.handle_ = nullptr; }
    DynLib& operator=(DynLib&& o) noexcept {
        if (this != &o) { close(); handle_ = o.handle_; error_ = std::move(o.error_); o.handle_ = nullptr; }
        return *this;
    }

    bool open(const std::string& path);   // false on failure; error() says why
    void close();
    bool isOpen() const { return handle_ != nullptr; }
    const std::string& error() const { return error_; }

    // Resolve `name` as a function pointer of type T; nullptr when closed or missing.
    template <class T> T symbol(const char* name) const { return reinterpret_cast<T>(rawSymbol(name)); }

private:
    void* rawSymbol(const char* name) const;
    void*       handle_ = nullptr;
    std::string error_;
};

} // namespace oss
