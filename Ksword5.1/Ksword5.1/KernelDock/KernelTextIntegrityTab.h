#pragma once

#include "KernelCleanImageBaseline.h"

#include <QWidget>
#include <QSet>
#include <QPersistentModelIndex>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QTableWidget;

// 内核可执行节完整性页。
//
// 它把每个已加载模块的可执行节内容与经过重定位的磁盘净映像做全量逐字节比对。
// 无论代码页是怎么变成可写的，只要 .text 被改过，这里就会留下差异区间。
//
// 差异分两类：落在 PE 动态重定位位点上的（import optimization / retpoline，属于
// 启动期的正常改写），以及无法用任何已知机制解释的。后者在 HVCI 正在强制执行的
// 机器上尤其严重——那种环境下内核代码页本来就不应该能被改写。
class KernelTextIntegrityTab final : public QWidget
{
public:
    explicit KernelTextIntegrityTab(QWidget* parent = nullptr);
    ~KernelTextIntegrityTab() override;

protected:
    void showEvent(QShowEvent* event) override;

private:
    void initializeUi();
    void startScan();
    void cancelScan();
    void appendModuleResult(const ks::kernel::KernelTextIntegrityResult& result);
    void finishScan(bool cancelled);
    void rebuildRangeTable();
    void appendRangeRows(const ks::kernel::KernelTextIntegrityResult& result);
    void updateVerdict();

    static QString originText(ks::kernel::KernelTextDiffRange::Origin origin);
    static QString byteText(const std::vector<std::uint8_t>& bytes);
    static QString hex64(std::uint64_t value);
    static QString hex32(std::uint32_t value);

    QLabel* m_verdictLabel = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLineEdit* m_moduleFilterEdit = nullptr;
    QComboBox* m_backendCombo = nullptr;
    QCheckBox* m_unexplainedOnlyCheck = nullptr;
    QPushButton* m_scanButton = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QTableWidget* m_moduleTable = nullptr;
    QTableWidget* m_rangeTable = nullptr;

    std::vector<ks::kernel::KernelTextIntegrityResult> m_results;
    QSet<QPersistentModelIndex> m_rangeExclusions;
    std::shared_ptr<std::atomic_bool> m_cancelFlag;
    bool m_firstScanStarted = false;
    bool m_scanRunning = false;
    bool m_scanCancelled = false;
    std::uint32_t m_unexplainedRangeCount = 0U;
    std::uint32_t m_knownRangeCount = 0U;
    std::uint32_t m_unparsedModuleCount = 0U;
    std::uint32_t m_untrustedModuleCount = 0U;
    std::uint32_t m_incompleteModuleCount = 0U;
    // HVCI 是否处于强制执行状态，决定「无法解释的差异」的定级。
    bool m_hvciEnforcing = false;
    bool m_hvciEvidenceUsable = false;
};
