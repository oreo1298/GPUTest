#pragma once
// Minimal dlopen/LoadLibrary wrapper.

#include <string>

namespace gt::telemetry {

class DynLib {
public:
    DynLib() = default;
    explicit DynLib(const std::string& name) { open(name); }
    ~DynLib() { close(); }
    DynLib(const DynLib&) = delete;
    DynLib& operator=(const DynLib&) = delete;
    bool open(const std::string& name);
    void close();
    bool loaded() const { return m_handle != nullptr; }
    void* symbol(const char* name) const;
    template <typename Fn>
    bool load(Fn& fn, const char* name) const {
        fn = reinterpret_cast<Fn>(symbol(name));
        return fn != nullptr;
    }
    const std::string& error() const { return m_error; }

private:
    void* m_handle = nullptr;
    std::string m_error;
};

} // namespace gt::telemetry
