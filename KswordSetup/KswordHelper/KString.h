#ifndef KSWORD_HELPER_KSTRING_HEAD_FILE
#define KSWORD_HELPER_KSTRING_HEAD_FILE

#include <cstddef>
#include <string>
#include <vector>

// KString is a lightweight UTF-8 byte string wrapper. It intentionally avoids
// locale-heavy behavior: trimming recognizes ASCII whitespace and all other
// operations work on byte sequences, which is predictable for configuration and
// JSON helper usage.
class KString {
public:
    // Constructors copy input bytes into the string. A null C string becomes empty.
    KString();
    KString(const char* value);
    KString(const std::string& value);

    // stdString returns a copy of the stored UTF-8 bytes.
    std::string stdString() const;

    // c_str returns a stable NUL-terminated pointer until the next mutation.
    const char* c_str() const;

    // size and isEmpty report the byte length, not Unicode code-point count.
    std::size_t size() const;
    bool isEmpty() const;

    // trim returns a copy without leading or trailing ASCII whitespace.
    KString trim() const;

    // split separates the string by a byte delimiter. Empty parts are controlled
    // by keepEmptyParts and the returned vector preserves input order.
    std::vector<KString> split(const KString& separator, bool keepEmptyParts = true) const;

    // join concatenates parts with separator between entries and returns the result.
    static KString join(const std::vector<KString>& parts, const KString& separator);

    // replace returns a copy where every non-overlapping before token is replaced
    // by after. An empty before token leaves the string unchanged.
    KString replace(const KString& before, const KString& after) const;

    // startsWith and endsWith compare byte prefixes/suffixes.
    bool startsWith(const KString& prefix) const;
    bool endsWith(const KString& suffix) const;

    // toInt parses a signed integer. ok is set when provided; defaultValue is
    // returned on invalid or out-of-range input.
    int toInt(bool* ok = nullptr, int base = 10, int defaultValue = 0) const;

    // toInt64 parses a signed 64-bit integer with the same status contract as toInt.
    long long toInt64(bool* ok = nullptr, int base = 10, long long defaultValue = 0) const;

    // toDouble parses a floating-point number using the classic C locale.
    double toDouble(bool* ok = nullptr, double defaultValue = 0.0) const;

    // fromNumber converts numeric values to KString using deterministic formatting.
    static KString fromNumber(int value);
    static KString fromNumber(long long value);
    static KString fromNumber(double value, int precision = 6);

    // Assignment and concatenation helpers keep call sites concise.
    KString& operator=(const std::string& value);
    KString& operator+=(const KString& other);
    KString operator+(const KString& other) const;

    // Comparison operators compare the underlying byte strings.
    bool operator==(const KString& other) const;
    bool operator!=(const KString& other) const;
    bool operator<(const KString& other) const;

    // Implicit conversion supports existing std::string-oriented APIs.
    operator std::string() const;

private:
    // m_value owns the raw UTF-8 bytes.
    std::string m_value;
};

#endif
