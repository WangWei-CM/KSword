#pragma once

// ============================================================
// ApplicationControlPage.h
// 作用：
// 1) 在 MiscDock 内提供“应用控制”只读诊断页；
// 2) 聚合 AppLocker、WDAC / Code Integrity、Defender / ASR、事件日志和文件诊断；
// 3) 仅做查看、复制和导出，不做任何策略写入/删除/禁用。
// ============================================================

#include "../Framework.h"

#include <QWidget>

#include <cstdint>
#include <utility>
#include <vector>

class QLabel;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QVBoxLayout;
class QPoint;

namespace ks::misc
{
    // ApplicationControlPage：
    // - 输入：Qt 父控件；
    // - 处理：异步采集 AppLocker / WDAC / Defender / CodeIntegrity / 文件诊断信息；
    // - 输出：通过表格、状态标签和文本框展示，只读无副作用。
    class ApplicationControlPage final : public QWidget
    {
    public:
        // 构造函数：
        // - parent 为 Qt 父控件；
        // - 创建 UI 并立即发起一次后台刷新。
        explicit ApplicationControlPage(QWidget* parent = nullptr);

        // 析构函数：
        // - 页面采用 Qt 对象树管理控件；
        // - 后台任务使用 QPointer 回投，无需显式收尾。
        ~ApplicationControlPage() override = default;

    private:
        // AppLockerRuleRecord：AppLocker 规则行的只读展示模型。
        struct AppLockerRuleRecord
        {
            QString collectionText;     // collectionText：规则集合显示名。
            QString actionText;         // actionText：Allow / Deny。
            QString userText;           // userText：用户或组文本。
            QString sidText;            // sidText：SID 原值。
            QString conditionTypeText;  // conditionTypeText：Publisher / Path / Hash。
            QString conditionText;      // conditionText：路径、发布者或哈希摘要。
            QString descriptionText;    // descriptionText：规则描述。
            QString riskText;          // riskText：风险标记文本。
        };

        // PolicyFileRecord：WDAC / Code Integrity 策略文件信息。
        struct PolicyFileRecord
        {
            QString pathText;      // pathText：文件路径。
            QString existsText;    // existsText：是否存在。
            QString sizeText;      // sizeText：文件大小。
            QString modifiedText;  // modifiedText：修改时间。
            QString countText;     // countText：策略数量/分片数量。
            QString detailText;    // detailText：补充说明。
        };

        // EventRecord：Code Integrity 事件日志行。
        struct EventRecord
        {
            QString timeText;      // timeText：事件时间。
            QString idText;        // idText：事件 ID。
            QString levelText;    // levelText：事件级别。
            QString verdictText;  // verdictText：允许/阻止/审计/其他。
            QString messageText;  // messageText：摘要消息。
        };

        // KeyValueRecord：Defender/状态类表格行。
        struct KeyValueRecord
        {
            QString nameText;     // nameText：字段名。
            QString valueText;    // valueText：字段值。
            QString detailText;   // detailText：补充说明。
        };

    private:
        // initializeUi：
        // - 创建顶部工具栏和五个子页；
        // - 无输入参数，无返回值。
        void initializeUi();

        // buildAppLockerPage：
        // - 构建 AppLocker 查看页；
        // - 无输入参数，无返回值。
        QWidget* buildAppLockerPage();

        // buildWdacPage：
        // - 构建 WDAC / Code Integrity 查看页；
        // - 无输入参数，无返回值。
        QWidget* buildWdacPage();

        // buildDefenderPage：
        // - 构建 Defender / ASR 查看页；
        // - 无输入参数，无返回值。
        QWidget* buildDefenderPage();

        // buildEventLogPage：
        // - 构建事件日志页；
        // - 无输入参数，无返回值。
        QWidget* buildEventLogPage();

        // buildFileDiagnosisPage：
        // - 构建文件诊断页；
        // - 无输入参数，无返回值。
        QWidget* buildFileDiagnosisPage();

        // initializeTable：
        // - 为表格设置统一的只读、行选择和右键菜单行为；
        // - table 为目标表格；columnResizeModeLast 为最后一列的拉伸方式；
        // - 无返回值。
        void initializeTable(QTableWidget* table, bool stretchLastColumn = true);

        // refreshAsync：
        // - 后台采集 AppLocker / WDAC / Defender / Event Log 诊断数据；
        // - 无返回值，结果回投到 UI 线程。
        void refreshAsync();

        // applyRefreshResult：
        // - 在 UI 线程应用后台刷新结果；
        // - 无返回值。
        void applyRefreshResult(
            QString statusText,
            QString appLockerSummary,
            QString wdacSummary,
            QString defenderSummary,
            QString eventSummary,
            QVector<AppLockerRuleRecord> appLockerRules,
            QVector<PolicyFileRecord> policyFiles,
            QVector<EventRecord> events,
            QVector<KeyValueRecord> defenderRows);

        // runFileDiagnosisAsync：
        // - 对输入文件路径做只读诊断；
        // - 无返回值，结果回投到 UI 线程。
        void runFileDiagnosisAsync();

        // applyFileDiagnosisResult：
        // - 在 UI 线程应用文件诊断结果；
        // - 无返回值。
        void applyFileDiagnosisResult(QString summaryText, QVector<KeyValueRecord> rows);

        // exportCurrentTableTsv：
        // - 导出当前激活页的主表格为 TSV；
        // - 无返回值，失败通过消息框提示。
        void exportCurrentTableTsv();

        // currentExportTable：
        // - 获取当前激活页对应的导出表格；
        // - 返回 nullptr 表示当前页无可导出表格。
        QTableWidget* currentExportTable() const;

        // showTableContextMenu：
        // - 为表格弹出复制/导出上下文菜单；
        // - table 为目标表格，localPosition 为视口坐标；
        // - 无返回值。
        void showTableContextMenu(QTableWidget* table, const QPoint& localPosition);

        // copyTableCell：
        // - 将指定单元格复制到剪贴板；
        // - 无返回值。
        void copyTableCell(QTableWidget* table, int row, int column) const;

        // copyTableRow：
        // - 将指定整行复制为 TSV 风格文本；
        // - 无返回值。
        void copyTableRow(QTableWidget* table, int row) const;

        // copySelectedRows：
        // - 将表格当前选中行复制为 TSV 风格文本；
        // - 无返回值。
        void copySelectedRows(QTableWidget* table) const;

        // tableToTsv：
        // - 把表格全部内容导出为 TSV 文本；
        // - selectedOnly=true 时仅导出当前选中行；
        // - 返回 TSV 字符串。
        QString tableToTsv(QTableWidget* table, bool selectedOnly) const;

        // runPowerShellCaptureText：
        // - 异步线程内通过 powershell.exe 执行脚本并抓取标准输出；
        // - scriptText 为 PowerShell 脚本；
        // - timeoutMs 为超时时间；
        // - 返回执行输出，失败时返回空串并可带错误说明。
        static QString runPowerShellCaptureText(const QString& scriptText, int timeoutMs, QString* errorTextOut);

        // parseAppLockerPolicyXml：
        // - 从 Get-AppLockerPolicy -Effective -Xml 输出中解析规则；
        // - xmlText 为 XML 文本；
        // - 返回解析结果和摘要文本。
        static std::pair<QVector<AppLockerRuleRecord>, QString> parseAppLockerPolicyXml(const QString& xmlText);

        // buildAppLockerRiskText：
        // - 根据 AppLocker 规则生成风险标记；
        // - 返回多行风险文本或空串。
        static QString buildAppLockerRiskText(
            const QString& actionText,
            const QString& sidText,
            const QString& conditionTypeText,
            const QString& conditionText);

        // parseJsonObjectArrayToEvents：
        // - 将 PowerShell JSON 输出转换为事件表行；
        // - jsonText 为原始 JSON；
        // - 返回事件行和摘要文本。
        static std::pair<QVector<EventRecord>, QString> parseEventsJson(const QString& jsonText);

        // parseDefenderJson：
        // - 将 Defender PowerShell JSON 输出转换为键值表行；
        // - jsonText 为原始 JSON；
        // - 返回键值表行和摘要文本。
        static std::pair<QVector<KeyValueRecord>, QString> parseDefenderJson(const QString& jsonText);

        // rebuildEventTable：
        // - 输入：读取当前缓存事件与事件分类筛选控件；
        // - 处理：按允许/阻止/审计/事件过滤，并重建事件表；
        // - 返回：无返回值。
        void rebuildEventTable();

        // selectedEventLimit：
        // - 输入：读取事件条数下拉框；
        // - 处理：将 UI 文本转换为 PowerShell -MaxEvents 使用的数量；
        // - 返回：正整数事件上限。
        int selectedEventLimit() const;

        // buildPathMatchHint：
        // - 用当前 AppLocker 规则对指定路径生成可能命中提示；
        // - filePathText 为输入路径；
        // - 返回命中摘要。
        QString buildPathMatchHint(const QString& filePathText) const;

    private:
        QVBoxLayout* m_rootLayout = nullptr;        // m_rootLayout：页面根布局。
        QWidget* m_toolbarWidget = nullptr;         // m_toolbarWidget：顶部工具栏容器。
        QPushButton* m_refreshButton = nullptr;     // m_refreshButton：刷新按钮。
        QPushButton* m_exportButton = nullptr;      // m_exportButton：导出按钮。
        QLabel* m_statusLabel = nullptr;            // m_statusLabel：总体状态标签。
        QTabWidget* m_tabWidget = nullptr;          // m_tabWidget：五个子页的总 Tab。

        QWidget* m_appLockerPage = nullptr;         // m_appLockerPage：AppLocker 页面。
        QWidget* m_wdacPage = nullptr;              // m_wdacPage：WDAC / Code Integrity 页面。
        QWidget* m_defenderPage = nullptr;          // m_defenderPage：Defender / ASR 页面。
        QWidget* m_eventPage = nullptr;             // m_eventPage：事件日志页面。
        QWidget* m_fileDiagnosisPage = nullptr;      // m_fileDiagnosisPage：文件诊断页面。

        QPlainTextEdit* m_appLockerSummary = nullptr;   // m_appLockerSummary：AppLocker 说明文本。
        QTableWidget* m_appLockerTable = nullptr;       // m_appLockerTable：AppLocker 规则表。
        QPlainTextEdit* m_wdacSummary = nullptr;        // m_wdacSummary：WDAC 说明文本。
        QTableWidget* m_policyFileTable = nullptr;      // m_policyFileTable：WDAC 策略文件表。
        QTableWidget* m_codeIntegrityEventTable = nullptr; // m_codeIntegrityEventTable：Code Integrity 事件表。
        QPlainTextEdit* m_defenderSummary = nullptr;    // m_defenderSummary：Defender 状态文本。
        QTableWidget* m_defenderTable = nullptr;        // m_defenderTable：Defender 键值表。
        QPlainTextEdit* m_eventSummary = nullptr;       // m_eventSummary：事件日志文本。
        QTableWidget* m_eventTable = nullptr;           // m_eventTable：事件表。
        QComboBox* m_eventVerdictFilterCombo = nullptr; // m_eventVerdictFilterCombo：事件分类筛选器。
        QComboBox* m_eventLimitCombo = nullptr;         // m_eventLimitCombo：事件读取数量选择。
        QLineEdit* m_filePathEdit = nullptr;            // m_filePathEdit：文件诊断输入框。
        QPushButton* m_fileBrowseButton = nullptr;      // m_fileBrowseButton：浏览按钮。
        QPushButton* m_fileDiagnoseButton = nullptr;    // m_fileDiagnoseButton：诊断按钮。
        QPlainTextEdit* m_fileDiagnosisSummary = nullptr; // m_fileDiagnosisSummary：文件诊断说明文本。
        QTableWidget* m_fileDiagnosisTable = nullptr;   // m_fileDiagnosisTable：文件诊断结果表。

        QVector<AppLockerRuleRecord> m_appLockerRules;  // m_appLockerRules：最近一次 AppLocker 规则快照。
        QVector<EventRecord> m_eventRows;               // m_eventRows：最近一次完整事件日志缓存。
    };
}
