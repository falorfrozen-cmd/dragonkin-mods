#include "json.h"
#include <cstdlib>
#include <cstring>

namespace json {
namespace {

struct Parser {
    const std::string& t;
    size_t i = 0;
    int line = 1, col = 1;
    std::string err;
    int depth = 0;

    explicit Parser(const std::string& text) : t(text) {}

    bool Fail(const std::string& what) {
        if (err.empty()) err = "line " + std::to_string(line) + ", column " + std::to_string(col) + ": " + what;
        return false;
    }
    char Peek() const { return i < t.size() ? t[i] : '\0'; }
    char Next() {
        char c = t[i++];
        if (c == '\n') ++line, col = 1;
        else ++col;
        return c;
    }
    bool SkipSpace() {
        for (;;) {
            while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) Next();
            if (i + 1 < t.size() && t[i] == '/' && t[i + 1] == '/') {
                while (i < t.size() && t[i] != '\n') Next();
                continue;
            }
            if (i + 1 < t.size() && t[i] == '/' && t[i + 1] == '*') {
                Next(), Next();
                while (i + 1 < t.size() && !(t[i] == '*' && t[i + 1] == '/')) Next();
                if (i + 1 >= t.size()) return Fail("unterminated comment");
                Next(), Next();
                continue;
            }
            return true;
        }
    }
    static void AppendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) out += (char)(0xC0 | (cp >> 6)), out += (char)(0x80 | (cp & 0x3F));
        else if (cp < 0x10000)
            out += (char)(0xE0 | (cp >> 12)), out += (char)(0x80 | ((cp >> 6) & 0x3F)), out += (char)(0x80 | (cp & 0x3F));
        else
            out += (char)(0xF0 | (cp >> 18)), out += (char)(0x80 | ((cp >> 12) & 0x3F)),
                out += (char)(0x80 | ((cp >> 6) & 0x3F)), out += (char)(0x80 | (cp & 0x3F));
    }
    bool Hex4(unsigned& v) {
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = Peek();
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return Fail("bad \\u escape");
            v = v * 16 + d;
            Next();
        }
        return true;
    }
    bool String(std::string& out) {
        Next();  // opening quote
        for (;;) {
            if (i >= t.size()) return Fail("unterminated string");
            char c = Next();
            if (c == '"') return true;
            if (c == '\n') return Fail("line break inside a string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= t.size()) return Fail("unterminated string");
            char e = Next();
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && Peek() == '\\') {
                    Next();
                    if (Next() != 'u') return Fail("bad surrogate pair");
                    unsigned lo;
                    if (!Hex4(lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                AppendUtf8(out, cp);
                break;
            }
            default: return Fail(std::string("unknown escape \\") + e);
            }
        }
    }
    bool Value_(Value& v) {
        if (!SkipSpace()) return false;
        if (++depth > 64) return Fail("nesting too deep");
        v.line = line, v.col = col;
        char c = Peek();
        bool ok = true;
        if (c == '{') {
            v.type = Value::Object;
            Next();
            for (;;) {
                if (!SkipSpace()) return false;
                if (Peek() == '}') {
                    Next();
                    break;
                }
                if (Peek() != '"') return Fail("expected a \"key\" or '}'");
                std::string key;
                if (!String(key)) return false;
                if (!SkipSpace()) return false;
                if (Peek() != ':') return Fail("expected ':' after \"" + key + "\"");
                Next();
                Value child;
                if (!Value_(child)) return false;
                for (auto& kv : v.obj)
                    if (kv.first == key) return Fail("duplicate key \"" + key + "\"");
                v.obj.emplace_back(key, std::move(child));
                if (!SkipSpace()) return false;
                if (Peek() == ',') Next();
                else if (Peek() != '}') return Fail("expected ',' or '}'");
            }
        } else if (c == '[') {
            v.type = Value::Array;
            Next();
            for (;;) {
                if (!SkipSpace()) return false;
                if (Peek() == ']') {
                    Next();
                    break;
                }
                Value child;
                if (!Value_(child)) return false;
                v.arr.push_back(std::move(child));
                if (!SkipSpace()) return false;
                if (Peek() == ',') Next();
                else if (Peek() != ']') return Fail("expected ',' or ']'");
            }
        } else if (c == '"') {
            v.type = Value::String;
            ok = String(v.s);
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            v.type = Value::Number;
            const char* start = t.c_str() + i;
            char* end = nullptr;
            v.n = strtod(start, &end);
            if (end == start) return Fail("bad number");
            for (const char* p = start; p < end; ++p) Next();
        } else if (t.compare(i, 4, "true") == 0) {
            v.type = Value::Bool, v.b = true;
            for (int k = 0; k < 4; ++k) Next();
        } else if (t.compare(i, 5, "false") == 0) {
            v.type = Value::Bool;
            for (int k = 0; k < 5; ++k) Next();
        } else if (t.compare(i, 4, "null") == 0) {
            for (int k = 0; k < 4; ++k) Next();
        } else {
            return Fail(c ? std::string("unexpected character '") + c + "'" : "unexpected end of file");
        }
        --depth;
        return ok;
    }
};

}  // namespace

const Value* Value::get(const std::string& key) const {
    for (auto& kv : obj)
        if (kv.first == key) return &kv.second;
    return nullptr;
}
std::string Value::str(const std::string& key, const std::string& def) const {
    const Value* v = get(key);
    return v && v->is(String) ? v->s : def;
}
double Value::num(const std::string& key, double def) const {
    const Value* v = get(key);
    return v && v->is(Number) ? v->n : def;
}
bool Value::boolean(const std::string& key, bool def) const {
    const Value* v = get(key);
    return v && v->is(Bool) ? v->b : def;
}

bool Parse(const std::string& text, Value& out, std::string& error) {
    Parser p(text);
    size_t start = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;  // UTF-8 BOM
    p.i = start;
    out = Value{};
    if (!p.Value_(out) || !p.SkipSpace()) {
        error = p.err;
        return false;
    }
    if (p.i != text.size()) {
        p.Fail("unexpected text after the end of the document");
        error = p.err;
        return false;
    }
    return true;
}

}  // namespace json
