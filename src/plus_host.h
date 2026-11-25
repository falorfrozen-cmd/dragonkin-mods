#pragma once
// Dragonkin Plus side of the loader's plugin API (see sdk/dml_plugin.h).
#include "../sdk/dml_plugin.h"

extern const DmlHost* g_host;

inline int HostContribute(void* owner, const char* path, int op, double value, const char* source) {
    return g_host && g_host->apiVersion >= 2 ? g_host->ContributeProperty(owner, path, op, value, source) : 0;
}
inline float HostFactor(const char* attr) {
    return g_host && g_host->apiVersion >= 2 ? g_host->GetAttributeFactor(attr) : 1.0f;
}
