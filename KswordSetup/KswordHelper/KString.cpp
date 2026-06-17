#include "KString.h"

#include <cctype>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace {

// IsAsciiSpace returns true for whitespace recognized by common text formats.
bool IsAsciiSpace(char ch) {
    return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

// SetOk writes parse status only when the caller provided an output pointer.
void SetOk(bool* ok, bool value) {
    if (ok) {
        *ok = value;
    }
}

} // namespace

KString::KString()
    : m_value() {
}

KString::KString(const char* value)
    : m_value(value ? value : "") {
}

KString::KString(const std::string& value)
    : m_value(value) {
}

std::string KString::stdString() const {
    return m_value;
}

const char* KString::c_str() const {
    return m_value.c_str();
}

std::size_t KString::size() const {
    return m_value.size();
}

bool KString::isEmpty() const {
    return m_value.empty();
}

KString KString::trim() const {
    std::size_t begin = 0;
    while (begin < m_value.size() && IsAsciiSpace(m_value[begin])) {
        ++begin;
    }

    std::size_t end = m_value.size();
    while (end > begin && IsAsciiSpace(m_value[end - 1])) {
        --end;
    }

    return KString(m_value.substr(begin, end - begin));
}

std::vector<KString> KString::split(const KString& separator, bool keepEmptyParts) const {
    std::vector<KString> result;
    const std::string sep = separator.stdString();
    if (sep.empty()) {
        result.push_back(*this);
        return result;
    }

    std::size_t start = 0;
    while (start <= m_value.size()) {
        const std::size_t pos = m_value.find(sep, start);
        const std::size_t end = (pos == std::string::npos) ? m_value.size() : pos;
        if (keepEmptyParts || end > start) {
            result.push_back(KString(m_value.substr(start, end - start)));
        }
        if (pos == std::string::npos) {
            break;
        }
        start = pos + sep.size();
    }

    return result;
}

KString KString::join(const std::vector<KString>& parts, const KString& separator) {
    std::string result;
    const std::string sep = separator.stdString();
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            result += sep;
        }
        result += parts[i].stdString();
    }
    return KString(result);
}

KString KString::replace(const KString& before, const KString& after) const {
    const std::string from = before.stdString();
    if (from.empty()) {
        return *this;
    }

    const std::string to = after.stdString();
    std::string result;
    std::size_t start = 0;
    while (start < m_value.size()) {
        const std::size_t pos = m_value.find(from, start);
        if (pos == std::string::npos) {
            result += m_value.substr(start);
            break;
        }
        result += m_value.substr(start, pos - start);
        result += to;
        start = pos + from.size();
    }
    if (m_value.empty()) {
        return KString();
    }
    return KString(result);
}

bool KString::startsWith(const KString& prefix) const {
    const std::string prefixBytes = prefix.stdString();
    if (prefixBytes.size() > m_value.size()) {
        return false;
    }
    return m_value.compare(0, prefixBytes.size(), prefixBytes) == 0;
}

bool KString::endsWith(const KString& suffix) const {
    const std::string suffixBytes = suffix.stdString();
    if (suffixBytes.size() > m_value.size()) {
        return false;
    }
    return m_value.compare(m_value.size() - suffixBytes.size(), suffixBytes.size(), suffixBytes) == 0;
}

int KString::toInt(bool* ok, int base, int defaultValue) const {
    bool parsedOk = false;
    const long long value = toInt64(&parsedOk, base, defaultValue);
    if (!parsedOk || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        SetOk(ok, false);
        return defaultValue;
    }
    SetOk(ok, true);
    return static_cast<int>(value);
}

long long KString::toInt64(bool* ok, int base, long long defaultValue) const {
    const std::string text = trim().stdString();
    if (text.empty()) {
        SetOk(ok, false);
        return defaultValue;
    }

    try {
        std::size_t parsed = 0;
        const long long value = std::stoll(text, &parsed, base);
        if (parsed != text.size()) {
            SetOk(ok, false);
            return defaultValue;
        }
        SetOk(ok, true);
        return value;
    } catch (const std::exception&) {
        SetOk(ok, false);
        return defaultValue;
    }
}

double KString::toDouble(bool* ok, double defaultValue) const {
    const std::string text = trim().stdString();
    if (text.empty()) {
        SetOk(ok, false);
        return defaultValue;
    }

    double value = 0.0;
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    stream >> value;
    if (stream.fail() || !stream.eof()) {
        SetOk(ok, false);
        return defaultValue;
    }

    SetOk(ok, true);
    return value;
}

KString KString::fromNumber(int value) {
    return fromNumber(static_cast<long long>(value));
}

KString KString::fromNumber(long long value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << value;
    return KString(stream.str());
}

KString KString::fromNumber(double value, int precision) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(precision) << value;
    return KString(stream.str());
}

KString& KString::operator=(const std::string& value) {
    m_value = value;
    return *this;
}

KString& KString::operator+=(const KString& other) {
    m_value += other.m_value;
    return *this;
}

KString KString::operator+(const KString& other) const {
    return KString(m_value + other.m_value);
}

bool KString::operator==(const KString& other) const {
    return m_value == other.m_value;
}

bool KString::operator!=(const KString& other) const {
    return m_value != other.m_value;
}

bool KString::operator<(const KString& other) const {
    return m_value < other.m_value;
}

KString::operator std::string() const {
    return m_value;
}
