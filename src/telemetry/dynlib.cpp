#include "dynlib.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace gt::telemetry {

bool DynLib::open(const std::string& name) {
    close();
#ifdef _WIN32
    std::wstring w(name.begin(), name.end());
    // Absolute paths load as-is; bare names only from System32 (no DLL planting from CWD).
    const bool absolute = name.find(':') != std::string::npos || name.rfind("\\\\", 0) == 0;
    HMODULE h = absolute ? LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)
                         : LoadLibraryExW(w.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    m_handle = reinterpret_cast<void*>(h);
    if (!m_handle) m_error = "LoadLibrary(" + name + ") failed (error " + std::to_string(GetLastError()) + ")";
#else
    m_handle = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m_handle) {
        const char* e = dlerror();
        m_error = e ? e : ("dlopen(" + name + ") failed");
    }
#endif
    return m_handle != nullptr;
}

void DynLib::close() {
    if (!m_handle) return;
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(m_handle));
#else
    dlclose(m_handle);
#endif
    m_handle = nullptr;
}

void* DynLib::symbol(const char* name) const {
    if (!m_handle) return nullptr;
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(m_handle), name));
#else
    return dlsym(m_handle, name);
#endif
}

} // namespace gt::telemetry
