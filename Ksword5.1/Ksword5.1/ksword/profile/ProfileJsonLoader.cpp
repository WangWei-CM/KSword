#include "ProfileJsonLoader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QStringList>

namespace
{
    // hasQtCompressedJsonSuffix:
    // - Input pathText: candidate profile path.
    // - Processing: performs a case-insensitive suffix check for the local
    //   "*.json.qz" convention used by the release copy target.
    // - Return: true when the path names a Qt-compressed profile payload.
    bool hasQtCompressedJsonSuffix(const QString& pathText)
    {
        return pathText.endsWith(QStringLiteral(".json.qz"), Qt::CaseInsensitive);
    }

    // appendUniqueCleanPath:
    // - Input paths/pathText: mutable path list and a candidate path.
    // - Processing: trims, normalizes separators, and keeps the first
    //   case-insensitive occurrence so diagnostics stay readable.
    // - Return: no return value; paths is updated in place.
    void appendUniqueCleanPath(QStringList& paths, const QString& pathText)
    {
        const QString trimmedPath = pathText.trimmed();
        if (trimmedPath.isEmpty())
        {
            return;
        }

        const QString cleanPath = QDir::cleanPath(trimmedPath);
        if (!cleanPath.isEmpty() && !paths.contains(cleanPath, Qt::CaseInsensitive))
        {
            paths.push_back(cleanPath);
        }
    }

    // profileCandidatePaths:
    // - Input jsonPath: canonical plain JSON path or explicit compressed path.
    // - Processing: constructs the exact search order used by the runtime:
    //   compressed first for normal "*.json" callers, plain fallback second.
    // - Return: ordered candidate list, possibly empty when input is blank.
    QStringList profileCandidatePaths(const QString& jsonPath)
    {
        QStringList paths;
        const QString cleanPath = QDir::cleanPath(jsonPath.trimmed());
        if (cleanPath.isEmpty())
        {
            return paths;
        }

        if (hasQtCompressedJsonSuffix(cleanPath))
        {
            appendUniqueCleanPath(paths, cleanPath);
            appendUniqueCleanPath(paths, cleanPath.left(cleanPath.size() - 3));
        }
        else
        {
            appendUniqueCleanPath(paths, cleanPath + QStringLiteral(".qz"));
            appendUniqueCleanPath(paths, cleanPath);
        }

        return paths;
    }
}

namespace ks::profile
{
    QString resolveProfileJsonPath(const QString& jsonPath)
    {
        // The resolver intentionally does not parse or open files.  It only
        // answers "which candidate exists" so old search loops can keep using
        // their "*.json" names while Release deployments ship "*.json.qz".
        for (const QString& candidatePath : profileCandidatePaths(jsonPath))
        {
            const QFileInfo fileInfo(candidatePath);
            if (fileInfo.exists() && fileInfo.isFile())
            {
                return fileInfo.absoluteFilePath();
            }
        }

        return QString();
    }

    QByteArray readProfileJsonBytes(const QString& jsonPath, QString* errorTextOut)
    {
        if (errorTextOut != nullptr)
        {
            errorTextOut->clear();
        }

        QStringList diagnostics;
        for (const QString& candidatePath : profileCandidatePaths(jsonPath))
        {
            const QFileInfo fileInfo(candidatePath);
            if (!fileInfo.exists() || !fileInfo.isFile())
            {
                diagnostics << QStringLiteral("not found: %1").arg(QDir::toNativeSeparators(candidatePath));
                continue;
            }

            const QString resolvedPath = fileInfo.absoluteFilePath();
            QFile file(resolvedPath);
            if (!file.open(QIODevice::ReadOnly))
            {
                diagnostics << QStringLiteral("open failed: %1 (%2)")
                    .arg(QDir::toNativeSeparators(resolvedPath), file.errorString());
                continue;
            }

            const QByteArray fileBytes = file.readAll();
            if (!hasQtCompressedJsonSuffix(resolvedPath))
            {
                if (fileBytes.isEmpty())
                {
                    diagnostics << QStringLiteral("plain profile JSON is empty: %1")
                        .arg(QDir::toNativeSeparators(resolvedPath));
                    continue;
                }
                return fileBytes;
            }

            // qCompress writes a four-byte big-endian uncompressed length
            // followed by a zlib stream.  The Python build helper mirrors that
            // exact layout, so qUncompress is the safest reader and avoids
            // maintaining a custom decompressor in application code.  If this
            // compressed candidate is broken, the loop intentionally continues
            // to a plain JSON fallback when one exists.
            if (fileBytes.size() < 4)
            {
                diagnostics << QStringLiteral("compressed profile JSON is truncated: %1")
                    .arg(QDir::toNativeSeparators(resolvedPath));
                continue;
            }

            const QByteArray jsonBytes = qUncompress(fileBytes);
            if (jsonBytes.isEmpty())
            {
                diagnostics << QStringLiteral("qUncompress failed: %1")
                    .arg(QDir::toNativeSeparators(resolvedPath));
                continue;
            }

            return jsonBytes;
        }

        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("Profile JSON not readable: %1%2")
                .arg(QDir::toNativeSeparators(jsonPath))
                .arg(diagnostics.isEmpty()
                    ? QString()
                    : QStringLiteral(" (%1)").arg(diagnostics.join(QStringLiteral("; "))));
        }
        return {};
    }

    QJsonDocument readProfileJsonDocument(
        const QString& jsonPath,
        QJsonParseError* parseErrorOut,
        QString* errorTextOut)
    {
        if (errorTextOut != nullptr)
        {
            errorTextOut->clear();
        }
        if (parseErrorOut != nullptr)
        {
            *parseErrorOut = QJsonParseError{};
        }

        QString readErrorText;
        const QByteArray jsonBytes = readProfileJsonBytes(jsonPath, &readErrorText);
        if (jsonBytes.isEmpty())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = readErrorText;
            }
            return {};
        }

        QJsonParseError localParseError{};
        const QJsonDocument document = QJsonDocument::fromJson(jsonBytes, &localParseError);
        if (parseErrorOut != nullptr)
        {
            *parseErrorOut = localParseError;
        }
        if (localParseError.error != QJsonParseError::NoError && errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("Profile JSON parse failed: %1").arg(localParseError.errorString());
        }

        return document;
    }
}
