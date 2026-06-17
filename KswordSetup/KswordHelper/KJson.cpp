#include "KJson.h"

#include <cmath>
#include <cctype>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace {

// Parser and writer constants stay local to this translation unit.
const int kPrettyIndentSpaces = 4;
const int kMaxJsonDepth = 256;

// AppendIndent writes depth * kPrettyIndentSpaces spaces into output and returns
// no value. The writer uses it only for pretty formatting.
void AppendIndent(std::string& output, int depth) {
    output.append(static_cast<std::size_t>(depth * kPrettyIndentSpaces), ' ');
}

// AppendHex4 writes a four-digit uppercase JSON \\u escape code and returns no
// value. It is used for control characters that do not have short escapes.
void AppendHex4(std::string& output, unsigned int codePoint) {
    static const char kHex[] = "0123456789ABCDEF";
    output.push_back('\\');
    output.push_back('u');
    output.push_back(kHex[(codePoint >> 12) & 0x0F]);
    output.push_back(kHex[(codePoint >> 8) & 0x0F]);
    output.push_back(kHex[(codePoint >> 4) & 0x0F]);
    output.push_back(kHex[codePoint & 0x0F]);
}

// AppendUtf8 encodes one Unicode code point as UTF-8 bytes. Input is assumed to
// be a valid scalar value; invalid callers receive no appended bytes.
void AppendUtf8(std::string& output, unsigned int codePoint) {
    if (codePoint <= 0x7F) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | ((codePoint >> 6) & 0x1F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | ((codePoint >> 12) & 0x0F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0x10FFFF) {
        output.push_back(static_cast<char>(0xF0 | ((codePoint >> 18) & 0x07)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

// DecodeUtf8At validates and decodes one UTF-8 sequence at index. It returns
// false for malformed, overlong, surrogate, or out-of-range sequences.
bool DecodeUtf8At(const std::string& input, std::size_t index, std::size_t& bytesRead, unsigned int& codePoint) {
    bytesRead = 0;
    codePoint = 0;

    if (index >= input.size()) {
        return false;
    }

    const unsigned char first = static_cast<unsigned char>(input[index]);
    if (first <= 0x7F) {
        bytesRead = 1;
        codePoint = first;
        return true;
    }

    std::size_t expected = 0;
    unsigned int minimum = 0;
    if ((first & 0xE0) == 0xC0) {
        expected = 2;
        codePoint = first & 0x1F;
        minimum = 0x80;
    } else if ((first & 0xF0) == 0xE0) {
        expected = 3;
        codePoint = first & 0x0F;
        minimum = 0x800;
    } else if ((first & 0xF8) == 0xF0) {
        expected = 4;
        codePoint = first & 0x07;
        minimum = 0x10000;
    } else {
        return false;
    }

    if (index + expected > input.size()) {
        return false;
    }

    for (std::size_t i = 1; i < expected; ++i) {
        const unsigned char next = static_cast<unsigned char>(input[index + i]);
        if ((next & 0xC0) != 0x80) {
            return false;
        }
        codePoint = (codePoint << 6) | (next & 0x3F);
    }

    if (codePoint < minimum || codePoint > 0x10FFFF) {
        return false;
    }
    if (codePoint >= 0xD800 && codePoint <= 0xDFFF) {
        return false;
    }

    bytesRead = expected;
    return true;
}

// EscapeString writes a JSON string literal, including surrounding quotes. It
// preserves valid UTF-8 bytes and escapes JSON control characters.
void EscapeString(std::string& output, const std::string& value) {
    output.push_back('"');
    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        switch (ch) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (ch < 0x20) {
                AppendHex4(output, ch);
            } else {
                output.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    output.push_back('"');
}

void SerializeValue(const KJsonValue& value, KJsonFormat format, int depth, std::string& output);

// SerializeArray writes one array recursively. It returns no value and delegates
// scalar/container details back to SerializeValue().
void SerializeArray(const KJsonArray& arrayValue, KJsonFormat format, int depth, std::string& output) {
    if (arrayValue.isEmpty()) {
        output += "[]";
        return;
    }

    const bool pretty = format == KJsonFormat::Pretty;
    output.push_back('[');
    if (pretty) {
        output.push_back('\n');
    }

    for (std::size_t i = 0; i < arrayValue.size(); ++i) {
        if (pretty) {
            AppendIndent(output, depth + 1);
        }
        SerializeValue(arrayValue.at(i), format, depth + 1, output);
        if (i + 1 < arrayValue.size()) {
            output.push_back(',');
        }
        if (pretty) {
            output.push_back('\n');
        }
    }

    if (pretty) {
        AppendIndent(output, depth);
    }
    output.push_back(']');
}

// SerializeObject writes one object recursively. std::map iteration gives stable
// key order, so the same object serializes to the same bytes every time.
void SerializeObject(const KJsonObject& objectValue, KJsonFormat format, int depth, std::string& output) {
    if (objectValue.isEmpty()) {
        output += "{}";
        return;
    }

    const bool pretty = format == KJsonFormat::Pretty;
    output.push_back('{');
    if (pretty) {
        output.push_back('\n');
    }

    std::size_t index = 0;
    const std::size_t memberCount = objectValue.size();
    for (KJsonObject::const_iterator it = objectValue.begin(); it != objectValue.end(); ++it, ++index) {
        if (pretty) {
            AppendIndent(output, depth + 1);
        }
        EscapeString(output, it->first);
        output.push_back(':');
        if (pretty) {
            output.push_back(' ');
        }
        SerializeValue(it->second, format, depth + 1, output);
        if (index + 1 < memberCount) {
            output.push_back(',');
        }
        if (pretty) {
            output.push_back('\n');
        }
    }

    if (pretty) {
        AppendIndent(output, depth);
    }
    output.push_back('}');
}

// SerializeValue writes one JSON node and returns no value. Non-finite doubles
// are emitted as null because JSON has no NaN or infinity tokens.
void SerializeValue(const KJsonValue& value, KJsonFormat format, int depth, std::string& output) {
    switch (value.type()) {
    case KJsonValue::Null:
        output += "null";
        break;
    case KJsonValue::Bool:
        output += value.toBool() ? "true" : "false";
        break;
    case KJsonValue::Int64: {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << value.toInt();
        output += stream.str();
        break;
    }
    case KJsonValue::Double: {
        const double doubleValue = value.toDouble();
        if (!std::isfinite(doubleValue)) {
            output += "null";
            break;
        }
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(std::numeric_limits<double>::max_digits10) << doubleValue;
        output += stream.str();
        break;
    }
    case KJsonValue::String:
        EscapeString(output, value.toString());
        break;
    case KJsonValue::Array:
        SerializeArray(value.toArray(), format, depth, output);
        break;
    case KJsonValue::Object:
        SerializeObject(value.toObject(), format, depth, output);
        break;
    }
}

// HexValue converts an ASCII hex digit to its numeric value. It returns -1 for
// any non-hex byte so callers can report a JSON escape error.
int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

// KJsonParser implements a recursive descent parser over UTF-8 bytes. It owns
// no output memory beyond temporary values and records only the first error.
class KJsonParser {
public:
    // The constructor receives the immutable input buffer and optional error sink.
    KJsonParser(const std::string& input, KJsonParseError* parseError)
        : m_input(input),
          m_error(parseError),
          m_index(0),
          m_line(1),
          m_column(1),
          m_failed(false) {
        if (m_error) {
            m_error->clear();
        }
    }

    // parseDocument parses one complete JSON value and rejects trailing garbage.
    KJsonDocument parseDocument() {
        KJsonValue root;
        skipWhitespace();
        if (!parseValue(root, 0)) {
            return KJsonDocument();
        }
        skipWhitespace();
        if (!atEnd()) {
            fail(KJsonParseError::GarbageAtEnd, "Unexpected bytes after the JSON value");
            return KJsonDocument();
        }
        if (m_error) {
            m_error->clear();
        }
        return KJsonDocument(root);
    }

private:
    // atEnd returns true when all bytes have been consumed.
    bool atEnd() const {
        return m_index >= m_input.size();
    }

    // peek returns the current byte or NUL at end of input.
    char peek() const {
        return atEnd() ? '\0' : m_input[m_index];
    }

    // advance consumes one byte and updates one-based line/column counters.
    char advance() {
        if (atEnd()) {
            return '\0';
        }
        const char ch = m_input[m_index++];
        if (ch == '\n') {
            ++m_line;
            m_column = 1;
        } else {
            ++m_column;
        }
        return ch;
    }

    // consume advances one byte when it matches expected and returns success.
    bool consume(char expected) {
        if (peek() != expected) {
            return false;
        }
        advance();
        return true;
    }

    // skipWhitespace consumes the four whitespace bytes allowed by JSON.
    void skipWhitespace() {
        while (!atEnd()) {
            const char ch = peek();
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                advance();
            } else {
                break;
            }
        }
    }

    // fail records the first error and returns false for convenient parser code.
    bool fail(KJsonParseError::ParseError error, const std::string& message) {
        if (!m_failed && m_error) {
            m_error->error = error;
            m_error->offset = static_cast<int>(m_index);
            m_error->line = m_line;
            m_error->column = m_column;
            m_error->message = message;
        }
        m_failed = true;
        return false;
    }

    // parseValue dispatches to the correct grammar production based on the next byte.
    bool parseValue(KJsonValue& output, int depth) {
        if (depth > kMaxJsonDepth) {
            return fail(KJsonParseError::DeepNesting, "JSON nesting depth limit exceeded");
        }

        skipWhitespace();
        if (atEnd()) {
            return fail(KJsonParseError::IllegalValue, "Expected a JSON value");
        }

        const char ch = peek();
        if (ch == 'n') {
            return parseLiteral("null", KJsonValue(), output);
        }
        if (ch == 't') {
            return parseLiteral("true", KJsonValue(true), output);
        }
        if (ch == 'f') {
            return parseLiteral("false", KJsonValue(false), output);
        }
        if (ch == '"') {
            std::string stringValue;
            if (!parseString(stringValue)) {
                return false;
            }
            output = KJsonValue(stringValue);
            return true;
        }
        if (ch == '[') {
            KJsonArray arrayValue;
            if (!parseArray(arrayValue, depth + 1)) {
                return false;
            }
            output = KJsonValue(arrayValue);
            return true;
        }
        if (ch == '{') {
            KJsonObject objectValue;
            if (!parseObject(objectValue, depth + 1)) {
                return false;
            }
            output = KJsonValue(objectValue);
            return true;
        }
        if (ch == '-' || (ch >= '0' && ch <= '9')) {
            return parseNumber(output);
        }

        return fail(KJsonParseError::IllegalValue, "Unexpected byte while reading JSON value");
    }

    // parseLiteral consumes a fixed token such as true, false, or null.
    bool parseLiteral(const char* literal, const KJsonValue& value, KJsonValue& output) {
        for (const char* cursor = literal; *cursor != '\0'; ++cursor) {
            if (peek() != *cursor) {
                return fail(KJsonParseError::IllegalValue, std::string("Expected '") + literal + "'");
            }
            advance();
        }
        output = value;
        return true;
    }

    // parseString consumes a JSON string, decodes escapes, validates UTF-8, and
    // writes the unescaped UTF-8 bytes into output.
    bool parseString(std::string& output) {
        if (!consume('"')) {
            return fail(KJsonParseError::UnterminatedString, "Expected string opening quote");
        }

        while (!atEnd()) {
            const unsigned char ch = static_cast<unsigned char>(peek());
            if (ch == '"') {
                advance();
                return true;
            }
            if (ch == '\\') {
                advance();
                if (!parseEscape(output)) {
                    return false;
                }
                continue;
            }
            if (ch < 0x20) {
                return fail(KJsonParseError::UnterminatedString, "Unescaped control character in string");
            }
            if (ch < 0x80) {
                output.push_back(advance());
                continue;
            }

            std::size_t bytesRead = 0;
            unsigned int codePoint = 0;
            if (!DecodeUtf8At(m_input, m_index, bytesRead, codePoint)) {
                return fail(KJsonParseError::IllegalUTF8String, "Invalid UTF-8 sequence in string");
            }
            for (std::size_t i = 0; i < bytesRead; ++i) {
                output.push_back(advance());
            }
        }

        return fail(KJsonParseError::UnterminatedString, "Missing string closing quote");
    }

    // parseEscape consumes the byte after a backslash and appends its decoded
    // value to output. Unicode surrogate pairs are combined into UTF-8.
    bool parseEscape(std::string& output) {
        if (atEnd()) {
            return fail(KJsonParseError::UnterminatedString, "String ends inside an escape sequence");
        }

        const char escapeCode = advance();
        switch (escapeCode) {
        case '"':
            output.push_back('"');
            return true;
        case '\\':
            output.push_back('\\');
            return true;
        case '/':
            output.push_back('/');
            return true;
        case 'b':
            output.push_back('\b');
            return true;
        case 'f':
            output.push_back('\f');
            return true;
        case 'n':
            output.push_back('\n');
            return true;
        case 'r':
            output.push_back('\r');
            return true;
        case 't':
            output.push_back('\t');
            return true;
        case 'u':
            return parseUnicodeEscape(output);
        default:
            return fail(KJsonParseError::IllegalEscapeSequence, "Unsupported string escape sequence");
        }
    }

    // parseUnicodeEscape decodes four hex digits after \\u. If it sees a high
    // surrogate, it requires and combines a following low-surrogate escape.
    bool parseUnicodeEscape(std::string& output) {
        unsigned int codeUnit = 0;
        if (!parseHex4(codeUnit)) {
            return false;
        }

        if (codeUnit >= 0xD800 && codeUnit <= 0xDBFF) {
            if (!consume('\\') || !consume('u')) {
                return fail(KJsonParseError::IllegalEscapeSequence, "Expected low surrogate after high surrogate");
            }
            unsigned int lowSurrogate = 0;
            if (!parseHex4(lowSurrogate)) {
                return false;
            }
            if (lowSurrogate < 0xDC00 || lowSurrogate > 0xDFFF) {
                return fail(KJsonParseError::IllegalEscapeSequence, "Invalid low surrogate in Unicode escape");
            }
            const unsigned int highTenBits = codeUnit - 0xD800;
            const unsigned int lowTenBits = lowSurrogate - 0xDC00;
            const unsigned int codePoint = 0x10000 + ((highTenBits << 10) | lowTenBits);
            AppendUtf8(output, codePoint);
            return true;
        }

        if (codeUnit >= 0xDC00 && codeUnit <= 0xDFFF) {
            return fail(KJsonParseError::IllegalEscapeSequence, "Low surrogate without preceding high surrogate");
        }

        AppendUtf8(output, codeUnit);
        return true;
    }

    // parseHex4 consumes exactly four ASCII hex digits and writes the numeric
    // value into output.
    bool parseHex4(unsigned int& output) {
        output = 0;
        for (int i = 0; i < 4; ++i) {
            if (atEnd()) {
                return fail(KJsonParseError::IllegalEscapeSequence, "Unicode escape ended early");
            }
            const int value = HexValue(peek());
            if (value < 0) {
                return fail(KJsonParseError::IllegalEscapeSequence, "Unicode escape contains a non-hex digit");
            }
            output = (output << 4) | static_cast<unsigned int>(value);
            advance();
        }
        return true;
    }

    // parseNumber consumes the JSON number grammar and stores either Int64 or
    // Double depending on fractional/exponent notation.
    bool parseNumber(KJsonValue& output) {
        const std::size_t start = m_index;
        bool floatingPoint = false;

        if (peek() == '-') {
            advance();
        }

        if (atEnd()) {
            return fail(KJsonParseError::IllegalNumber, "Number ended after sign");
        }

        if (peek() == '0') {
            advance();
            if (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
                return fail(KJsonParseError::IllegalNumber, "Leading zero is not allowed in JSON numbers");
            }
        } else if (peek() >= '1' && peek() <= '9') {
            while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
                advance();
            }
        } else {
            return fail(KJsonParseError::IllegalNumber, "Expected digit in JSON number");
        }

        if (!atEnd() && peek() == '.') {
            floatingPoint = true;
            advance();
            if (atEnd() || !std::isdigit(static_cast<unsigned char>(peek()))) {
                return fail(KJsonParseError::IllegalNumber, "Expected digit after decimal point");
            }
            while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
                advance();
            }
        }

        if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
            floatingPoint = true;
            advance();
            if (!atEnd() && (peek() == '+' || peek() == '-')) {
                advance();
            }
            if (atEnd() || !std::isdigit(static_cast<unsigned char>(peek()))) {
                return fail(KJsonParseError::IllegalNumber, "Expected exponent digits");
            }
            while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
                advance();
            }
        }

        const std::string numberText = m_input.substr(start, m_index - start);
        if (floatingPoint) {
            double doubleValue = 0.0;
            std::istringstream stream(numberText);
            stream.imbue(std::locale::classic());
            stream >> doubleValue;
            if (stream.fail() || !stream.eof() || !std::isfinite(doubleValue)) {
                return fail(KJsonParseError::IllegalNumber, "Floating-point number is out of range");
            }
            output = KJsonValue(doubleValue);
            return true;
        }

        try {
            std::size_t parsed = 0;
            const long long intValue = std::stoll(numberText, &parsed, 10);
            if (parsed != numberText.size()) {
                return fail(KJsonParseError::IllegalNumber, "Integer contains invalid bytes");
            }
            output = KJsonValue(intValue);
            return true;
        } catch (const std::exception&) {
            return fail(KJsonParseError::IllegalNumber, "Integer is out of 64-bit range");
        }
    }

    // parseArray consumes '[' elements ']' and fills output in input order.
    bool parseArray(KJsonArray& output, int depth) {
        if (!consume('[')) {
            return fail(KJsonParseError::IllegalValue, "Expected array opening bracket");
        }

        skipWhitespace();
        if (consume(']')) {
            return true;
        }

        while (!atEnd()) {
            KJsonValue item;
            if (!parseValue(item, depth)) {
                return false;
            }
            output.append(item);
            skipWhitespace();
            if (consume(',')) {
                skipWhitespace();
                continue;
            }
            if (consume(']')) {
                return true;
            }
            return fail(KJsonParseError::MissingValueSeparator, "Expected ',' or ']' after array value");
        }

        return fail(KJsonParseError::MissingValueSeparator, "Array ended before closing bracket");
    }

    // parseObject consumes '{' members '}' and fills output by key. Duplicate
    // keys intentionally keep the last value, matching many JSON object maps.
    bool parseObject(KJsonObject& output, int depth) {
        if (!consume('{')) {
            return fail(KJsonParseError::IllegalValue, "Expected object opening brace");
        }

        skipWhitespace();
        if (consume('}')) {
            return true;
        }

        while (!atEnd()) {
            if (peek() != '"') {
                return fail(KJsonParseError::IllegalValue, "Expected object member name");
            }
            std::string key;
            if (!parseString(key)) {
                return false;
            }
            skipWhitespace();
            if (!consume(':')) {
                return fail(KJsonParseError::MissingNameSeparator, "Expected ':' after object member name");
            }
            KJsonValue memberValue;
            if (!parseValue(memberValue, depth)) {
                return false;
            }
            output.insert(key, memberValue);
            skipWhitespace();
            if (consume(',')) {
                skipWhitespace();
                continue;
            }
            if (consume('}')) {
                return true;
            }
            return fail(KJsonParseError::MissingValueSeparator, "Expected ',' or '}' after object member");
        }

        return fail(KJsonParseError::MissingValueSeparator, "Object ended before closing brace");
    }

    // Input and mutable parser state are deliberately simple byte indexes.
    const std::string& m_input;
    KJsonParseError* m_error;
    std::size_t m_index;
    int m_line;
    int m_column;
    bool m_failed;
};

} // namespace

KJsonParseError::KJsonParseError()
    : error(NoError), offset(0), line(1), column(1), message() {
}

void KJsonParseError::clear() {
    error = NoError;
    offset = 0;
    line = 1;
    column = 1;
    message.clear();
}

bool KJsonParseError::hasError() const {
    return error != NoError;
}

std::string KJsonParseError::errorString() const {
    if (!hasError()) {
        return "No error";
    }

    std::ostringstream stream;
    stream << (message.empty() ? "JSON parse error" : message)
           << " at line " << line
           << ", column " << column
           << " (offset " << offset << ")";
    return stream.str();
}

KJsonValue::KJsonValue()
    : m_type(Null),
      m_boolValue(false),
      m_intValue(0),
      m_doubleValue(0.0),
      m_stringValue(),
      m_arrayValue(),
      m_objectValue() {
}

KJsonValue::KJsonValue(std::nullptr_t)
    : KJsonValue() {
}

KJsonValue::KJsonValue(bool value)
    : KJsonValue() {
    reset(Bool);
    m_boolValue = value;
}

KJsonValue::KJsonValue(int value)
    : KJsonValue(static_cast<long long>(value)) {
}

KJsonValue::KJsonValue(long long value)
    : KJsonValue() {
    reset(Int64);
    m_intValue = value;
}

KJsonValue::KJsonValue(double value)
    : KJsonValue() {
    reset(Double);
    m_doubleValue = value;
}

KJsonValue::KJsonValue(const char* value)
    : KJsonValue() {
    if (value) {
        reset(String);
        m_stringValue = value;
    }
}

KJsonValue::KJsonValue(const std::string& value)
    : KJsonValue() {
    reset(String);
    m_stringValue = value;
}

KJsonValue::KJsonValue(const KJsonArray& value)
    : KJsonValue() {
    reset(Array);
    m_arrayValue.reset(new KJsonArray(value));
}

KJsonValue::KJsonValue(const KJsonObject& value)
    : KJsonValue() {
    reset(Object);
    m_objectValue.reset(new KJsonObject(value));
}

KJsonValue::~KJsonValue() {
}

KJsonValue::Type KJsonValue::type() const {
    return m_type;
}

bool KJsonValue::isNull() const {
    return m_type == Null;
}

bool KJsonValue::isBool() const {
    return m_type == Bool;
}

bool KJsonValue::isInt() const {
    return m_type == Int64;
}

bool KJsonValue::isDouble() const {
    return m_type == Double;
}

bool KJsonValue::isNumber() const {
    return m_type == Int64 || m_type == Double;
}

bool KJsonValue::isString() const {
    return m_type == String;
}

bool KJsonValue::isArray() const {
    return m_type == Array;
}

bool KJsonValue::isObject() const {
    return m_type == Object;
}

bool KJsonValue::toBool(bool defaultValue) const {
    return m_type == Bool ? m_boolValue : defaultValue;
}

long long KJsonValue::toInt(long long defaultValue) const {
    return m_type == Int64 ? m_intValue : defaultValue;
}

double KJsonValue::toDouble(double defaultValue) const {
    if (m_type == Double) {
        return m_doubleValue;
    }
    if (m_type == Int64) {
        return static_cast<double>(m_intValue);
    }
    return defaultValue;
}

std::string KJsonValue::toString(const std::string& defaultValue) const {
    return m_type == String ? m_stringValue : defaultValue;
}

KJsonArray KJsonValue::toArray() const {
    if (m_type == Array && m_arrayValue) {
        return *m_arrayValue;
    }
    return KJsonArray();
}

KJsonObject KJsonValue::toObject() const {
    if (m_type == Object && m_objectValue) {
        return *m_objectValue;
    }
    return KJsonObject();
}

std::string KJsonValue::toJson(KJsonFormat format) const {
    std::string output;
    SerializeValue(*this, format, 0, output);
    return output;
}

void KJsonValue::reset(Type nextType) {
    m_type = nextType;
    m_boolValue = false;
    m_intValue = 0;
    m_doubleValue = 0.0;
    m_stringValue.clear();
    m_arrayValue.reset();
    m_objectValue.reset();
}

KJsonArray::KJsonArray()
    : m_values() {
}

KJsonArray::KJsonArray(const std::vector<KJsonValue>& values)
    : m_values(values) {
}

void KJsonArray::append(const KJsonValue& value) {
    m_values.push_back(value);
}

void KJsonArray::clear() {
    m_values.clear();
}

KJsonValue KJsonArray::value(std::size_t index, const KJsonValue& defaultValue) const {
    if (index >= m_values.size()) {
        return defaultValue;
    }
    return m_values[index];
}

const KJsonValue& KJsonArray::at(std::size_t index) const {
    return m_values.at(index);
}

KJsonValue& KJsonArray::operator[](std::size_t index) {
    return m_values[index];
}

const KJsonValue& KJsonArray::operator[](std::size_t index) const {
    return m_values[index];
}

std::size_t KJsonArray::size() const {
    return m_values.size();
}

bool KJsonArray::isEmpty() const {
    return m_values.empty();
}

KJsonArray::iterator KJsonArray::begin() {
    return m_values.begin();
}

KJsonArray::iterator KJsonArray::end() {
    return m_values.end();
}

KJsonArray::const_iterator KJsonArray::begin() const {
    return m_values.begin();
}

KJsonArray::const_iterator KJsonArray::end() const {
    return m_values.end();
}

std::vector<KJsonValue> KJsonArray::values() const {
    return m_values;
}

std::string KJsonArray::toJson(KJsonFormat format) const {
    std::string output;
    SerializeArray(*this, format, 0, output);
    return output;
}

KJsonObject::KJsonObject()
    : m_values() {
}

void KJsonObject::insert(const std::string& key, const KJsonValue& value) {
    m_values[key] = value;
}

bool KJsonObject::remove(const std::string& key) {
    return m_values.erase(key) > 0;
}

void KJsonObject::clear() {
    m_values.clear();
}

bool KJsonObject::contains(const std::string& key) const {
    return m_values.find(key) != m_values.end();
}

KJsonValue KJsonObject::value(const std::string& key, const KJsonValue& defaultValue) const {
    const_iterator it = m_values.find(key);
    if (it == m_values.end()) {
        return defaultValue;
    }
    return it->second;
}

KJsonValue& KJsonObject::operator[](const std::string& key) {
    return m_values[key];
}

std::size_t KJsonObject::size() const {
    return m_values.size();
}

bool KJsonObject::isEmpty() const {
    return m_values.empty();
}

std::vector<std::string> KJsonObject::keys() const {
    std::vector<std::string> result;
    result.reserve(m_values.size());
    for (const_iterator it = m_values.begin(); it != m_values.end(); ++it) {
        result.push_back(it->first);
    }
    return result;
}

KJsonObject::iterator KJsonObject::begin() {
    return m_values.begin();
}

KJsonObject::iterator KJsonObject::end() {
    return m_values.end();
}

KJsonObject::const_iterator KJsonObject::begin() const {
    return m_values.begin();
}

KJsonObject::const_iterator KJsonObject::end() const {
    return m_values.end();
}

std::string KJsonObject::toJson(KJsonFormat format) const {
    std::string output;
    SerializeObject(*this, format, 0, output);
    return output;
}

KJsonDocument::KJsonDocument()
    : m_value() {
}

KJsonDocument::KJsonDocument(const KJsonValue& value)
    : m_value(value) {
}

KJsonDocument::KJsonDocument(const KJsonObject& object)
    : m_value(object) {
}

KJsonDocument::KJsonDocument(const KJsonArray& array)
    : m_value(array) {
}

KJsonDocument KJsonDocument::fromJson(const std::string& json, KJsonParseError* parseError) {
    KJsonParser parser(json, parseError);
    return parser.parseDocument();
}

std::string KJsonDocument::toJson(KJsonFormat format) const {
    return m_value.toJson(format);
}

bool KJsonDocument::isNull() const {
    return m_value.isNull();
}

bool KJsonDocument::isArray() const {
    return m_value.isArray();
}

bool KJsonDocument::isObject() const {
    return m_value.isObject();
}

KJsonValue KJsonDocument::value() const {
    return m_value;
}

KJsonArray KJsonDocument::array() const {
    return m_value.toArray();
}

KJsonObject KJsonDocument::object() const {
    return m_value.toObject();
}

void KJsonDocument::setValue(const KJsonValue& value) {
    m_value = value;
}

