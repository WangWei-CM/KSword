#pragma once

// ============================================================
// BootEditorTab.h
// 作用：
// 1) 提供 Windows BCD（Boot Configuration Data）可视化编辑器；
// 2) 支持枚举、筛选、编辑、复制、删除、导入导出、一次性启动等操作；
// 3) 提供自定义 bcdedit 命令执行区与原始输出日志区，覆盖高级使用场景。
// ============================================================

#include "../../Framework.h"

#include <QMap>
#include <QWidget>

#include <vector>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QGroupBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSplitter;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QToolButton;
class QVBoxLayout;

class BootEditorTab final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数：
    // - 作用：初始化引导编辑器 UI，并触发首轮 BCD 枚举。
    // - 参数 parent：Qt 父控件。
    explicit BootEditorTab(QWidget* parent = nullptr);
    ~BootEditorTab() override = default;

private:
    // BcdEntry：
    // - 作用：描述一条 BCD 对象（如 bootmgr、current loader、恢复项等）。
    struct BcdEntry
    {
        QString objectTypeText;           // objectTypeText：对象类型文本（来自 bcdedit 分块标题）。
        QString identifierText;           // identifierText：对象标识符（如 {bootmgr}/{current}/GUID）。
        QMap<QString, QString> elementMap; // elementMap：规范化 key -> value 字段集合。
        QString rawBlockText;             // rawBlockText：对象原始块文本，便于高级排查。
        bool isBootManager = false;       // isBootManager：是否为 {bootmgr}。
        bool isCurrent = false;           // isCurrent：是否为 {current}。
        bool isDefault = false;           // isDefault：是否为当前默认启动项。
    };

    // BcdCommandResult：
    // - 作用：承接单条 bcdedit 执行结果，统一反馈到 UI 与日志。
    struct BcdCommandResult
    {
        bool startSucceeded = false;  // startSucceeded：进程是否成功启动。
        bool timeout = false;         // timeout：命令是否超时。
        int exitCode = -1;            // exitCode：bcdedit 进程退出码。
        QString standardOutputText;   // standardOutputText：标准输出文本。
        QString standardErrorText;    // standardErrorText：标准错误文本。
        QString mergedOutputText;     // mergedOutputText：stdout + stderr 合并文本。
    };

private:
    // ===================== 初始化 =====================
    void initializeUi();
    void initializeToolbar();
    void initializeCenterPane();
    void initializeConnections();

    // ===================== 数据刷新与同步 =====================
    void refreshBcdEntries();
    void rebuildEntryTable();
    void syncEditorFromSelection();
    void clearEditorForNoSelection();
    void updateStatusSummary();

    // ===================== 交互动作 =====================
    void applySelectedEntryChanges();
    void applyBootManagerChanges();
    void setLegacyBootForSelectedEntry();
    void setLegacyBootForDefaultEntry();
    void setStandardBootForSelectedEntry();
    void setSelectedAsDefaultEntry();
    void addSelectedToBootSequence();
    void createCopyFromSelectedEntry();
    void deleteSelectedEntry();
    void exportBcdStore();
    void importBcdStore();
    void executeCustomCommand();
    void copySelectedRowToClipboard();

    // ===================== 工具函数 =====================
    int currentEntryIndex() const;
    const BcdEntry* currentEntry() const;
    bool entryMatchesFilter(const BcdEntry& entry) const;
    QString readElementValue(const BcdEntry& entry, const QStringList& candidateKeyList) const;
    bool readElementBool(
        const BcdEntry& entry,
        const QStringList& candidateKeyList,
        bool defaultValue) const;
    void appendCommandLog(const QString& commandTitle, const BcdCommandResult& commandResult);
    BcdCommandResult runBcdEdit(
        const QStringList& argumentList,
        int timeoutMs,
        const QString& commandDescription);
    bool runBcdAndExpectSuccess(
        const QStringList& argumentList,
        const QString& operationText,
        bool showSuccessToast);
    bool applyBootMenuPolicyByIdentifier(
        const QString& identifierText,
        const QString& policyValueText,
        const QString& operationText);

    // ===================== 解析函数 =====================
    static QString normalizeElementKey(const QString& rawKeyText);
    static std::vector<BcdEntry> parseBcdEnumOutput(const QString& enumOutputText);
    static QString boolToBcdOnOff(bool enabled);
    static QString boolToBcdYesNo(bool enabled);

private:
    // ===================== 顶层布局 =====================
    QVBoxLayout* m_rootLayout = nullptr;        // m_rootLayout：引导页根布局。
    QWidget* m_toolbarWidget = nullptr;         // m_toolbarWidget：顶部工具栏容器。
    QHBoxLayout* m_toolbarLayout = nullptr;     // m_toolbarLayout：顶部工具栏布局。
    QSplitter* m_mainSplitter = nullptr;        // m_mainSplitter：左右分割器。
    QTableWidget* m_entryTable = nullptr;       // m_entryTable：BCD 条目列表表格。
    QWidget* m_editorPane = nullptr;            // m_editorPane：右侧编辑区容器。
    QVBoxLayout* m_editorPaneLayout = nullptr;  // m_editorPaneLayout：右侧编辑区布局。

    // ===================== 顶部工具按钮 =====================
    QToolButton* m_refreshButton = nullptr;      // m_refreshButton：刷新 BCD 枚举按钮。
    QToolButton* m_exportButton = nullptr;       // m_exportButton：导出 BCD 存储按钮。
    QToolButton* m_importButton = nullptr;       // m_importButton：导入 BCD 存储按钮。
    QToolButton* m_copyEntryButton = nullptr;    // m_copyEntryButton：复制当前启动项按钮。
    QToolButton* m_deleteEntryButton = nullptr;  // m_deleteEntryButton：删除当前启动项按钮。
    QToolButton* m_setDefaultButton = nullptr;   // m_setDefaultButton：设为默认启动项按钮。
    QToolButton* m_bootOnceButton = nullptr;     // m_bootOnceButton：一次性启动（bootsequence）按钮。
    QToolButton* m_copyRowButton = nullptr;      // m_copyRowButton：复制当前行概要信息按钮。
    QLineEdit* m_filterEdit = nullptr;           // m_filterEdit：关键词筛选输入框。
    QLabel* m_adminHintLabel = nullptr;          // m_adminHintLabel：管理员权限提示标签。

    // ===================== 基础信息区 =====================
    QLabel* m_identifierValueLabel = nullptr;   // m_identifierValueLabel：当前条目标识符显示。
    QLabel* m_typeValueLabel = nullptr;         // m_typeValueLabel：当前条目类型显示。
    QLineEdit* m_descriptionEdit = nullptr;     // m_descriptionEdit：description 字段编辑框。
    QLineEdit* m_deviceEdit = nullptr;          // m_deviceEdit：device 字段编辑框。
    QLineEdit* m_osDeviceEdit = nullptr;        // m_osDeviceEdit：osdevice 字段编辑框。
    QLineEdit* m_pathEdit = nullptr;            // m_pathEdit：path 字段编辑框。
    QLineEdit* m_systemRootEdit = nullptr;      // m_systemRootEdit：systemroot 字段编辑框。
    QLineEdit* m_localeEdit = nullptr;          // m_localeEdit：locale 字段编辑框。
    QComboBox* m_bootMenuPolicyCombo = nullptr; // m_bootMenuPolicyCombo：bootmenupolicy 选项框。
    QSpinBox* m_timeoutSpin = nullptr;          // m_timeoutSpin：bootmgr timeout 配置。
    QLabel* m_legacyModeHintLabel = nullptr;    // m_legacyModeHintLabel：传统引导说明标签。
    QPushButton* m_setLegacyForSelectedButton = nullptr; // m_setLegacyForSelectedButton：对当前条目启用 Legacy/F8。
    QPushButton* m_setLegacyForDefaultButton = nullptr;  // m_setLegacyForDefaultButton：对默认条目启用 Legacy/F8。
    QPushButton* m_setStandardForSelectedButton = nullptr; // m_setStandardForSelectedButton：对当前条目恢复 Standard。

    // ===================== 高级开关区 =====================
    QCheckBox* m_testSigningCheck = nullptr;       // m_testSigningCheck：testsigning 开关。
    QCheckBox* m_noIntegrityCheck = nullptr;       // m_noIntegrityCheck：nointegritychecks 开关。
    QCheckBox* m_debugCheck = nullptr;             // m_debugCheck：debug 开关。
    QCheckBox* m_bootLogCheck = nullptr;           // m_bootLogCheck：bootlog 开关。
    QCheckBox* m_baseVideoCheck = nullptr;         // m_baseVideoCheck：basevideo 开关。
    QCheckBox* m_recoveryEnabledCheck = nullptr;   // m_recoveryEnabledCheck：recoveryenabled 开关。
    QComboBox* m_safeBootCombo = nullptr;          // m_safeBootCombo：safeboot 模式选择框。

    // ===================== 应用动作区 =====================
    QPushButton* m_applyEntryButton = nullptr;    // m_applyEntryButton：应用当前条目修改按钮。
    QPushButton* m_applyBootMgrButton = nullptr;  // m_applyBootMgrButton：应用 bootmgr 参数按钮。
    QPushButton* m_reloadOneButton = nullptr;     // m_reloadOneButton：重读当前选中条目按钮。

    // ===================== 自定义命令区 =====================
    QLineEdit* m_customCommandEdit = nullptr;      // m_customCommandEdit：用户输入的 bcdedit 参数。
    QPushButton* m_runCustomCommandButton = nullptr; // m_runCustomCommandButton：执行自定义命令按钮。
    QPlainTextEdit* m_rawOutputEdit = nullptr;     // m_rawOutputEdit：原始输出与命令日志文本框。
    QLabel* m_statusLabel = nullptr;               // m_statusLabel：底部状态摘要标签。

    // ===================== 数据缓存 =====================
    std::vector<BcdEntry> m_entryList;      // m_entryList：当前 BCD 枚举结果缓存。
    QString m_lastEnumRawText;              // m_lastEnumRawText：最近一次完整原始枚举文本。
    QString m_defaultIdentifierText;        // m_defaultIdentifierText：当前默认启动项标识符缓存。
};
