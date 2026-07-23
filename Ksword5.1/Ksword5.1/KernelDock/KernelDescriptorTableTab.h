#pragma once

#include "../ArkDriverClient/ArkDriverTypes.h"

#include <QString>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <vector>

class QLabel;
class QComboBox;
class QLineEdit;
class QPoint;
class QPushButton;
class QShowEvent;
class QTableWidget;
class QTextEdit;

// KernelDescriptorTableTab：以只读 R0 CPU 快照展示每 CPU IDT/GDT。
// IDT 行同时展示 handler 模块归属，GDT 行展开 segment/TSS 位域。
class KernelDescriptorTableTab final : public QWidget
{
public:
    explicit KernelDescriptorTableTab(QWidget* parent = nullptr);
    ~KernelDescriptorTableTab() override = default;

protected:
    void showEvent(QShowEvent* event) override;

private:
    void initializeUi();
    void refreshAsync();
    void applyResult(ksword::ark::DriverIntegrityResult result);
    void rebuildTable();
    void showCurrentDetail();
    void showCopyMenu(const QPoint& position);
    bool rowMatchesFilter(const ksword::ark::DriverIntegrityEvidenceEntry& row) const;
    QString columnText(const ksword::ark::DriverIntegrityEvidenceEntry& row, int column) const;
    QString detailText(const ksword::ark::DriverIntegrityEvidenceEntry& row) const;
    static QString tableName(const ksword::ark::DriverIntegrityEvidenceEntry& row);
    static QString descriptorTypeText(const ksword::ark::DriverIntegrityEvidenceEntry& row);
    static QString riskText(std::uint32_t riskFlags);
    static QString hex64(std::uint64_t value);
    static QString hex32(std::uint32_t value);
    static QString rowClipboardText(QTableWidget* table, int row, bool includeHeader);

    QComboBox* m_tableFilterCombo = nullptr;
    QLineEdit* m_filterEdit = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QLabel* m_statusLabel = nullptr;
    QTableWidget* m_table = nullptr;
    QTextEdit* m_detailEdit = nullptr;
    std::vector<ksword::ark::DriverIntegrityEvidenceEntry> m_rows;
    bool m_refreshRunning = false;
    bool m_firstRefreshStarted = false;
};
