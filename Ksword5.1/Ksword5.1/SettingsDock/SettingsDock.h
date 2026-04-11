#pragma once

#include "AppearanceSettings.h"

#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QTabWidget;
class QToolButton;

class SettingsDock : public QWidget
{
    Q_OBJECT

public:
    // 构造函数作用：
    // - 初始化“设置”页签容器；
    // - 加载外观 JSON 配置并同步到 UI；
    // - 对外发出外观设置变化信号。
    // 调用方式：MainWindow 创建 SettingsDock 时自动调用。
    // 入参 parent：Qt 父对象指针。
    explicit SettingsDock(QWidget* parent = nullptr);

    // currentAppearanceSettings 作用：
    // - 返回当前内存中的外观设置快照。
    // 调用方式：MainWindow 初始化时读取一次默认设置。
    // 返回：AppearanceSettings 配置结构体副本。
    ks::settings::AppearanceSettings currentAppearanceSettings() const;

signals:
    // appearanceSettingsChanged 作用：
    // - 当用户点击“应用”并保存成功后通知主窗口；
    // - 主题/背景会立即应用，启动相关选项用于下次启动。
    // 调用方式：内部保存成功后 emit。
    // 入参 settings：最新界面与启动配置。
    void appearanceSettingsChanged(const ks::settings::AppearanceSettings& settings);

private:
    // initializeUi 作用：
    // - 构建 SettingsDock 的根布局与 Tab 容器。
    // 调用方式：构造函数内部调用。
    void initializeUi();

    // initializeAppearanceTab 作用：
    // - 创建“外观与启动”标签页控件（主题按钮、背景路径、透明度、启动行为）。
    // 调用方式：initializeUi 内部调用。
    void initializeAppearanceTab();

    // bindAppearanceSignals 作用：
    // - 绑定外观页所有控件事件到“待应用”流程；
    // - 仅在点击应用按钮后才触发保存与生效。
    // 调用方式：initializeAppearanceTab 末尾调用。
    void bindAppearanceSignals();

    // loadSettingsFromJson 作用：
    // - 读取 JSON 配置并刷新 UI。
    // 调用方式：构造函数末尾调用。
    void loadSettingsFromJson();

    // applySettingsToUi 作用：
    // - 把配置结构体回填到各控件显示。
    // 调用方式：loadSettingsFromJson/内部回滚时调用。
    // 入参 settings：待显示的界面与启动配置。
    void applySettingsToUi(const ks::settings::AppearanceSettings& settings);

    // collectSettingsFromUi 作用：
    // - 从控件读取用户输入，组装配置结构体。
    // 调用方式：保存前调用。
    // 返回：由当前 UI 生成的配置结构体。
    ks::settings::AppearanceSettings collectSettingsFromUi() const;

    // markPendingChanges 作用：
    // - 标记当前 UI 有未应用改动；
    // - 刷新“应用按钮”可用态与提示文案。
    // 调用方式：任意设置控件值变化后调用。
    // 入参 triggerReason：触发原因文本（调试与日志辅助）。
    void markPendingChanges(const QString& triggerReason);

    // updateApplyButtonState 作用：
    // - 根据 m_hasPendingChanges 更新应用按钮状态；
    // - 统一维护“是否有待应用改动”的视觉反馈。
    // 调用方式：标记待应用后、保存成功后、加载配置后调用。
    void updateApplyButtonState();

    // saveAndEmitFromUi 作用：
    // - 从 UI 采集配置并写入 JSON；
    // - 保存成功后发出变更信号。
    // 调用方式：用户交互触发时调用。
    // 入参 triggerReason：触发原因文本（用于日志）。
    void saveAndEmitFromUi(const QString& triggerReason);

    // updateThemeButtonStyle 作用：
    // - 根据当前选中状态更新主题按钮样式高亮。
    // 调用方式：按钮点击后、配置加载后调用。
    void updateThemeButtonStyle();

    // updateOpacityValueLabel 作用：
    // - 同步透明度百分比文本标签。
    // 调用方式：滑条值变化时调用。
    // 入参 opacityPercent：0~100 透明度值。
    void updateOpacityValueLabel(int opacityPercent);

    // updateWindowScaleFactorHintLabel 作用：
    // - 根据缩放因子更新百分比提示文案；
    // - 明确提示“重启后生效”。
    // 调用方式：缩放输入框变化或配置回填后调用。
    // 入参 normalizedScaleFactor：已校正缩放因子。
    void updateWindowScaleFactorHintLabel(double normalizedScaleFactor);

    // openBackgroundFileDialog 作用：
    // - 打开文件选择对话框，供用户挑选背景图路径。
    // 调用方式：点击“浏览背景图”按钮时调用。
    void openBackgroundFileDialog();

    // resetBackgroundPathToDefault 作用：
    // - 把背景图路径恢复为默认 style/ksword_background.png。
    // 调用方式：点击“恢复默认背景路径”按钮时调用。
    void resetBackgroundPathToDefault();

    // parseWindowScaleFactorFromUi 作用：
    // - 从输入框解析并校正启动窗口缩放因子；
    // - 非法输入会自动回退到 1.0。
    // 调用方式：collectSettingsFromUi 内部调用。
    // 返回：合法缩放因子（0.50~2.00）。
    double parseWindowScaleFactorFromUi() const;

private:
    // m_tabWidget 作用：设置页签容器，当前至少包含“外观”页。
    QTabWidget* m_tabWidget = nullptr;

    // m_appearanceTab 作用：外观与启动设置页 QWidget 容器。
    QWidget* m_appearanceTab = nullptr;

    // m_themeButtonGroup 作用：三种主题按钮的互斥分组。
    QButtonGroup* m_themeButtonGroup = nullptr;

    // m_followSystemButton 作用：选择“跟随系统主题”模式。
    QToolButton* m_followSystemButton = nullptr;

    // m_lightModeButton 作用：选择“浅色主题”模式。
    QToolButton* m_lightModeButton = nullptr;

    // m_darkModeButton 作用：选择“深色主题”模式。
    QToolButton* m_darkModeButton = nullptr;

    // m_backgroundPathEdit 作用：编辑背景图路径文本。
    QLineEdit* m_backgroundPathEdit = nullptr;

    // m_browseBackgroundButton 作用：打开背景图文件选择器。
    QToolButton* m_browseBackgroundButton = nullptr;

    // m_resetBackgroundButton 作用：恢复默认背景图路径。
    QToolButton* m_resetBackgroundButton = nullptr;

    // m_backgroundOpacitySlider 作用：调整背景图透明度。
    QSlider* m_backgroundOpacitySlider = nullptr;

    // m_backgroundOpacityValueLabel 作用：显示透明度百分比文本。
    QLabel* m_backgroundOpacityValueLabel = nullptr;

    // m_startupDefaultTabCombo 作用：设置“应用启动时默认激活的主标签页”。
    QComboBox* m_startupDefaultTabCombo = nullptr;

    // m_startupMaximizedCheckBox 作用：设置下次启动时是否默认最大化显示。
    QCheckBox* m_startupMaximizedCheckBox = nullptr;

    // m_startupAutoAdminCheckBox 作用：设置下次启动时是否先尝试申请管理员权限。
    QCheckBox* m_startupAutoAdminCheckBox = nullptr;

    // m_startupWindowScaleFactorEdit 作用：设置下次启动主窗口缩放因子（重启生效）。
    QLineEdit* m_startupWindowScaleFactorEdit = nullptr;

    // m_startupWindowScaleHintLabel 作用：显示缩放因子对应百分比提示文本。
    QLabel* m_startupWindowScaleHintLabel = nullptr;

    // m_applySettingsButton 作用：统一提交当前设置改动并触发实际生效。
    QPushButton* m_applySettingsButton = nullptr;

    // m_currentAppearanceSettings 作用：缓存当前有效界面与启动配置。
    ks::settings::AppearanceSettings m_currentAppearanceSettings;

    // m_isApplyingUiState 作用：标记“正在回填 UI”，防止触发递归保存。
    bool m_isApplyingUiState = false;

    // m_hasPendingChanges 作用：标记是否存在“未点击应用”的待生效改动。
    bool m_hasPendingChanges = false;
};
