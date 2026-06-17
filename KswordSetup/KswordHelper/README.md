# KswordHelper

`KswordHelper` is a foundational utility layer intended to sit beside
`KswordWinAPICore`. It has no Qt dependency and no third-party dependency; only
C++ standard library headers are used.

## Contents

- `KJsonValue`, `KJsonObject`, `KJsonArray`, `KJsonDocument`, `KJsonParseError`
  - Recursive descent JSON parser.
  - Supports null, bool, int64, double, UTF-8 string, array, and object.
  - Handles JSON escapes, Unicode `\uXXXX`, surrogate pairs, and line/column
    parse errors.
  - Supports compact and pretty JSON output.
- `KString`
  - trim, split, join, replace, startsWith, endsWith, toInt, toDouble,
    fromNumber.
- `KVariant`
  - Lightweight QVariant-style holder for bool, int64, double, string, JSON
    object, and JSON array.
- `KPoint`, `KSize`, `KRect`
  - GUI-independent integer geometry helpers.
- `KSettings`
  - JSON-file backed settings with value, setValue, load, and save.
  - Slash-separated keys such as `window/width` create nested JSON objects.
- `KSignal`
  - Header-only signal-slot template using `std::function`.
- `KObject`
  - objectName, parent/children, dynamic properties, and basic lifecycle
    management.

## Ownership note

`KObject` follows a Qt-like parent owns children rule. If a child is attached to
a parent, it should be heap allocated or detached before either object is
destroyed. Destroying a parent deletes all direct and nested children.

## Integration files

The project file was intentionally not modified. Mainline integration should add:

Headers:

- `KswordFrame3.0\KswordHelper\KswordHelper.h`
- `KswordFrame3.0\KswordHelper\KJson.h`
- `KswordFrame3.0\KswordHelper\KString.h`
- `KswordFrame3.0\KswordHelper\KVariant.h`
- `KswordFrame3.0\KswordHelper\KGeometry.h`
- `KswordFrame3.0\KswordHelper\KSettings.h`
- `KswordFrame3.0\KswordHelper\KSignal.h`
- `KswordFrame3.0\KswordHelper\KObject.h`

Sources:

- `KswordFrame3.0\KswordHelper\KJson.cpp`
- `KswordFrame3.0\KswordHelper\KString.cpp`
- `KswordFrame3.0\KswordHelper\KVariant.cpp`
- `KswordFrame3.0\KswordHelper\KGeometry.cpp`
- `KswordFrame3.0\KswordHelper\KSettings.cpp`
- `KswordFrame3.0\KswordHelper\KObject.cpp`

`KSignal.h` is template-only and does not need a `.cpp` file.
