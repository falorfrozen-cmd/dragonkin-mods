#include "common.h"
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>

static std::wstring g_modDir;
static std::mutex g_logMutex;
static FILE* g_log = nullptr;

std::wstring ModDir() { return g_modDir; }

void LogInit(HMODULE self, const wchar_t* fileName) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p = path;
    g_modDir = p.substr(0, p.find_last_of(L"\\/") + 1);
    // Shared open so the log can be read while the game is running.
    g_log = _wfsopen((g_modDir + fileName).c_str(), L"w", _SH_DENYNO);
}

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// >0 while a fault is expected and handled. volatile: otherwise the compiler folds
// the ++/-- pair around the inlined copy away and the crash logger sees noise.
static thread_local volatile int t_expectFaults = 0;

static bool SafeCopy(const void* src, void* dst, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeRead(const void* src, void* dst, size_t n) {
    if (reinterpret_cast<uintptr_t>(src) < 0x10000) return false;
    ++t_expectFaults;
    bool ok = SafeCopy(src, dst, n);
    --t_expectFaults;
    return ok;
}

// ---------------------------------------------------------------------------
// Crash diagnostics: log unexpected hardware exceptions with module+offset.
// ---------------------------------------------------------------------------
static std::string ModuleOf(uint64_t addr, uint64_t* offset) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &m) || !m)
        return {};
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* name = strrchr(path, '\\');
    *offset = addr - reinterpret_cast<uint64_t>(m);
    return name ? name + 1 : path;
}

static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    bool interesting = code == EXCEPTION_ACCESS_VIOLATION || code == 0xC0000409 || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                       code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                       code == EXCEPTION_PRIV_INSTRUCTION;
    if (!interesting || t_expectFaults > 0) return EXCEPTION_CONTINUE_SEARCH;
    // Mod plugins (DLLs under the Mods folder) run inside their own fault guards and probe
    // memory on purpose; their faults are theirs to report. Skip a fault whose code — or,
    // for a fault inside a system DLL such as memcpy, whose first caller — is a plugin.
    {
        auto pathOf = [](uint64_t addr) {
            HMODULE h = nullptr;
            wchar_t p[MAX_PATH] = {};
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(addr), &h))
                GetModuleFileNameW(h, p, MAX_PATH);
            return std::wstring(p);
        };
        std::wstring modsDir = g_modDir + L"Mods\\";
        auto isPlugin = [&](const std::wstring& p) { return !p.empty() && _wcsnicmp(p.c_str(), modsDir.c_str(), modsDir.size()) == 0; };
        auto isSystem = [](const std::wstring& p) { return p.empty() || _wcsnicmp(p.c_str(), L"C:\\Windows\\", 11) == 0; };
        std::wstring at = pathOf(e->ContextRecord->Rip);
        if (isPlugin(at)) return EXCEPTION_CONTINUE_SEARCH;
        if (isSystem(at)) {
            ++t_expectFaults;
            auto* sp = reinterpret_cast<uint64_t*>(e->ContextRecord->Rsp);
            for (int i = 0; i < 64; ++i) {
                uint64_t v = 0;
                if (!SafeCopy(sp + i, &v, 8)) break;
                std::wstring p = pathOf(v);
                if (p.empty() || isSystem(p)) continue;
                if (isPlugin(p)) {
                    --t_expectFaults;
                    return EXCEPTION_CONTINUE_SEARCH;
                }
                break;
            }
            --t_expectFaults;
        }
    }
    static volatile LONG count = 0;
    if (InterlockedIncrement(&count) > 30) return EXCEPTION_CONTINUE_SEARCH;
    ++t_expectFaults;
    uint64_t rip = e->ContextRecord->Rip, off = 0;
    std::string mod = ModuleOf(rip, &off);
    uint64_t target = e->ExceptionRecord->NumberParameters >= 2 ? e->ExceptionRecord->ExceptionInformation[1] : 0;
    Log("EXCEPTION 0x%08lX at %s+0x%llX (thread %lu, data address %p)", code, mod.empty() ? "?" : mod.c_str(), off,
        GetCurrentThreadId(), (void*)target);
    auto* sp = reinterpret_cast<uint64_t*>(e->ContextRecord->Rsp);
    int shown = 0;
    for (int i = 0; i < 512 && shown < 14; ++i) {
        uint64_t v = 0;
        if (!SafeCopy(sp + i, &v, 8)) break;
        uint64_t o = 0;
        std::string m = ModuleOf(v, &o);
        if (m.empty() || o < 0x1000) continue;
        Log("    stack[%03d] %s+0x%llX", i, m.c_str(), o);
        ++shown;
    }
    --t_expectFaults;
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashLogger() { AddVectoredExceptionHandler(1, CrashLogger); }

Section GetExeSection(const char* name) {
    Section s;
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        char n[9] = {};
        memcpy(n, sec->Name, 8);
        if (strcmp(n, name) == 0) {
            s.base = base + sec->VirtualAddress;
            s.size = sec->Misc.VirtualSize;
            break;
        }
    }
    return s;
}
