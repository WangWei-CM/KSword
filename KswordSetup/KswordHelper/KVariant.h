#ifndef KSWORD_HELPER_KVARIANT_HEAD_FILE
#define KSWORD_HELPER_KVARIANT_HEAD_FILE

#include "KJson.h"

#include <cstddef>
#include <memory>
#include <string>

// KVariant is a small QVariant-style value container for helper-layer types. It
// supports null, bool, int64, double, UTF-8 string, JSON object, and JSON array.
class KVariant {
public:
    // Type identifies the active payload stored by this object.
    enum Type {
        Invalid,
        Bool,
        Int64,
        Double,
        String,
        JsonObject,
        JsonArray
    };

    // Constructors copy scalar or JSON container inputs into the variant.
    KVariant();
    KVariant(std::nullptr_t);
    KVariant(bool value);
    KVariant(int value);
    KVariant(long long value);
    KVariant(double value);
    KVariant(const char* value);
    KVariant(const std::string& value);
    KVariant(const KJsonObject& value);
    KVariant(const KJsonArray& value);
    ~KVariant();

    // type and typeName expose the current payload category.
    Type type() const;
    std::string typeName() const;

    // clear resets the value to Invalid and releases owned containers.
    void clear();

    // Type checks return true only for the exact stored payload.
    bool isValid() const;
    bool isNull() const;
    bool isBool() const;
    bool isInt() const;
    bool isDouble() const;
    bool isNumber() const;
    bool isString() const;
    bool isJsonObject() const;
    bool isJsonArray() const;

    // Conversion helpers return converted data when possible; otherwise they
    // return the supplied default value.
    bool toBool(bool defaultValue = false) const;
    long long toInt(long long defaultValue = 0) const;
    double toDouble(double defaultValue = 0.0) const;
    std::string toString(const std::string& defaultValue = std::string()) const;
    KJsonObject toJsonObject() const;
    KJsonArray toJsonArray() const;

    // toJsonValue converts this variant into the equivalent JSON value. Invalid
    // variants become JSON null.
    KJsonValue toJsonValue() const;

    // fromJsonValue converts a JSON value into the nearest variant type.
    static KVariant fromJsonValue(const KJsonValue& value);

private:
    // reset switches payload type and clears inactive state.
    void reset(Type nextType);

    // Scalar payload fields are simple direct members for C++11/14 compatibility.
    Type m_type;
    bool m_boolValue;
    long long m_intValue;
    double m_doubleValue;
    std::string m_stringValue;

    // JSON containers are pointer-backed to keep copying cheap and explicit.
    std::shared_ptr<KJsonObject> m_objectValue;
    std::shared_ptr<KJsonArray> m_arrayValue;
};

#endif

