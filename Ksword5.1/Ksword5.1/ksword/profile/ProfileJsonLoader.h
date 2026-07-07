#pragma once

// ============================================================
// ksword/profile/ProfileJsonLoader.h
// Namespace: ks::profile
// Purpose:
// - Provide one runtime entry point for profile JSON loading.
// - Prefer Qt qCompress-compatible "*.json.qz" files in Release builds.
// - Keep plain "*.json" fallback for local development and manual debugging.
// ============================================================

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QString>

namespace ks::profile
{
    // resolveProfileJsonPath:
    // - Input jsonPath: a canonical "*.json" path or an explicit "*.json.qz" path.
    // - Processing: when a plain JSON path is supplied, checks the sibling
    //   compressed file first; when a compressed path is supplied, checks it
    //   first and then falls back to the stripped plain JSON path.
    // - Return: the existing file path selected for reading, or an empty string
    //   when neither compressed nor plain profile data exists.
    QString resolveProfileJsonPath(const QString& jsonPath);

    // readProfileJsonBytes:
    // - Input jsonPath: a profile JSON path, usually ending in "*.json".
    // - Processing: resolves compressed/plain candidates, reads the file, and
    //   qUncompresses "*.qz" payloads produced by the build-time Python helper.
    // - Return: uncompressed JSON bytes; returns an empty QByteArray on failure
    //   and writes a human-readable reason to errorTextOut when provided.
    QByteArray readProfileJsonBytes(const QString& jsonPath, QString* errorTextOut = nullptr);

    // readProfileJsonDocument:
    // - Input jsonPath: profile JSON path plus optional parse/error outputs.
    // - Processing: obtains uncompressed bytes through readProfileJsonBytes()
    //   and parses them with QJsonDocument::fromJson().
    // - Return: parsed JSON document; returns a null document on read or parse
    //   failure. parseErrorOut receives Qt's parse status when provided.
    QJsonDocument readProfileJsonDocument(
        const QString& jsonPath,
        QJsonParseError* parseErrorOut = nullptr,
        QString* errorTextOut = nullptr);
}
