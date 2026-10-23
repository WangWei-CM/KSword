#pragma once

#include "../UI/StructuredFieldView.h"

#include <QBuffer>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QRunnable>
#include <QTemporaryFile>
#include <QThreadPool>

#include <functional>
#include <limits>
#include <memory>

namespace ks::monitor
{
    // Values only. The capture thread alone creates/uses/destroys its QTemporaryFile.
    // GUI owns the archive lifetime but never reads or locks its writer state.
    class WmiEventArchive final
    {
    public:
        struct WriterOps
        {
            std::function<qint64(QFile&, const char*, qint64)> write =
                [](QFile& file, const char* bytes, qint64 count) { return file.write(bytes, count); };
            std::function<bool(QFile&)> flush = [](QFile& file) { return file.flush(); };
            std::function<bool(QFile&, qint64)> resize =
                [](QFile& file, qint64 size) { return file.resize(size); };
        };
        explicit WmiEventArchive(QString fileTemplate = {})
            : WmiEventArchive(std::move(fileTemplate), WriterOps{}) {}
        WmiEventArchive(QString fileTemplate, WriterOps operations)
            : m_template(fileTemplate.isEmpty()
                ? QDir::tempPath() + QStringLiteral("/ksword-wmi-XXXXXX.kswmi") : std::move(fileTemplate)),
              m_ops(std::move(operations)) {}

        ~WmiEventArchive()
        {
            // finish() is required on the writer thread, including all startup exits.
            Q_ASSERT(!m_file);
            if (!m_path.isEmpty())
                QThreadPool::globalInstance()->start(QRunnable::create([path = m_path]() { QFile::remove(path); }));
        }

        WmiEventArchive(const WmiEventArchive&) = delete;
        WmiEventArchive& operator=(const WmiEventArchive&) = delete;

        template<class Row>
        bool append(const Row& row, QString* error)
        {
            if (m_failed || m_finished) return fail(QStringLiteral("WMI archive is already closed or failed"), error);
            if (!m_file && !open(error)) return false;
            QByteArray payload;
            QDataStream stream(&payload, QIODevice::WriteOnly);
            stream.setVersion(QDataStream::Qt_6_9);
            stream << quint64(row.sequence) << row.timestamp << row.provider << row.className << row.pidAndName << row.document.title;
            if (!writeNodes(stream, row.document.nodes, 0) || stream.status() != QDataStream::Ok)
                return fail(QStringLiteral("WMI archive record serialization failed"), error);

            QByteArray prefix;
            QDataStream length(&prefix, QIODevice::WriteOnly);
            length.setVersion(QDataStream::Qt_6_9);
            length << quint64(payload.size());
            if (!writeAll(*m_file, prefix) || !writeAll(*m_file, payload) || !m_ops.flush(*m_file))
            {
                QString failure = m_file->errorString();
                // Unbuffered QFile writes plus a successful per-frame flush define
                // acceptance into the OS file cache, not power-loss durability.
                // Never let a failed tail change the last accepted prefix/count.
                if (!m_ops.resize(*m_file, m_committedBytes))
                {
                    m_incompleteTail = true;
                    failure += QStringLiteral("; failed to remove incomplete tail; accepted prefix bytes=%1")
                        .arg(m_committedBytes);
                }
                return fail(failure, error);
            }
            m_committedBytes = m_file->pos();
            ++m_count;
            return true;
        }

        bool finish(QString* error = nullptr)
        {
            if (m_file)
            {
                if (!m_failed && !m_ops.flush(*m_file)) fail(m_file->errorString(), error);
                m_file->close();
                m_file.reset();
            }
            m_finished = true;
            if (m_failed && error) *error = m_error;
            return !m_failed;
        }

        QString path() const { return m_path; } // writer-only until its finished value is published
        quint64 count() const { return m_count; }
        qint64 committedBytes() const { return m_committedBytes; }
        bool hasIncompleteTail() const { return m_incompleteTail; }
        QString error() const { return m_error; }

        template<class Row, class Visitor>
        static bool scan(const QString& path, Visitor visitor, QString* error = nullptr,
            qint64 committedBytes = -1)
        {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) { if (error) *error = file.errorString(); return false; }
            if (file.read(8) != QByteArray("KSWMI001", 8)) { if (error) *error = QStringLiteral("Invalid WMI archive header"); return false; }
            const qint64 end = committedBytes < 0 ? file.size() : committedBytes;
            if (end < 8 || end > file.size())
            { if (error) *error = QStringLiteral("Invalid WMI archive accepted prefix"); return false; }
            while (file.pos() < end)
            {
                if (end - file.pos() < qint64(sizeof(quint64)))
                { if (error) *error = QStringLiteral("Truncated WMI archive record length"); return false; }
                QDataStream length(&file); length.setVersion(QDataStream::Qt_6_9);
                quint64 bytes = 0; length >> bytes;
                if (length.status() != QDataStream::Ok || bytes > quint64(end - file.pos())
                    || bytes > quint64((std::numeric_limits<qsizetype>::max)()))
                { if (error) *error = QStringLiteral("Truncated WMI archive record"); return false; }
                const QByteArray payload = file.read(static_cast<qint64>(bytes));
                if (quint64(payload.size()) != bytes) { if (error) *error = file.errorString(); return false; }
                QDataStream stream(payload); stream.setVersion(QDataStream::Qt_6_9);
                Row row;
                stream >> row.sequence >> row.timestamp >> row.provider >> row.className >> row.pidAndName >> row.document.title;
                if (!readNodes(stream, row.document.nodes, 0) || stream.status() != QDataStream::Ok || !stream.atEnd())
                { if (error) *error = QStringLiteral("Invalid WMI archive document"); return false; }
                if (!visitor(row)) return true;
            }
            return file.error() == QFileDevice::NoError;
        }

    private:
        bool writeAll(QFile& file, const QByteArray& data)
        {
            qint64 written = 0;
            while (written < data.size())
            {
                const auto count = m_ops.write(file, data.constData() + written, data.size() - written);
                if (count <= 0 || count > data.size() - written) return false;
                written += count;
            }
            return true;
        }

        bool open(QString* error)
        {
            m_file = std::make_unique<QTemporaryFile>(m_template);
            m_file->setAutoRemove(false);
            if (!static_cast<QFile*>(m_file.get())->open(QIODevice::ReadWrite | QIODevice::Unbuffered))
                return fail(m_file->errorString(), error);
            m_path = m_file->fileName();
            if (!writeAll(*m_file, QByteArray("KSWMI001", 8)) || !m_ops.flush(*m_file))
                return fail(m_file->errorString(), error);
            m_committedBytes = m_file->pos();
            return true;
        }

        bool fail(const QString& message, QString* error)
        {
            m_failed = true;
            if (m_error.isEmpty()) m_error = message;
            if (error) *error = m_error;
            return false;
        }

        static bool writeNodes(QDataStream& stream, const QVector<ks::ui::FieldNode>& nodes, const int depth)
        {
            if (depth > 256) return false;
            stream << quint64(nodes.size());
            for (const auto& node : nodes)
            {
                stream << quint8(node.kind) << node.name << node.value << node.translateValue << node.initiallyExpanded
                    << node.translateName << quint8(node.status) << node.summaryLabel << quint8(node.reportRole)
                    << qint32(node.reportedCount) << node.preferredScope;
                if (!writeNodes(stream, node.children, depth + 1)) return false;
            }
            return stream.status() == QDataStream::Ok;
        }

        static bool readNodes(QDataStream& stream, QVector<ks::ui::FieldNode>& nodes, const int depth)
        {
            if (depth > 256) return false;
            quint64 count = 0; stream >> count;
            if (stream.status() != QDataStream::Ok || count > quint64(stream.device()->bytesAvailable())) return false;
            nodes.reserve(static_cast<qsizetype>(count));
            for (quint64 i = 0; i < count; ++i)
            {
                ks::ui::FieldNode node;
                quint8 kind = 0, status = 0, role = 0; qint32 reported = -1;
                stream >> kind >> node.name >> node.value >> node.translateValue >> node.initiallyExpanded
                    >> node.translateName >> status >> node.summaryLabel >> role >> reported >> node.preferredScope;
                if (kind > quint8(ks::ui::FieldNode::Kind::Note) || status > quint8(ks::ui::FieldNode::Status::Error)
                    || role > quint8(ks::ui::FieldNode::ReportRole::Metadata)) return false;
                node.kind = static_cast<ks::ui::FieldNode::Kind>(kind);
                node.status = static_cast<ks::ui::FieldNode::Status>(status);
                node.reportRole = static_cast<ks::ui::FieldNode::ReportRole>(role);
                node.reportedCount = reported;
                if (!readNodes(stream, node.children, depth + 1)) return false;
                nodes.push_back(std::move(node));
            }
            return stream.status() == QDataStream::Ok;
        }

        QString m_template, m_path, m_error;
        std::unique_ptr<QTemporaryFile> m_file;
        WriterOps m_ops;
        quint64 m_count = 0;
        qint64 m_committedBytes = 0;
        bool m_failed = false, m_finished = false, m_incompleteTail = false;
    };
}
