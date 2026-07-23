#pragma once

#include "../ArkDriverClient/ArkDriverTypes.h"

#include <QString>
#include <QWidget>

#include <cstdint>
#include <vector>

class QLabel;
class QPoint;
class QTableWidget;
class QTabWidget;
class QPushButton;

// KernelIoctlAuditTab：
// - 左页按 \\Driver 对象汇总全部 MajorFunction 派遣入口；
// - 右页直接显示 KswordARK 统一 dispatch registry；
// - 两页均为只读证据，并提供行/单元格复制。
class KernelIoctlAuditTab final : public QWidget
{
public:
    explicit KernelIoctlAuditTab(QWidget* parent = nullptr);
    ~KernelIoctlAuditTab() override = default;

private:
    struct DispatchRow
    {
        QString driverName;
        std::uint32_t majorFunction = 0;
        std::uint64_t dispatchAddress = 0;
        std::uint64_t moduleBase = 0;
        QString moduleName;
        std::uint32_t flags = 0;
        QString status;
    };

    struct Snapshot
    {
        std::vector<DispatchRow> dispatchRows;
        std::vector<ksword::ark::IoctlRegistryEntry> registryRows;
        QString errorText;
        std::uint32_t registryTotal = 0;
        std::uint32_t registryDuplicate = 0;
        bool registryOk = false;
    };

    void initializeUi();
    void refreshAsync();
    void applySnapshot(Snapshot snapshot);
    void populateTables();
    void showCopyMenu(QTableWidget* table, const QPoint& position);
    static QString hex64(std::uint64_t value);
    static QString hex32(std::uint32_t value);
    static QString tableRowText(QTableWidget* table, int row, bool includeHeader);

    QTabWidget* m_innerTabs = nullptr;
    QWidget* m_dispatchPage = nullptr;
    QWidget* m_registryPage = nullptr;
    QTableWidget* m_dispatchTable = nullptr;
    QTableWidget* m_registryTable = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QLabel* m_statusLabel = nullptr;
    std::vector<DispatchRow> m_dispatchRows;
    std::vector<ksword::ark::IoctlRegistryEntry> m_registryRows;
    std::uint32_t m_registryTotal = 0;
    std::uint32_t m_registryDuplicate = 0;
    QString m_errorText;
    bool m_registryOk = false;
    bool m_refreshRunning = false;
};
