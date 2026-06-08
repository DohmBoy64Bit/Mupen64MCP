#include "daemon.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <iostream>

bool validateDebugCore(const std::string &corePath) {
#ifdef _WIN32
    HMODULE mod = LoadLibraryA(corePath.c_str());
    if (!mod) return false;
    auto pGetVer = (m64p_error(*)(m64p_plugin_type *, int *, int *, const char **, int *))
        GetProcAddress(mod, "PluginGetVersion");
    if (!pGetVer) { FreeLibrary(mod); return false; }
    m64p_plugin_type type;
    int version;
    const char *name;
    int caps = 0;
    m64p_error r = pGetVer(&type, &version, &caps, &name, nullptr);
    FreeLibrary(mod);
    return (r == M64ERR_SUCCESS) && (caps & M64CAPS_DEBUGGER);
#else
    void *mod = dlopen(corePath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!mod) return false;
    auto pGetVer = (decltype(nullptr))dlsym(mod, "PluginGetVersion");
    if (!pGetVer) { dlclose(mod); return false; }
    m64p_plugin_type type;
    int version;
    const char *name;
    int caps = 0;
    m64p_error r = ((m64p_error(*)(m64p_plugin_type *, int *, int *, const char **, int *))pGetVer)
        (&type, &version, &caps, &name, nullptr);
    dlclose(mod);
    return (r == M64ERR_SUCCESS) && (caps & M64CAPS_DEBUGGER);
#endif
}
