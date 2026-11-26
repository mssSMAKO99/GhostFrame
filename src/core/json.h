#pragma once

#include <string>
#include <utility>
#include <vector>

// A minimal, dependency-free JSON reader shared by the settings document and the
// shortcut manifest.  It is intentionally strict: the caller treats a parse
// failure as a corrupt file and falls back to defaults.
namespace gf {

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::wstring text;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::wstring, JsonValue>> object;

    const JsonValue* Get(const wchar_t* key) const {
        if (type != Type::Object) {
            return nullptr;
        }
        for (const auto& entry : object) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        return nullptr;
    }
};

// Parses a complete document.  Returns false (with `error`) on malformed input.
bool ParseJson(const std::wstring& text, JsonValue& out, std::wstring& error);

// Appends a quoted, escaped JSON string (including both quotes).
void AppendJsonString(std::wstring& out, const std::wstring& text);

}  // namespace gf
