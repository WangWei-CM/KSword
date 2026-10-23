#pragma once

#include <QByteArray>
#include <QDialog>
#include <QString>
#include <QVector>

#include <cstdint>
#include <optional>
#include <memory>

class QLabel;

namespace ks::ui
{
    class SnapshotWorkbenchWidget;
    class AsyncOperation;
    enum class DisassemblyArchitecture : int
    {
        X86 = 0,
        X64
    };

    struct DisassemblyRow
    {
        std::uint64_t address = 0U;
        std::uint32_t byteOffset = 0U;
        QByteArray bytes;
        QString mnemonic;
        QString operands;
        bool decoded = false;
    };

    struct DisassemblyResult
    {
        QVector<DisassemblyRow> rows;
        QString backendName;
        QString diagnosticText;
        bool complete = false;
    };

    struct DisassemblySelection
    {
        std::uint64_t address = 0U;
        std::uint32_t byteOffset = 0U;
        QByteArray originalBytes;
    };

    class InstructionDecoder final
    {
    public:
        static DisassemblyResult decode(
            const QByteArray& bytes,
            std::uint64_t baseAddress,
            DisassemblyArchitecture architecture,
            std::uint32_t maximumInstructions = 4096U);
        // Exact legacy backend diagnostics, without materializing discarded row projections.
        static DisassemblyResult describe(const QByteArray& bytes, std::uint64_t baseAddress,
            DisassemblyArchitecture architecture, std::uint32_t maximumInstructions = 4096U);
    private:
        static DisassemblyResult decodeImpl(const QByteArray& bytes, std::uint64_t baseAddress,
            DisassemblyArchitecture architecture, std::uint32_t maximumInstructions, bool collectRows);
    };

    class KernelDisassemblyDialog final : public QDialog
    {
        Q_OBJECT

    public:
        explicit KernelDisassemblyDialog(QWidget* parent = nullptr);
        ~KernelDisassemblyDialog() override;

        static void openKernelAddress(
            QWidget* parent,
            std::uint64_t address,
            const QString& sourceDescription,
            std::uint32_t byteCount = 4096U);

        void setSnapshot(
            const QByteArray& bytes,
            std::uint64_t baseAddress,
            DisassemblyArchitecture architecture,
            const QString& sourceDescription);
        void setKernelMutationEnabled(bool enabled);

        QByteArray originalBytes() const;
        std::uint64_t baseAddress() const;
        std::optional<DisassemblySelection>
            selectedByteRange() const;

    signals:
        void requestModifyBytes(
            std::uint64_t address,
            QByteArray originalBytes);

    private:
        void rebuildRows();
        void updateDecodeDescription();
        void readKernelSnapshot(std::uint64_t address, std::uint32_t length, QString sourceDescription);
        std::unique_ptr<AsyncOperation> m_readOperation;
        std::unique_ptr<AsyncOperation> m_descriptionOperation;
        std::uint64_t m_descriptionRevision = 0;
        bool m_readPending = false;
        std::uint64_t m_readRevision = 0;
        void executeKernelMutation(
            std::uint64_t address,
            QByteArray originalBytes);

        QByteArray m_originalBytes;
        std::uint64_t m_baseAddress = 0U;
        DisassemblyArchitecture m_architecture =
            DisassemblyArchitecture::X64;
        QString m_sourceDescription;
        std::uint64_t m_snapshotRevision = 0;
        QLabel* m_sourceLabel = nullptr;
        QLabel* m_backendLabel = nullptr;
        QLabel* m_mutationRiskLabel = nullptr;
        QLabel* m_mutationStatusLabel = nullptr;
        SnapshotWorkbenchWidget* m_editor = nullptr;
        bool m_kernelMutationEnabled = false;
    };
}
