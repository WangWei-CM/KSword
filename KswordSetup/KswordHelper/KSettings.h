#ifndef KSWORD_HELPER_KSETTINGS_HEAD_FILE
#define KSWORD_HELPER_KSETTINGS_HEAD_FILE

#include "KJson.h"
#include "KVariant.h"

#include <string>

// KSettings is a small JSON-file backed settings container. It stores a root
// JSON object and supports slash-separated keys such as "window/width".
class KSettings {
public:
    // Constructors optionally bind the settings object to a file path.
    KSettings();
    explicit KSettings(const std::string& filePath);

    // setFileName changes the target path for later load()/save() calls.
    void setFileName(const std::string& filePath);

    // fileName returns the currently configured settings file path.
    std::string fileName() const;

    // load reads and parses the configured file. It returns true on success.
    bool load();

    // load(filePath) updates the target path first, then loads it.
    bool load(const std::string& filePath);

    // save writes the current settings object to the configured file as pretty JSON.
    bool save() const;

    // save(filePath) writes to an explicit path without mutating the target path.
    bool save(const std::string& filePath) const;

    // value returns the variant stored at key or defaultValue when missing.
    KVariant value(const std::string& key, const KVariant& defaultValue = KVariant()) const;

    // setValue writes value at key, creating intermediate JSON objects as needed.
    void setValue(const std::string& key, const KVariant& value);

    // contains reports whether key resolves to an existing setting value.
    bool contains(const std::string& key) const;

    // clear removes all settings from memory and returns no value.
    void clear();

    // object returns a copy of the root JSON object for integration code.
    KJsonObject object() const;

    // setObject replaces the root JSON object and returns no value.
    void setObject(const KJsonObject& object);

    // lastParseError returns the last load failure detail, if any.
    KJsonParseError lastParseError() const;

    // lastErrorString returns parse/load details in human-readable form.
    std::string lastErrorString() const;

private:
    // m_filePath stores the target JSON file path.
    std::string m_filePath;

    // m_root stores settings as a JSON object.
    KJsonObject m_root;

    // m_lastParseError stores parser or root-contract load failures.
    KJsonParseError m_lastParseError;
};

#endif
