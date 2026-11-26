#include "json.h"

#include <cwchar>

namespace gf {

class JsonParser {
public:
    explicit JsonParser(const std::wstring& text) : text_(text) {}

    bool Parse(JsonValue& out) {
        SkipSpace();
        if (!ParseValue(out)) {
            return false;
        }
        SkipSpace();
        if (pos_ != text_.size()) {
            return Fail(L"trailing characters");
        }
        return true;
    }

    const std::wstring& error() const { return error_; }

private:
    bool Fail(const wchar_t* message) {
        if (error_.empty()) {
            error_ = message;
        }
        return false;
    }

    bool Eof() const { return pos_ >= text_.size(); }
    wchar_t Peek() const { return Eof() ? L'\0' : text_[pos_]; }
    wchar_t Next() { return Eof() ? L'\0' : text_[pos_++]; }

    void SkipSpace() {
        while (!Eof()) {
            const wchar_t ch = text_[pos_];
            if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool Literal(const wchar_t* word) {
        const size_t length = wcslen(word);
        if (text_.compare(pos_, length, word) != 0) {
            return false;
        }
        pos_ += length;
        return true;
    }

    bool ParseValue(JsonValue& out) {
        SkipSpace();
        if (Eof()) {
            return Fail(L"unexpected end of input");
        }
        switch (Peek()) {
            case L'{':
                return ParseObject(out);
            case L'[':
                return ParseArray(out);
            case L'"': {
                out.type = JsonValue::Type::String;
                return ParseString(out.text);
            }
            case L't':
                if (Literal(L"true")) {
                    out.type = JsonValue::Type::Bool;
                    out.boolean = true;
                    return true;
                }
                return Fail(L"invalid literal");
            case L'f':
                if (Literal(L"false")) {
                    out.type = JsonValue::Type::Bool;
                    out.boolean = false;
                    return true;
                }
                return Fail(L"invalid literal");
            case L'n':
                if (Literal(L"null")) {
                    out.type = JsonValue::Type::Null;
                    return true;
                }
                return Fail(L"invalid literal");
            default:
                return ParseNumber(out);
        }
    }

    bool ParseObject(JsonValue& out) {
        out.type = JsonValue::Type::Object;
        ++pos_;  // '{'
        SkipSpace();
        if (Peek() == L'}') {
            ++pos_;
            return true;
        }
        for (;;) {
            SkipSpace();
            if (Peek() != L'"') {
                return Fail(L"expected a key");
            }
            std::wstring key;
            if (!ParseString(key)) {
                return false;
            }
            SkipSpace();
            if (Next() != L':') {
                return Fail(L"expected ':'");
            }
            JsonValue value;
            if (!ParseValue(value)) {
                return false;
            }
            out.object.emplace_back(std::move(key), std::move(value));
            SkipSpace();
            const wchar_t separator = Next();
            if (separator == L',') {
                continue;
            }
            if (separator == L'}') {
                return true;
            }
            return Fail(L"expected ',' or '}'");
        }
    }

    bool ParseArray(JsonValue& out) {
        out.type = JsonValue::Type::Array;
        ++pos_;  // '['
        SkipSpace();
        if (Peek() == L']') {
            ++pos_;
            return true;
        }
        for (;;) {
            JsonValue value;
            if (!ParseValue(value)) {
                return false;
            }
            out.array.push_back(std::move(value));
            SkipSpace();
            const wchar_t separator = Next();
            if (separator == L',') {
                continue;
            }
            if (separator == L']') {
                return true;
            }
            return Fail(L"expected ',' or ']'");
        }
    }

    bool ParseString(std::wstring& out) {
        out.clear();
        ++pos_;  // '"'
        while (!Eof()) {
            wchar_t ch = Next();
            if (ch == L'"') {
                return true;
            }
            if (ch != L'\\') {
                out.push_back(ch);
                continue;
            }
            if (Eof()) {
                return Fail(L"unterminated escape");
            }
            const wchar_t escape = Next();
            switch (escape) {
                case L'"':
                    out.push_back(L'"');
                    break;
                case L'\\':
                    out.push_back(L'\\');
                    break;
                case L'/':
                    out.push_back(L'/');
                    break;
                case L'b':
                    out.push_back(L'\b');
                    break;
                case L'f':
                    out.push_back(L'\f');
                    break;
                case L'n':
                    out.push_back(L'\n');
                    break;
                case L'r':
                    out.push_back(L'\r');
                    break;
                case L't':
                    out.push_back(L'\t');
                    break;
                case L'u': {
                    unsigned code = 0;
                    if (!ParseHex4(code)) {
                        return Fail(L"invalid \\u escape");
                    }
                    // Combine a UTF-16 surrogate pair when present.
                    if (code >= 0xD800 && code <= 0xDBFF && pos_ + 1 < text_.size() &&
                        text_[pos_] == L'\\' && text_[pos_ + 1] == L'u') {
                        pos_ += 2;
                        unsigned low = 0;
                        if (!ParseHex4(low)) {
                            return Fail(L"invalid low surrogate");
                        }
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    if (code <= 0xFFFF) {
                        out.push_back(static_cast<wchar_t>(code));
                    } else {
                        code -= 0x10000;
                        out.push_back(static_cast<wchar_t>(0xD800 + (code >> 10)));
                        out.push_back(static_cast<wchar_t>(0xDC00 + (code & 0x3FF)));
                    }
                    break;
                }
                default:
                    return Fail(L"invalid escape");
            }
        }
        return Fail(L"unterminated string");
    }

    bool ParseHex4(unsigned& value) {
        value = 0;
        for (int i = 0; i < 4; ++i) {
            if (Eof()) {
                return false;
            }
            const wchar_t ch = Next();
            unsigned digit = 0;
            if (ch >= L'0' && ch <= L'9') {
                digit = static_cast<unsigned>(ch - L'0');
            } else if (ch >= L'a' && ch <= L'f') {
                digit = static_cast<unsigned>(ch - L'a' + 10);
            } else if (ch >= L'A' && ch <= L'F') {
                digit = static_cast<unsigned>(ch - L'A' + 10);
            } else {
                return false;
            }
            value = (value << 4) | digit;
        }
        return true;
    }

    bool ParseNumber(JsonValue& out) {
        const size_t start = pos_;
        if (Peek() == L'-') {
            ++pos_;
        }
        bool digits = false;
        while (!Eof() && Peek() >= L'0' && Peek() <= L'9') {
            ++pos_;
            digits = true;
        }
        if (Peek() == L'.') {
            ++pos_;
            while (!Eof() && Peek() >= L'0' && Peek() <= L'9') {
                ++pos_;
                digits = true;
            }
        }
        if (Peek() == L'e' || Peek() == L'E') {
            ++pos_;
            if (Peek() == L'+' || Peek() == L'-') {
                ++pos_;
            }
            while (!Eof() && Peek() >= L'0' && Peek() <= L'9') {
                ++pos_;
            }
        }
        if (!digits) {
            return Fail(L"expected a value");
        }
        out.type = JsonValue::Type::Number;
        out.number = wcstod(text_.substr(start, pos_ - start).c_str(), nullptr);
        return true;
    }

    const std::wstring& text_;
    size_t pos_ = 0;
    std::wstring error_;
};
bool ParseJson(const std::wstring& text, JsonValue& out, std::wstring& error) {
    JsonParser parser(text);
    if (!parser.Parse(out)) {
        error = parser.error();
        return false;
    }
    error.clear();
    return true;
}
void AppendJsonString(std::wstring& out, const std::wstring& text) {
    out.push_back(L'"');
    for (wchar_t ch : text) {
        switch (ch) {
            case L'"':
                out += L"\\\"";
                break;
            case L'\\':
                out += L"\\\\";
                break;
            case L'\n':
                out += L"\\n";
                break;
            case L'\r':
                out += L"\\r";
                break;
            case L'\t':
                out += L"\\t";
                break;
            default:
                if (ch < 0x20) {
                    wchar_t buffer[8] = {};
                    swprintf_s(buffer, L"\\u%04X", static_cast<unsigned>(ch));
                    out += buffer;
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    out.push_back(L'"');
}
}  // namespace gf
