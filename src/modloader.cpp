#include "modloader.h"
#include "common.h"
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "../sdk/dml_plugin.h"
#include "common.h"
#include "devcmd.h"
#include "core.h"
#include "i18n.h"
#include "imgui.h"
#include "json.h"
#include "ue.h"

#pragma comment(lib, "shell32.lib")

namespace modloader {
namespace {

using i18n::T;
using i18n::TL;

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------
struct LText {  // text in several languages; "en" is the fallback
    std::map<std::string, std::string> t;
    std::string Get() const {
        if (t.empty()) return {};
        auto it = t.find(i18n::LanguageCode(i18n::Language()));
        if (it != t.end()) return it->second;
        it = t.find("en");
        return it != t.end() ? it->second : t.begin()->second;
    }
    bool empty() const { return t.empty(); }
};

struct Step {
    enum Kind { Command, Console, Plugin, SetControl } kind;
    std::string text;   // command line / console command / plugin action / control id
    double value = 0;   // SetControl
};

struct Control {
    enum Kind { Slider, IntSlider, Toggle, Button } kind = Slider;
    std::string id;
    LText label, tip;
    double min = 0, max = 1, def = 0, value = 0;
    bool log = false;
    std::string format;
    std::vector<Step> steps;  // Button
};

struct ValueRef {  // a number, or "$control" / "!$control" (toggles) taken from a control
    bool isRef = false;
    bool negate = false;
    std::string ref;
    double lit = 0;
};

struct Leaf {  // one numeric field found by walking a property path (valid for this tick only)
    void* p;
    char kind;  // 'f' float, 'd' double, 'i' int32, 'b' bool (byte + mask)
    uint8_t mask;
};

struct Effect {
    enum Kind { Attribute, Property, Lock, Console } kind = Attribute;
    enum Mode { Multiply, Set, Add } mode = Multiply;
    std::string when;  // toggle control id ("" = always while the mod is on), "!" prefix negates
    ValueRef value;
    int line = 0;
    // Attribute / Lock
    std::string attr, maxAttr;
    // Property
    std::string target, cls, pathText;
    std::vector<std::string> path;
    int watch = -1;
    struct Obj {
        void* obj;
        int idx;
    };
    std::vector<Obj> objs;
    DWORD lastResolve = 0;
    // Console
    std::string on, off;
    bool consoleApplied = false;
    // state
    bool active = false;
    std::string status;  // shown in the UI ("12 values", "attribute not found", ...)
};

struct Hotkey {
    unsigned vk = 0;
    bool ctrl = false, shift = false, alt = false;
    std::string text;    // as written, for the UI
    std::string toggle;  // toggle control to flip
    std::vector<Step> steps;
};

struct Mod {
    std::string folderName, folder, id, version, author, pluginFile;
    LText name, description;
    bool enabled = false, trustNative = false, failed = false;
    std::vector<Control> controls;
    std::vector<Effect> effects;
    std::vector<Hotkey> hotkeys;
    std::vector<std::string> errors, warnings;
    std::deque<std::string> log;
    int faults = 0;
    // plugin
    HMODULE module = nullptr;
    DmlPlugin plugin{};
    bool pluginEnabled = false;  // OnEnable called
    DWORD lastTick = 0;          // for the plugin's OnTick seconds

    Control* FindControl(const std::string& cid) {
        for (auto& c : controls)
            if (c.id == cid) return &c;
        return nullptr;
    }
    void AddLog(const std::string& line) {
        log.push_back(line);
        while (log.size() > 30) log.pop_front();
        Log("[mod %s] %s", id.c_str(), line.c_str());
    }
};

std::recursive_mutex g_mutex;  // mods list and control values (panel thread + game thread)
std::vector<std::unique_ptr<Mod>> g_mods;
std::unordered_map<std::string, float> g_attrFactors;  // claimed attribute -> product of mods' multiply effects (this tick)
std::unordered_map<std::string, float> g_attrFactorsDone;  // the same, complete, from the previous tick (what plugins read)
std::unordered_map<std::string, std::string> g_claimed;  // attribute -> id of the plugin mod that manages it
bool g_reload = false;
bool g_dirty = false;
std::deque<std::pair<std::string, std::string>> g_buttonQueue;  // (mod id, control id) pressed in the panel
std::deque<unsigned> g_keyQueue;
// developer console
std::deque<std::string> g_devQueue;
std::deque<std::string> g_devLog;

std::wstring ModsDir() { return ModDir() + L"Mods\\"; }
std::wstring IniPath() { return ModDir() + L"DragonkinMod.ini"; }

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------------------
// Settings: [Mod:<id>] Enabled=, TrustNative=, <control>=
// ---------------------------------------------------------------------------
std::wstring Section(const Mod& m) { return L"Mod:" + Wide(m.id); }

bool IniGet(const std::wstring& sec, const std::string& key, double& out) {
    wchar_t buf[64];
    GetPrivateProfileStringW(sec.c_str(), Wide(key).c_str(), L"", buf, 64, IniPath().c_str());
    if (!buf[0]) return false;
    out = _wtof(buf);
    return true;
}
void IniSet(const std::wstring& sec, const std::string& key, double v) {
    wchar_t buf[64];
    swprintf_s(buf, L"%.6g", v);
    WritePrivateProfileStringW(sec.c_str(), Wide(key).c_str(), buf, IniPath().c_str());
}

void LoadModSettings(Mod& m) {
    double v;
    std::wstring sec = Section(m);
    m.enabled = IniGet(sec, "Enabled", v) && v != 0;
    m.trustNative = IniGet(sec, "TrustNative", v) && v != 0;
    for (auto& c : m.controls)
        if (c.kind != Control::Button && IniGet(sec, c.id, v)) c.value = std::clamp(v, c.min, c.max);
}

void SaveSettings() {
    for (auto& m : g_mods) {
        if (m->id.empty()) continue;
        std::wstring sec = Section(*m);
        IniSet(sec, "Enabled", m->enabled ? 1 : 0);
        IniSet(sec, "TrustNative", m->trustNative ? 1 : 0);
        for (auto& c : m->controls)
            if (c.kind != Control::Button) IniSet(sec, c.id, c.value);
    }
}

// ---------------------------------------------------------------------------
// Manifest parsing
// ---------------------------------------------------------------------------
struct Ctx {
    Mod& m;
    void Err(const json::Value& v, const std::string& msg) {
        m.errors.push_back("line " + std::to_string(v.line) + ": " + msg);
    }
    void Warn(const json::Value& v, const std::string& msg) {
        m.warnings.push_back("line " + std::to_string(v.line) + ": " + msg);
    }
    void Unknown(const json::Value& obj, std::initializer_list<const char*> known) {
        for (auto& kv : obj.obj) {
            bool ok = false;
            for (const char* k : known) ok |= kv.first == k;
            if (!ok) Warn(kv.second, "unknown key \"" + kv.first + "\" (ignored)");
        }
    }
};

bool ReadText(Ctx& c, const json::Value* v, LText& out, const char* what, bool required) {
    if (!v) {
        if (required) c.m.errors.push_back(std::string("missing \"") + what + "\"");
        return !required;
    }
    if (v->is(json::Value::String)) {
        out.t["en"] = v->s;
        return true;
    }
    if (v->is(json::Value::Object)) {
        for (auto& kv : v->obj) {
            if (!kv.second.is(json::Value::String)) {
                c.Err(kv.second, std::string("\"") + what + "\" translations must be strings");
                return false;
            }
            std::string code = kv.first;
            if (code == "zh-TW" || code == "zh_TW" || code == "zh-hant") code = "zh-Hant";
            if (code == "zh-CN" || code == "zh_CN" || code == "zh-hans") code = "zh-Hans";
            if (i18n::LanguageFromCode(code.c_str()) == i18n::EN && code != "en")
                c.Warn(kv.second, "unknown language code \"" + kv.first + "\"");
            out.t[code] = kv.second.s;
        }
        if (!out.t.count("en")) c.Warn(*v, std::string("\"") + what + "\" has no \"en\" text (English is the fallback)");
        return true;
    }
    c.Err(*v, std::string("\"") + what + "\" must be a string or {\"en\": ..., \"tr\": ...}");
    return false;
}

bool ReadValue(Ctx& c, const json::Value& v, ValueRef& out) {
    if (v.is(json::Value::Number)) {
        out.lit = v.n;
        return true;
    }
    if (v.is(json::Value::Bool)) {
        out.lit = v.b ? 1 : 0;
        return true;
    }
    if (v.is(json::Value::String) && !v.s.empty()) {
        std::string s = v.s;
        if (s[0] == '!') out.negate = true, s = s.substr(1);
        if (!s.empty() && s[0] == '$') {
            out.isRef = true;
            out.ref = s.substr(1);
            return true;
        }
    }
    c.Err(v, "value must be a number, true/false or \"$controlId\"");
    return false;
}

bool ReadSteps(Ctx& c, const json::Value* v, std::vector<Step>& out) {
    if (!v) return true;
    if (!v->is(json::Value::Array)) {
        c.Err(*v, "\"actions\" must be a list");
        return false;
    }
    for (auto& s : v->arr) {
        if (!s.is(json::Value::Object) || s.obj.size() != (s.get("value") ? 2u : 1u)) {
            c.Err(s, "each action is one of {\"command\": \"...\"}, {\"console\": \"...\"}, {\"plugin\": \"...\"}, "
                     "{\"set\": \"controlId\", \"value\": 1}");
            return false;
        }
        Step st;
        const auto& kv = s.obj[0];
        if (!kv.second.is(json::Value::String)) {
            c.Err(kv.second, "action text must be a string");
            return false;
        }
        st.text = kv.second.s;
        if (kv.first == "command") st.kind = Step::Command;
        else if (kv.first == "console") st.kind = Step::Console;
        else if (kv.first == "plugin") st.kind = Step::Plugin;
        else if (kv.first == "set") {
            st.kind = Step::SetControl;
            const json::Value* val = s.get("value");
            if (!val || !(val->is(json::Value::Number) || val->is(json::Value::Bool))) {
                c.Err(s, "\"set\" needs a numeric \"value\"");
                return false;
            }
            st.value = val->is(json::Value::Bool) ? (val->b ? 1 : 0) : val->n;
        } else {
            c.Err(kv.second, "unknown action \"" + kv.first + "\"");
            return false;
        }
        out.push_back(st);
    }
    return true;
}

bool ValidId(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char ch : s)
        if (!(isalnum((unsigned char)ch) || ch == '-' || ch == '_' || ch == '.')) return false;
    return true;
}

unsigned KeyFromName(const std::string& n) {
    std::string u;
    for (char ch : n) u += (char)toupper((unsigned char)ch);
    if (u.size() == 1 && ((u[0] >= 'A' && u[0] <= 'Z') || (u[0] >= '0' && u[0] <= '9'))) return (unsigned char)u[0];
    if (u.size() >= 2 && u[0] == 'F') {
        int f = atoi(u.c_str() + 1);
        if (f >= 1 && f <= 24) return VK_F1 + f - 1;
    }
    if (u.rfind("NUMPAD", 0) == 0 && u.size() == 7 && isdigit((unsigned char)u[6])) return VK_NUMPAD0 + (u[6] - '0');
    static const std::pair<const char*, unsigned> named[] = {
        {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE}, {"HOME", VK_HOME}, {"END", VK_END}, {"PAGEUP", VK_PRIOR},
        {"PAGEDOWN", VK_NEXT}, {"PAUSE", VK_PAUSE}, {"SPACE", VK_SPACE}, {"TAB", VK_TAB}, {"UP", VK_UP},
        {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"BACKSPACE", VK_BACK}, {"ENTER", VK_RETURN},
    };
    for (auto& k : named)
        if (u == k.first) return k.second;
    return 0;
}

std::vector<std::string> SplitPath(const std::string& p) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < p.size(); ++i) {
        char ch = p[i];
        if (ch == '.' ) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else if (ch == '[') {
            if (!cur.empty()) out.push_back(cur);
            size_t e = p.find(']', i);
            if (e == std::string::npos) return {};
            out.push_back(p.substr(i, e - i + 1));  // keeps the brackets: "[Main]", "[3]"
            cur.clear();
            i = e;
        } else {
            cur += ch;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void ParseManifest(Mod& m, const json::Value& root) {
    Ctx c{m};
    if (!root.is(json::Value::Object)) return c.Err(root, "the file must contain one { ... } object");
    c.Unknown(root, {"schema", "id", "name", "version", "author", "description", "controls", "effects", "hotkeys", "plugin",
                     "loader"});
    double schema = root.num("schema", 1);
    if (schema > kSchema) m.errors.push_back("needs a newer mod loader (schema " + std::to_string((int)schema) + ")");
    m.id = root.str("id");
    if (!ValidId(m.id)) m.errors.push_back("\"id\" is required: letters, digits, - _ . (max 64)");
    ReadText(c, root.get("name"), m.name, "name", true);
    ReadText(c, root.get("description"), m.description, "description", false);
    m.version = root.str("version", "1.0");
    m.author = root.str("author");
    m.pluginFile = root.str("plugin");
    if (!m.pluginFile.empty() &&
        (m.pluginFile.find_first_of("\\/:") != std::string::npos || m.pluginFile.size() < 5 ||
         _stricmp(m.pluginFile.c_str() + m.pluginFile.size() - 4, ".dll") != 0))
        m.errors.push_back("\"plugin\" must be a .dll file name inside the mod folder");

    if (const json::Value* cs = root.get("controls")) {
        if (!cs->is(json::Value::Array)) c.Err(*cs, "\"controls\" must be a list");
        else
            for (auto& v : cs->arr) {
                if (!v.is(json::Value::Object)) {
                    c.Err(v, "a control must be an object");
                    continue;
                }
                c.Unknown(v, {"id", "type", "label", "tooltip", "min", "max", "default", "log", "format", "actions"});
                Control ctl;
                ctl.id = v.str("id");
                if (!ValidId(ctl.id)) c.Err(v, "control needs a valid \"id\"");
                if (m.FindControl(ctl.id)) c.Err(v, "duplicate control id \"" + ctl.id + "\"");
                std::string type = v.str("type", "slider");
                if (type == "slider") ctl.kind = Control::Slider;
                else if (type == "int") ctl.kind = Control::IntSlider;
                else if (type == "toggle") ctl.kind = Control::Toggle;
                else if (type == "button") ctl.kind = Control::Button;
                else c.Err(v, "unknown control type \"" + type + "\" (slider, int, toggle, button)");
                ReadText(c, v.get("label"), ctl.label, "label", true);
                ReadText(c, v.get("tooltip"), ctl.tip, "tooltip", false);
                if (ctl.kind == Control::Toggle) {
                    ctl.min = 0, ctl.max = 1;
                    ctl.def = v.boolean("default", false) ? 1 : v.num("default", 0) != 0 ? 1 : 0;
                } else if (ctl.kind != Control::Button) {
                    ctl.min = v.num("min", 0), ctl.max = v.num("max", 1);
                    ctl.def = v.num("default", ctl.min);
                    if (!(ctl.max > ctl.min)) c.Err(v, "\"max\" must be greater than \"min\"");
                    if (ctl.def < ctl.min || ctl.def > ctl.max) c.Err(v, "\"default\" must be between min and max");
                    ctl.log = v.boolean("log", false);
                    if (ctl.log && ctl.min < 0) c.Err(v, "a log slider cannot go below 0");
                    ctl.format = v.str("format", ctl.kind == Control::IntSlider ? "%d" : "%.2f");
                    int specs = 0;
                    for (size_t i = 0; i < ctl.format.size(); ++i)
                        if (ctl.format[i] == '%') {
                            if (i + 1 < ctl.format.size() && ctl.format[i + 1] == '%') ++i;
                            else ++specs;
                        }
                    if (specs != 1) c.Err(v, "\"format\" needs exactly one number placeholder, e.g. \"%.1fx\"");
                }
                if (ctl.kind == Control::Button) {
                    if (!v.get("actions")) c.Err(v, "a button needs \"actions\"");
                    ReadSteps(c, v.get("actions"), ctl.steps);
                }
                ctl.value = ctl.def;
                m.controls.push_back(std::move(ctl));
            }
    }

    auto checkRef = [&](const json::Value& at, const std::string& ref, bool toggle) {
        Control* ctl = m.FindControl(ref);
        if (!ctl) return c.Err(at, "no control with id \"" + ref + "\"");
        if (ctl->kind == Control::Button) c.Err(at, "\"" + ref + "\" is a button and has no value");
        if (toggle && ctl->kind != Control::Toggle) c.Err(at, "\"" + ref + "\" must be a toggle control");
    };

    if (const json::Value* es = root.get("effects")) {
        if (!es->is(json::Value::Array)) c.Err(*es, "\"effects\" must be a list");
        else
            for (auto& v : es->arr) {
                if (!v.is(json::Value::Object)) {
                    c.Err(v, "an effect must be an object");
                    continue;
                }
                Effect e;
                e.line = v.line;
                std::string type = v.str("type");
                c.Unknown(v, {"type", "attribute", "max", "mode", "value", "when", "target", "class", "path", "on", "off"});
                if (const json::Value* w = v.get("when")) {
                    if (!w->is(json::Value::String) || w->s.empty()) c.Err(*w, "\"when\" must be \"$toggleId\" or \"!$toggleId\"");
                    else {
                        std::string s = w->s, ref = s[0] == '!' ? s.substr(1) : s;
                        if (ref.empty() || ref[0] != '$') c.Err(*w, "\"when\" must start with $");
                        else checkRef(*w, ref.substr(1), true), e.when = (s[0] == '!' ? "!" : "") + ref.substr(1);
                    }
                }
                std::string mode = v.str("mode", "multiply");
                if (mode == "multiply") e.mode = Effect::Multiply;
                else if (mode == "set") e.mode = Effect::Set;
                else if (mode == "add") e.mode = Effect::Add;
                else c.Err(v, "\"mode\" is multiply, set or add");
                bool needsValue = true;
                if (type == "attribute") {
                    e.kind = Effect::Attribute;
                    e.attr = v.str("attribute");
                    if (e.attr.empty()) c.Err(v, "\"attribute\" is required (e.g. \"XPMultiplier\")");
                } else if (type == "property") {
                    e.kind = Effect::Property;
                    e.target = v.str("target");
                    e.cls = v.str("class");
                    e.pathText = v.str("path");
                    e.path = SplitPath(e.pathText);
                    if (e.target.empty() == e.cls.empty()) c.Err(v, "give exactly one of \"target\" or \"class\"");
                    if (e.path.empty()) c.Err(v, "\"path\" is required, e.g. \"m_XpGains.*.m_fGain\"");
                } else if (type == "lock") {
                    e.kind = Effect::Lock;
                    e.attr = v.str("attribute");
                    e.maxAttr = v.str("max");
                    needsValue = false;
                    if (e.attr.empty() || e.maxAttr.empty()) c.Err(v, "a lock needs \"attribute\" and \"max\"");
                } else if (type == "console") {
                    e.kind = Effect::Console;
                    e.on = v.str("on");
                    e.off = v.str("off");
                    needsValue = false;
                    if (e.on.empty()) c.Err(v, "a console effect needs \"on\"");
                } else {
                    c.Err(v, "unknown effect type \"" + type + "\" (attribute, property, lock, console)");
                    continue;
                }
                if (needsValue) {
                    const json::Value* val = v.get("value");
                    if (!val) c.Err(v, "\"value\" is required");
                    else if (ReadValue(c, *val, e.value) && e.value.isRef) checkRef(*val, e.value.ref, false);
                }
                m.effects.push_back(std::move(e));
            }
    }

    if (const json::Value* hs = root.get("hotkeys")) {
        if (!hs->is(json::Value::Array)) c.Err(*hs, "\"hotkeys\" must be a list");
        else
            for (auto& v : hs->arr) {
                if (!v.is(json::Value::Object)) {
                    c.Err(v, "a hotkey must be an object");
                    continue;
                }
                c.Unknown(v, {"key", "toggle", "actions"});
                Hotkey hk;
                hk.text = v.str("key");
                std::string rest = hk.text;
                for (;;) {
                    size_t plus = rest.find('+');
                    if (plus == std::string::npos) break;
                    std::string mod = rest.substr(0, plus);
                    for (auto& ch : mod) ch = (char)toupper((unsigned char)ch);
                    if (mod == "CTRL") hk.ctrl = true;
                    else if (mod == "SHIFT") hk.shift = true;
                    else if (mod == "ALT") hk.alt = true;
                    else c.Err(v, "unknown modifier \"" + mod + "\"");
                    rest = rest.substr(plus + 1);
                }
                hk.vk = KeyFromName(rest);
                if (!hk.vk) c.Err(v, "unknown key \"" + hk.text + "\" (F1-F24, A-Z, 0-9, Numpad0-9, Insert, Home, ...)");
                if (hk.vk == VK_F7 && !hk.ctrl && !hk.shift && !hk.alt) c.Err(v, "F7 opens the menu and cannot be used");
                std::string tg = v.str("toggle");
                if (!tg.empty()) {
                    if (tg[0] != '$') c.Err(v, "\"toggle\" must be \"$toggleId\"");
                    else checkRef(v, tg.substr(1), true), hk.toggle = tg.substr(1);
                }
                ReadSteps(c, v.get("actions"), hk.steps);
                if (hk.toggle.empty() && hk.steps.empty()) c.Err(v, "a hotkey needs \"toggle\" or \"actions\"");
                m.hotkeys.push_back(std::move(hk));
            }
    }
}

std::string ReadFileUtf8(const std::wstring& path, bool& ok) {
    ok = false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return {};
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && s.size() < 1024 * 1024) s.append(buf, n);
    fclose(f);
    ok = true;
    return s;
}

void LoadAll() {
    g_mods.clear();
    CreateDirectoryW(ModsDir().c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((ModsDir() + L"*").c_str(), &fd);
    std::vector<std::wstring> folders;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') folders.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(folders.begin(), folders.end());
    for (auto& folder : folders) {
        auto m = std::make_unique<Mod>();
        m->folderName = Narrow(folder);
        m->folder = Narrow(ModsDir() + folder + L"\\");
        bool ok;
        std::string text = ReadFileUtf8(ModsDir() + folder + L"\\mod.json", ok);
        if (!ok) {
            continue;  // not a mod folder
        }
        json::Value root;
        std::string err;
        if (!json::Parse(text, root, err)) {
            m->errors.push_back("mod.json: " + err);
        } else {
            ParseManifest(*m, root);
        }
        for (auto& other : g_mods)
            if (!m->id.empty() && other->id == m->id)
                m->errors.push_back("another mod (" + other->folderName + ") already uses the id \"" + m->id + "\"");
        m->failed = !m->errors.empty();
        if (m->name.empty()) m->name.t["en"] = m->folderName;
        if (!m->failed) LoadModSettings(*m);
        Log("mod %s (%s): %s, %zu controls, %zu effects, %zu hotkeys, %zu errors, %zu warnings", m->folderName.c_str(),
            m->id.c_str(), m->enabled ? "enabled" : "disabled", m->controls.size(), m->effects.size(), m->hotkeys.size(),
            m->errors.size(), m->warnings.size());
        for (auto& e : m->errors) Log("  error: %s", e.c_str());
        g_mods.push_back(std::move(m));
    }
}

// ---------------------------------------------------------------------------
// Fault guard: one plain call under SEH (no C++ objects in this frame)
// ---------------------------------------------------------------------------
using GuardFn = void (*)(void*);
int Guarded(GuardFn fn, void* arg) {
    __try {
        fn(arg);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------
double Eval(Mod& m, const ValueRef& v) {
    double x = v.lit;
    if (v.isRef)
        if (Control* c = m.FindControl(v.ref)) x = c->value;
    return v.negate ? (x != 0 ? 0 : 1) : x;
}

bool WhenTrue(Mod& m, const std::string& when) {
    if (when.empty()) return true;
    bool neg = when[0] == '!';
    Control* c = m.FindControl(neg ? when.substr(1) : when);
    bool on = c && c->value != 0;
    return neg ? !on : on;
}

// ---------------------------------------------------------------------------
// Property paths
// ---------------------------------------------------------------------------
struct PathCtx {
    const std::vector<std::string>* path;
    std::vector<Leaf>* out;
    std::string error;
};

void CollectAt(void* field, uint8_t* p, PathCtx& c, size_t ti);

// Finds a property of a struct/class (including supers) by name.
ue::PropInfo FindProp(void* strct, const std::string& name) { return ue::FindProperty(strct, name.c_str()); }

std::string KeyText(void* keyField, uint8_t* p) {
    auto info = ue::DescribeProperty(keyField);
    if (info.type == "NameProperty") return ue::NameFromFName(p);
    if (info.type == "EnumProperty" || info.type == "ByteProperty") {
        int64_t v = 0;
        memcpy(&v, p, info.elementSize > 8 ? 8 : info.elementSize);
        void* uenum = info.type == "EnumProperty" ? ue::PropertySlot(keyField, 1) : ue::PropertySlot(keyField, 0);
        std::string n = uenum ? ue::EnumValueName(uenum, v) : "";
        size_t colon = n.rfind("::");
        if (colon != std::string::npos) n = n.substr(colon + 2);
        return n.empty() ? std::to_string(v) : n;
    }
    if (info.type == "IntProperty") return std::to_string(*(int32_t*)p);
    if (info.type == "StructProperty") {  // e.g. GameplayTag
        return ue::NameFromFName(p);
    }
    return "";
}

bool KeyMatches(const std::string& token, const std::string& key) {
    std::string want = token.substr(1, token.size() - 2);
    if (want == key) return true;
    // enum names often carry a prefix ("e_Main"): accept the part after '_'
    size_t us = key.find('_');
    return us != std::string::npos && key.substr(us + 1) == want;
}

void CollectStruct(void* strct, uint8_t* base, PathCtx& c, size_t ti) {
    if (ti >= c.path->size()) return;
    auto p = FindProp(strct, (*c.path)[ti]);
    if (!p) {
        c.error = "no property \"" + (*c.path)[ti] + "\" in " + ue::NameOf(strct);
        return;
    }
    CollectAt(p.field, base + p.offset, c, ti + 1);
}

void CollectAt(void* field, uint8_t* p, PathCtx& c, size_t ti) {
    auto info = ue::DescribeProperty(field);
    const auto& path = *c.path;
    if (ti >= path.size()) {  // leaf
        Leaf l{p, 0, 0};
        if (info.type == "FloatProperty") l.kind = 'f';
        else if (info.type == "DoubleProperty") l.kind = 'd';
        else if (info.type == "IntProperty") l.kind = 'i';
        else if (info.type == "BoolProperty") {
            int ps = ue::FPropertySize();
            uint8_t byteOffset = ps > 0 ? ReadOr<uint8_t>((uint8_t*)field + ps + 1, 0) : 0;
            l.mask = ps > 0 ? ReadOr<uint8_t>((uint8_t*)field + ps + 3, 0xFF) : 0xFF;
            l.p = p + byteOffset;
            l.kind = 'b';
        } else {
            c.error = "\"" + ue::FieldName(field) + "\" is a " + info.type + "; only float, double, int and bool can be changed";
            return;
        }
        c.out->push_back(l);
        return;
    }
    const std::string& tok = path[ti];
    if (info.type == "StructProperty") {
        void* s = ue::PropertyStruct(field);
        if (!s) return void(c.error = "unknown struct type");
        return CollectStruct(s, p, c, ti);
    }
    if (info.type == "ObjectProperty") {
        void* o = ReadOr<void*>(p, nullptr);
        if (!o || !LooksLikePtr(o)) return;  // null reference: nothing to change
        return CollectStruct(ue::ClassOf(o), (uint8_t*)o, c, ti);
    }
    if (info.type == "ArrayProperty") {
        void* inner = ue::ArrayInner(field);
        ue::TArrayRaw a{};
        if (!inner || !Read(p, a) || a.num < 0 || a.num > 100000) return;
        int esz = ue::DescribeProperty(inner).elementSize;
        bool all = tok == "*";
        int only = -1;
        if (!all) {
            if (tok.size() < 3 || tok[0] != '[') return void(c.error = "after an array use \"*\" or \"[index]\"");
            only = atoi(tok.c_str() + 1);
        }
        for (int i = 0; i < a.num; ++i)
            if (all || i == only) CollectAt(inner, (uint8_t*)a.data + i * esz, c, ti + 1);
        return;
    }
    if (info.type == "MapProperty") {
        void* key = ue::PropertySlot(field, 0);
        void* val = ue::PropertySlot(field, 1);
        if (!key || !val) return;
        bool all = tok == "*";
        if (!all && (tok.size() < 3 || tok[0] != '[')) return void(c.error = "after a map use \"*\" or \"[Key]\"");
        int ks = ue::DescribeProperty(key).elementSize, vs = ue::DescribeProperty(val).elementSize;
        int valAlign = vs >= 8 ? 8 : (vs >= 4 ? 4 : 1);
        int valOff = (ks + valAlign - 1) / valAlign * valAlign;
        struct E {
            PathCtx* c;
            void *key, *val;
            int valOff;
            size_t ti;
            bool all;
            const std::string* tok;
        } e{&c, key, val, valOff, ti, all, &tok};
        ue::ForEachSparseElement(
            p, ue::SetElementStride(valOff + vs),
            [](uint8_t* el, void* u) {
                auto* x = static_cast<E*>(u);
                if (!x->all && !KeyMatches(*x->tok, KeyText(x->key, el))) return;
                CollectAt(x->val, el + x->valOff, *x->c, x->ti + 1);
            },
            &e);
        return;
    }
    c.error = "cannot go into \"" + ue::FieldName(field) + "\" (" + info.type + ")";
}

// ---------------------------------------------------------------------------
// Effect engine. Every tick the active effects contribute to shared registries
// (attributes, property leaves), so several mods can touch the same value without
// stacking on each other's writes; values nobody contributes to go back to the
// game's original.
// ---------------------------------------------------------------------------
struct Contribution {
    Effect::Mode mode;
    double v;
    const void* src;     // the mod (or a built-in feature) that wants the change
    std::string srcName;
};
// Changed values are merged by their address in the current tick (different paths
// can reach the same field: boom.TargetArmLength and CameraView.m_pCameraBoom.TargetArmLength).
// Addresses are found again every tick by walking the path; to give a value back, the
// path of its first contributor is walked again, and if the value moved (array
// reallocated) it is left alone instead of writing to the old address.
struct LeafState {
    void* owner = nullptr;
    int ownerIdx = -1;
    std::vector<std::string> path;
    size_t ordinal = 0;
    double orig = 0, last = NAN;
    std::vector<Contribution> contribs;
    Leaf cur{};
    bool hasCur = false;
    // Fight protection: if the game keeps rewriting a value we change, stacking our
    // change on every rewrite would run away; such a value is left alone for a while.
    int rewrites = 0;
    DWORD windowStart = 0, blockedUntil = 0;
};
std::unordered_map<void*, LeafState> g_leaves;
struct AttrState {
    float orig = 0, last = NAN;
    std::vector<Contribution> contribs;
};
std::unordered_map<std::string, AttrState> g_attrs;

double ReadLeaf(const Leaf& l) {
    switch (l.kind) {
    case 'f': return ReadOr<float>(l.p, 0);
    case 'd': return ReadOr<double>(l.p, 0);
    case 'i': return ReadOr<int32_t>(l.p, 0);
    case 'b': return (ReadOr<uint8_t>(l.p, 0) & l.mask) ? 1 : 0;
    }
    return 0;
}
void WriteLeaf(const Leaf& l, double v) {
    switch (l.kind) {
    case 'f': *(float*)l.p = (float)v; break;
    case 'd': *(double*)l.p = v; break;
    case 'i': *(int32_t*)l.p = (int32_t)std::lround(v); break;
    case 'b': {
        uint8_t& b = *(uint8_t*)l.p;
        b = v != 0 ? (b | l.mask) : (b & ~l.mask);
        break;
    }
    }
}
double Combine(double orig, const std::vector<Contribution>& cs) {
    double v = orig;
    for (auto& c : cs) {
        if (c.mode == Effect::Set) v = c.v;
        else if (c.mode == Effect::Add) v += c.v;
        else v *= c.v;
    }
    return v;
}
bool Same(double a, double b) { return fabs(a - b) <= 1e-6 * std::max(1.0, fabs(a)); }

std::vector<Leaf> WalkPath(void* owner, const std::vector<std::string>& path, std::string* error) {
    std::vector<Leaf> leaves;
    PathCtx pc{&path, &leaves, {}};
    CollectStruct(ue::ClassOf(owner), (uint8_t*)owner, pc, 0);
    if (error) *error = pc.error;
    return leaves;
}

// Adds a wanted change for every value at `path` under `owner`; returns how many.
size_t Contribute(void* owner, const std::vector<std::string>& path, const std::string& pathText, Effect::Mode mode, double v,
                  const void* src, const std::string& srcName, std::string* error) {
    if (!owner || !std::isfinite(v)) return 0;
    std::vector<Leaf> leaves = WalkPath(owner, path, error);
    int idx = ue::IndexOf(owner);
    (void)pathText;
    for (size_t k = 0; k < leaves.size(); ++k) {
        LeafState& st = g_leaves[leaves[k].p];
        if (!st.owner) st.owner = owner, st.ownerIdx = idx, st.path = path, st.ordinal = k;
        st.cur = leaves[k];
        st.hasCur = true;
        st.contribs.push_back({mode, v, src, srcName});
    }
    return leaves.size();
}

void ResolveProperty(Mod& m, Effect& e) {
    // Objects: one target (re-resolved every 2 s) or every live instance of a class.
    std::vector<void*> objs;
    if (!e.target.empty()) {
        if (GetTickCount() - e.lastResolve < 2000 && !e.objs.empty()) {
            for (auto& o : e.objs) objs.push_back(o.obj);
        } else {
            e.lastResolve = GetTickCount();
            if (void* o = dev::ResolveTarget(e.target)) objs.push_back(o);
        }
    } else {
        if (e.watch < 0) e.watch = core::WatchClass(e.cls);
        objs = core::WatchedInstances(e.watch);
    }
    std::vector<Effect::Obj> next;
    for (void* o : objs) {
        int idx = -1;
        for (auto& x : e.objs)
            if (x.obj == o) idx = x.idx;
        if (idx < 0) idx = ue::IndexOf(o);
        if (ue::IsLive(o, idx)) next.push_back({o, idx});
    }
    e.objs.swap(next);
    (void)m;
}

void CallPlugin(Mod& m, int what, const char* action = nullptr, float dt = 0);

void RunSteps(Mod& m, const std::vector<Step>& steps) {
    for (auto& s : steps) {
        switch (s.kind) {
        case Step::Command: {
            // ${control} placeholders take control values
            std::string line = s.text;
            for (auto& c : m.controls) {
                std::string ph = "${" + c.id + "}";
                for (size_t pos; (pos = line.find(ph)) != std::string::npos;) {
                    char num[32];
                    snprintf(num, sizeof(num), "%g", c.value);
                    line.replace(pos, ph.size(), num);
                }
            }
            std::string out = dev::Exec(line);
            while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
            m.AddLog("> " + line + (out.empty() ? "" : "\n" + out));
            break;
        }
        case Step::Console:
            m.AddLog((core::ExecConsoleCommand(s.text) ? "console: " : "console failed: ") + s.text);
            break;
        case Step::Plugin:
            CallPlugin(m, 3, s.text.c_str());
            break;
        case Step::SetControl:
            if (Control* c = m.FindControl(s.text)) {
                c->value = std::clamp(s.value, c->min, c->max);
                g_dirty = true;
            }
            break;
        }
    }
}

// --- plugins --------------------------------------------------------------
thread_local std::string t_execOut;
thread_local Mod* g_pluginMod = nullptr;  // the mod whose plugin is being called on this thread

void HostLog(const char* text) {
    if (g_pluginMod && text) g_pluginMod->AddLog(text);
}
const char* HostExec(const char* line) {
    t_execOut = line ? dev::Exec(line) : std::string();
    return t_execOut.c_str();
}
int HostConsole(const char* cmd) { return cmd && core::ExecConsoleCommand(cmd) ? 1 : 0; }
double HostGetControl(const char* id) {
    Control* c = g_pluginMod && id ? g_pluginMod->FindControl(id) : nullptr;
    return c ? c->value : 0;
}
void HostSetControl(const char* id, double v) {
    if (Control* c = g_pluginMod && id ? g_pluginMod->FindControl(id) : nullptr) {
        c->value = std::clamp(v, c->min, c->max);
        g_dirty = true;
    }
}
int HostGetAttr(const char* name, float* b, float* cur) {
    float x = 0, y = 0;
    if (!name || !core::GetAttr(name, x, y)) return 0;
    if (b) *b = x;
    if (cur) *cur = y;
    return 1;
}
int HostSetAttr(const char* name, float b, float cur) { return name && core::SetAttr(name, b, cur) ? 1 : 0; }
int HostLanguage() { return i18n::Language(); }
size_t Contribute(void* owner, const std::vector<std::string>& path, const std::string& pathText, Effect::Mode mode, double v,
                  const void* src, const std::string& srcName, std::string* error);
std::vector<std::string> SplitPath(const std::string& p);
int HostContribute(void* owner, const char* path, int op, double value, const char* source) {
    if (!owner || !path || !LooksLikePtr(owner)) return 0;
    static std::unordered_map<std::string, std::vector<std::string>> cache;
    auto it = cache.find(path);
    if (it == cache.end()) it = cache.emplace(path, SplitPath(path)).first;
    Effect::Mode mode = op == DML_OP_SET ? Effect::Set : op == DML_OP_ADD ? Effect::Add : Effect::Multiply;
    const void* src = g_pluginMod ? (const void*)g_pluginMod : (const void*)source;
    std::string name = source ? source : g_pluginMod ? g_pluginMod->id : "plugin";
    return (int)Contribute(owner, it->second, path, mode, value, src, name, nullptr);
}
void HostClaim(const char* attr) {
    if (attr) g_claimed[attr] = g_pluginMod ? g_pluginMod->id : "a plugin";
}
float HostFactor(const char* attr) {
    // Plugins tick in folder order, maybe before the mods that contribute, so they read
    // the complete result of the previous tick.
    auto it = attr ? g_attrFactorsDone.find(attr) : g_attrFactorsDone.end();
    return it == g_attrFactorsDone.end() ? 1.0f : it->second;
}
thread_local std::string t_folder;
const char* HostFolder() {
    t_folder = g_pluginMod ? g_pluginMod->folder : std::string();
    return t_folder.c_str();
}
int HostModsEnabled() { return core::ModsEnabled() ? 1 : 0; }

DmlHost MakeHost() {
    DmlHost h{};
    h.apiVersion = DML_API_VERSION;
    h.Log = HostLog;
    h.Exec = HostExec;
    h.Console = HostConsole;
    h.GetControl = HostGetControl;
    h.SetControl = HostSetControl;
    h.GetAttribute = HostGetAttr;
    h.SetAttribute = HostSetAttr;
    h.imguiVersion = IMGUI_VERSION;
    h.imguiContext = ImGui::GetCurrentContext();
    ImGuiMemAllocFunc a;
    ImGuiMemFreeFunc f;
    void* user;
    ImGui::GetAllocatorFunctions(&a, &f, &user);
    h.imguiAlloc = a;
    h.imguiFree = f;
    h.imguiAllocUser = user;
    h.GetLanguage = HostLanguage;
    h.ContributeProperty = HostContribute;
    h.ClaimAttribute = HostClaim;
    h.GetAttributeFactor = HostFactor;
    h.ModFolder = HostFolder;
    h.ModsEnabled = HostModsEnabled;
    return h;
}
DmlHost g_host;  // filled when the first plugin loads (the panel's ImGui context exists by then)

struct PluginCall {
    Mod* m;
    int what;
    const char* action;
    float dt;
    DmlInitFn init;
    int initResult;
    int tab = 0;
    const char* line = nullptr;
    char* out = nullptr;
    int outSize = 0;
};
void PluginCallRaw(void* arg) {
    auto* c = static_cast<PluginCall*>(arg);
    DmlPlugin& p = c->m->plugin;
    switch (c->what) {
    case 0: c->initResult = c->init(&g_host, &p); break;
    case 1: if (p.OnEnable) p.OnEnable(); break;
    case 2: if (p.OnDisable) p.OnDisable(); break;
    case 3: if (p.OnAction) p.OnAction(c->action); break;
    case 4: if (p.OnTick) p.OnTick(c->dt); break;
    case 5: if (p.DrawTab) p.DrawTab(c->tab); break;
    case 6: c->initResult = p.OnCommand ? p.OnCommand(c->line, c->out, c->outSize) : 0; break;
    case 7: c->initResult = p.GetTabCount ? p.GetTabCount() : 0; break;
    case 8: c->action = p.GetTabName ? p.GetTabName(c->tab) : nullptr; break;
    }
}

void DisableForFault(Mod& m, const std::string& where) {
    ++m.faults;
    m.AddLog("crash caught in " + where);
    if (m.faults >= 3) {
        m.enabled = false;
        m.errors.push_back("turned off after repeated crashes (" + where + ")");
        g_dirty = true;
    }
}

void CallPlugin(Mod& m, int what, const char* action, float dt) {
    if (!m.module) return;
    PluginCall c{&m, what, action, dt, nullptr, 0};
    g_pluginMod = &m;
    int fault = Guarded(PluginCallRaw, &c);
    g_pluginMod = nullptr;
    if (fault) DisableForFault(m, "plugin");
}

void LoadPlugin(Mod& m) {
    if (m.module || m.pluginFile.empty() || !m.trustNative) return;
    std::wstring path = Wide(m.folder + m.pluginFile);
    HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        m.errors.push_back("could not load " + m.pluginFile + " (error " + std::to_string(GetLastError()) + ")");
        m.enabled = false;
        return;
    }
    auto init = reinterpret_cast<DmlInitFn>(GetProcAddress(h, DML_INIT_EXPORT));
    if (!init) {
        FreeLibrary(h);
        m.errors.push_back(m.pluginFile + " does not export DmlInit");
        m.enabled = false;
        return;
    }
    m.module = h;
    m.plugin = DmlPlugin{};
    if (!g_host.apiVersion) g_host = MakeHost();
    PluginCall c{&m, 0, nullptr, 0, init, 0};
    g_pluginMod = &m;
    int fault = Guarded(PluginCallRaw, &c);
    g_pluginMod = nullptr;
    if (fault || !c.initResult || m.plugin.apiVersion < 1 || m.plugin.apiVersion > DML_API_VERSION) {
        m.module = nullptr;
        FreeLibrary(h);
        m.errors.push_back(fault ? "plugin crashed while starting" : "plugin refused to load or has a wrong API version");
        m.enabled = false;
        return;
    }
    m.AddLog("plugin loaded: " + m.pluginFile);
}

void UnloadPlugin(Mod& m) {
    if (!m.module) return;
    for (auto it = g_claimed.begin(); it != g_claimed.end();)  // its attributes are no longer managed
        if (it->second == m.id) it = g_claimed.erase(it);
        else ++it;
    if (m.pluginEnabled) CallPlugin(m, 2);
    m.pluginEnabled = false;
    FreeLibrary(m.module);
    m.module = nullptr;
    m.AddLog("plugin unloaded");
}

// --- main tick ---------------------------------------------------------------
struct TickArg {
    Mod* m;
    bool globalOn;
};

void TickModRaw(void* arg);

void ApplyRegistries() {
    // Attributes that the mod loader manages itself (not built-in multipliers).
    for (auto it = g_attrs.begin(); it != g_attrs.end();) {
        AttrState& s = it->second;
        float base, cur;
        if (!core::GetAttr(it->first.c_str(), base, cur)) {
            ++it;
            continue;
        }
        bool managing = !std::isnan(s.last);
        if (managing && fabsf(base - s.last) > 1e-4f) managing = false, s.last = NAN;  // the game rewrote it
        if (!managing) s.orig = base;
        if (s.contribs.empty()) {
            if (managing) core::SetAttr(it->first.c_str(), s.orig, cur + (s.orig - base));
            it = g_attrs.erase(it);
            continue;
        }
        float target = (float)Combine(s.orig, s.contribs);
        if (fabsf(base - target) > 1e-5f) core::SetAttr(it->first.c_str(), target, cur + (target - base));
        s.last = target;
        s.contribs.clear();
        ++it;
    }
    // Property values.
    for (auto it = g_leaves.begin(); it != g_leaves.end();) {
        LeafState& st = it->second;
        if (!ue::IsLive(st.owner, st.ownerIdx)) {  // object unloaded: forget it, never touch its memory
            it = g_leaves.erase(it);
            continue;
        }
        Leaf l = st.cur;
        if (!st.hasCur) {  // nobody contributed this tick: find the value again to give it back
            std::vector<Leaf> now = WalkPath(st.owner, st.path, nullptr);
            if (st.ordinal >= now.size() || now[st.ordinal].p != it->first) {  // gone or moved: leave it alone
                it = g_leaves.erase(it);
                continue;
            }
            l = now[st.ordinal];
        }
        double now = ReadLeaf(l);
        bool ours = !std::isnan(st.last) && Same(now, st.last);
        if (!ours) {
            if (!std::isnan(st.last)) {  // we had written it and something else changed it
                DWORD t = GetTickCount();
                if (t - st.windowStart > 3000) st.windowStart = t, st.rewrites = 0;
                if (++st.rewrites > 8) {
                    st.blockedUntil = t + 30000;
                    st.rewrites = 0;
                    for (auto& c : st.contribs)
                        for (auto& m : g_mods)
                            if (m.get() == c.src)
                                m->AddLog("the game keeps changing a value of this mod; it is left alone for 30 s");
                }
            }
            st.orig = now;  // first time, or the game rewrote it
        }
        if (st.blockedUntil && GetTickCount() < st.blockedUntil) {
            st.last = NAN;  // not ours while blocked
            st.contribs.clear();
            st.hasCur = false;
            ++it;
            continue;
        }
        st.blockedUntil = 0;
        if (st.contribs.empty()) {
            if (ours) WriteLeaf(l, st.orig);  // nobody wants a change any more: the game's value comes back
            it = g_leaves.erase(it);
            continue;
        }
        double target = Combine(st.orig, st.contribs);
        if (!Same(now, target)) WriteLeaf(l, target);
        st.last = ReadLeaf(l);  // as stored (int rounding, bool)
        st.contribs.clear();
        st.hasCur = false;
        ++it;
    }
}

void TickModRaw(void* arg) {
    auto* a = static_cast<TickArg*>(arg);
    Mod& m = *a->m;
    bool on = a->globalOn && m.enabled && !m.failed;
    // plugin lifecycle
    if (on && !m.pluginFile.empty() && m.trustNative && !m.module) LoadPlugin(m);
    if (m.module) {
        if (on && !m.pluginEnabled) CallPlugin(m, 1), m.pluginEnabled = true;
        if (!on && m.pluginEnabled) CallPlugin(m, 2), m.pluginEnabled = false;
    }
    for (auto& e : m.effects) {
        bool active = on && WhenTrue(m, e.when);
        if (active && e.kind != Effect::Lock && e.kind != Effect::Console && !std::isfinite(Eval(m, e.value))) {
            e.status = "value is not a finite number: ignored";
            active = false;
        }
        e.active = active;
        switch (e.kind) {
        case Effect::Attribute: {
            if (!active) break;
            double v = Eval(m, e.value);
            auto claim = g_claimed.find(e.attr);
            if (claim != g_claimed.end()) {
                if (e.mode != Effect::Multiply) {
                    e.status = "\"" + e.attr + "\" is managed by " + claim->second + ": only mode \"multiply\" works";
                    break;
                }
                auto f = g_attrFactors.try_emplace(e.attr, 1.0f).first;  // product starts at 1
                f->second *= (float)v;
                e.status = "x" + std::to_string(v).substr(0, 5) + " (combined with " + claim->second + ")";
            } else {
                float b, c;
                if (!core::GetAttr(e.attr.c_str(), b, c)) {
                    e.status = "attribute not available yet (enter the game) or unknown";
                    break;
                }
                g_attrs[e.attr].contribs.push_back({e.mode, v, &m, m.id});
                e.status = "ok";
            }
            break;
        }
        case Effect::Lock: {
            if (!active) break;
            float vb, vc, mb, mc;
            if (!core::GetAttr(e.attr.c_str(), vb, vc) || !core::GetAttr(e.maxAttr.c_str(), mb, mc)) {
                e.status = "attribute not available yet (enter the game) or unknown";
                break;
            }
            if (mc > 0 && vc < mc) core::SetAttr(e.attr.c_str(), mc, mc);
            e.status = "ok";
            break;
        }
        case Effect::Console:
            if (active != e.consoleApplied) {
                const std::string& cmd = active ? e.on : e.off;
                if (!cmd.empty()) m.AddLog((core::ExecConsoleCommand(cmd) ? "console: " : "console failed: ") + cmd);
                e.consoleApplied = active;
            }
            e.status = active ? "on" : "off";
            break;
        case Effect::Property: {
            if (!active) {
                e.objs.clear();  // their leaves get no contribution and are restored
                e.status.clear();
                break;
            }
            ResolveProperty(m, e);
            double v = Eval(m, e.value);
            size_t n = 0;
            std::string err;
            for (auto& o : e.objs) n += Contribute(o.obj, e.path, e.pathText, e.mode, v, &m, m.id, &err);
            if (n) e.status = std::to_string(n) + " value(s) in " + std::to_string(e.objs.size()) + " object(s)";
            else if (!err.empty()) e.status = err;
            else e.status = e.objs.empty() ? (e.target.empty() ? "waiting for " + e.cls : "target not found") : "no values on this path";
            break;
        }
        }
    }
    if (m.module && m.pluginEnabled) {
        DWORD now = GetTickCount();
        if (!m.lastTick) m.lastTick = now;
        CallPlugin(m, 4, nullptr, (now - m.lastTick) / 1000.0f);
        m.lastTick = now;
    }
}

void TickImpl() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_reload) {
        g_reload = false;
        for (auto& m : g_mods) {
            m->enabled = false;  // switch everything off so values return to the game's
        }
        for (auto& m : g_mods) {
            TickArg a{m.get(), false};
            Guarded(TickModRaw, &a);
            UnloadPlugin(*m);
        }
        g_attrFactors.clear();
        ApplyRegistries();  // nothing contributes now: restore originals
        LoadAll();
        Log("mods reloaded: %zu", g_mods.size());
    }
    // Queued input (panel buttons, hotkeys)
    while (!g_buttonQueue.empty()) {
        auto [mid, cid] = g_buttonQueue.front();
        g_buttonQueue.pop_front();
        for (auto& m : g_mods)
            if (m->id == mid && m->enabled && !m->failed)
                if (Control* c = m->FindControl(cid)) RunSteps(*m, c->steps);
    }
    while (!g_keyQueue.empty()) {
        unsigned vk = g_keyQueue.front();
        g_keyQueue.pop_front();
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
        for (auto& m : g_mods) {
            if (!m->enabled || m->failed || !core::ModsEnabled()) continue;
            for (auto& hk : m->hotkeys) {
                if (hk.vk != vk || hk.ctrl != ctrl || hk.shift != shift || hk.alt != alt) continue;
                if (!hk.toggle.empty())
                    if (Control* c = m->FindControl(hk.toggle)) {
                        c->value = c->value != 0 ? 0 : 1;
                        g_dirty = true;
                        m->AddLog("hotkey " + hk.text + ": " + hk.toggle + (c->value != 0 ? " on" : " off"));
                    }
                RunSteps(*m, hk.steps);
            }
        }
    }
    // Developer console
    while (!g_devQueue.empty()) {
        std::string line = g_devQueue.front();
        g_devQueue.pop_front();
        std::string out = dev::Exec(line);
        g_devLog.push_back("> " + line);
        size_t start = 0;
        while (start < out.size()) {
            size_t nl = out.find('\n', start);
            if (nl == std::string::npos) nl = out.size();
            if (nl > start) g_devLog.push_back(out.substr(start, nl - start));
            start = nl + 1;
        }
        while (g_devLog.size() > 400) g_devLog.pop_front();
    }

    g_attrFactors.clear();
    bool globalOn = core::ModsEnabled();
    for (auto& m : g_mods) {
        TickArg a{m.get(), globalOn};
        if (Guarded(TickModRaw, &a)) DisableForFault(*m, "effects");
    }
    // Conflict hints: values changed by more than one mod.
    for (auto& m : g_mods) m->warnings.erase(std::remove_if(m->warnings.begin(), m->warnings.end(),
                                                            [](const std::string& w) { return w.rfind("shares", 0) == 0; }),
                                             m->warnings.end());
    for (auto& kv : g_leaves) {
        auto& cs = kv.second.contribs;
        for (auto& a : cs)
            for (auto& b : cs) {
                if (a.src == b.src) continue;
                for (auto& m : g_mods)
                    if (m.get() == a.src) {
                        std::string w = "shares values with \"" + b.srcName + "\" (both apply)";
                        if (std::find(m->warnings.begin(), m->warnings.end(), w) == m->warnings.end()) m->warnings.push_back(w);
                    }
            }
    }
    ApplyRegistries();
    g_attrFactorsDone = g_attrFactors;

    static DWORD lastSave = 0;
    if (g_dirty && GetTickCount() - lastSave > 1500) {
        lastSave = GetTickCount();
        g_dirty = false;
        SaveSettings();
    }
}

// --- plugin tabs and commands ------------------------------------------------
struct TabRef {
    Mod* m;
    int index;
};
int CallPluginInt(Mod& m, int what, int tab, const char** name = nullptr) {
    PluginCall c{&m, what, nullptr, 0, nullptr, 0};
    c.tab = tab;
    Mod* prev = g_pluginMod;
    g_pluginMod = &m;
    int fault = Guarded(PluginCallRaw, &c);
    g_pluginMod = prev;
    if (fault) {
        DisableForFault(m, "plugin panel");
        return -1;
    }
    if (name) *name = c.action;
    return c.initResult;
}
// The panel's ImGui context may not exist yet when a plugin loads; it is handed over here,
// on the panel thread, before any plugin draws.
void RefreshHostImGui() {
    if (!g_host.apiVersion || g_host.imguiContext == ImGui::GetCurrentContext()) return;
    g_host.imguiContext = ImGui::GetCurrentContext();
    ImGuiMemAllocFunc a;
    ImGuiMemFreeFunc f;
    void* user;
    ImGui::GetAllocatorFunctions(&a, &f, &user);
    g_host.imguiAlloc = a;
    g_host.imguiFree = f;
    g_host.imguiAllocUser = user;
}
std::vector<TabRef> CollectTabs() {
    RefreshHostImGui();
    std::vector<TabRef> tabs;
    for (auto& mp : g_mods) {
        Mod& m = *mp;
        if (!m.module || !m.pluginEnabled || !m.enabled || m.plugin.apiVersion < 2 || !m.plugin.GetTabCount || !m.plugin.DrawTab)
            continue;
        int n = CallPluginInt(m, 7, 0);
        for (int i = 0; i < n && i < 16; ++i) tabs.push_back({&m, i});
    }
    return tabs;
}

// --- UI helpers ---------------------------------------------------------------
void Tip(const std::string& logical) {
    if (logical.empty() || !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    ImGui::SetTooltip("%s", i18n::Wrap(logical, ImGui::GetFontSize() * 26.0f).c_str());
}

}  // namespace

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------
void Init() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    LoadAll();
    Log("mod loader %s: %zu mod(s) in %s", kVersion, g_mods.size(), Narrow(ModsDir()).c_str());
}

void Tick() {
    // Runs inside the game's own fault guard as well; TickImpl uses C++ objects, so it
    // cannot have its own __try. Per-mod work below is guarded individually.
    TickImpl();
}

void Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (auto& m : g_mods) UnloadPlugin(*m);
    if (g_dirty) SaveSettings();
}

void LoadEnabledPlugins() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!core::ModsEnabled()) return;
    for (auto& m : g_mods)
        if (!m->failed && m->enabled && m->trustNative && !m->pluginFile.empty() && !m->module) {
            LoadPlugin(*m);
            Log("plugin of %s loaded early: %s", m->id.c_str(), m->module ? "ok" : "failed");
        }
}

int PluginTabCount() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return (int)CollectTabs().size();
}

std::string PluginTabName(int i) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto tabs = CollectTabs();
    if (i < 0 || i >= (int)tabs.size()) return "?";
    const char* name = nullptr;
    CallPluginInt(*tabs[i].m, 8, tabs[i].index, &name);
    return name && *name ? name : tabs[i].m->id;
}

void DrawPluginTab(int i) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto tabs = CollectTabs();
    if (i < 0 || i >= (int)tabs.size()) return;
    CallPluginInt(*tabs[i].m, 5, tabs[i].index);
}

bool PluginCommand(const std::string& line, std::string& out) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    static std::vector<char> buf(64 * 1024);
    for (auto& mp : g_mods) {
        Mod& m = *mp;
        if (!m.module || m.plugin.apiVersion < 2 || !m.plugin.OnCommand) continue;
        buf[0] = 0;
        PluginCall c{&m, 6, nullptr, 0, nullptr, 0};
        c.line = line.c_str();
        c.out = buf.data();
        c.outSize = (int)buf.size();
        Mod* prev = g_pluginMod;
        g_pluginMod = &m;
        int fault = Guarded(PluginCallRaw, &c);
        g_pluginMod = prev;
        if (fault) {
            DisableForFault(m, "plugin command");
            continue;
        }
        if (c.initResult) {
            buf[buf.size() - 1] = 0;
            out = buf.data();
            return true;
        }
    }
    return false;
}

void OnKeyDown(unsigned vk) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (auto& m : g_mods)
        for (auto& hk : m->hotkeys)
            if (hk.vk == vk) {
                g_keyQueue.push_back(vk);
                return;
            }
}

std::string Report() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    std::string r = "loader " + std::string(kVersion) + ", " + std::to_string(g_mods.size()) + " mod(s)";
    for (auto& m : g_mods) {
        r += "\n  [" + std::string(m->failed ? "FAILED" : m->enabled ? "on" : "off") + "] " + m->id + " (" + m->folderName + ")";
        for (auto& c : m->controls)
            if (c.kind != Control::Button) r += " " + c.id + "=" + std::to_string(c.value).substr(0, 7);
        for (auto& e : m->errors) r += "\n    error: " + e;
        for (auto& w : m->warnings) r += "\n    warning: " + w;
        for (size_t i = 0; i < m->effects.size(); ++i)
            r += "\n    effect " + std::to_string(i + 1) + (m->effects[i].active ? " active: " : " idle: ") + m->effects[i].status;
        for (auto& l : m->log) r += "\n    log: " + l;
    }
    for (auto& kv : g_attrFactors) r += "\n  factor " + kv.first + " = " + std::to_string(kv.second);
    r += "\n  managed values: " + std::to_string(g_leaves.size()) + " properties, " + std::to_string(g_attrs.size()) + " attributes";
    return r;
}

bool SetModEnabled(const std::string& id, bool on) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (auto& m : g_mods)
        if (m->id == id && !m->failed) {
            m->enabled = on;
            if (on) m->faults = 0;
            g_dirty = true;
            return true;
        }
    return false;
}

bool SetControl(const std::string& id, const std::string& control, double value) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (auto& m : g_mods)
        if (m->id == id)
            if (Control* c = m->FindControl(control)) {
                c->value = std::clamp(value, c->min, c->max);
                g_dirty = true;
                return true;
            }
    return false;
}

bool PressButton(const std::string& id, const std::string& control) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_buttonQueue.emplace_back(id, control);
    return true;
}

void RequestReload() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_reload = true;
}

void SetTrustNative(const std::string& id, bool on) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (auto& m : g_mods)
        if (m->id == id) {
            m->trustNative = on;
            g_dirty = true;
        }
}

void DrawModsTab() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    ImGui::TextUnformatted(i18n::F("Mod loader %s — %d mod(s)", kVersion, (int)g_mods.size()).c_str());
    if (ImGui::Button(T("Reload mods"))) g_reload = true;
    Tip(TL("Reads every mod.json again. Values changed by mods go back to the game's original first."));
    ImGui::SameLine();
    if (ImGui::Button(T("Open mods folder"))) {
        CreateDirectoryW(ModsDir().c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", ModsDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::Separator();
    if (g_mods.empty()) {
        ImGui::TextDisabled("%s", i18n::Wrap(TL("No mods installed. Put a mod folder (with its mod.json) into the Mods folder and press Reload mods."),
                                             ImGui::GetContentRegionAvail().x)
                                      .c_str());
        return;
    }
    const float w = ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 13.0f;
    for (auto& mp : g_mods) {
        Mod& m = *mp;
        ImGui::PushID(m.folderName.c_str());
        bool on = m.enabled;
        ImGui::BeginDisabled(m.failed);
        if (ImGui::Checkbox("##on", &on)) {
            m.enabled = on;
            if (on) m.faults = 0;
            g_dirty = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        std::string title = i18n::Shape(m.name.Get()) + "  v" + m.version + (m.author.empty() ? "" : "  · " + m.author);
        ImVec4 col = m.failed ? ImVec4{0.95f, 0.4f, 0.35f, 1} : m.enabled ? ImVec4{0.45f, 0.85f, 0.45f, 1} : ImVec4{0.8f, 0.8f, 0.8f, 1};
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        bool open = ImGui::CollapsingHeader((title + "###hdr").c_str(), m.enabled || m.failed ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        ImGui::PopStyleColor();
        if (open) {
            ImGui::Indent();
            if (!m.description.empty())
                ImGui::TextWrapped("%s", i18n::Wrap(m.description.Get(), ImGui::GetContentRegionAvail().x).c_str());
            for (auto& e : m.errors) ImGui::TextColored({0.95f, 0.4f, 0.35f, 1}, "%s", e.c_str());
            for (auto& wn : m.warnings) ImGui::TextColored({0.95f, 0.75f, 0.3f, 1}, "%s", wn.c_str());
            if (!m.pluginFile.empty()) {
                if (m.enabled && !m.trustNative)
                    ImGui::TextColored({0.95f, 0.75f, 0.3f, 1}, "%s",
                                       i18n::Wrap(TL("This mod needs \"Allow native code\" to run."), ImGui::GetContentRegionAvail().x).c_str());
                bool trust = m.trustNative;
                if (ImGui::Checkbox(T("Allow native code (plugin DLL)"), &trust)) {
                    m.trustNative = trust;
                    g_dirty = true;
                }
                Tip(TL("This mod contains a program file. Only allow it if you trust its author: native code can do anything on your PC."));
                if (m.module) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", T("(loaded)"));
                }
            }
            ImGui::BeginDisabled(!m.enabled || m.failed);
            for (auto& c : m.controls) {
                ImGui::PushID(c.id.c_str());
                std::string label = i18n::Shape(c.label.Get());
                switch (c.kind) {
                case Control::Toggle: {
                    bool v = c.value != 0;
                    if (ImGui::Checkbox(label.c_str(), &v)) c.value = v ? 1 : 0, g_dirty = true;
                    Tip(c.tip.Get());
                    break;
                }
                case Control::Button:
                    if (ImGui::Button(label.c_str())) g_buttonQueue.emplace_back(m.id, c.id);
                    Tip(c.tip.Get());
                    break;
                case Control::Slider:
                case Control::IntSlider: {
                    ImGui::SetNextItemWidth(w);
                    bool changed;
                    if (c.kind == Control::IntSlider) {
                        int v = (int)std::lround(c.value);
                        changed = ImGui::SliderInt("##v", &v, (int)c.min, (int)c.max, c.format.c_str(),
                                                   c.log ? ImGuiSliderFlags_Logarithmic : 0);
                        if (changed) c.value = v;
                    } else {
                        float v = (float)c.value;
                        changed = ImGui::SliderFloat("##v", &v, (float)c.min, (float)c.max, c.format.c_str(),
                                                     c.log ? ImGuiSliderFlags_Logarithmic : 0);
                        if (changed) c.value = v;
                    }
                    if (changed) g_dirty = true;
                    Tip(c.tip.Get());
                    ImGui::SameLine();
                    if (ImGui::SmallButton(T("reset"))) c.value = c.def, g_dirty = true;
                    ImGui::SameLine();
                    ImGui::TextUnformatted(label.c_str());
                    break;
                }
                }
                ImGui::PopID();
            }
            ImGui::EndDisabled();
            if (!m.hotkeys.empty()) {
                std::string keys;
                for (auto& hk : m.hotkeys) keys += (keys.empty() ? "" : ", ") + hk.text;
                ImGui::TextDisabled("%s", i18n::F("Hotkeys: %s", keys.c_str()).c_str());
            }
            if (m.enabled && !m.effects.empty() && ImGui::TreeNode(T("Details"))) {
                for (size_t i = 0; i < m.effects.size(); ++i)
                    ImGui::TextDisabled("%zu. %s — %s", i + 1, m.effects[i].active ? "on" : "off", m.effects[i].status.c_str());
                for (auto& l : m.log) ImGui::TextDisabled("%s", l.c_str());
                ImGui::TreePop();
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
}

void DrawDeveloperTab() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    ImGui::TextDisabled("%s", i18n::Wrap(TL("Inspect and change the running game. Commands: find <Class>, props <target> [filter], get/set "
                                              "<target> <prop> [value], sig <Class> <Function>, call <target> <Function> [p=v], "
                                              "attrs [filter], pos, tp x y z, exec <console>, names <text>, qol. Targets: pawn, pc, ps, "
                                              "world, cdo:Class, first:Class, near:Class, idx:N, a.b.c"),
                                         ImGui::GetContentRegionAvail().x)
                                  .c_str());
    static char line[512] = "";
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 6);
    bool enter = ImGui::InputText("##devcmd", line, sizeof(line), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button(T("Run")) || enter) && line[0]) {
        g_devQueue.push_back(line);
        line[0] = 0;
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Clear"))) g_devLog.clear();
    if (ImGui::BeginChild("devlog", {0, 0}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        for (auto& l : g_devLog) ImGui::TextUnformatted(l.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

}  // namespace modloader
