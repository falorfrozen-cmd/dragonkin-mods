#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Logging (DragonkinMod.log next to the DLL)
// ---------------------------------------------------------------------------
void LogInit(HMODULE self, const wchar_t* fileName = L"DragonkinMod.log");
void Log(const char* fmt, ...);
std::wstring ModDir();  // folder that contains our DLL, with trailing backslash
void InstallCrashLogger();  // logs unexpected exceptions (module+offset) to the log

// ---------------------------------------------------------------------------
// Memory helpers. Every read of memory we have not validated goes through
// SafeRead, which turns access violations into a "false" result.
// ---------------------------------------------------------------------------
bool SafeRead(const void* src, void* dst, size_t n);

template <class T>
inline bool Read(const void* addr, T& out) { return SafeRead(addr, &out, sizeof(T)); }

template <class T>
inline T ReadOr(const void* addr, T fallback) {
    T v;
    return Read(addr, v) ? v : fallback;
}

inline bool LooksLikePtr(const void* p) {
    auto v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000 && v < 0x7FFFFFFFFFFFull && (v & 7) == 0;
}

struct Section {
    uint8_t* base = nullptr;
    size_t size = 0;
    bool Contains(const void* p) const {
        auto b = reinterpret_cast<const uint8_t*>(p);
        return b >= base && b < base + size;
    }
};

// Finds a PE section of the main executable by name (".text", ".data", ...).
Section GetExeSection(const char* name);
