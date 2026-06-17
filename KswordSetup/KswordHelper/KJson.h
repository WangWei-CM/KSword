#ifndef KSWORD_HELPER_KJSON_HEAD_FILE
#define KSWORD_HELPER_KJSON_HEAD_FILE

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// KJsonFormat selects the byte layout used by toJson(). Compact removes all
// insignificant whitespace, while Pretty writes stable indentation for humans.
enum class KJsonFormat {
    Compact,
    Pretty
};

class KJsonArray;
class KJsonObject;

// KJsonParseError stores the first parser failure with byte offset plus line and
// column information. Inputs are filled by KJsonDocument::fromJson(); callers
// read the fields directly or call errorString().
class KJsonParseError {
public:
    // ParseError classifies failures without forcing callers to parse text.
    enum ParseError {
        NoError,
        UnterminatedString,
        MissingNameSeparator,
        MissingValueSeparator,
        IllegalValue,
        IllegalNumber,
        IllegalEscapeSequence,
        IllegalUTF8String,
        DeepNesting,
        GarbageAtEnd,
        UnknownError
    };

    // The constructor creates a clean no-error state and returns no value.
    KJsonParseError();

    // clear resets all public fields so the same object can be reused.
    void clear();

    // hasError returns true when error is anything other than NoError.
    bool hasError() const;

    // errorString returns a human-readable message with line and column data.
    std::string errorString() const;

    // error is the machine-readable failure category.
    ParseError error;

    // offset is the zero-based byte index where parsing failed.
    int offset;

    // line is the one-based input line where parsing failed.
    int line;

    // column is the one-based byte column where parsing failed.
    int column;

    // message gives parser-specific detail such as the expected token.
    std::string message;
};

// KJsonValue is the tagged JSON node used by arrays, objects, and documents.
// It stores UTF-8 strings as std::string bytes and supports JSON scalar and
// container types without any third-party dependency.
class KJsonValue {
public:
    // Type describes the currently active payload.
    enum Type {
        Null,
        Bool,
        Int64,
        Double,
        String,
        Array,
        Object
    };

    // Constructors normalize C++ values into JSON value types.
    KJsonValue();
    KJsonValue(std::nullptr_t);
    KJsonValue(bool value);
    KJsonValue(int value);
    KJsonValue(long long value);
    KJsonValue(double value);
    KJsonValue(const char* value);
    KJsonValue(const std::string& value);
    KJsonValue(const KJsonArray& value);
    KJsonValue(const KJsonObject& value);
    ~KJsonValue();

    // type returns the active JSON type tag.
    Type type() const;

    // Type checks return true only for the exact stored JSON type, except
    // isNumber() accepts both Int64 and Double.
    bool isNull() const;
    bool isBool() const;
    bool isInt() const;
    bool isDouble() const;
    bool isNumber() const;
    bool isString() const;
    bool isArray() const;
    bool isObject() const;

    // Conversion helpers return stored data when the type matches; otherwise
    // they return the caller-provided default value.
    bool toBool(bool defaultValue = false) const;
    long long toInt(long long defaultValue = 0) const;
    double toDouble(double defaultValue = 0.0) const;
    std::string toString(const std::string& defaultValue = std::string()) const;
    KJsonArray toArray() const;
    KJsonObject toObject() const;

    // toJson serializes this node as UTF-8 JSON and returns the produced bytes.
    std::string toJson(KJsonFormat format = KJsonFormat::Compact) const;

private:
    // KJsonArray and KJsonObject need direct access for efficient storage.
    friend class KJsonArray;
    friend class KJsonObject;
    friend class KJsonDocument;

    // reset clears inactive payloads and switches to the requested type.
    void reset(Type nextType);

    // m_type selects which payload fields are meaningful.
    Type m_type;

    // Scalar fields are kept directly to avoid requiring C++17 std::variant.
    bool m_boolValue;
    long long m_intValue;
    double m_doubleValue;
    std::string m_stringValue;

    // Containers are held behind shared_ptr to break the recursive type cycle.
    std::shared_ptr<KJsonArray> m_arrayValue;
    std::shared_ptr<KJsonObject> m_objectValue;
};

// KJsonArray is an ordered list of KJsonValue nodes. It provides Qt-like value()
// and append() helpers while exposing STL iterators for simple loops.
class KJsonArray {
public:
    typedef std::vector<KJsonValue>::iterator iterator;
    typedef std::vector<KJsonValue>::const_iterator const_iterator;

    // Constructors create an empty array or copy an existing vector.
    KJsonArray();
    explicit KJsonArray(const std::vector<KJsonValue>& values);

    // append pushes one value to the end and returns no value.
    void append(const KJsonValue& value);

    // clear removes all elements and returns no value.
    void clear();

    // value returns the indexed element or defaultValue when index is invalid.
    KJsonValue value(std::size_t index, const KJsonValue& defaultValue = KJsonValue()) const;

    // at returns the indexed element and expects the caller to pass a valid index.
    const KJsonValue& at(std::size_t index) const;

    // operator[] exposes direct mutable or const access for valid indexes.
    KJsonValue& operator[](std::size_t index);
    const KJsonValue& operator[](std::size_t index) const;

    // size and isEmpty describe the current element count.
    std::size_t size() const;
    bool isEmpty() const;

    // begin/end return STL iterators over stored values.
    iterator begin();
    iterator end();
    const_iterator begin() const;
    const_iterator end() const;

    // values returns a copy of the underlying vector for callers needing STL data.
    std::vector<KJsonValue> values() const;

    // toJson serializes this array as UTF-8 JSON and returns the produced bytes.
    std::string toJson(KJsonFormat format = KJsonFormat::Compact) const;

private:
    // m_values owns array elements in insertion order.
    std::vector<KJsonValue> m_values;
};

// KJsonObject is a string-keyed map of KJsonValue nodes. std::map gives stable
// key order so compact and pretty output are deterministic across runs.
class KJsonObject {
public:
    typedef std::map<std::string, KJsonValue>::iterator iterator;
    typedef std::map<std::string, KJsonValue>::const_iterator const_iterator;

    // The default constructor creates an empty object.
    KJsonObject();

    // insert assigns value to key and returns no value.
    void insert(const std::string& key, const KJsonValue& value);

    // remove erases key and returns true when a value existed.
    bool remove(const std::string& key);

    // clear removes all key/value pairs and returns no value.
    void clear();

    // contains reports whether key exists in the object.
    bool contains(const std::string& key) const;

    // value returns the stored value or defaultValue when key is missing.
    KJsonValue value(const std::string& key, const KJsonValue& defaultValue = KJsonValue()) const;

    // operator[] returns a mutable value reference, inserting null when missing.
    KJsonValue& operator[](const std::string& key);

    // size and isEmpty describe the current member count.
    std::size_t size() const;
    bool isEmpty() const;

    // keys returns all keys in deterministic map order.
    std::vector<std::string> keys() const;

    // begin/end return STL iterators over key/value pairs.
    iterator begin();
    iterator end();
    const_iterator begin() const;
    const_iterator end() const;

    // toJson serializes this object as UTF-8 JSON and returns the produced bytes.
    std::string toJson(KJsonFormat format = KJsonFormat::Compact) const;

private:
    // m_values owns values by key in sorted order.
    std::map<std::string, KJsonValue> m_values;
};

// KJsonDocument owns one root JSON value and provides Qt-like fromJson()/toJson()
// entry points. The root may be any JSON value; isObject() and isArray() help
// callers enforce configuration-file contracts when needed.
class KJsonDocument {
public:
    // Constructors create a null document or wrap a value/object/array root.
    KJsonDocument();
    explicit KJsonDocument(const KJsonValue& value);
    explicit KJsonDocument(const KJsonObject& object);
    explicit KJsonDocument(const KJsonArray& array);

    // fromJson parses UTF-8 JSON bytes. It returns a null document on failure
    // and fills parseError when a pointer is provided.
    static KJsonDocument fromJson(const std::string& json, KJsonParseError* parseError = nullptr);

    // toJson serializes the root value using the selected format.
    std::string toJson(KJsonFormat format = KJsonFormat::Compact) const;

    // Type helpers query the root value.
    bool isNull() const;
    bool isArray() const;
    bool isObject() const;

    // Accessors return converted root values or empty containers on mismatch.
    KJsonValue value() const;
    KJsonArray array() const;
    KJsonObject object() const;

    // setValue replaces the root value and returns no value.
    void setValue(const KJsonValue& value);

private:
    // m_value stores the parsed or assigned root node.
    KJsonValue m_value;
};

#endif
