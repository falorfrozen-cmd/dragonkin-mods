#include "ue.h"
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace ue {

Offsets off;

namespace {
constexpr int kChunk = 64 * 1024;  // FChunkedFixedUObjectArray::NumElementsPerChunk

uint8_t** g_chunks = nullptr;  // FUObjectItem* Objects[]
int32_t* g_num = nullptr;      // NumElements
uint8_t** g_nameBlocks = nullptr;
bool g_ready = false;

void* g_classClass = nullptr;
void* g_objectClass = nullptr;
void* g_structClass = nullptr;
void* g_functionClass = nullptr;
void* g_scriptStructClass = nullptr;

std::mutex g_cacheMutex;
std::unordered_map<std::string, void*> g_classCache;

bool LooksLikeAddr(const void* p) {
    auto v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000 && v < 0x7FFFFFFFFFFFull;
}

// ---------------------------------------------------------------------------
// FName
// ---------------------------------------------------------------------------
bool NameEntry(uint32_t index, std::string& out) {
    uint32_t block = index >> 16;
    uint32_t offset = (index & 0xFFFF) * 2;  // FNameEntryAllocator::Stride == alignof(FNameEntry) == 2
    if (!g_nameBlocks || block >= 8192) return false;
    uint8_t* b = nullptr;
    if (!Read(&g_nameBlocks[block], b) || !b) return false;
    uint16_t header = 0;
    if (!Read(b + offset, header)) return false;
    int len = header >> 6;  // bIsWide:1, LowercaseProbeHash:5, Len:10
    bool wide = header & 1;
    if (len <= 0 || len > 1024) return false;
    if (!wide) {
        char buf[1025];
        if (!SafeRead(b + offset + 2, buf, len)) return false;
        out.assign(buf, len);
    } else {
        wchar_t wbuf[1025];
        if (!SafeRead(b + offset + 2, wbuf, len * 2)) return false;
        char buf[4096];
        int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, len, buf, sizeof(buf), nullptr, nullptr);
        out.assign(buf, n > 0 ? n : 0);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Locating the global tables
// ---------------------------------------------------------------------------
bool FindNamePool() {
    Section data = GetExeSection(".data");
    if (!data.base) return false;
    for (uint8_t* p = data.base; p + 8 <= data.base + data.size; p += 8) {
        uint8_t* v = *reinterpret_cast<uint8_t**>(p);
        if (!LooksLikeAddr(v)) continue;
        uint8_t buf[20];
        if (!SafeRead(v, buf, sizeof(buf))) continue;
        uint16_t h0 = *reinterpret_cast<uint16_t*>(buf);
        uint16_t h1 = *reinterpret_cast<uint16_t*>(buf + 6);
        if ((h0 >> 6) == 4 && !(h0 & 1) && memcmp(buf + 2, "None", 4) == 0 && (h1 >> 6) == 12 &&
            memcmp(buf + 8, "ByteProperty", 12) == 0) {
            g_nameBlocks = reinterpret_cast<uint8_t**>(p);
            return true;
        }
    }
    return false;
}

bool FindObjectArray() {
    Section data = GetExeSection(".data");
    if (!data.base) return false;
    struct Raw {
        uint8_t** objects;
        uint8_t* preAllocated;
        int32_t maxElements, numElements, maxChunks, numChunks;
    };
    for (uint8_t* p = data.base; p + sizeof(Raw) <= data.base + data.size; p += 8) {
        Raw r;
        memcpy(&r, p, sizeof(r));
        if (r.maxElements < 0x10000 || r.maxElements > 0x4000000) continue;
        if (r.numElements < 0x1000 || r.numElements > r.maxElements) continue;
        if (r.maxChunks != (r.maxElements + kChunk - 1) / kChunk) continue;
        int need = (r.numElements + kChunk - 1) / kChunk;
        if (r.numChunks < need || r.numChunks > need + 1) continue;
        if (!LooksLikePtr(r.objects)) continue;
        uint8_t* chunk0 = nullptr;
        if (!Read(r.objects, chunk0) || !LooksLikePtr(chunk0)) continue;
        for (int stride : {0x18, 0x10, 0x20, 0x28}) {
            bool ok = true;
            for (int i = 1; i < 48 && ok; ++i) {
                void* o = nullptr;
                int32_t idx = -1;
                ok = Read(chunk0 + i * stride, o) && LooksLikePtr(o) && Read((uint8_t*)o + off.ObjIndex, idx) && idx == i;
            }
            if (ok) {
                off.ItemStride = stride;
                g_chunks = r.objects;
                g_num = reinterpret_cast<int32_t*>(p + offsetof(Raw, numElements));
                return true;
            }
        }
    }
    return false;
}

void* ReadPtr(const void* base, int offset) {
    void* v = nullptr;
    if (!Read((const uint8_t*)base + offset, v) || !LooksLikePtr(v)) return nullptr;
    return v;
}

// Raw scan used before offsets are known: first object with exact class and name.
void* ScanByClassAndName(void* cls, const char* name, const char* outerName = nullptr) {
    int n = NumObjects();
    for (int i = 0; i < n; ++i) {
        void* o = ObjectAt(i);
        if (!o || ClassOf(o) != cls) continue;
        if (NameOf(o) != name) continue;
        if (outerName) {
            void* outer = OuterOf(o);
            if (!outer || NameOf(outer) != outerName) continue;
        }
        return o;
    }
    return nullptr;
}

int g_fpropertySize = -1;

// Pattern used by Dumper-7: "test dword ptr [reg+FunctionFlags], imm32"
bool FindTestFlags(const uint8_t* code, int range, uint32_t imm) {
    uint8_t buf[0x1000];
    if (range > (int)sizeof(buf) || !SafeRead(code, buf, range)) return false;
    for (int j = 0; j + 11 < range; ++j) {
        if (buf[j] != 0xF7 || (buf[j + 1] & 0xC0) != 0x80 || ((buf[j + 1] >> 3) & 7) != 0) continue;
        int dispAt = ((buf[j + 1] & 7) == 4) ? j + 3 : j + 2;  // SIB byte present?
        if (dispAt + 8 > range) continue;
        if (*reinterpret_cast<int32_t*>(buf + dispAt) == off.FunctionFlags &&
            *reinterpret_cast<uint32_t*>(buf + dispAt + 4) == imm)
            return true;
    }
    return false;
}

bool FindOffsets() {
    // UClass "Class" is the only object that is its own class.
    int n = NumObjects();
    for (int i = 0; i < n && !g_classClass; ++i) {
        void* o = ObjectAt(i);
        if (o && ClassOf(o) == o && NameOf(o) == "Class") g_classClass = o;
    }
    if (!g_classClass) { Log("reflection: UClass 'Class' not found"); return false; }

    g_objectClass = ScanByClassAndName(g_classClass, "Object", "/Script/CoreUObject");
    void* actor = ScanByClassAndName(g_classClass, "Actor", "/Script/Engine");
    void* pawn = ScanByClassAndName(g_classClass, "Pawn", "/Script/Engine");
    g_structClass = ScanByClassAndName(g_classClass, "Struct", "/Script/CoreUObject");
    g_functionClass = ScanByClassAndName(g_classClass, "Function", "/Script/CoreUObject");
    g_scriptStructClass = ScanByClassAndName(g_classClass, "ScriptStruct", "/Script/CoreUObject");
    if (!g_objectClass || !actor || !pawn || !g_structClass || !g_functionClass || !g_scriptStructClass) {
        Log("reflection: core classes missing (Object=%p Actor=%p Pawn=%p Struct=%p Function=%p ScriptStruct=%p)",
            g_objectClass, actor, pawn, g_structClass, g_functionClass, g_scriptStructClass);
        return false;
    }

    // UStruct::SuperStruct
    for (int o = 0x28; o <= 0xB0 && off.SuperStruct < 0; o += 8)
        if (ReadPtr(actor, o) == g_objectClass && ReadPtr(pawn, o) == actor) off.SuperStruct = o;
    if (off.SuperStruct < 0) { Log("reflection: SuperStruct offset not found"); return false; }

    void* vec = ScanByClassAndName(g_scriptStructClass, "Vector", "/Script/CoreUObject");
    void* vec2 = ScanByClassAndName(g_scriptStructClass, "Vector2D", "/Script/CoreUObject");
    void* quat = ScanByClassAndName(g_scriptStructClass, "Quat", "/Script/CoreUObject");
    if (!vec || !vec2 || !quat) { Log("reflection: Vector/Vector2D/Quat structs not found"); return false; }

    // UStruct::ChildProperties, FField::NamePrivate, FField::Next — using Vector {X,Y,Z}.
    auto isXYZ = [](const std::string& s) { return s == "X" || s == "Y" || s == "Z"; };
    for (int co = 0x30; co <= 0xB8 && off.ChildProperties < 0; co += 8) {
        void* f = ReadPtr(vec, co);
        if (!f) continue;
        for (int no = 0x18; no <= 0x40 && off.ChildProperties < 0; no += 8) {
            std::string n0 = NameFromFName((uint8_t*)f + no);
            if (!isXYZ(n0)) continue;
            for (int nx = 0x10; nx <= 0x40; nx += 8) {
                if (nx == no) continue;
                void* g = ReadPtr(f, nx);
                if (!g) continue;
                std::string n1 = NameFromFName((uint8_t*)g + no);
                if (isXYZ(n1) && n1 != n0) {
                    off.ChildProperties = co;
                    off.FFieldName = no;
                    off.FFieldNext = nx;
                    break;
                }
            }
        }
    }
    if (off.ChildProperties < 0) { Log("reflection: ChildProperties/FField layout not found"); return false; }

    // FProperty::Offset_Internal — X=0, Y=8, Z=16 (doubles), ElementSize=8, ArrayDim=1.
    void* fields[3] = {};
    {
        void* f = ReadPtr(vec, off.ChildProperties);
        for (int i = 0; i < 3 && f; ++i, f = ReadPtr(f, off.FFieldNext)) fields[i] = f;
    }
    if (!fields[2]) { Log("reflection: Vector has fewer than 3 properties"); return false; }
    for (int k = 0x24; k <= 0x80 && off.PropOffset < 0; k += 4) {
        bool ok = true;
        for (void* f : fields) {
            std::string nm = FieldName(f);
            int expect = nm == "X" ? 0 : nm == "Y" ? 8 : 16;
            int32_t value = -1, elem = -1, dim = -1;
            ok = ok && Read((uint8_t*)f + k, value) && Read((uint8_t*)f + k - 0x10, elem) &&
                 Read((uint8_t*)f + k - 0x14, dim) && value == expect && elem == 8 && dim == 1;
        }
        if (ok) off.PropOffset = k;
    }
    if (off.PropOffset < 0) { Log("reflection: FProperty::Offset_Internal not found"); return false; }

    // sizeof(FProperty): FStructProperty::Struct is the first member after it.
    // Transform.Translation is a StructProperty whose struct is Vector.
    if (void* xf = ScanByClassAndName(g_scriptStructClass, "Transform", "/Script/CoreUObject")) {
        for (void* f = ReadPtr(xf, off.ChildProperties); f && g_fpropertySize < 0; f = ReadPtr(f, off.FFieldNext))
            for (int o = off.PropOffset + 4; o <= off.PropOffset + 0x60; o += 4)
                if (o % 8 == 0 && ReadPtr(f, o) == vec) {
                    g_fpropertySize = o;
                    break;
                }
    }

    // UStruct::PropertiesSize (followed by MinAlignment)
    for (int k = 0x40; k <= 0xC0 && off.PropertiesSize < 0; k += 4) {
        if (ReadOr<int32_t>((uint8_t*)vec + k, 0) == 24 && ReadOr<int32_t>((uint8_t*)vec + k + 4, 0) == 8 &&
            ReadOr<int32_t>((uint8_t*)vec2 + k, 0) == 16 && ReadOr<int32_t>((uint8_t*)quat + k, 0) == 32 &&
            ReadOr<int32_t>((uint8_t*)quat + k + 4, 0) == 16)
            off.PropertiesSize = k;
    }
    if (off.PropertiesSize < 0) { Log("reflection: PropertiesSize not found"); return false; }

    // UStruct::Children (UField* list of functions) — normally right before ChildProperties.
    auto childrenOk = [&](int c) {
        void* f = ReadPtr(actor, c);
        return f && ClassOf(f) == g_functionClass;
    };
    if (childrenOk(off.ChildProperties - 8)) off.Children = off.ChildProperties - 8;
    for (int c = 0x30; c <= 0xB8 && off.Children < 0; c += 8)
        if (c != off.SuperStruct && childrenOk(c)) off.Children = c;
    if (off.Children < 0) { Log("reflection: Children offset not found"); return false; }

    // UFunction::FunctionFlags is the first member after UStruct.
    int structSize = StructSize(g_structClass);
    void* destroy = FindFunction(actor, "K2_DestroyActor");
    void* beginPlay = FindFunction(actor, "ReceiveBeginPlay");
    if (!destroy || !beginPlay) { Log("reflection: Actor functions not found"); return false; }
    for (int k = structSize; k <= structSize + 0x20 && off.FunctionFlags < 0; k += 4) {
        uint32_t fd = ReadOr<uint32_t>((uint8_t*)destroy + k, 0), fb = ReadOr<uint32_t>((uint8_t*)beginPlay + k, 0);
        if ((fd & FUNC_Native) && !(fb & FUNC_Native) && (fb & 0x800 /*FUNC_Event*/)) off.FunctionFlags = k;
    }
    if (off.FunctionFlags < 0) { Log("reflection: FunctionFlags not found (sizeof UStruct=0x%X)", structSize); return false; }

    // UObject::ProcessEvent vtable index.
    void* cdo = ScanByClassAndName(g_objectClass, "Default__Object");
    void** vtable = cdo ? *reinterpret_cast<void***>(cdo) : nullptr;
    Section text = GetExeSection(".text");
    for (int i = 0; vtable && i < 0x180 && off.ProcessEventIndex < 0; ++i) {
        void* fn = nullptr;
        if (!Read(&vtable[i], fn) || !text.Contains(fn)) continue;
        if (FindTestFlags((const uint8_t*)fn, 0x400, 0x400) && FindTestFlags((const uint8_t*)fn, 0xF00, 0x400000))
            off.ProcessEventIndex = i;
    }
    if (off.ProcessEventIndex < 0) Log("reflection: ProcessEvent not found — function calls disabled");
    return true;
}
}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool Ready() { return g_ready; }
int32_t NumObjects() { return g_num ? *g_num : 0; }

void* ObjectAt(int32_t i) {
    if (!g_chunks || i < 0 || i >= NumObjects()) return nullptr;
    uint8_t* chunk = g_chunks[i / kChunk];
    if (!chunk) return nullptr;
    return *reinterpret_cast<void**>(chunk + (i % kChunk) * off.ItemStride);
}

bool IsLive(void* obj, int32_t index) { return obj && ObjectAt(index) == obj; }

std::string NameFromFName(const void* addr) {
    uint32_t parts[2];
    if (!SafeRead(addr, parts, sizeof(parts))) return {};
    std::string s;
    if (!NameEntry(parts[0], s)) return {};
    if (parts[1] > 0) s += "_" + std::to_string(parts[1] - 1);
    return s;
}

std::string NameOf(void* obj) { return obj ? NameFromFName((uint8_t*)obj + off.ObjName) : std::string("null"); }

std::string PathOf(void* obj) {
    std::string path;
    for (int depth = 0; obj && depth < 16; ++depth, obj = OuterOf(obj)) path = NameOf(obj) + (path.empty() ? "" : "." + path);
    return path;
}

bool IsChildOf(void* cls, void* base) {
    for (int depth = 0; cls && depth < 64; ++depth, cls = SuperOf(cls))
        if (cls == base) return true;
    return false;
}

bool IsA(void* obj, void* cls) { return obj && cls && IsChildOf(ClassOf(obj), cls); }

void* FindClass(const char* name) {
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        auto it = g_classCache.find(name);
        if (it != g_classCache.end()) return it->second;
    }
    int n = NumObjects();
    for (int i = 0; i < n; ++i) {
        void* o = ObjectAt(i);
        if (!o || !IsChildOf(ClassOf(o), g_classClass) || NameOf(o) != name) continue;
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_classCache[name] = o;
        return o;
    }
    return nullptr;
}

void* FindObject(const char* name, void* ofClass) {
    int n = NumObjects();
    for (int i = 0; i < n; ++i) {
        void* o = ObjectAt(i);
        if (!o || (ofClass && !IsA(o, ofClass))) continue;
        if (NameOf(o) == name) return o;
    }
    return nullptr;
}

void* FindFirstInstance(void* cls) {
    int n = NumObjects();
    for (int i = 0; i < n; ++i) {
        void* o = ObjectAt(i);
        if (o && !IsCDO(o) && IsA(o, cls)) return o;
    }
    return nullptr;
}

PropInfo DescribeProperty(void* field) {
    PropInfo p;
    p.field = field;
    p.offset = ReadOr<int32_t>((uint8_t*)field + off.PropOffset, -1);
    p.elementSize = ReadOr<int32_t>((uint8_t*)field + off.PropOffset - 0x10, 0);
    if (void* fc = ReadPtr(field, off.FFieldClass)) p.type = NameFromFName(fc);  // FFieldClass::Name at +0
    return p;
}

void ForEachProperty(void* strct, void (*cb)(void*, void*), void* user) {
    int guard = 0;
    for (void* f = ReadPtr(strct, off.ChildProperties); f && guard < 4096; f = ReadPtr(f, off.FFieldNext), ++guard) cb(f, user);
}

PropInfo FindProperty(void* strct, const char* name) {
    for (int depth = 0; strct && depth < 64; ++depth, strct = SuperOf(strct)) {
        int guard = 0;
        for (void* f = ReadPtr(strct, off.ChildProperties); f && guard < 4096; f = ReadPtr(f, off.FFieldNext), ++guard)
            if (FieldName(f) == name) return DescribeProperty(f);
    }
    return {};
}

void* FindFunction(void* cls, const char* name) {
    for (int depth = 0; cls && depth < 64; ++depth, cls = SuperOf(cls)) {
        int guard = 0;
        for (void* f = ReadPtr(cls, off.Children); f && guard < 8192; f = ReadPtr(f, off.FieldNext), ++guard)
            if (ClassOf(f) == g_functionClass && NameOf(f) == name) return f;
    }
    return nullptr;
}

static bool CallProcessEvent(void* fn, void* obj, void* func, void* params) {
    using PE = void(__fastcall*)(void*, void*, void*);
    __try {
        reinterpret_cast<PE>(fn)(obj, func, params);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallFunction(void* obj, void* func, void* params) {
    if (!obj || !func || off.ProcessEventIndex < 0) return false;
    void* fn = (*reinterpret_cast<void***>(obj))[off.ProcessEventIndex];
    if (!CallProcessEvent(fn, obj, func, params)) {
        Log("ProcessEvent crashed calling %s on %s", NameOf(func).c_str(), NameOf(obj).c_str());
        return false;
    }
    return true;
}

// Init runs on our worker thread while the game mutates objects; a stale pointer
// must never take the game down, so the whole search is wrapped in SEH.
static int SafeFindOffsets() {
    __try {
        return FindOffsets() ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int ForEachSparseElement(const void* setAddr, int stride, void (*cb)(uint8_t*, void*), void* user) {
    struct Raw {
        uint8_t* data;
        int32_t num, max;
        uint32_t inlineBits[4];
        uint32_t* secondaryBits;
        int32_t numBits, maxBits;
        int32_t firstFree, numFree;
    } r{};
    if (!Read(setAddr, r) || r.num <= 0 || r.num > 100000 || !LooksLikePtr(r.data)) return 0;
    const uint32_t* bits = r.secondaryBits ? r.secondaryBits : r.inlineBits;
    int count = 0;
    for (int i = 0; i < r.num; ++i) {
        uint32_t word = 0;
        if (r.numFree > 0 && !Read(&bits[i / 32], word)) break;
        if (r.numFree > 0 && !(word & (1u << (i % 32)))) continue;  // free slot
        cb(r.data + (size_t)i * stride, user);
        ++count;
    }
    return count;
}

bool CallByName(void* obj, const char* fnName, const std::vector<std::pair<const char*, const void*>>& args,
                std::vector<uint8_t>* outParams) {
    if (!obj) return false;
    void* fn = FindFunction(ClassOf(obj), fnName);
    if (!fn) return false;
    std::vector<uint8_t> buf(StructSize(fn) + 64, 0);
    for (auto& a : args) {
        auto p = FindProperty(fn, a.first);
        if (!p) return false;
        memcpy(buf.data() + p.offset, a.second, p.elementSize);
    }
    bool ok = CallFunction(obj, fn, buf.data());
    if (outParams) *outParams = std::move(buf);
    return ok;
}

bool Init() {
    if (g_ready) return true;
    if (!g_nameBlocks && !FindNamePool()) return false;
    if (!g_chunks && !FindObjectArray()) return false;
    if (NumObjects() < 50000) return false;  // engine still booting
    Log("reflection: FNamePool blocks @ %p, GUObjectArray chunks @ %p (%d objects, item stride 0x%X)", g_nameBlocks,
        g_chunks, NumObjects(), off.ItemStride);
    int found = SafeFindOffsets();
    if (found < 0) Log("reflection: access violation during offset search, retrying");
    if (found != 1) {
        // reset partial results so the next attempt starts clean
        Offsets fresh;
        fresh.ItemStride = off.ItemStride;
        off = fresh;
        g_classClass = nullptr;
        return false;
    }
    Log("reflection: SuperStruct=0x%X Children=0x%X ChildProperties=0x%X PropertiesSize=0x%X", off.SuperStruct,
        off.Children, off.ChildProperties, off.PropertiesSize);
    Log("reflection: FField Name=0x%X Next=0x%X, FProperty Offset=0x%X size=0x%X, FunctionFlags=0x%X, ProcessEvent=vtable[%d]",
        off.FFieldName, off.FFieldNext, off.PropOffset, g_fpropertySize, off.FunctionFlags, off.ProcessEventIndex);
    g_ready = true;
    return true;
}

// ---------------------------------------------------------------------------
// Dump
// ---------------------------------------------------------------------------
namespace {
struct DumpCtx {
    FILE* f;
};
// Describes what a container/struct/object property points at, by probing the
// pointer slots that follow the FProperty base (struct, class, inner/key/value props).
// IsChildOf for pointers that may be garbage (every step is a checked read).
bool SafeIsChildOf(void* cls, void* base) {
    for (int depth = 0; cls && depth < 64; ++depth) {
        if (cls == base) return true;
        cls = ReadPtr(cls, off.SuperStruct);
    }
    return false;
}

std::string InnerTypes(void* field, int depth = 0) {
    std::string out;
    for (int o = off.PropOffset + 4; o <= off.PropOffset + 0x54; o += 4) {
        if (o % 8) continue;
        void* p = ReadPtr(field, o);
        if (!p) continue;
        void* cls = nullptr;
        if (Read((uint8_t*)p + off.ObjClass, cls) && LooksLikePtr(cls) &&
            (cls == g_scriptStructClass || SafeIsChildOf(cls, g_classClass))) {
            out += " " + NameOf(p);
            continue;
        }
        void* fc = ReadPtr(p, off.FFieldClass);
        if (!fc) continue;
        std::string t = NameFromFName(fc);
        if (t.size() > 8 && t.compare(t.size() - 8, 8, "Property") == 0 && depth < 2)
            out += " <" + t + InnerTypes(p, depth + 1) + ">";
    }
    return out;
}

void DumpProp(void* field, void* user) {
    auto* ctx = static_cast<DumpCtx*>(user);
    PropInfo p = DescribeProperty(field);
    fprintf(ctx->f, "    +0x%04X  %-22s %s (size 0x%X)%s\n", p.offset, p.type.c_str(), FieldName(field).c_str(),
            p.elementSize, InnerTypes(field).c_str());
}
std::string OutermostName(void* o) {
    void* last = o;
    for (int d = 0; o && d < 16; ++d) { last = o; o = OuterOf(o); }
    return NameOf(last);
}
}  // namespace

std::string FieldName(void* field) { return NameFromFName((uint8_t*)field + off.FFieldName); }
uint64_t PropertyFlags(void* field) { return ReadOr<uint64_t>((uint8_t*)field + off.PropOffset - 0xC, 0); }
int FPropertySize() { return g_fpropertySize; }
void* PropertySlot(void* field, int index) {
    return g_fpropertySize > 0 ? ReadPtr(field, g_fpropertySize + 8 * index) : nullptr;
}

bool FNameFromString(const std::string& s, uint32_t& comparisonIndex) {
    static std::vector<uint8_t> buf(0x20000);
    for (uint32_t block = 0; block < 8192; ++block) {
        uint8_t* b = nullptr;
        if (!Read(&g_nameBlocks[block], b) || !b) break;
        if (!SafeRead(b, buf.data(), buf.size())) {  // last block may be shorter
            if (!SafeRead(b, buf.data(), 0x10000)) break;
            memset(buf.data() + 0x10000, 0, 0x10000);
        }
        for (uint32_t o = 0; o + 2 < buf.size();) {
            uint16_t h = *reinterpret_cast<uint16_t*>(&buf[o]);
            int len = h >> 6;
            bool wide = h & 1;
            if (len == 0) break;
            if (!wide && len == (int)s.size() && o + 2 + len <= buf.size() &&
                _strnicmp(reinterpret_cast<char*>(&buf[o + 2]), s.c_str(), len) == 0) {
                comparisonIndex = (block << 16) | (o / 2);
                return true;
            }
            o += 2 + len * (wide ? 2 : 1);
            o = (o + 1) & ~1u;
        }
    }
    return false;
}

std::vector<std::string> FindNames(const std::string& sub, int max) {
    std::vector<std::string> out;
    std::vector<uint8_t> buf(0x20000);
    std::string lsub = sub;
    for (auto& ch : lsub) ch = (char)tolower((unsigned char)ch);
    for (uint32_t block = 0; block < 8192 && (int)out.size() < max; ++block) {
        uint8_t* b = nullptr;
        if (!Read(&g_nameBlocks[block], b) || !b) break;
        if (!SafeRead(b, buf.data(), buf.size())) {
            if (!SafeRead(b, buf.data(), 0x10000)) break;
            memset(buf.data() + 0x10000, 0, 0x10000);
        }
        for (uint32_t o = 0; o + 2 < buf.size() && (int)out.size() < max;) {
            uint16_t h = *reinterpret_cast<uint16_t*>(&buf[o]);
            int len = h >> 6;
            bool wide = h & 1;
            if (len == 0) break;
            if (!wide && o + 2 + len <= buf.size()) {
                std::string s(reinterpret_cast<char*>(&buf[o + 2]), len), l = s;
                for (auto& ch : l) ch = (char)tolower((unsigned char)ch);
                if (l.find(lsub) != std::string::npos) out.push_back(s);
            }
            o += 2 + len * (wide ? 2 : 1);
            o = (o + 1) & ~1u;
        }
    }
    return out;
}

std::string EnumValueName(void* uenum, int64_t value) {
    // UEnum::Names is a TArray<TPair<FName, int64>> right after UField + FString CppType.
    for (int o : {0x40, 0x38, 0x48, 0x50}) {
        TArrayRaw arr{};
        if (!Read((uint8_t*)uenum + o, arr) || arr.num <= 0 || arr.num > 4096 || !LooksLikePtr(arr.data)) continue;
        std::string first = NameFromFName(arr.data);
        if (first.empty()) continue;
        for (int i = 0; i < arr.num; ++i) {
            uint8_t* e = (uint8_t*)arr.data + i * 16;
            if (ReadOr<int64_t>(e + 8, INT64_MIN) != value) continue;
            std::string n = NameFromFName(e);
            auto p = n.rfind("::");
            return p == std::string::npos ? n : n.substr(p + 2);
        }
        return "";
    }
    return "";
}

void* PropertyStruct(void* field) {
    for (int o = off.PropOffset + 8; o <= off.PropOffset + 0x54; o += 4) {
        if (o % 8) continue;
        void* p = ReadPtr(field, o);
        void* cls = nullptr;
        if (p && Read((uint8_t*)p + off.ObjClass, cls) && cls == g_scriptStructClass) return p;
    }
    return nullptr;
}

void DumpPackage(const char* prefix, const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") || !f) return;
    DumpCtx ctx{f};
    size_t plen = strlen(prefix);
    int n = NumObjects(), count = 0;
    for (int i = 0; i < n; ++i) {
        void* o = ObjectAt(i);
        if (!o) continue;
        void* cls = ClassOf(o);
        bool isClass = IsChildOf(cls, g_classClass);
        bool isStruct = cls == g_scriptStructClass;
        if (!isClass && !isStruct) continue;
        if (OutermostName(o).compare(0, plen, prefix) != 0) continue;
        void* super = SuperOf(o);
        fprintf(f, "%s %s : %s  (size 0x%X)\n", isClass ? "class" : "struct", NameOf(o).c_str(),
                super ? NameOf(super).c_str() : "-", StructSize(o));
        ForEachProperty(o, DumpProp, &ctx);
        if (isClass) {
            int guard = 0;
            for (void* fn = ReadPtr(o, off.Children); fn && guard < 8192; fn = ReadPtr(fn, off.FieldNext), ++guard)
                if (ClassOf(fn) == g_functionClass)
                    fprintf(f, "    fn %s  (flags 0x%08X)\n", NameOf(fn).c_str(),
                            ReadOr<uint32_t>((uint8_t*)fn + off.FunctionFlags, 0));
        }
        fputc('\n', f);
        ++count;
    }
    fclose(f);
    Log("dump: wrote %d types of %s", count, prefix);
}

}  // namespace ue
