#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mini_json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;

    Value() : type_(Type::Null), bool_value_(false), number_value_(0.0) {}
    Value(std::nullptr_t) : Value() {}
    Value(bool v) : type_(Type::Bool), bool_value_(v), number_value_(0.0) {}
    Value(int v) : type_(Type::Number), bool_value_(false), number_value_(static_cast<double>(v)) {}
    Value(unsigned v) : type_(Type::Number), bool_value_(false), number_value_(static_cast<double>(v)) {}
    Value(std::int64_t v) : type_(Type::Number), bool_value_(false), number_value_(static_cast<double>(v)) {}
    Value(double v) : type_(Type::Number), bool_value_(false), number_value_(v) {}
    Value(const char* v) : type_(Type::String), bool_value_(false), number_value_(0.0), string_value_(v ? v : "") {}
    Value(const std::string& v) : type_(Type::String), bool_value_(false), number_value_(0.0), string_value_(v) {}
    Value(std::string&& v) : type_(Type::String), bool_value_(false), number_value_(0.0), string_value_(std::move(v)) {}
    Value(const Array& v) : type_(Type::Array), bool_value_(false), number_value_(0.0), array_value_(v) {}
    Value(Array&& v) : type_(Type::Array), bool_value_(false), number_value_(0.0), array_value_(std::move(v)) {}
    Value(const Object& v) : type_(Type::Object), bool_value_(false), number_value_(0.0), object_value_(v) {}
    Value(Object&& v) : type_(Type::Object), bool_value_(false), number_value_(0.0), object_value_(std::move(v)) {}

    static Value array() { return Value(Array{}); }
    static Value object() { return Value(Object{}); }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool() const {
        require(Type::Bool, "bool");
        return bool_value_;
    }

    double asNumber() const {
        require(Type::Number, "number");
        return number_value_;
    }

    int asInt() const {
        const double n = asNumber();
        if (!std::isfinite(n)) throw std::runtime_error("JSON number is not finite");
        return static_cast<int>(std::llround(n));
    }

    std::int64_t asInt64() const {
        const double n = asNumber();
        if (!std::isfinite(n)) throw std::runtime_error("JSON number is not finite");
        return static_cast<std::int64_t>(std::llround(n));
    }

    const std::string& asString() const {
        require(Type::String, "string");
        return string_value_;
    }

    const Array& asArray() const {
        require(Type::Array, "array");
        return array_value_;
    }

    Array& asArray() {
        require(Type::Array, "array");
        return array_value_;
    }

    const Object& asObject() const {
        require(Type::Object, "object");
        return object_value_;
    }

    Object& asObject() {
        require(Type::Object, "object");
        return object_value_;
    }

    bool contains(const std::string& key) const {
        return isObject() && object_value_.find(key) != object_value_.end();
    }

    const Value& at(const std::string& key) const {
        require(Type::Object, "object");
        const auto it = object_value_.find(key);
        if (it == object_value_.end()) throw std::runtime_error("Missing JSON key: " + key);
        return it->second;
    }

    Value& operator[](const std::string& key) {
        if (type_ == Type::Null) {
            type_ = Type::Object;
            object_value_.clear();
        }
        require(Type::Object, "object");
        return object_value_[key];
    }

    const Value& operator[](std::size_t index) const {
        require(Type::Array, "array");
        if (index >= array_value_.size()) throw std::runtime_error("JSON array index out of range");
        return array_value_[index];
    }

    void pushBack(const Value& value) {
        if (type_ == Type::Null) {
            type_ = Type::Array;
            array_value_.clear();
        }
        require(Type::Array, "array");
        array_value_.push_back(value);
    }

    std::string dump(int indent = -1) const {
        std::ostringstream out;
        dumpInto(out, *this, indent, 0);
        return out.str();
    }

private:
    Type type_;
    bool bool_value_;
    double number_value_;
    std::string string_value_;
    Array array_value_;
    Object object_value_;

    void require(Type expected, const char* name) const {
        if (type_ != expected) throw std::runtime_error(std::string("JSON value is not a ") + name);
    }

    static std::string escape(const std::string& text) {
        std::ostringstream out;
        for (unsigned char ch : text) {
            switch (ch) {
                case '"': out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\b': out << "\\b"; break;
                case '\f': out << "\\f"; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default:
                    if (ch < 0x20) {
                        out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch)
                            << std::dec << std::setfill(' ');
                    } else {
                        out << static_cast<char>(ch);
                    }
            }
        }
        return out.str();
    }

    static void newlineAndIndent(std::ostringstream& out, int indent, int depth) {
        if (indent < 0) return;
        out << '\n' << std::string(static_cast<std::size_t>(indent * depth), ' ');
    }

    static void dumpInto(std::ostringstream& out, const Value& value, int indent, int depth) {
        switch (value.type_) {
            case Type::Null:
                out << "null";
                break;
            case Type::Bool:
                out << (value.bool_value_ ? "true" : "false");
                break;
            case Type::Number: {
                if (!std::isfinite(value.number_value_)) throw std::runtime_error("Cannot dump non-finite JSON number");
                out << std::setprecision(12) << value.number_value_;
                break;
            }
            case Type::String:
                out << '"' << escape(value.string_value_) << '"';
                break;
            case Type::Array: {
                out << '[';
                for (std::size_t i = 0; i < value.array_value_.size(); ++i) {
                    if (i != 0) out << ',';
                    if (indent >= 0) newlineAndIndent(out, indent, depth + 1);
                    dumpInto(out, value.array_value_[i], indent, depth + 1);
                }
                if (!value.array_value_.empty() && indent >= 0) newlineAndIndent(out, indent, depth);
                out << ']';
                break;
            }
            case Type::Object: {
                out << '{';
                std::size_t index = 0;
                for (const auto& kv : value.object_value_) {
                    if (index++ != 0) out << ',';
                    if (indent >= 0) newlineAndIndent(out, indent, depth + 1);
                    out << '"' << escape(kv.first) << '"' << (indent >= 0 ? ": " : ":");
                    dumpInto(out, kv.second, indent, depth + 1);
                }
                if (!value.object_value_.empty() && indent >= 0) newlineAndIndent(out, indent, depth);
                out << '}';
                break;
            }
        }
    }
};

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text), pos_(0) {}

    Value parse() {
        skipWhitespace();
        Value value = parseValue();
        skipWhitespace();
        if (pos_ != text_.size()) fail("Unexpected characters after JSON value");
        return value;
    }

private:
    const std::string& text_;
    std::size_t pos_;

    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error(message + " at byte " + std::to_string(pos_));
    }

    void skipWhitespace() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    char peek() const {
        return pos_ < text_.size() ? text_[pos_] : '\0';
    }

    char get() {
        if (pos_ >= text_.size()) fail("Unexpected end of JSON");
        return text_[pos_++];
    }

    bool consume(char expected) {
        if (peek() == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect(char expected) {
        if (!consume(expected)) fail(std::string("Expected '") + expected + "'");
    }

    Value parseValue() {
        skipWhitespace();
        switch (peek()) {
            case 'n': parseLiteral("null"); return Value();
            case 't': parseLiteral("true"); return Value(true);
            case 'f': parseLiteral("false"); return Value(false);
            case '"': return Value(parseString());
            case '[': return parseArray();
            case '{': return parseObject();
            default:
                if (peek() == '-' || std::isdigit(static_cast<unsigned char>(peek()))) return Value(parseNumber());
                fail("Invalid JSON value");
        }
    }

    void parseLiteral(const char* literal) {
        while (*literal) {
            if (get() != *literal++) fail("Invalid JSON literal");
        }
    }

    static int hexValue(char ch) {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    }

    std::uint32_t parseHex4() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = hexValue(get());
            if (digit < 0) fail("Invalid Unicode escape");
            value = (value << 4U) | static_cast<std::uint32_t>(digit);
        }
        return value;
    }

    static void appendUtf8(std::string& out, std::uint32_t cp) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6U)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12U)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6U) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18U)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12U) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6U) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (true) {
            const char ch = get();
            if (ch == '"') break;
            if (static_cast<unsigned char>(ch) < 0x20) fail("Control character in JSON string");
            if (ch != '\\') {
                out.push_back(ch);
                continue;
            }
            const char esc = get();
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = parseHex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (get() != '\\' || get() != 'u') fail("Invalid Unicode surrogate pair");
                        const std::uint32_t low = parseHex4();
                        if (low < 0xDC00 || low > 0xDFFF) fail("Invalid Unicode low surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10U) + (low - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: fail("Invalid JSON escape");
            }
        }
        return out;
    }

    double parseNumber() {
        const std::size_t start = pos_;
        consume('-');
        if (consume('0')) {
            // A leading zero is valid only by itself before fraction/exponent.
        } else {
            if (!std::isdigit(static_cast<unsigned char>(peek()))) fail("Invalid JSON number");
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        if (consume('.')) {
            if (!std::isdigit(static_cast<unsigned char>(peek()))) fail("Invalid JSON fraction");
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (!std::isdigit(static_cast<unsigned char>(peek()))) fail("Invalid JSON exponent");
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        const std::string token = text_.substr(start, pos_ - start);
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0' || !std::isfinite(value)) fail("Invalid JSON number");
        return value;
    }

    Value parseArray() {
        expect('[');
        Value::Array values;
        skipWhitespace();
        if (consume(']')) return Value(std::move(values));
        while (true) {
            values.push_back(parseValue());
            skipWhitespace();
            if (consume(']')) break;
            expect(',');
            skipWhitespace();
        }
        return Value(std::move(values));
    }

    Value parseObject() {
        expect('{');
        Value::Object values;
        skipWhitespace();
        if (consume('}')) return Value(std::move(values));
        while (true) {
            if (peek() != '"') fail("Expected object key");
            const std::string key = parseString();
            skipWhitespace();
            expect(':');
            skipWhitespace();
            values[key] = parseValue();
            skipWhitespace();
            if (consume('}')) break;
            expect(',');
            skipWhitespace();
        }
        return Value(std::move(values));
    }
};

inline Value parse(const std::string& text) {
    return Parser(text).parse();
}

}  // namespace mini_json
