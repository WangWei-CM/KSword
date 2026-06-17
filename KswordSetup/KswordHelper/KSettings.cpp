#include "KSettings.h"

#include <fstream>
#include <sstream>
#include <vector>

namespace {

// SplitKey converts slash-separated setting names into path segments. Empty
// segments are ignored so callers can pass either "a/b" or "/a/b/".
std::vector<std::string> SplitKey(const std::string& key) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= key.size()) {
        const std::size_t pos = key.find('/', start);
        const std::size_t end = (pos == std::string::npos) ? key.size() : pos;
        if (end > start) {
            parts.push_back(key.substr(start, end - start));
        }
        if (pos == std::string::npos) {
            break;
        }
        start = pos + 1;
    }
    return parts;
}

// FindPath resolves a key path inside object. It returns false when any segment
// is missing or when an intermediate value is not a JSON object.
bool FindPath(const KJsonObject& object, const std::vector<std::string>& parts, KJsonValue& output) {
    if (parts.empty()) {
        return false;
    }

    KJsonObject current = object;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (!current.contains(parts[i])) {
            return false;
        }
        const KJsonValue value = current.value(parts[i]);
        if (i + 1 == parts.size()) {
            output = value;
            return true;
        }
        if (!value.isObject()) {
            return false;
        }
        current = value.toObject();
    }
    return false;
}

// SetPath writes value into object by recursive copy/update. It creates missing
// intermediate objects and overwrites non-object intermediates with objects.
void SetPath(KJsonObject& object, const std::vector<std::string>& parts, std::size_t index, const KJsonValue& value) {
    if (index >= parts.size()) {
        return;
    }

    if (index + 1 == parts.size()) {
        object.insert(parts[index], value);
        return;
    }

    KJsonObject child;
    const KJsonValue existing = object.value(parts[index]);
    if (existing.isObject()) {
        child = existing.toObject();
    }
    SetPath(child, parts, index + 1, value);
    object.insert(parts[index], KJsonValue(child));
}

// MarkLoadError writes a synthetic parse error for file and root-contract errors.
void MarkLoadError(KJsonParseError& error, const std::string& message) {
    error.error = KJsonParseError::IllegalValue;
    error.offset = 0;
    error.line = 1;
    error.column = 1;
    error.message = message;
}

} // namespace

KSettings::KSettings()
    : m_filePath(), m_root(), m_lastParseError() {
}

KSettings::KSettings(const std::string& filePath)
    : m_filePath(filePath), m_root(), m_lastParseError() {
}

void KSettings::setFileName(const std::string& filePath) {
    m_filePath = filePath;
}

std::string KSettings::fileName() const {
    return m_filePath;
}

bool KSettings::load() {
    if (m_filePath.empty()) {
        MarkLoadError(m_lastParseError, "Settings file path is empty");
        return false;
    }
    return load(m_filePath);
}

bool KSettings::load(const std::string& filePath) {
    m_filePath = filePath;
    m_lastParseError.clear();

    std::ifstream input(filePath.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        MarkLoadError(m_lastParseError, "Unable to open settings file for reading");
        return false;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string bytes = buffer.str();

    KJsonParseError parseError;
    const KJsonDocument document = KJsonDocument::fromJson(bytes, &parseError);
    if (parseError.hasError()) {
        m_lastParseError = parseError;
        return false;
    }
    if (!document.isObject()) {
        MarkLoadError(m_lastParseError, "Settings root must be a JSON object");
        return false;
    }

    m_root = document.object();
    m_lastParseError.clear();
    return true;
}

bool KSettings::save() const {
    if (m_filePath.empty()) {
        return false;
    }
    return save(m_filePath);
}

bool KSettings::save(const std::string& filePath) const {
    std::ofstream output(filePath.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }

    const KJsonDocument document(m_root);
    output << document.toJson(KJsonFormat::Pretty);
    output << '\n';
    return output.good();
}

KVariant KSettings::value(const std::string& key, const KVariant& defaultValue) const {
    KJsonValue jsonValue;
    if (!FindPath(m_root, SplitKey(key), jsonValue)) {
        return defaultValue;
    }
    return KVariant::fromJsonValue(jsonValue);
}

void KSettings::setValue(const std::string& key, const KVariant& value) {
    const std::vector<std::string> parts = SplitKey(key);
    if (parts.empty()) {
        return;
    }
    SetPath(m_root, parts, 0, value.toJsonValue());
}

bool KSettings::contains(const std::string& key) const {
    KJsonValue ignored;
    return FindPath(m_root, SplitKey(key), ignored);
}

void KSettings::clear() {
    m_root.clear();
    m_lastParseError.clear();
}

KJsonObject KSettings::object() const {
    return m_root;
}

void KSettings::setObject(const KJsonObject& object) {
    m_root = object;
}

KJsonParseError KSettings::lastParseError() const {
    return m_lastParseError;
}

std::string KSettings::lastErrorString() const {
    return m_lastParseError.errorString();
}
