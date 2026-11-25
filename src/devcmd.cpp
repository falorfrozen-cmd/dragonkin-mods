#include "devcmd.h"
#include <share.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <sstream>
#include <vector>
#include "common.h"
#include "overlay.h"
#include "i18n.h"
#include "modloader.h"
#include "core.h"
#include "ue.h"

namespace dev {
namespace {

std::mutex g_mutex;
std::deque<std::string> g_queue;
DWORD g_resumeAt = 0;
std::string g_out;

void Out(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_out += buf;
    g_out += "\n";
}

void Flush() {
    if (g_out.empty()) return;
    FILE* f = _wfsopen((ModDir() + L"DragonkinMod_out.txt").c_str(), L"a", _SH_DENYNO);
    if (f) {
        fputs(g_out.c_str(), f);
        fclose(f);
    }
    g_out.clear();
}

std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> v;
    std::istringstream in(s);
    std::string t;
    while (in >> t) v.push_back(t);
    return v;
}

// ---------------------------------------------------------------------------
// Targets
// ---------------------------------------------------------------------------
void* GetObjProp(void* o, const char* name) {
    if (!o) return nullptr;
    auto p = ue::FindProperty(ue::ClassOf(o), name);
    void* v = nullptr;
    if (!p || !Read((uint8_t*)o + p.offset, v) || !LooksLikePtr(v)) return nullptr;
    return v;
}

bool ActorLocation(void* actor, double xyz[3]);

void* ResolveBase(const std::string& t);

// "base.prop.prop" follows object properties (e.g. ps.m_PlayerInventory).
void* Resolve(const std::string& t) {
    auto dot = t.find('.');
    if (dot == std::string::npos || t.find(':') != std::string::npos && t.find(':') > dot) return ResolveBase(t);
    void* o = ResolveBase(t.substr(0, dot));
    std::string rest = t.substr(dot + 1);
    while (o && !rest.empty()) {
        auto d = rest.find('.');
        o = GetObjProp(o, rest.substr(0, d).c_str());
        rest = d == std::string::npos ? "" : rest.substr(d + 1);
    }
    return o;
}

void* ResolveBase(const std::string& t) {
    core::Context c = core::GetContext();
    if (t == "null" || t == "none") return nullptr;
    if (t == "pawn") return c.pawn;
    if (t == "pc") return c.pc;
    if (t == "ps") return c.ps;
    if (t == "world") return c.world;
    if (t == "engine") return c.engine;
    if (t == "attrset") return c.attrSet;
    if (t == "gi") return GetObjProp(c.world, "OwningGameInstance");
    auto colon = t.find(':');
    if (colon == std::string::npos) return nullptr;
    std::string kind = t.substr(0, colon), arg = t.substr(colon + 1);
    if (kind == "cdo") return ue::FindObject(("Default__" + arg).c_str());
    if (kind == "idx") return ue::ObjectAt(atoi(arg.c_str()));
    if (kind == "first" || kind == "near") {
        void* cls = ue::FindClass(arg.c_str());
        if (!cls) return nullptr;
        if (kind == "first") return ue::FindFirstInstance(cls);
        double me[3];
        if (!ActorLocation(c.pawn, me)) return ue::FindFirstInstance(cls);
        void* best = nullptr;
        double bestD = 1e300;
        int n = ue::NumObjects();
        for (int i = 0; i < n; ++i) {
            void* o = ue::ObjectAt(i);
            if (!o || ue::IsCDO(o) || !ue::IsA(o, cls)) continue;
            double p[3];
            void* actor = o;
            if (!ActorLocation(actor, p)) {  // components: use their owner actor
                actor = ue::OuterOf(o);
                if (!ActorLocation(actor, p)) continue;
            }
            double d = (p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]) + (p[2] - me[2]) * (p[2] - me[2]);
            if (d < bestD) bestD = d, best = o;
        }
        return best;
    }
    return nullptr;
}

std::string Describe(void* o) {
    if (!o) return "null";
    char buf[64];
    snprintf(buf, sizeof(buf), " [idx %d]", ue::IndexOf(o));
    return ue::NameOf(ue::ClassOf(o)) + " " + ue::PathOf(o) + buf;
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------
bool IsInt(const std::string& t) {
    return t == "IntProperty" || t == "Int8Property" || t == "Int16Property" || t == "Int64Property" || t == "ByteProperty" ||
           t == "UInt16Property" || t == "UInt32Property" || t == "UInt64Property" || t == "EnumProperty";
}

std::string Format(void* field, uint8_t* p, int depth = 0) {
    auto info = ue::DescribeProperty(field);
    const std::string& t = info.type;
    char buf[256];
    if (IsInt(t)) {
        int64_t v = 0;
        SafeRead(p, &v, info.elementSize > 8 ? 8 : info.elementSize);
        if (t == "IntProperty" && info.elementSize == 4) v = (int32_t)v;
        if (t == "Int8Property") v = (int8_t)v;
        snprintf(buf, sizeof(buf), "%lld", (long long)v);
        // Enum name: FEnumProperty keeps UEnum* in its 2nd slot, FByteProperty in its 1st.
        void* uenum = t == "EnumProperty" ? ue::PropertySlot(field, 1) : (t == "ByteProperty" ? ue::PropertySlot(field, 0) : nullptr);
        if (uenum) {
            std::string n = ue::EnumValueName(uenum, v);
            if (!n.empty()) return n + "(" + buf + ")";
        }
        return buf;
    }
    if (t == "FloatProperty") return snprintf(buf, sizeof(buf), "%g", ReadOr<float>(p, NAN)), buf;
    if (t == "DoubleProperty") return snprintf(buf, sizeof(buf), "%g", ReadOr<double>(p, NAN)), buf;
    if (t == "BoolProperty") {
        int ps = ue::FPropertySize();
        uint8_t byteOffset = ps > 0 ? ReadOr<uint8_t>((uint8_t*)field + ps + 1, 0) : 0;
        uint8_t mask = ps > 0 ? ReadOr<uint8_t>((uint8_t*)field + ps + 3, 0xFF) : 0xFF;
        return (ReadOr<uint8_t>(p + byteOffset, 0) & mask) ? "true" : "false";
    }
    if (t == "NameProperty") return ue::NameFromFName(p);
    if (t == "StrProperty") {
        ue::TArrayRaw s{};
        if (!Read(p, s) || !s.data || s.num <= 0 || s.num > 4096) return "\"\"";
        std::wstring w((wchar_t*)s.data, s.num - 1);
        char u[8192];
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), u, sizeof(u), nullptr, nullptr);
        return "\"" + std::string(u, n > 0 ? n : 0) + "\"";
    }
    if (t == "ObjectProperty" || t == "ClassProperty" || t == "InterfaceProperty") {
        void* o = ReadOr<void*>(p, nullptr);
        return o ? ue::NameOf(o) : "null";
    }
    if (t == "StructProperty") {
        void* s = ue::PropertyStruct(field);
        std::string sn = s ? ue::NameOf(s) : "?";
        if (sn == "Vector" || sn == "Rotator")
            return snprintf(buf, sizeof(buf), "(%.1f, %.1f, %.1f)", ReadOr<double>(p, 0), ReadOr<double>(p + 8, 0),
                            ReadOr<double>(p + 16, 0)),
                   buf;
        if (sn == "GameplayTag") return ue::NameFromFName(p);
        if (!s || depth >= 2) return sn + "{...}";
        std::string r = sn + "{";
        int count = 0;
        for (void* c = s; c; c = ue::SuperOf(c)) {
            struct Ctx {
                std::string* r;
                uint8_t* p;
                int depth;
                int* count;
            } ctx{&r, p, depth, &count};
            ue::ForEachProperty(
                c,
                [](void* f, void* u) {
                    auto* x = static_cast<Ctx*>(u);
                    if (++*x->count > 14) return;
                    auto fi = ue::DescribeProperty(f);
                    *x->r += ue::FieldName(f) + "=" + Format(f, x->p + fi.offset, x->depth + 1) + " ";
                },
                &ctx);
        }
        return r + "}";
    }
    if (t == "ArrayProperty") {
        ue::TArrayRaw a{};
        if (!Read(p, a)) return "[?]";
        void* inner = ue::ArrayInner(field);
        std::string r = "[" + std::to_string(a.num) + "]";
        if (!inner || depth >= 2 || a.num <= 0 || !a.data) return r;
        int esz = ue::DescribeProperty(inner).elementSize;
        for (int i = 0; i < a.num && i < 20; ++i) r += (i ? ", " : " ") + Format(inner, (uint8_t*)a.data + i * esz, depth + 1);
        if (a.num > 20) r += ", ...";
        return r;
    }
    if (t == "MapProperty" || t == "SetProperty") {
        int32_t num = ReadOr<int32_t>(p + 8, 0), freeN = ReadOr<int32_t>(p + 0x34, 0);
        std::string r = "(" + std::to_string(num - freeN) + " entries)";
        void* key = ue::PropertySlot(field, 0);
        void* val = t == "MapProperty" ? ue::PropertySlot(field, 1) : nullptr;
        if (!key || depth >= 2) return r;
        int ks = ue::DescribeProperty(key).elementSize, vs = val ? ue::DescribeProperty(val).elementSize : 0;
        int valAlign = vs >= 8 ? 8 : (vs >= 4 ? 4 : 1);
        int valOff = val ? (ks + valAlign - 1) / valAlign * valAlign : 0;
        struct Ctx {
            void *key, *val;
            int valOff, depth, shown;
            std::string* r;
        } ctx{key, val, valOff, depth, 0, &r};
        ue::ForEachSparseElement(
            p, ue::SetElementStride(val ? valOff + vs : ks),
            [](uint8_t* e, void* u) {
                auto* x = static_cast<Ctx*>(u);
                if (++x->shown > 40) return;
                *x->r += " " + Format(x->key, e, x->depth + 1);
                if (x->val) *x->r += "=" + Format(x->val, e + x->valOff, x->depth + 1);
            },
            &ctx);
        return r;
    }
    return "<" + t + ">";
}

std::vector<std::wstring*> g_strings;  // FString arguments stay alive forever (tiny leak by design)

bool Parse(void* field, uint8_t* p, const std::string& v) {
    auto info = ue::DescribeProperty(field);
    const std::string& t = info.type;
    if (IsInt(t)) {
        int64_t x = _strtoi64(v.c_str(), nullptr, 0);
        memcpy(p, &x, info.elementSize > 8 ? 8 : info.elementSize);
        return true;
    }
    if (t == "FloatProperty") return *(float*)p = (float)atof(v.c_str()), true;
    if (t == "DoubleProperty") return *(double*)p = atof(v.c_str()), true;
    if (t == "BoolProperty") {
        bool b = v == "1" || v == "true";
        int ps = ue::FPropertySize();
        uint8_t byteOffset = ps > 0 ? ReadOr<uint8_t>((uint8_t*)field + ps + 1, 0) : 0;
        uint8_t mask = ps > 0 ? ReadOr<uint8_t>((uint8_t*)field + ps + 3, 0xFF) : 0xFF;
        uint8_t& byte = p[byteOffset];
        byte = b ? (byte | mask) : (byte & ~mask);
        return true;
    }
    if (t == "NameProperty") {
        uint32_t idx;
        if (!ue::FNameFromString(v, idx)) return false;
        ((uint32_t*)p)[0] = idx;
        ((uint32_t*)p)[1] = 0;
        return true;
    }
    if (t == "StrProperty") {
        auto* w = new std::wstring(v.begin(), v.end());
        g_strings.push_back(w);
        *(ue::FStringView*)p = {w->c_str(), (int32_t)w->size() + 1, (int32_t)w->size() + 1};
        return true;
    }
    if (t == "ObjectProperty" || t == "ClassProperty") {
        void* o = Resolve(v);
        if (!o && v.rfind("class:", 0) == 0) o = ue::FindClass(v.substr(6).c_str());
        *(void**)p = o;
        return o || v == "null";
    }
    if (t == "StructProperty") {
        void* s = ue::PropertyStruct(field);
        std::string sn = s ? ue::NameOf(s) : "";
        if (sn == "Vector" || sn == "Rotator") {
            double d[3] = {};
            if (sscanf_s(v.c_str(), "%lf,%lf,%lf", &d[0], &d[1], &d[2]) != 3) return false;
            memcpy(p, d, sizeof(d));
            return true;
        }
        if (sn == "GameplayTag") {
            uint32_t idx;
            if (!ue::FNameFromString(v, idx)) return false;
            ((uint32_t*)p)[0] = idx;
            ((uint32_t*)p)[1] = 0;
            return true;
        }
        // Wrapper structs whose first member is a tag/name (e.g. HnSCountableID{m_Id}).
        if (s) {
            void* first = nullptr;
            ue::ForEachProperty(
                s, [](void* f, void* u) { if (!*static_cast<void**>(u)) *static_cast<void**>(u) = f; }, &first);
            if (first && ue::DescribeProperty(first).offset == 0) return Parse(first, p, v);
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Function calls
// ---------------------------------------------------------------------------
struct Param {
    void* field;
    std::string name;
    ue::PropInfo info;
    uint64_t flags;
};

std::vector<Param> Params(void* fn) {
    std::vector<Param> v;
    ue::ForEachProperty(
        fn,
        [](void* f, void* u) {
            auto* list = static_cast<std::vector<Param>*>(u);
            list->push_back({f, ue::FieldName(f), ue::DescribeProperty(f), ue::PropertyFlags(f)});
        },
        &v);
    return v;
}

bool Call(void* obj, const std::string& fnName, const std::vector<std::string>& args, bool quiet = false,
          std::string* result = nullptr) {
    void* fn = ue::FindFunction(ue::ClassOf(obj), fnName.c_str());
    if (!fn) {
        Out("  ! %s has no function %s", ue::NameOf(ue::ClassOf(obj)).c_str(), fnName.c_str());
        return false;
    }
    std::vector<uint8_t> buf(ue::StructSize(fn) + 64, 0);
    auto params = Params(fn);
    for (auto& a : args) {
        auto eq = a.find('=');
        if (eq == std::string::npos) continue;
        std::string k = a.substr(0, eq), v = a.substr(eq + 1);
        bool found = false;
        for (auto& p : params)
            if (_stricmp(p.name.c_str(), k.c_str()) == 0) {
                found = true;
                if (!Parse(p.field, buf.data() + p.info.offset, v)) Out("  ! cannot parse %s=%s (%s)", k.c_str(), v.c_str(), p.info.type.c_str());
            }
        if (!found) Out("  ! %s has no parameter %s", fnName.c_str(), k.c_str());
    }
    core::Context c = core::GetContext();
    for (auto& p : params)  // fill world context automatically
        if ((p.name == "WorldContextObject" || p.name == "WorldContext") && !*(void**)(buf.data() + p.info.offset))
            *(void**)(buf.data() + p.info.offset) = c.world;
    if (!ue::CallFunction(obj, fn, buf.data())) {
        Out("  ! call crashed");
        return false;
    }
    std::string r;
    for (auto& p : params)
        if (p.flags & (ue::CPF_OutParm | ue::CPF_ReturnParm)) r += " " + p.name + "=" + Format(p.field, buf.data() + p.info.offset);
    if (result) *result = r;
    if (!quiet) Out("  ok%s", r.c_str());
    return true;
}

bool ActorLocation(void* actor, double xyz[3]) {
    if (!actor) return false;
    void* fn = ue::FindFunction(ue::ClassOf(actor), "K2_GetActorLocation");
    if (!fn) return false;
    std::vector<uint8_t> buf(ue::StructSize(fn) + 16, 0);
    if (!ue::CallFunction(actor, fn, buf.data())) return false;
    memcpy(xyz, buf.data(), 24);  // ReturnValue (FVector) is the only parameter
    return true;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
void Run(const std::string& line) {
    auto a = Split(line);
    if (a.empty() || a[0][0] == '#') return;
    Out("> %s", line.c_str());
    const std::string& cmd = a[0];
    auto rest = [&](size_t from) {
        size_t pos = 0;
        for (size_t i = 0; i < from && pos != std::string::npos; ++i) {
            pos = line.find_first_not_of(" \t", pos);
            pos = line.find_first_of(" \t", pos);
        }
        return pos == std::string::npos ? std::string() : line.substr(line.find_first_not_of(" \t", pos));
    };

    if (cmd == "echo") return Out("%s", rest(1).c_str());
    static const char* known[] = {"wait", "names", "mods", "modreload", "modon", "modtrust", "modset", "modpress", "lang",
                                  "panelinfo", "paneloffset", "snapshot", "exec", "attrs", "setattr", "pos", "tp", "find",
                                  "sig", "callall", "call", "get", "set", "props"};
    bool isLoader = false;
    for (const char* k : known) isLoader |= cmd == k;
    if (!isLoader) {
        std::string reply;
        if (modloader::PluginCommand(line, reply)) {
            while (!reply.empty() && reply.back() == '\n') reply.pop_back();
            return Out("%s", reply.c_str());
        }
    }
    if (cmd == "wait" && a.size() > 1) return void(g_resumeAt = GetTickCount() + atoi(a[1].c_str()));
    if (cmd == "names" && a.size() > 1) {
        for (auto& n : ue::FindNames(a[1], a.size() > 2 ? atoi(a[2].c_str()) : 40)) Out("  %s", n.c_str());
        return;
    }
    if (cmd == "exec") return Out(core::ExecConsoleCommand(rest(1)) ? "  ok" : "  ! failed");
    if (cmd == "attrs") {
        core::Context c = core::GetContext();
        if (!c.attrSet) return Out("  ! no attribute set");
        std::string filter = a.size() > 1 ? a[1] : "";
        ue::ForEachProperty(
            ue::ClassOf(c.attrSet),
            [](void* f, void* u) {
                std::string n = ue::FieldName(f);
                auto* flt = static_cast<std::string*>(u);
                if (!flt->empty() && _strnicmp(n.c_str(), flt->c_str(), flt->size()) != 0 && n.find(*flt) == std::string::npos) return;
                float b, cur;
                if (core::GetAttr(n.c_str(), b, cur)) Out("  %-40s base=%-10g cur=%g", n.c_str(), b, cur);
            },
            &filter);
        return;
    }
    if (cmd == "setattr" && a.size() > 2) {
        float v = (float)atof(a[2].c_str());
        return Out(core::SetAttr(a[1].c_str(), v, v) ? "  ok" : "  ! unknown attribute");
    }
    if (cmd == "pos") {
        double p[3];
        if (!ActorLocation(core::GetContext().pawn, p)) return Out("  ! no pawn");
        return Out("  %.1f %.1f %.1f", p[0], p[1], p[2]);
    }
    if (cmd == "tp" && a.size() > 3) {
        core::Context c = core::GetContext();
        if (!c.pawn) return Out("  ! no pawn");
        std::string v = "NewLocation=" + a[1] + "," + a[2] + "," + a[3];
        Call(c.pawn, "K2_TeleportTo", {"DestLocation=" + a[1] + "," + a[2] + "," + a[3], "DestRotation=0,0,0"});
        return;
    }
    if (cmd == "find" && a.size() > 1) {
        void* cls = ue::FindClass(a[1].c_str());
        if (!cls) return Out("  ! class not found");
        int max = a.size() > 2 ? atoi(a[2].c_str()) : 30, shown = 0, total = 0, n = ue::NumObjects();
        for (int i = 0; i < n; ++i) {
            void* o = ue::ObjectAt(i);
            if (!o || ue::IsCDO(o) || !ue::IsA(o, cls)) continue;
            if (++total <= max) {
                double p[3];
                void* actor = ActorLocation(o, p) ? o : (ActorLocation(ue::OuterOf(o), p) ? ue::OuterOf(o) : nullptr);
                if (actor)
                    Out("  %s @ %.0f %.0f %.0f", Describe(o).c_str(), p[0], p[1], p[2]);
                else
                    Out("  %s", Describe(o).c_str());
            }
        }
        return Out("  (%d instances)", total);
    }
    if (cmd == "mods") return Out("  %s", modloader::Report().c_str());
    if (cmd == "modreload") return modloader::RequestReload(), Out("  reload requested");
    if (cmd == "modon" && a.size() > 2) return Out(modloader::SetModEnabled(a[1], a[2] != "0") ? "  ok" : "  ! no such mod");
    if (cmd == "modtrust" && a.size() > 2) return modloader::SetTrustNative(a[1], a[2] != "0"), Out("  ok");
    if (cmd == "modset" && a.size() > 3)
        return Out(modloader::SetControl(a[1], a[2], atof(a[3].c_str())) ? "  ok" : "  ! no such mod/control");
    if (cmd == "modpress" && a.size() > 2) return modloader::PressButton(a[1], a[2]), Out("  queued");
    if (cmd == "lang" && a.size() > 1)
        return i18n::SetLanguage(i18n::LanguageFromCode(a[1].c_str())), Out("  %s", i18n::LanguageCode(i18n::Language()));
    if (cmd == "panelinfo") return Out("  %s", overlay::PanelInfo().c_str());
    if (cmd == "paneloffset" && a.size() > 2) return overlay::SetPanelOffset(atoi(a[1].c_str()), atoi(a[2].c_str())), Out("  ok");
    if (cmd == "snapshot" && a.size() > 1) return overlay::RequestSnapshot(atoi(a[1].c_str())), Out("  requested");
    if (a.size() < 2) return Out("  ! missing arguments");
    void* target = Resolve(a[1]);

    if (cmd == "sig" && a.size() > 2) {
        void* cls = target ? ue::ClassOf(target) : ue::FindClass(a[1].c_str());
        void* fn = cls ? ue::FindFunction(cls, a[2].c_str()) : nullptr;
        if (!fn) return Out("  ! function not found");
        for (auto& p : Params(fn))
            Out("  +0x%03X %-18s %-28s%s%s", p.info.offset, p.info.type.c_str(), p.name.c_str(),
                (p.flags & ue::CPF_ReturnParm) ? " [return]" : "", (p.flags & ue::CPF_OutParm) ? " [out]" : "");
        return;
    }
    if (cmd == "callall" && a.size() > 2) {
        void* cls = ue::FindClass(a[1].c_str());
        if (!cls) return Out("  ! class not found");
        std::vector<void*> objs;
        int n = ue::NumObjects();
        for (int i = 0; i < n; ++i) {
            void* o = ue::ObjectAt(i);
            if (o && !ue::IsCDO(o) && ue::IsA(o, cls)) objs.push_back(o);
        }
        std::vector<std::string> args(a.begin() + 3, a.end());
        int done = 0;
        for (void* o : objs) {
            std::string r;
            if (Call(o, a[2], args, true, &r) && ++done <= 40) Out("  %s:%s", ue::NameOf(o).c_str(), r.c_str());
        }
        return Out("  called on %d/%zu instances", done, objs.size());
    }
    if (!target) return Out("  ! target '%s' not found", a[1].c_str());
    if (cmd == "call" && a.size() > 2) return void(Call(target, a[2], std::vector<std::string>(a.begin() + 3, a.end())));
    if (cmd == "get" && a.size() > 2) {
        auto p = ue::FindProperty(ue::ClassOf(target), a[2].c_str());
        if (!p) return Out("  ! no property %s", a[2].c_str());
        return Out("  %s = %s", a[2].c_str(), Format(p.field, (uint8_t*)target + p.offset).c_str());
    }
    if (cmd == "set" && a.size() > 3) {
        auto p = ue::FindProperty(ue::ClassOf(target), a[2].c_str());
        if (!p) return Out("  ! no property %s", a[2].c_str());
        if (!Parse(p.field, (uint8_t*)target + p.offset, a[3])) return Out("  ! cannot parse value");
        return Out("  %s = %s", a[2].c_str(), Format(p.field, (uint8_t*)target + p.offset).c_str());
    }
    if (cmd == "props") {
        Out("  %s", Describe(target).c_str());
        std::string filter = a.size() > 2 ? a[2] : "";
        for (void* c = ue::ClassOf(target); c; c = ue::SuperOf(c)) {
            struct Ctx {
                void* obj;
                std::string* filter;
            } ctx{target, &filter};
            ue::ForEachProperty(
                c,
                [](void* f, void* u) {
                    auto* x = static_cast<Ctx*>(u);
                    std::string n = ue::FieldName(f);
                    if (!x->filter->empty() && n.find(*x->filter) == std::string::npos) return;
                    auto fi = ue::DescribeProperty(f);
                    Out("  +0x%04X %-20s %-36s %s", fi.offset, fi.type.c_str(), n.c_str(),
                        Format(f, (uint8_t*)x->obj + fi.offset).c_str());
                },
                &ctx);
        }
        return;
    }
    Out("  ! unknown command");
}

}  // namespace

void PollCommandFile() {
    std::wstring path = ModDir() + L"DragonkinMod_cmd.txt";
    FILE* f = _wfsopen(path.c_str(), L"r", _SH_DENYNO);
    if (!f) return;
    std::vector<std::string> lines;
    char buf[2048];
    while (fgets(buf, sizeof(buf), f)) {
        std::string s = buf;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) lines.push_back(s);
    }
    fclose(f);
    DeleteFileW(path.c_str());
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& l : lines) g_queue.push_back(l);
}

void* ResolveTarget(const std::string& target) { return Resolve(target); }

std::string Exec(const std::string& line) {
    std::string saved;
    saved.swap(g_out);
    DWORD resume = g_resumeAt;
    Run(line);
    g_resumeAt = resume;  // "wait" means nothing here
    std::string result;
    result.swap(g_out);
    g_out.swap(saved);
    if (result.rfind("> ", 0) == 0) {  // drop the echo of the command itself
        size_t nl = result.find('\n');
        result = nl == std::string::npos ? std::string() : result.substr(nl + 1);
    }
    return result;
}

void RunPending() {
    for (int budget = 0; budget < 50; ++budget) {
        if (GetTickCount() < g_resumeAt) break;
        std::string line;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_queue.empty()) break;
            line = g_queue.front();
            g_queue.pop_front();
        }
        Run(line);
        Flush();
    }
    Flush();
}

}  // namespace dev
