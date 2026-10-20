#pragma once

// ============================================================
// SoundSourcePage.h
// 作用：
// 1) 提供“杂项 -> 声音来源”全局视图；
// 2) 以 PID 过滤模式复用于进程详情页；
// 3) 异步刷新并保留短时“最近发声”历史。
// ============================================================

#include "../../Framework.h"
#include "SoundSourceDetector.h"

#include <QHash>
#include <QWidget>

#include <cstdint>
#include <vector>

class QButtonGroup;
class QCheckBox;
class QEvent;
class QLabel;
class QShowEvent;
class QHideEvent;
class QTableWidget;
class QTimer;
class QToolButton;
class QPoint;

namespace ks::misc
{
    class SoundSourcePage final : public QWidget
    {
        Q_OBJECT

    public:
        // 构造函数：
        // - processIdFilter=0 时显示系统全部输出音频会话；
        // - processIdFilter!=0 时只显示进程详情页对应 PID；
        // - 页面显示后自动刷新，隐藏后停止定时器。
        explicit SoundSourcePage(
            std::uint32_t processIdFilter = 0,
            std::uint64_t expectedCreationTime100ns = 0,
            QWidget* parent = nullptr);
        ~SoundSourcePage() override = default;

    protected:
        // showEvent/hideEvent：只在页面可见时执行周期采样，避免后台常驻扫描。
        void showEvent(QShowEvent* event) override;
        void hideEvent(QHideEvent* event) override;
        void changeEvent(QEvent* event) override;

    private:
        enum class ColumnPreset
        {
            Overview,
            AudioPath,
            KernelEvidence,
            Custom
        };

        // initializeUi：创建标题、工具栏、A/B/C 列组按钮和结果表格。
        void initializeUi();
        // initializeConnections：连接手动/自动刷新、列组与表头菜单。
        void initializeConnections();
        // requestRefresh：排队一次后台 Core Audio + 可选 R0 检测。
        void requestRefresh(bool manualRequest);
        // applyScanResult：在 UI 线程校验 ticket 后合并结果与最近发声历史。
        void applyScanResult(
            std::uint64_t ticket,
            const SoundSourceScanResult& result);
        // mergeRecentHistory：保留短促声音来源，避免一次刷新后立即消失。
        void mergeRecentHistory(std::vector<SoundSourceRecord>& records);
        // rebuildTable：按当前静默过滤和列组重建表格。
        void rebuildTable();
        // updateSummary：根据当前缓存刷新顶部结论和状态。
        void updateSummary(const SoundSourceScanResult& result);
        // applyColumnPreset：应用互补的概览/音频链路/R0 证据精简列组。
        void applyColumnPreset(ColumnPreset preset);
        // showHeaderContextMenu：允许用户逐列自定义显隐，并清除 A/B/C 选中态。
        void showHeaderContextMenu(const QPoint& position);
        // setCustomColumnLayout：把列组按钮切换为“自定义列布局”状态。
        void setCustomColumnLayout();
        // applyThemeStyle：同步深浅色下的按钮、表格和状态颜色。
        void applyThemeStyle();
        // recordKey：返回 endpoint + session instance 稳定键。
        QString recordKey(const SoundSourceRecord& record) const;

    private:
        std::uint32_t m_processIdFilter = 0;          // 0=全局，其它值=进程详情范围。
        std::uint64_t m_expectedCreationTime100ns = 0; // 进程详情 PID 对应的创建时间。
        QLabel* m_titleLabel = nullptr;               // 页面标题。
        QLabel* m_explanationLabel = nullptr;         // R3/R0 证据边界说明。
        QLabel* m_summaryLabel = nullptr;             // 当前/最近发声摘要。
        QLabel* m_statusLabel = nullptr;              // 刷新、降级和错误状态。
        QToolButton* m_refreshButton = nullptr;       // 图标化手动刷新按钮。
        QCheckBox* m_autoRefreshCheck = nullptr;      // 自动刷新开关。
        QCheckBox* m_showSilentCheck = nullptr;       // 是否展示所有静默会话。
        QButtonGroup* m_columnPresetGroup = nullptr;  // A/B/C 互斥列组。
        QToolButton* m_overviewPresetButton = nullptr; // A：概览列组。
        QToolButton* m_audioPresetButton = nullptr;    // B：音频链路列组。
        QToolButton* m_kernelPresetButton = nullptr;   // C：R0 证据列组。
        QTableWidget* m_table = nullptr;               // 声音来源结果表。
        QTimer* m_refreshTimer = nullptr;               // 页面可见时的自动刷新计时器。
        std::vector<SoundSourceRecord> m_records;       // 最近一次会话与历史合并缓存。
        QHash<QString, SoundSourceRecord> m_recentRecords; // 最近确认出声的记录。
        bool m_tableDirty = false;                     // Hidden results retain values until next show.
        bool m_columnsSized = false;                   // Measure initially or after a font change, not every sample.
        bool m_refreshing = false;                      // 防止并发重入刷新。
        bool m_autoKernelProbeEnabled = true;           // R0 不可用后自动刷新只做 R3。
        std::uint64_t m_refreshTicket = 0;              // 防止旧后台结果覆盖新页面状态。
        ColumnPreset m_columnPreset = ColumnPreset::Overview; // 当前列布局。
    };
}
