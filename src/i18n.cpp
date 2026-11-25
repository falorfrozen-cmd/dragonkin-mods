#include "i18n.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "imgui.h"

namespace i18n {
namespace {

struct Row {
    const char* text[LANG_COUNT];
};
const Row kRows[] = {
#include "lang_table.inc"
};

std::atomic<int> g_lang{EN};
std::mutex g_mutex;
// Per language: key -> logical text, and key -> display text. The texts live in a
// pool that never frees or moves anything, so pointers from T()/TL() stay valid.
std::deque<std::string> g_pool;
const char* Intern(std::string s) {
    g_pool.push_back(std::move(s));
    return g_pool.back().c_str();
}
std::unordered_map<std::string, const char*> g_logical[LANG_COUNT];
std::unordered_map<std::string, const char*> g_display[LANG_COUNT];
bool g_loaded = false;

void LoadTable() {
    if (g_loaded) return;
    g_loaded = true;
    for (const Row& r : kRows)
        for (int l = 0; l < LANG_COUNT; ++l)
            if (r.text[l] && *r.text[l]) g_logical[l][r.text[EN]] = r.text[l];  // static storage
}

// ---------------------------------------------------------------------------
// UTF-8 helpers
// ---------------------------------------------------------------------------
std::vector<char32_t> Decode(const std::string& s) {
    std::vector<char32_t> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        char32_t cp;
        int n;
        if (c < 0x80) cp = c, n = 1;
        else if ((c >> 5) == 6) cp = c & 0x1F, n = 2;
        else if ((c >> 4) == 14) cp = c & 0x0F, n = 3;
        else if ((c >> 3) == 30) cp = c & 0x07, n = 4;
        else { ++i; continue; }
        if (i + n > s.size()) break;
        for (int k = 1; k < n; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        out.push_back(cp);
        i += n;
    }
    return out;
}

void Append(std::string& out, char32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) out += (char)(0xC0 | (cp >> 6)), out += (char)(0x80 | (cp & 0x3F));
    else if (cp < 0x10000)
        out += (char)(0xE0 | (cp >> 12)), out += (char)(0x80 | ((cp >> 6) & 0x3F)), out += (char)(0x80 | (cp & 0x3F));
    else
        out += (char)(0xF0 | (cp >> 18)), out += (char)(0x80 | ((cp >> 12) & 0x3F)), out += (char)(0x80 | ((cp >> 6) & 0x3F)),
            out += (char)(0x80 | (cp & 0x3F));
}

// ---------------------------------------------------------------------------
// Arabic shaping: contextual forms from Arabic Presentation Forms-B.
// {isolated, final, initial, medial}; initial == 0 means the letter only joins to
// the previous letter (alef, dal, reh, waw, ...).
// ---------------------------------------------------------------------------
struct Forms {
    char32_t iso, fin, ini, med;
};
const Forms kForms[] = {
    /*0621*/ {0xFE80, 0, 0, 0},           /*0622*/ {0xFE81, 0xFE82, 0, 0},       /*0623*/ {0xFE83, 0xFE84, 0, 0},
    /*0624*/ {0xFE85, 0xFE86, 0, 0},      /*0625*/ {0xFE87, 0xFE88, 0, 0},       /*0626*/ {0xFE89, 0xFE8A, 0xFE8B, 0xFE8C},
    /*0627*/ {0xFE8D, 0xFE8E, 0, 0},      /*0628*/ {0xFE8F, 0xFE90, 0xFE91, 0xFE92}, /*0629*/ {0xFE93, 0xFE94, 0, 0},
    /*062A*/ {0xFE95, 0xFE96, 0xFE97, 0xFE98}, /*062B*/ {0xFE99, 0xFE9A, 0xFE9B, 0xFE9C}, /*062C*/ {0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0},
    /*062D*/ {0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4}, /*062E*/ {0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8}, /*062F*/ {0xFEA9, 0xFEAA, 0, 0},
    /*0630*/ {0xFEAB, 0xFEAC, 0, 0},      /*0631*/ {0xFEAD, 0xFEAE, 0, 0},       /*0632*/ {0xFEAF, 0xFEB0, 0, 0},
    /*0633*/ {0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4}, /*0634*/ {0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8}, /*0635*/ {0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC},
    /*0636*/ {0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0}, /*0637*/ {0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4}, /*0638*/ {0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8},
    /*0639*/ {0xFEC9, 0xFECA, 0xFECB, 0xFECC}, /*063A*/ {0xFECD, 0xFECE, 0xFECF, 0xFED0},
};
const Forms kForms2[] = {
    /*0641*/ {0xFED1, 0xFED2, 0xFED3, 0xFED4}, /*0642*/ {0xFED5, 0xFED6, 0xFED7, 0xFED8}, /*0643*/ {0xFED9, 0xFEDA, 0xFEDB, 0xFEDC},
    /*0644*/ {0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0}, /*0645*/ {0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4}, /*0646*/ {0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8},
    /*0647*/ {0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC}, /*0648*/ {0xFEED, 0xFEEE, 0, 0},       /*0649*/ {0xFEEF, 0xFEF0, 0, 0},
    /*064A*/ {0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4},
};

const Forms* FormsOf(char32_t c) {
    if (c >= 0x0621 && c <= 0x063A) return &kForms[c - 0x0621];
    if (c >= 0x0641 && c <= 0x064A) return &kForms2[c - 0x0641];
    return nullptr;
}
bool IsTatweel(char32_t c) { return c == 0x0640; }
bool IsTransparent(char32_t c) { return (c >= 0x064B && c <= 0x065F) || c == 0x0670; }  // harakat
bool JoinsNext(char32_t c) {  // can connect to the following letter
    if (IsTatweel(c)) return true;
    const Forms* f = FormsOf(c);
    return f && f->ini;
}
bool JoinsPrev(char32_t c) {
    if (IsTatweel(c)) return true;
    const Forms* f = FormsOf(c);
    return f && f->fin;
}
bool IsArabicLetter(char32_t c) { return FormsOf(c) || IsTatweel(c); }

std::vector<char32_t> ShapeArabic(const std::vector<char32_t>& in) {
    std::vector<char32_t> out;
    out.reserve(in.size());
    auto neighbour = [&](size_t i, int dir) -> char32_t {
        for (long long k = (long long)i + dir; k >= 0 && k < (long long)in.size(); k += dir)
            if (!IsTransparent(in[(size_t)k])) return in[(size_t)k];
        return 0;
    };
    for (size_t i = 0; i < in.size(); ++i) {
        char32_t c = in[i];
        const Forms* f = FormsOf(c);
        if (!f) {
            out.push_back(c);
            continue;
        }
        char32_t prev = neighbour(i, -1), next = neighbour(i, +1);
        bool joinPrev = f->fin && prev && JoinsNext(prev);
        // Lam + alef ligatures.
        if (c == 0x0644 && next) {
            char32_t lig = 0;
            switch (next) {
            case 0x0622: lig = 0xFEF5; break;
            case 0x0623: lig = 0xFEF7; break;
            case 0x0625: lig = 0xFEF9; break;
            case 0x0627: lig = 0xFEFB; break;
            }
            if (lig) {
                out.push_back(joinPrev ? lig + 1 : lig);
                // skip the alef (and harakat between)
                size_t k = i + 1;
                while (k < in.size() && IsTransparent(in[k])) out.push_back(in[k++]);
                i = k;
                continue;
            }
        }
        bool joinNext = f->ini && next && JoinsPrev(next);
        char32_t form = joinPrev && joinNext ? f->med : joinPrev ? f->fin : joinNext ? f->ini : f->iso;
        out.push_back(form ? form : f->iso);
    }
    return out;
}

bool IsRtlChar(char32_t c) {
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}
bool IsStrongLtr(char32_t c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c < 0x590 && c != 0xD7 && c != 0xF7) ||
           (c >= 0x2E80 && c <= 0x9FFF);
}
char32_t Mirror(char32_t c) {
    switch (c) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '<': return '>';
    case '>': return '<';
    case '{': return '}';
    case '}': return '{';
    }
    return c;
}

// Simplified bidi for one line: the paragraph is right-to-left; runs of Latin
// letters/digits (with the punctuation and single spaces inside them, and printf
// specifiers) keep their order; everything else is reversed and mirrored.
std::vector<char32_t> VisualOrder(const std::vector<char32_t>& line) {
    struct Unit {
        size_t a, b;  // [a, b)
        bool ltr;
    };
    std::vector<Unit> units;
    size_t i = 0, n = line.size();
    auto ltrStart = [&](size_t k) { return IsStrongLtr(line[k]) || (line[k] == '%' && k + 1 < n && line[k + 1] != ' '); };
    while (i < n) {
        if (ltrStart(i)) {
            size_t j = i + 1;
            while (j < n) {
                char32_t c = line[j];
                if (IsStrongLtr(c) || c == '%' || c == '.' || c == ',' || c == '_' || c == '-' || c == '+' || c == '/' ||
                    c == ':' || c == '\'' || c == '#') {
                    ++j;
                    continue;
                }
                // a single space continues the run only if more Latin follows
                if (c == ' ' && j + 1 < n && ltrStart(j + 1)) {
                    ++j;
                    continue;
                }
                break;
            }
            // trailing punctuation belongs to the surrounding (RTL) text
            while (j > i + 1 && (line[j - 1] == '.' || line[j - 1] == ',' || line[j - 1] == ':')) --j;
            units.push_back({i, j, true});
            i = j;
        } else {
            units.push_back({i, i + 1, false});
            ++i;
        }
    }
    std::vector<char32_t> out;
    out.reserve(n);
    for (auto u = units.rbegin(); u != units.rend(); ++u) {
        if (u->ltr)
            out.insert(out.end(), line.begin() + u->a, line.begin() + u->b);
        else
            out.push_back(Mirror(line[u->a]));
    }
    return out;
}

bool HasRtl(const std::vector<char32_t>& cps) {
    for (char32_t c : cps)
        if (IsRtlChar(c)) return true;
    return false;
}

std::string ShapeLine(const std::vector<char32_t>& logical) {
    std::string out;
    for (char32_t c : VisualOrder(ShapeArabic(logical))) Append(out, c);
    return out;
}

}  // namespace

void SetLanguage(int lang) {
    if (lang >= 0 && lang < LANG_COUNT) g_lang = lang;
}
int Language() { return g_lang; }

const char* LanguageCode(int lang) {
    static const char* codes[] = {"en", "tr", "fr", "de", "ar", "zh-Hant", "zh-Hans"};
    return lang >= 0 && lang < LANG_COUNT ? codes[lang] : "en";
}

int LanguageFromCode(const char* code) {
    for (int l = 0; l < LANG_COUNT; ++l)
        if (code && _stricmp(code, LanguageCode(l)) == 0) return l;
    return EN;
}

const char* LanguageName(int lang) {
    static std::string names[LANG_COUNT];
    static bool init = false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!init) {
        init = true;
        const char* raw[] = {"English", u8"Türkçe", u8"Français", "Deutsch", u8"العربية", u8"繁體中文", u8"简体中文"};
        for (int l = 0; l < LANG_COUNT; ++l) names[l] = l == AR ? ShapeLine(Decode(raw[l])) : raw[l];
    }
    return lang >= 0 && lang < LANG_COUNT ? names[lang].c_str() : names[0].c_str();
}

void AddTranslation(int lang, const std::string& key, const std::string& text) {
    if (lang < 0 || lang >= LANG_COUNT) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadTable();
    auto cur = g_logical[lang].find(key);
    if (cur != g_logical[lang].end() && text == cur->second) return;
    g_logical[lang][key] = Intern(text);
    g_display[lang].erase(key);  // only the map entry goes; the old text stays in the pool
}

const char* TL(const char* key) {
    if (!key) return "";
    int l = g_lang;
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadTable();
    auto it = g_logical[l].find(key);
    if (it != g_logical[l].end()) return it->second;
    if (l != EN) {
        auto en = g_logical[EN].find(key);
        if (en != g_logical[EN].end()) return en->second;
    }
    return key;
}

std::string Shape(const std::string& logical) {
    if (g_lang != AR) return logical;
    std::vector<char32_t> cps = Decode(logical);
    if (!HasRtl(cps)) return logical;
    std::string out;
    std::vector<char32_t> line;
    for (size_t i = 0; i <= cps.size(); ++i) {
        if (i == cps.size() || cps[i] == '\n') {
            out += ShapeLine(line);
            if (i < cps.size()) out += '\n';
            line.clear();
        } else {
            line.push_back(cps[i]);
        }
    }
    return out;
}

const char* T(const char* key) {
    if (!key) return "";
    int l = g_lang;
    const char* logical = TL(key);
    if (l != AR) return logical;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_display[l].find(key);
    if (it == g_display[l].end()) it = g_display[l].emplace(key, Intern(Shape(logical))).first;
    return it->second;
}

std::string F(const char* keyFormat, ...) {
    const char* fmt = TL(keyFormat);
    char buf[2048];
    va_list ap;
    va_start(ap, keyFormat);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return Shape(buf);
}

std::string Wrap(const std::string& logical, float width) {
    // Split into breakable pieces: words (with their trailing space) and single CJK characters.
    std::vector<char32_t> cps = Decode(logical);
    std::vector<std::vector<char32_t>> lines(1);
    std::vector<char32_t> piece;
    auto widthOf = [](const std::vector<char32_t>& v) {
        std::string s;
        for (char32_t c : v) Append(s, c);
        return ImGui::CalcTextSize(s.c_str()).x;
    };
    auto flush = [&]() {
        if (piece.empty()) return;
        std::vector<char32_t> trial = lines.back();
        trial.insert(trial.end(), piece.begin(), piece.end());
        if (!lines.back().empty() && widthOf(trial) > width) {
            while (!lines.back().empty() && lines.back().back() == ' ') lines.back().pop_back();
            lines.push_back(piece);
        } else {
            lines.back() = trial;
        }
        piece.clear();
    };
    for (char32_t c : cps) {
        bool cjk = (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF) ||
                   (c >= 0x3000 && c <= 0x303F);
        if (c == '\n') {
            flush();
            lines.emplace_back();
            continue;
        }
        if (cjk) {
            flush();
            piece.push_back(c);
            flush();
            continue;
        }
        piece.push_back(c);
        if (c == ' ') flush();
    }
    flush();
    std::string out;
    for (size_t k = 0; k < lines.size(); ++k) {
        std::string line;
        for (char32_t c : lines[k]) Append(line, c);
        out += Shape(line);
        if (k + 1 < lines.size()) out += '\n';
    }
    return out;
}

}  // namespace i18n
