#include "KVariant.h"

#include "KString.h"

#include <algorithm>
#include <cctype>

namespace {

// ToLowerAscii creates a lowercase copy for simple boolean text parsing.
std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    return value;
}

} // namespace

KVariant::KVariant()
    : m_type(Invalid),
      m_boolValue(false),
      m_intValue(0),
      m_doubleValue(0.0),
      m_stringValue(),
      m_objectValue(),
      m_arrayValue() {
}

KVariant::KVariant(std::nullptr_t)
    : KVariant() {
}

KVariant::KVariant(bool value)
    : KVariant() {
    reset(Bool);
    m_boolValue = value;
}

KVariant::KVariant(int value)
    : KVariant(static_cast<long long>(value)) {
}

KVariant::KVariant(long long value)
    : KVariant() {
    reset(Int64);
    m_intValue = value;
}

KVariant::KVariant(double value)
    : KVariant() {
    reset(Double);
    m_doubleValue = value;
}

KVariant::KVariant(const char* value)
    : KVariant() {
    if (value) {
        reset(String);
        m_stringValue = value;
    }
}

KVariant::KVariant(const std::string& value)
    : KVariant() {
    reset(String);
    m_stringValue = value;
}

KVariant::KVariant(const KJsonObject& value)
    : KVariant() {
    reset(JsonObject);
    m_objectValue.reset(new KJsonObject(value));
}

KVariant::KVariant(const KJsonArray& value)
    : KVariant() {
    reset(JsonArray);
    m_arrayValue.reset(new KJsonArray(value));
}

KVariant::~KVariant() {
}

KVariant::Type KVariant::type() const {
    return m_type;
}

std::string KVariant::typeName() const {
    switch (m_type) {
    case Invalid:
        return "Invalid";
    case Bool:
        return "Bool";
    case Int64:
        return "Int64";
    case Double:
        return "Double";
    case String:
        return "String";
    case JsonObject:
        return "JsonObject";
    case JsonArray:
        return "JsonArray";
    }
    return "Unknown";
}

void KVariant::clear() {
    reset(Invalid);
}

bool KVariant::isValid() const {
    return m_type != Invalid;
}

bool KVariant::isNull() const {
    return m_type == Invalid;
}

bool KVariant::isBool() const {
    return m_type == Bool;
}

bool KVariant::isInt() const {
    return m_type == Int64;
}

bool KVariant::isDouble() const {
    return m_type == Double;
}

bool KVariant::isNumber() const {
    return m_type == Int64 || m_type == Double;
}

bool KVariant::isString() const {
    return m_type == String;
}

bool KVariant::isJsonObject() const {
    return m_type == JsonObject;
}

bool KVariant::isJsonArray() const {
    return m_type == JsonArray;
}

bool KVariant::toBool(bool defaultValue) const {
    if (m_type == Bool) {
        return m_boolValue;
    }
    if (m_type == Int64) {
        return m_intValue != 0;
    }
    if (m_type == Double) {
        return m_doubleValue != 0.0;
    }
    if (m_type == String) {
        const std::string text = ToLowerAscii(KString(m_stringValue).trim().stdString());
        if (text == "true" || text == "1" || text == "yes" || text == "on") {
            return true;
        }
        if (text == "false" || text == "0" || text == "no" || text == "off") {
            return false;
        }
    }
    return defaultValue;
}

long long KVariant::toInt(long long defaultValue) const {
    if (m_type == Int64) {
        return m_intValue;
    }
    if (m_type == Bool) {
        return m_boolValue ? 1 : 0;
    }
    if (m_type == Double) {
        return static_cast<long long>(m_doubleValue);
    }
    if (m_type == String) {
        bool ok = false;
        const long long value = KString(m_stringValue).toInt64(&ok, 10, defaultValue);
        return ok ? value : defaultValue;
    }
    return defaultValue;
}

double KVariant::toDouble(double defaultValue) const {
    if (m_type == Double) {
        return m_doubleValue;
    }
    if (m_type == Int64) {
        return static_cast<double>(m_intValue);
    }
    if (m_type == Bool) {
        return m_boolValue ? 1.0 : 0.0;
    }
    if (m_type == String) {
        bool ok = false;
        const double value = KString(m_stringValue).toDouble(&ok, defaultValue);
        return ok ? value : defaultValue;
    }
    return defaultValue;
}

std::string KVariant::toString(const std::string& defaultValue) const {
    if (m_type == String) {
        return m_stringValue;
    }
    if (m_type == Bool) {
        return m_boolValue ? "true" : "false";
    }
    if (m_type == Int64) {
        return KString::fromNumber(m_intValue).stdString();
    }
    if (m_type == Double) {
        return KString::fromNumber(m_doubleValue).stdString();
    }
    if (m_type == JsonObject && m_objectValue) {
        return m_objectValue->toJson(KJsonFormat::Compact);
    }
    if (m_type == JsonArray && m_arrayValue) {
        return m_arrayValue->toJson(KJsonFormat::Compact);
    }
    return defaultValue;
}

KJsonObject KVariant::toJsonObject() const {
    if (m_type == JsonObject && m_objectValue) {
        return *m_objectValue;
    }
    return KJsonObject();
}

KJsonArray KVariant::toJsonArray() const {
    if (m_type == JsonArray && m_arrayValue) {
        return *m_arrayValue;
    }
    return KJsonArray();
}

KJsonValue KVariant::toJsonValue() const {
    switch (m_type) {
    case Bool:
        return KJsonValue(m_boolValue);
    case Int64:
        return KJsonValue(m_intValue);
    case Double:
        return KJsonValue(m_doubleValue);
    case String:
        return KJsonValue(m_stringValue);
    case JsonObject:
        return m_objectValue ? KJsonValue(*m_objectValue) : KJsonValue(KJsonObject());
    case JsonArray:
        return m_arrayValue ? KJsonValue(*m_arrayValue) : KJsonValue(KJsonArray());
    case Invalid:
    default:
        return KJsonValue();
    }
}

KVariant KVariant::fromJsonValue(const KJsonValue& value) {
    if (value.isBool()) {
        return KVariant(value.toBool());
    }
    if (value.isInt()) {
        return KVariant(value.toInt());
    }
    if (value.isDouble()) {
        return KVariant(value.toDouble());
    }
    if (value.isString()) {
        return KVariant(value.toString());
    }
    if (value.isObject()) {
        return KVariant(value.toObject());
    }
    if (value.isArray()) {
        return KVariant(value.toArray());
    }
    return KVariant();
}

void KVariant::reset(Type nextType) {
    m_type = nextType;
    m_boolValue = false;
    m_intValue = 0;
    m_doubleValue = 0.0;
    m_stringValue.clear();
    m_objectValue.reset();
    m_arrayValue.reset();
}
