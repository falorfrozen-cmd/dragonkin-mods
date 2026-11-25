#pragma once
// Small JSON reader for mod manifests: objects, arrays, strings (with \u escapes),
// numbers, true/false/null. Errors carry the line and column. Comments (// and /* */)
// and trailing commas are accepted, since manifests are written by hand.
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;  // keeps the author's order
    int line = 0, col = 0;                            // where the value starts

    bool is(Type t) const { return type == t; }
    const Value* get(const std::string& key) const;  // object member or nullptr
    std::string str(const std::string& key, const std::string& def = "") const;
    double num(const std::string& key, double def = 0) const;
    bool boolean(const std::string& key, bool def = false) const;
};

// Returns false and fills `error` ("line 3, column 7: expected ':'") on bad input.
bool Parse(const std::string& text, Value& out, std::string& error);

}  // namespace json
