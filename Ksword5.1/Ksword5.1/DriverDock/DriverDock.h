#pragma once

// ============================================================
// DriverDock.h
// 作用：
// 1) 提供驱动服务枚举与状态查看能力；
// 2) 提供驱动服务注册、更新、挂载、卸载、删除能力；
// 3) 提供已加载内核模块枚举能力；
// 4) 提供调试输出捕获（DBWIN 兼容）能力。
// ============================================================

#include "../Framework.h"

#include <QWidget>

#include <atomic>   // std::atomic_bool：调试捕获线程运行标记。
#include <cstdint>  // std::uint32_t/std::uint64_t：状态值与地址值。
#include <memory>   // std::unique_ptr：线程对象托管。
#include <string>   // std::string：错误文本桥接与日志输出。
#include <thread>   // std::thread：后台调试输出捕获线程。
#include <vector>   // std::vector：驱动服务与模块快照容器。

// Qt 前置声明：减少头文件编译耦合。
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSplitter;
class QTableWidget;
class QTabWidget;
class QVBoxLayout;

// DriverDock：
// - 驱动页主控件；
// - 内含“概览/操作/调试输出”三个页签；
// - 所有驱动管理动作均通过 SCM API 实现。
class DriverDock final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数：
    // - 作用：初始化 UI、绑定信号槽并执行首轮刷新。
    // - 参数 parent：Qt 父控件指针。
    explicit DriverDock(QWidget* parent = nullptr);

    // 析构函数：
    // - 作用：停止调试捕获线程并回收资源。
    ~DriverDock() override;

private:
    // DriverServiceRecord：
    // - 作用：描述一条驱动服务记录的展示字段。
    struct DriverServiceRecord
    {
        QString serviceName;           // 服务名（唯一键，SCM 主键）。
        QString displayName;           // 显示名（可为空）。
        QString binaryPath;            // 驱动镜像路径（ImagePath）。
        QString description;           // 服务描述文本（可为空）。
        std::uint32_t currentState = 0;// 当前服务状态（SERVICE_* 常量值）。
        std::uint32_t startType = 0;   // 启动类型（BOOT/SYSTEM/AUTO/DEMAND/DISABLED）。
        std::uint32_t errorControl = 0;// 错误控制级别（IGNORE/NORMAL/SEVERE/CRITICAL）。
        std::uint32_t serviceType = 0; // 服务类型（目标应为 SERVICE_KERNEL_DRIVER）。
    };

    // LoadedKernelModuleRecord：
    // - 作用：描述一条已加载内核模块（驱动映像）记录。
    struct LoadedKernelModuleRecord
    {
        QString moduleName;            // 模块文件名（如 xxx.sys）。
        QString imagePath;             // 内核可见路径（通常为 \SystemRoot\...）。
        std::uint64_t baseAddress = 0; // 基址（十六进制展示）。
    };

private:
    // ========================= UI 初始化 =========================
    // initializeUi：
    // - 作用：构建驱动页顶层布局与三个子页签。
    void initializeUi();

    // initializeOverviewTab：
    // - 作用：构建“驱动概览”页（服务列表 + 已加载模块）。
    void initializeOverviewTab();

    // initializeOperateTab：
    // - 作用：构建“驱动操作”页（注册/挂载/卸载/删除）。
    void initializeOperateTab();

    // initializeDebugOutputTab：
    // - 作用：构建“调试输出”页（DBWIN 捕获控制与输出框）。
    void initializeDebugOutputTab();

    // initializeConnections：
    // - 作用：连接全部控件信号与业务槽函数。
    void initializeConnections();

    // ========================= 数据刷新 =========================
    // refreshDriverServiceRecords：
    // - 作用：刷新驱动服务缓存并重建服务表格。
    void refreshDriverServiceRecords();

    // refreshLoadedKernelModuleRecords：
    // - 作用：刷新已加载内核模块缓存并重建模块表格。
    void refreshLoadedKernelModuleRecords();

    // rebuildDriverServiceTableByFilter：
    // - 作用：按过滤关键词重建驱动服务表格。
    void rebuildDriverServiceTableByFilter();

    // rebuildLoadedModuleTable：
    // - 作用：按当前缓存重建已加载模块表格。
    void rebuildLoadedModuleTable();

    // syncOperateFormBySelectedService：
    // - 作用：把选中服务行参数同步到操作表单。
    void syncOperateFormBySelectedService();

    // refreshSelectedServiceStateToForm：
    // - 作用：按表单服务名回查 SCM 并刷新状态日志。
    void refreshSelectedServiceStateToForm();

    // ========================= 驱动操作 =========================
    // registerOrUpdateDriverService：
    // - 作用：注册新驱动服务，或更新已存在服务配置。
    void registerOrUpdateDriverService();

    // loadSelectedDriverService：
    // - 作用：启动（挂载）表单中的目标驱动服务。
    void loadSelectedDriverService();

    // unloadSelectedDriverService：
    // - 作用：停止（卸载）表单中的目标驱动服务。
    void unloadSelectedDriverService();

    // deleteSelectedDriverService：
    // - 作用：删除表单中的目标驱动服务。
    void deleteSelectedDriverService();

    // ========================= 调试输出 =========================
    // startDebugOutputCapture：
    // - 作用：启动 DBWIN 调试输出捕获线程。
    void startDebugOutputCapture();

    // stopDebugOutputCapture：
    // - 作用：停止 DBWIN 调试输出捕获线程。
    void stopDebugOutputCapture();

    // runDbwinCaptureLoop：
    // - 作用：线程函数，循环读取 DBWIN 共享缓冲。
    void runDbwinCaptureLoop();

    // updateDebugCaptureButtonState：
    // - 作用：刷新调试捕获按钮启用状态与提示文本。
    void updateDebugCaptureButtonState();

    // ========================= 输出辅助 =========================
    // appendOperateLogLine：
    // - 作用：向“驱动操作日志”窗口追加一行带时间戳文本。
    // - 参数 logText：待追加的日志正文。
    void appendOperateLogLine(const QString& logText);

    // appendDebugOutputLine：
    // - 作用：向“调试输出”窗口追加一行带时间戳文本。
    // - 参数 debugText：待追加的调试输出正文。
    void appendDebugOutputLine(const QString& debugText);

    // ========================= 静态工具函数 =========================
    // queryDriverServiceRecords：
    // - 作用：从 SCM 枚举全部驱动服务并填充输出容器。
    // - 参数 recordListOut：输出服务记录列表。
    // - 参数 errorTextOut：失败时输出错误文本，可空。
    // - 返回：true=成功；false=失败。
    static bool queryDriverServiceRecords(
        std::vector<DriverServiceRecord>& recordListOut,
        std::string* errorTextOut = nullptr);

    // queryLoadedKernelModuleRecords：
    // - 作用：枚举当前已加载内核模块并填充输出容器。
    // - 参数 recordListOut：输出模块记录列表。
    // - 参数 errorTextOut：失败时输出错误文本，可空。
    // - 返回：true=成功；false=失败。
    static bool queryLoadedKernelModuleRecords(
        std::vector<LoadedKernelModuleRecord>& recordListOut,
        std::string* errorTextOut = nullptr);

    // serviceStateToText：
    // - 作用：把服务状态值转换为中文文本。
    static QString serviceStateToText(std::uint32_t stateValue);

    // startTypeToText：
    // - 作用：把启动类型值转换为中文文本。
    static QString startTypeToText(std::uint32_t startTypeValue);

    // errorControlToText：
    // - 作用：把错误控制值转换为中文文本。
    static QString errorControlToText(std::uint32_t errorControlValue);

    // formatWin32ErrorText：
    // - 作用：把 Win32 错误码格式化为“十进制 + 文本”。
    static QString formatWin32ErrorText(std::uint32_t win32ErrorCode);

    // trimQuotedText：
    // - 作用：去掉路径文本首尾成对双引号。
    static QString trimQuotedText(const QString& textValue);

    // normalizeDriverBinaryPath：
    // - 作用：规整驱动镜像路径（必要时自动补双引号）。
    static QString normalizeDriverBinaryPath(const QString& pathText);

private:
    // ========================= 顶层布局 =========================
    QVBoxLayout* m_rootLayout = nullptr; // 根布局。
    QTabWidget* m_tabWidget = nullptr;   // 子页签容器。

    // ========================= 页签1：驱动概览 =========================
    QWidget* m_overviewPage = nullptr;            // 概览页容器。
    QVBoxLayout* m_overviewLayout = nullptr;      // 概览页主布局。
    QHBoxLayout* m_overviewToolLayout = nullptr;  // 概览页工具栏布局。
    QLineEdit* m_serviceFilterEdit = nullptr;     // 服务列表过滤输入框。
    QPushButton* m_refreshServiceButton = nullptr;// 刷新服务按钮。
    QPushButton* m_refreshModuleButton = nullptr; // 刷新模块按钮。
    QLabel* m_overviewStatusLabel = nullptr;      // 概览页状态标签。
    QSplitter* m_overviewSplitter = nullptr;      // 服务/模块分割器。
    QTableWidget* m_serviceTable = nullptr;       // 驱动服务表格。
    QTableWidget* m_moduleTable = nullptr;        // 已加载模块表格。

    // ========================= 页签2：驱动操作 =========================
    QWidget* m_operatePage = nullptr;                 // 操作页容器。
    QVBoxLayout* m_operateLayout = nullptr;           // 操作页主布局。
    QLineEdit* m_serviceNameEdit = nullptr;           // 服务名输入框。
    QLineEdit* m_displayNameEdit = nullptr;           // 显示名输入框。
    QLineEdit* m_binaryPathEdit = nullptr;            // 驱动路径输入框。
    QLineEdit* m_descriptionEdit = nullptr;           // 服务描述输入框。
    QComboBox* m_startTypeCombo = nullptr;            // 启动类型下拉框。
    QComboBox* m_errorControlCombo = nullptr;         // 错误控制下拉框。
    QPushButton* m_browsePathButton = nullptr;        // 浏览路径按钮。
    QPushButton* m_registerOrUpdateButton = nullptr;  // 注册/更新按钮。
    QPushButton* m_loadDriverButton = nullptr;        // 挂载按钮。
    QPushButton* m_unloadDriverButton = nullptr;      // 卸载按钮。
    QPushButton* m_deleteServiceButton = nullptr;     // 删除服务按钮。
    QPushButton* m_refreshStateButton = nullptr;      // 刷新状态按钮。
    QPlainTextEdit* m_operateLogOutput = nullptr;     // 操作日志输出框。

    // ========================= 页签3：调试输出 =========================
    QWidget* m_debugOutputPage = nullptr;             // 调试输出页容器。
    QVBoxLayout* m_debugOutputLayout = nullptr;       // 调试输出页主布局。
    QHBoxLayout* m_debugToolLayout = nullptr;         // 调试输出工具栏布局。
    QPushButton* m_startCaptureButton = nullptr;      // 启动捕获按钮。
    QPushButton* m_stopCaptureButton = nullptr;       // 停止捕获按钮。
    QPushButton* m_clearDebugOutputButton = nullptr;  // 清空输出按钮。
    QPushButton* m_copyDebugOutputButton = nullptr;   // 复制输出按钮。
    QLabel* m_debugCaptureStatusLabel = nullptr;      // 捕获状态标签。
    QPlainTextEdit* m_debugOutputEdit = nullptr;      // 调试输出文本框。

    // ========================= 数据缓存 =========================
    std::vector<DriverServiceRecord> m_driverServiceCache;      // 驱动服务缓存。
    std::vector<LoadedKernelModuleRecord> m_loadedModuleCache;  // 已加载模块缓存。

    // ========================= 调试捕获线程状态 =========================
    std::atomic_bool m_dbwinCaptureRunning{ false };       // 捕获线程运行标记。
    std::unique_ptr<std::thread> m_dbwinCaptureThread;     // 捕获线程对象。
};

