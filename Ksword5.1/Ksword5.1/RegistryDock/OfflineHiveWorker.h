#pragma once

#include <Windows.h>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>
#include <QTemporaryDir>
#include <QVector>
#include <array>
#include <memory>
#include <mutex>
#include <utility>

namespace ks::registry::offline
{
    inline constexpr DWORD MaximumValueBytes = 32 * 1024 * 1024;
    inline constexpr int BatchRows = 300;
    inline LPCWSTR wide(const QString& text) { return reinterpret_cast<LPCWSTR>(text.utf16()); }

    struct Key final
    {
        HKEY value = nullptr;
        ~Key() { if (value) RegCloseKey(value); }
        Key() = default;
        Key(const Key&) = delete;
        Key& operator=(const Key&) = delete;
        Key(Key&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
        Key& operator=(Key&& other) noexcept
        {
            if (this != &other) { if (value) RegCloseKey(value); value = std::exchange(other.value, nullptr); }
            return *this;
        }
    };
    inline LSTATUS linkStatus(HKEY key)
    {
        DWORD type = 0, size = 0;
        const auto status = RegQueryValueExW(key, L"SymbolicLinkValue", nullptr, &type, nullptr, &size);
        if (status == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
        if (status != ERROR_SUCCESS && status != ERROR_MORE_DATA) return status;
        return type == REG_LINK ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
    }
    inline LSTATUS openKey(HKEY hive, const QString& path, REGSAM access, Key* result)
    {
        if (!hive || path.contains(QChar(0))) return ERROR_INVALID_PARAMETER;
        Key current;
        auto status = RegOpenKeyExW(hive, L"", REG_OPTION_OPEN_LINK,
            access | KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &current.value);
        if (status != ERROR_SUCCESS) return status;
        if (!path.isEmpty()) for (const auto& part : path.split(QLatin1Char('\\'), Qt::KeepEmptyParts))
        {
            if (part.isEmpty()) return ERROR_INVALID_PARAMETER;
            status = linkStatus(current.value);
            if (status != ERROR_SUCCESS) return status;
            Key next;
            status = RegOpenKeyExW(current.value, wide(part), REG_OPTION_OPEN_LINK,
                access | KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &next.value);
            if (status != ERROR_SUCCESS) return status;
            current = std::move(next);
        }
        status = linkStatus(current.value);
        if (status == ERROR_SUCCESS) *result = std::move(current);
        return status;
    }
    inline LSTATUS readValue(HKEY key, const QString& name, DWORD* type, QByteArray* raw)
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            DWORD size = 0;
            auto status = RegQueryValueExW(key, wide(name), nullptr, type, nullptr, &size);
            if (status != ERROR_SUCCESS) return status;
            if (size > MaximumValueBytes) return ERROR_FILE_TOO_LARGE;
            QByteArray bytes(size, '\0');
            status = RegQueryValueExW(key, wide(name), nullptr, type, reinterpret_cast<LPBYTE>(bytes.data()), &size);
            if (status == ERROR_MORE_DATA) continue;
            if (status != ERROR_SUCCESS) return status;
            if (size > static_cast<DWORD>(bytes.size())) return ERROR_MORE_DATA;
            bytes.resize(size);
            *raw = std::move(bytes);
            return ERROR_SUCCESS;
        }
        return ERROR_RETRY;
    }

    struct Value
    {
        QString name;
        DWORD type = 0;
        QByteArray raw;
        qsizetype length = 0;
        LSTATUS status = ERROR_SUCCESS;
    };
    struct Snapshot
    {
        quint64 generation = 0;
        bool loaded = false;
        bool dirty = false;
        QString originalPath;
        QString copyPath;
    };
    struct CleanupReceipt
    {
        bool complete = false;
        bool retained = false;
        LSTATUS status = ERROR_SUCCESS;
        QString retainedPath;
    };
    // Native HKEY and temporary directory are accessed only inside serialized
    // backend operations. A retained cleanup job owns this value state through
    // final flush/close/removal; no QObject, widget or GUI continuation is here.
    struct State final
    {
        std::mutex io;
        std::mutex receiptMutex;
        HKEY hive = nullptr;
        std::unique_ptr<QTemporaryDir> temporary;
        Snapshot snapshot;
        CleanupReceipt cleanupReceipt;
    };
    enum class Action { Open, Keys, Values, ReadValue, ProbeWrite, CreateValue, UpdateValue, DeleteValue, CreateKey, DeleteKey, SaveCopy };
    enum class Outcome { Ok, NativeError, InvalidSource, CopyFailed, LoadFailed, InvalidTarget, FlushFailed,
        CloseFailed, SaveFailed, Conflict, AlreadyExists, NotEmpty, InvalidName, Stale };
    struct Request
    {
        Action action = Action::Keys;
        quint64 generation = 0;
        QString path, name, source, target;
        DWORD next = 0;
        Value original;
        DWORD type = 0;
        QByteArray data;
    };
    struct Result
    {
        Action action = Action::Keys;
        Outcome outcome = Outcome::Ok;
        LSTATUS status = ERROR_SUCCESS;
        LSTATUS reloadStatus = ERROR_SUCCESS;
        Snapshot snapshot;
        QString path, target;
        DWORD next = 0;
        bool more = false;
        bool submitted = false;
        bool verified = false;
        bool published = false;
        QVector<QString> keys;
        QVector<Value> values;
        Value value;
    };
    inline Result execute(State& state, const Request& request)
    {
        std::lock_guard<std::mutex> serialized(state.io);
        Result result;
        result.action = request.action;
        result.path = request.path;
        result.target = request.target;
        const auto finish = [&]() { result.snapshot = state.snapshot; result.snapshot.loaded = state.hive != nullptr; return result; };
        const auto fail = [&](Outcome outcome, LSTATUS status) { result.outcome = outcome; result.status = status; return finish(); };
        if (request.action == Action::Open)
        {
            if (state.hive || state.snapshot.dirty || state.temporary) return fail(Outcome::Conflict, ERROR_BUSY);
            const QFileInfo source(request.source);
            if (!source.isFile()) return fail(Outcome::InvalidSource, ERROR_FILE_NOT_FOUND);
            state.temporary = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/KSword-Hive-XXXXXX"));
            if (!state.temporary->isValid()) return fail(Outcome::InvalidSource, ERROR_PATH_NOT_FOUND);
            state.snapshot.originalPath = source.canonicalFilePath();
            state.snapshot.generation = request.generation;
            state.snapshot.copyPath = state.temporary->filePath(QStringLiteral("working-%1.hive").arg(request.generation));
            if (state.snapshot.originalPath.isEmpty() || !QFile::copy(state.snapshot.originalPath, state.snapshot.copyPath))
                return fail(Outcome::CopyFailed, ERROR_WRITE_FAULT);
            result.status = RegLoadAppKeyW(wide(state.snapshot.copyPath), &state.hive, KEY_READ | KEY_WRITE, REG_PROCESS_APPKEY, 0);
            if (result.status != ERROR_SUCCESS) { state.hive = nullptr; return fail(Outcome::LoadFailed, result.status); }
            state.snapshot.dirty = false;
            return finish();
        }
        if (request.generation != state.snapshot.generation) return fail(Outcome::Stale, ERROR_OPERATION_ABORTED);
        if (request.action == Action::SaveCopy)
        {
            const QFileInfo destination(request.target);
            const QString resolved = destination.exists() ? destination.canonicalFilePath() : destination.absoluteFilePath();
            const QString copy = QFileInfo(state.snapshot.copyPath).canonicalFilePath();
            if (request.target.isEmpty() || resolved.compare(state.snapshot.originalPath, Qt::CaseInsensitive) == 0
                || resolved.compare(copy, Qt::CaseInsensitive) == 0)
                return fail(Outcome::InvalidTarget, ERROR_INVALID_PARAMETER);
            result.status = state.hive ? RegFlushKey(state.hive) : ERROR_SUCCESS;
            if (result.status != ERROR_SUCCESS) return fail(Outcome::FlushFailed, result.status);
            result.status = state.hive ? RegCloseKey(state.hive) : ERROR_SUCCESS;
            if (result.status != ERROR_SUCCESS) return fail(Outcome::CloseFailed, result.status);
            state.hive = nullptr;
            QFile input(state.snapshot.copyPath);
            QSaveFile output(request.target);
            output.setDirectWriteFallback(false);
            bool copied = input.open(QIODevice::ReadOnly) && output.open(QIODevice::WriteOnly);
            while (copied && !input.atEnd())
            {
                const QByteArray block = input.read(1024 * 1024);
                if (input.error() != QFileDevice::NoError || block.isEmpty()) { copied = false; break; }
                qint64 written = 0;
                while (written < block.size())
                {
                    const qint64 count = output.write(block.constData() + written, block.size() - written);
                    if (count <= 0 || count > block.size() - written) { copied = false; break; }
                    written += count;
                }
            }
            if (copied) copied = output.commit();
            else output.cancelWriting();
            input.close();
            result.published = copied;
            if (copied) state.snapshot.dirty = false;
            result.reloadStatus = RegLoadAppKeyW(wide(state.snapshot.copyPath), &state.hive, KEY_READ | KEY_WRITE, REG_PROCESS_APPKEY, 0);
            if (result.reloadStatus != ERROR_SUCCESS) state.hive = nullptr;
            if (!copied) return fail(Outcome::SaveFailed, ERROR_WRITE_FAULT);
            return finish();
        }
        if (!state.hive) return fail(Outcome::NativeError, ERROR_INVALID_HANDLE);
        Key key;
        REGSAM access = KEY_READ;
        if (request.action == Action::CreateValue || request.action == Action::UpdateValue || request.action == Action::DeleteValue
            || request.action == Action::ReadValue || request.action == Action::ProbeWrite) access |= KEY_SET_VALUE;
        if (request.action == Action::CreateKey) access |= KEY_WRITE;
        if (request.action == Action::DeleteKey) access |= DELETE;
        result.status = openKey(state.hive, request.path, access, &key);
        if (result.status != ERROR_SUCCESS) return fail(Outcome::NativeError, result.status);
        if (request.action == Action::Keys)
        {
            DWORD index = request.next;
            for (int count = 0; count < BatchRows; ++count, ++index)
            {
                std::array<wchar_t, 256> name{};
                DWORD length = static_cast<DWORD>(name.size());
                result.status = RegEnumKeyExW(key.value, index, name.data(), &length, nullptr, nullptr, nullptr, nullptr);
                if (result.status != ERROR_SUCCESS)
                {
                    result.next = index;
                    if (result.status != ERROR_NO_MORE_ITEMS) return fail(Outcome::NativeError, result.status);
                    result.status = ERROR_SUCCESS;
                    return finish();
                }
                result.keys.push_back(QString::fromWCharArray(name.data(), length));
            }
            result.next = index;
            result.more = true;
            return finish();
        }
        if (request.action == Action::Values)
        {
            DWORD index = request.next;
            for (int count = 0; count < BatchRows; ++count, ++index)
            {
                std::array<wchar_t, 16384> name{};
                DWORD length = static_cast<DWORD>(name.size()), type = 0, size = 0;
                result.status = RegEnumValueW(key.value, index, name.data(), &length, nullptr, &type, nullptr, &size);
                if (result.status != ERROR_SUCCESS)
                {
                    result.next = index;
                    if (result.status != ERROR_NO_MORE_ITEMS) return fail(Outcome::NativeError, result.status);
                    result.status = ERROR_SUCCESS;
                    return finish();
                }
                Value value;
                value.name = QString::fromWCharArray(name.data(), length);
                value.type = type;
                value.length = size;
                if (size > MaximumValueBytes) value.status = ERROR_FILE_TOO_LARGE;
                else if (size <= 64 * 1024)
                {
                    value.status = readValue(key.value, value.name, &value.type, &value.raw);
                    if (value.status == ERROR_SUCCESS) value.length = value.raw.size();
                    // Only display bytes are returned by list projection; editors
                    // always issue ReadValue and get the full immutable baseline.
                    value.raw = value.raw.left(64);
                }
                result.values.push_back(std::move(value));
            }
            result.next = index;
            result.more = true;
            return finish();
        }
        if (request.action == Action::ProbeWrite) return finish();
        if (request.action == Action::ReadValue)
        {
            result.value.name = request.name;
            result.status = readValue(key.value, request.name, &result.value.type, &result.value.raw);
            result.value.status = result.status;
            result.value.length = result.value.raw.size();
            if (result.status != ERROR_SUCCESS) return fail(Outcome::NativeError, result.status);
            return finish();
        }
        if (request.action == Action::CreateValue || request.action == Action::UpdateValue || request.action == Action::DeleteValue)
        {
            if (request.name.contains(QChar(0)) || request.data.size() > MaximumValueBytes) return fail(Outcome::InvalidName, ERROR_INVALID_PARAMETER);
            DWORD type = 0;
            QByteArray raw;
            result.status = readValue(key.value, request.name, &type, &raw);
            if ((request.action == Action::CreateValue && result.status != ERROR_FILE_NOT_FOUND)
                || (request.action != Action::CreateValue && (result.status != ERROR_SUCCESS
                    || type != request.original.type || raw != request.original.raw || request.name != request.original.name)))
                return fail(Outcome::Conflict, result.status);
            result.status = request.action == Action::DeleteValue ? RegDeleteValueW(key.value, wide(request.name))
                : RegSetValueExW(key.value, wide(request.name), 0, request.type,
                    reinterpret_cast<const BYTE*>(request.data.constData()), static_cast<DWORD>(request.data.size()));
            if (result.status != ERROR_SUCCESS) return fail(Outcome::NativeError, result.status);
            result.submitted = true;
            state.snapshot.dirty = true;
            result.status = readValue(key.value, request.name, &type, &raw);
            result.verified = request.action == Action::DeleteValue ? result.status == ERROR_FILE_NOT_FOUND
                : result.status == ERROR_SUCCESS && type == request.type && raw == request.data;
            return finish();
        }
        if (request.action == Action::CreateKey)
        {
            if (request.name.isEmpty() || request.name.size() > 255 || request.name.contains(QLatin1Char('\\')) || request.name.contains(QChar(0)))
                return fail(Outcome::InvalidName, ERROR_INVALID_PARAMETER);
            Key created;
            DWORD disposition = 0;
            result.status = RegCreateKeyExW(key.value, wide(request.name), 0, nullptr, REG_OPTION_NON_VOLATILE,
                KEY_READ | KEY_WRITE, nullptr, &created.value, &disposition);
            if (result.status != ERROR_SUCCESS) return fail(Outcome::NativeError, result.status);
            if (disposition != REG_CREATED_NEW_KEY) return fail(Outcome::AlreadyExists, ERROR_ALREADY_EXISTS);
            result.submitted = true;
            state.snapshot.dirty = true;
            return finish();
        }
        if (request.action == Action::DeleteKey)
        {
            if (request.path.isEmpty()) return fail(Outcome::InvalidName, ERROR_INVALID_PARAMETER);
            DWORD children = 0, values = 0;
            result.status = RegQueryInfoKeyW(key.value, nullptr, nullptr, nullptr, &children, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
            if (result.status != ERROR_SUCCESS) return fail(Outcome::NativeError, result.status);
            if (children || values) return fail(Outcome::NotEmpty, ERROR_DIR_NOT_EMPTY);
            key = Key{};
            const auto slash = request.path.lastIndexOf(QLatin1Char('\\'));
            const auto parentPath = slash < 0 ? QString() : request.path.left(slash);
            const auto name = slash < 0 ? request.path : request.path.mid(slash + 1);
            Key parent;
            result.status = openKey(state.hive, parentPath, KEY_READ | KEY_WRITE, &parent);
            if (result.status == ERROR_SUCCESS) result.status = RegDeleteKeyExW(parent.value, wide(name), 0, 0);
            if (result.status != ERROR_SUCCESS) return fail(Outcome::NativeError, result.status);
            result.submitted = true;
            state.snapshot.dirty = true;
            return finish();
        }
        return fail(Outcome::NativeError, ERROR_INVALID_FUNCTION);
    }
    inline bool cleanup(State& state, const bool discard)
    {
        std::lock_guard<std::mutex> serialized(state.io);
        { std::lock_guard<std::mutex> lock(state.receiptMutex); if (state.cleanupReceipt.complete) return true; }
        CleanupReceipt receipt;
        const auto publish = [&]() { std::lock_guard<std::mutex> lock(state.receiptMutex); state.cleanupReceipt = receipt; };
        receipt.status = state.hive && state.snapshot.dirty && !discard ? RegFlushKey(state.hive) : ERROR_SUCCESS;
        if (receipt.status != ERROR_SUCCESS) { publish(); return false; }
        receipt.status = state.hive ? RegCloseKey(state.hive) : ERROR_SUCCESS;
        if (receipt.status != ERROR_SUCCESS) { publish(); return false; }
        state.hive = nullptr;
        state.snapshot.loaded = false;
        if (state.temporary)
        {
            if (state.snapshot.dirty && !discard)
            {
                // Forced parent/app shutdown cannot silently discard unsaved data.
                state.temporary->setAutoRemove(false);
                receipt.retained = true;
                receipt.retainedPath = state.snapshot.copyPath;
            }
            else if (!state.temporary->remove())
            {
                receipt.status = ERROR_WRITE_FAULT;
                receipt.retained = true;
                receipt.retainedPath = state.snapshot.copyPath;
                publish();
                return false;
            }
            state.temporary.reset();
        }
        receipt.complete = true;
        publish();
        return true;
    }
    inline CleanupReceipt cleanupReceipt(State& state)
    {
        std::lock_guard<std::mutex> lock(state.receiptMutex);
        return state.cleanupReceipt;
    }
}
