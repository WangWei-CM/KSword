#pragma once

// ============================================================
// RegistryOptimizationPage.h
// Purpose:
// 1) Dynamically load Dism++ style registry optimization JSON from profiles/;
// 2) Build the System Optimization UI at runtime instead of hardcoding rows;
// 3) Apply selected registry/service/explorer actions with explicit confirmation.
// ============================================================

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QWidget>

#include <cstdint>

class QLabel;
class QLineEdit;
class QPushButton;
class QSplitter;
class QTableWidget;
class QTextEdit;
class QTreeWidget;
class QTimer;

// RegistryOptimizationPage:
// - Input: parent widget plus profiles/registry_optimization_items.json at runtime;
// - Processing: parses item/group/scope/state/action objects and creates controls per visible row;
// - Return behavior: QWidget subclass has no direct return value; status is surfaced in the UI.
class RegistryOptimizationPage final : public QWidget
{
public:
    // Constructor:
    // - Input parent: Qt parent widget owning this page;
    // - Processing: builds controls, wires signals, then loads the JSON profile;
    // - Return: no explicit return value.
    explicit RegistryOptimizationPage(QWidget* parent = nullptr);

private:
    // OptimizationState:
    // - Input: parsed from one JSON states[] object;
    // - Processing: stores label, detection condition, warning, and action list;
    // - Return: value object used by table controls.
    struct OptimizationState
    {
        QString tagText;
        QString labelText;
        QString conditionText;
        QString warningText;
        QVector<QJsonObject> actionList;
    };

    // OptimizationScope:
    // - Input: parsed from one JSON scopes[] object;
    // - Processing: groups state choices by Current/Default/System scope;
    // - Return: value object used by one table row.
    struct OptimizationScope
    {
        QString scopeText;
        QString conditionText;
        QVector<OptimizationState> stateList;
    };

    // OptimizationItem:
    // - Input: parsed from one top-level JSON item object;
    // - Processing: keeps item metadata and all scope definitions;
    // - Return: value object stored in m_itemList.
    struct OptimizationItem
    {
        int groupIndex = 0;
        int itemIndex = 0;
        QString groupNameText;
        QString itemNameText;
        QString itemTypeText;
        QString groupConditionText;
        QString itemConditionText;
        QString warningText;
        QVector<OptimizationScope> scopeList;
    };

    // VisibleRow:
    // - Input: item/scope indexes selected by current group/filter;
    // - Processing: maps QTableWidget rows back to parsed model objects;
    // - Return: lightweight row reference.
    struct VisibleRow
    {
        int itemIndex = -1;
        int scopeIndex = -1;
    };

private:
    // ColumnPreset：
    // - 用途：记录系统优化表格当前使用的 A/B/自定义列组；
    // - A：操作视图，B：诊断视图，Custom：用户通过表头菜单手动改列。
    enum class ColumnPreset
    {
        A,
        B,
        Custom
    };

private:
    void initializeUi();
    void initializeConnections();
    void loadOptimizationProfile();
    QStringList profileCandidatePaths() const;
    void rebuildGroupTree();
    void rebuildItemTable();
    void refreshVisibleStates();
    void refreshVisibleRowState(int tableRow);
    void cancelStateRefresh();
    void updateDetailPanel(int tableRow);
    void updateStatusText(const QString& text);
    void applyColumnPreset(ColumnPreset preset);
    void refreshColumnPresetButtonStyles();
    void showHeaderColumnMenu(const QPoint& localPos);
    bool isColumnVisibleInPreset(int columnIndex, ColumnPreset preset) const;

    const OptimizationState* selectedTargetStateForRow(int tableRow) const;
    const OptimizationState* detectedStateForScope(const OptimizationScope& scope) const;
    bool applyVisibleRow(int tableRow);
    bool applyStateActions(
        const OptimizationItem& item,
        const OptimizationScope& scope,
        const OptimizationState& state,
        QStringList* detailLinesOut);
    bool executeAction(
        const QJsonObject& actionObject,
        QStringList* detailLinesOut,
        bool* restartExplorerOut);

    bool executeRegistryWriteAction(const QJsonObject& actionObject, QString* errorTextOut);
    bool executeRegistryDeleteAction(const QJsonObject& actionObject, QString* errorTextOut);
    bool executeRegistryMoveAction(const QJsonObject& actionObject, QString* errorTextOut);
    bool executeServiceStartAction(const QJsonObject& actionObject, QString* errorTextOut);
    bool executeExplorerNotifyAction(const QJsonObject& actionObject, QString* errorTextOut);
    bool executeFileCreateByZipAction(const QJsonObject& actionObject, QString* errorTextOut);

    static bool evaluateConditionText(const QString& conditionText);

private:
    QPushButton* m_columnPresetAButton = nullptr; // A 列组按钮：默认操作视图。
    QPushButton* m_columnPresetBButton = nullptr; // B 列组按钮：条件/警告诊断视图。
    QLineEdit* m_filterEdit = nullptr;           // Free-text filter for group/item/scope names.
    QPushButton* m_reloadButton = nullptr;       // Reloads JSON from profiles at runtime.
    QPushButton* m_refreshStateButton = nullptr; // Re-evaluates visible row state conditions.
    QTimer* m_filterDebounceTimer = nullptr;     // 延迟合并连续筛选输入。
    QSplitter* m_splitter = nullptr;             // Left groups, right item list/details.
    QTreeWidget* m_groupTree = nullptr;          // Dynamic group tree from JSON group_name.
    QTableWidget* m_itemTable = nullptr;         // Dynamic option rows and per-row controls.
    QTextEdit* m_detailText = nullptr;           // Read-only selected item/action details.
    QLabel* m_statusLabel = nullptr;             // Profile load/apply status.

    QString m_loadedProfilePath;                 // Actual JSON path selected from candidates.
    QVector<OptimizationItem> m_itemList;        // Parsed top-level optimization items.
    QVector<VisibleRow> m_visibleRows;           // Current table row to model mapping.
    ColumnPreset m_columnPreset = ColumnPreset::A; // 当前列组预设。
    bool m_rebuildingTable = false;              // Guards refresh signals during table rebuild.
    bool m_stateRefreshInProgress = false;       // 可见状态后台刷新是否仍在执行。
    std::uint64_t m_stateRefreshGeneration = 0;  // 忽略过期后台状态结果。
};
