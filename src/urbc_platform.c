#include "urbc_internal.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dlfcn.h>
#endif

#ifdef _WIN32
static URBC_THREAD_LOCAL char g_urbc_win32_dlerror[URBC_ERROR_CAP];

static const char *urbc_win32_store_error(DWORD code)
{
    DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD n;
    if (code == 0) code = GetLastError();
    if (code == 0) {
        g_urbc_win32_dlerror[0] = '\0';
        return NULL;
    }
    n = FormatMessageA(flags, NULL, code, 0,
                       g_urbc_win32_dlerror,
                       (DWORD)sizeof(g_urbc_win32_dlerror),
                       NULL);
    if (n == 0) {
        snprintf(g_urbc_win32_dlerror, sizeof(g_urbc_win32_dlerror),
                 "win32 error %lu", (unsigned long)code);
    } else {
        while (n > 0 && (g_urbc_win32_dlerror[n - 1] == '\r' || g_urbc_win32_dlerror[n - 1] == '\n')) {
            g_urbc_win32_dlerror[n - 1] = '\0';
            n--;
        }
    }
    return g_urbc_win32_dlerror;
}

void *urbc_dyn_open(const char *path, int flags)
{
    HMODULE mod;
    g_urbc_win32_dlerror[0] = '\0';
    if (!path) {
        SetLastError(ERROR_INVALID_PARAMETER);
        urbc_win32_store_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (flags & URBC_DLOPEN_NOLOAD) {
        mod = GetModuleHandleA(path);
        if (!mod) urbc_win32_store_error(GetLastError());
        return (void *)mod;
    }
    mod = LoadLibraryA(path);
    if (!mod) urbc_win32_store_error(GetLastError());
    return (void *)mod;
}

int urbc_dyn_close(void *handle)
{
    g_urbc_win32_dlerror[0] = '\0';
    if (!handle) return 0;
    if (!FreeLibrary((HMODULE)handle)) {
        urbc_win32_store_error(GetLastError());
        return -1;
    }
    return 0;
}

void *urbc_dyn_sym(void *handle, const char *name)
{
    FARPROC proc;
    g_urbc_win32_dlerror[0] = '\0';
    if (!handle || !name) {
        SetLastError(ERROR_INVALID_PARAMETER);
        urbc_win32_store_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    proc = GetProcAddress((HMODULE)handle, name);
    if (!proc) urbc_win32_store_error(GetLastError());
    return (void *)(uintptr_t)proc;
}

void *urbc_dyn_sym_self(const char *name)
{
    HANDLE snap;
    MODULEENTRY32 me;
    FARPROC proc;
    HMODULE self;
    g_urbc_win32_dlerror[0] = '\0';
    if (!name) {
        SetLastError(ERROR_INVALID_PARAMETER);
        urbc_win32_store_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    self = GetModuleHandleA(NULL);
    if (self) {
        proc = GetProcAddress(self, name);
        if (proc) return (void *)(uintptr_t)proc;
    }
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        urbc_win32_store_error(GetLastError());
        return NULL;
    }
    memset(&me, 0, sizeof(me));
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            proc = GetProcAddress(me.hModule, name);
            if (proc) {
                CloseHandle(snap);
                return (void *)(uintptr_t)proc;
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    urbc_win32_store_error(ERROR_PROC_NOT_FOUND);
    return NULL;
}

const char *urbc_dyn_last_error(void)
{
    if (!g_urbc_win32_dlerror[0]) return NULL;
    return g_urbc_win32_dlerror;
}
#else
void *urbc_dyn_open(const char *path, int flags)
{
    int native_flags = 0;
    if (flags & URBC_DLOPEN_NOW) native_flags |= RTLD_NOW;
    if (!(flags & URBC_DLOPEN_NOW)) native_flags |= RTLD_LAZY;
#ifdef RTLD_LOCAL
    if (flags & URBC_DLOPEN_LOCAL) native_flags |= RTLD_LOCAL;
#endif
#ifdef RTLD_GLOBAL
    if (flags & URBC_DLOPEN_GLOBAL) native_flags |= RTLD_GLOBAL;
#endif
#ifdef RTLD_NODELETE
    if (flags & URBC_DLOPEN_NODELETE) native_flags |= RTLD_NODELETE;
#endif
#ifdef RTLD_NOLOAD
    if (flags & URBC_DLOPEN_NOLOAD) native_flags |= RTLD_NOLOAD;
#endif
    return dlopen(path, native_flags);
}

int urbc_dyn_close(void *handle)
{
    return handle ? dlclose(handle) : 0;
}

void *urbc_dyn_sym(void *handle, const char *name)
{
    return dlsym(handle, name);
}

void *urbc_dyn_sym_self(const char *name)
{
    return dlsym(RTLD_DEFAULT, name);
}

const char *urbc_dyn_last_error(void)
{
    return dlerror();
}
#endif
