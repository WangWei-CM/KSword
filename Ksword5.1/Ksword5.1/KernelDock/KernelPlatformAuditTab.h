#pragma once

#include "../ArkDriverClient/ArkDriverTypes.h"

#include <QWidget>

#include <mutex>
#include <thread>
#include <vector>

class QEvent;
class QLabel;
class QLineEdit;
class QPoint;
class QPushButton;
class QShowEvent;
class QTabWidget;
class QTableWidget;

// KernelPlatformAuditTab：
// - Hal 模式提供四个明确的 HAL 子表；
// - Wdf 模式提供 KMDF 函数表和本驱动回调表；
// - 查询保持只读；四个 HAL 子表页与 WDF 函数页都可通过 ArkDriverClient 发起
//   重新定位、快照校验和原子 CAS 约束的函数槽编辑；
// - WDF 回调页列的是本驱动 .text 内的编译期地址，没有可写槽，始终只读。
class KernelPlatformAuditTab final : public QWidget
{
public:
    enum class Mode
    {
        Hal,
        Wdf
    };

    explicit KernelPlatformAuditTab(Mode mode, QWidget* parent = nullptr);
    ~KernelPlatformAuditTab() override;

protected:
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    struct Page
    {
        unsigned long scope = 0;
        QTableWidget* table = nullptr;
    };

    void initializeUi();
    void retranslateUi();
    void addPage(unsigned long scope, const char* navigationId);
    void refreshAsync();
    void applyResult(ksword::ark::PlatformAuditResult result);
    void populatePage(Page& page, const ksword::ark::PlatformAuditResult& result);
    void showContextMenu(QTableWidget* table, unsigned long scope, const QPoint& position);
    void editFunctionSlot(KSWORD_ARK_PLATFORM_AUDIT_ENTRY entry);
    bool confirmSlotEdit(
        const KSWORD_ARK_PLATFORM_AUDIT_ENTRY& entry,
        unsigned long long newAddress);
    // 可编辑判据只按 scope 走：WDF 回调页与 Wdf 模式其余只读行都排除在外。
    static bool scopeIsEditable(unsigned long scope);
    QString tableFamilyName() const;
    void setColumnGroup(int groupIndex);
    void applyColumnGroup();
    void updateColumnGroupButtons();
    void applyFilter();

    static QString fixedWide(const wchar_t* text, int capacity);
    static QString addressText(unsigned long long address);
    static QString statusText(unsigned long status, long lastStatus);
    static QString hookText(unsigned long hookStatus);
    static QString signatureText(unsigned long signatureId);
    static QString detailText(const KSWORD_ARK_PLATFORM_AUDIT_ENTRY& entry);
    static QString companyNameForModule(const QString& modulePath);
    static QString controlStatusText(unsigned long status, long lastStatus);
    static bool parseAddress(const QString& text, unsigned long long& addressOut);

    Mode m_mode;
    QTabWidget* m_innerTabs = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QPushButton* m_columnGroupAButton = nullptr;
    QPushButton* m_columnGroupBButton = nullptr;
    QPushButton* m_columnGroupCButton = nullptr;
    QLineEdit* m_filterEdit = nullptr;
    QLabel* m_statusLabel = nullptr;
    std::vector<Page> m_pages;
    ksword::ark::PlatformAuditResult m_lastResult;
    std::thread m_refreshThread;
    std::mutex m_refreshMutex;
    bool m_closing = false;
    bool m_firstRefreshStarted = false;
    bool m_refreshRunning = false;
    bool m_hasResult = false;
    int m_columnGroupIndex = 0;
};
