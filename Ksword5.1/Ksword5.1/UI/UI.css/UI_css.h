#pragma once
#include <QString>
#include <qstring.h>

// QSS_MainWindow_TabWidget 作用：
// - 统一项目内所有 QTabWidget / QTabBar 的基础样式；
// - 避免使用白底默认样式，保证深浅色都由调色板接管。
const QString QSS_MainWindow_TabWidget = R"(
    QTabWidget::pane {
        border: none;
        background-color: palette(base);
        top: -1px;
    }

    QTabBar {
        background-color: palette(window);
        border-bottom: none;
    }

    QTabBar::tab {
        background-color: palette(base);
        color: palette(text);
        padding: 6px 14px;
        margin-right: 1px;
        border: none;
        border-top-left-radius: 3px;
        border-top-right-radius: 3px;
    }

    QTabBar::tab:selected {
        background-color: #43A0FF;
        color: #FFFFFF;
        border-color: #43A0FF;
    }

    QTabBar::tab:hover:!selected {
        background-color: #2E8BFF;
        color: #FFFFFF;
    }
)";

// QSS_MainWindow_dockStyle 作用：
// - 统一主窗口 Dock 区域、ADS 停靠系统、滚动条的样式；
// - 强制当前选中 Tab 使用主题色背景 + 白字，解决白底黑字残留问题。
const QString QSS_MainWindow_dockStyle = R"(
    QDockWidget {
        border: none;
        margin: 1px;
        background-color: palette(window);
        color: palette(text);
    }

    QDockWidget::title {
        background-color: palette(window);
        color: palette(text);
        padding: 6px 10px;
        border-bottom: none;
    }

    QDockWidget > QWidget {
        background-color: palette(base);
        color: palette(text);
        border: none;
    }

    QMainWindow QTabBar {
        background-color: palette(window);
        border-bottom: none;
    }

    QMainWindow QTabBar::tab {
        background-color: palette(base);
        color: palette(text);
        border: none;
        padding: 6px 14px;
    }

    QMainWindow QTabBar::tab:selected {
        background-color: #43A0FF;
        color: #FFFFFF;
        border-color: #43A0FF;
    }

    QMainWindow QTabBar::tab:hover:!selected {
        background-color: #2E8BFF;
        color: #FFFFFF;
    }

    ads--CDockManager,
    ads--CDockContainerWidget,
    ads--CDockAreaWidget,
    ads--CDockAreaTitleBar,
    ads--CFloatingDockContainer {
        background-color: palette(window);
        color: palette(text);
    }

    ads--CDockAreaTabBar {
        background-color: palette(window);
        border-bottom: none;
    }

    ads--CDockWidgetTab,
    ads--CAutoHideTab {
        background-color: palette(base);
        color: palette(text);
        border: none;
        padding: 4px 10px;
    }

    ads--CDockWidgetTab QLabel,
    ads--CAutoHideTab QLabel {
        color: palette(text);
    }

    ads--CDockWidgetTab[activeTab="true"],
    ads--CAutoHideTab[activeTab="true"] {
        background-color: #43A0FF;
        color: #FFFFFF;
        border-color: #43A0FF;
    }

    ads--CDockWidgetTab[activeTab="true"] QLabel,
    ads--CAutoHideTab[activeTab="true"] QLabel {
        color: #FFFFFF;
        font-weight: 600;
    }

    ads--CDockWidgetTab:hover,
    ads--CAutoHideTab:hover {
        background-color: #2E8BFF;
        color: #FFFFFF;
    }

    ads--CDockAreaTitleBar QToolButton,
    ads--CDockAreaTitleBar QPushButton {
        background-color: transparent;
        color: palette(text);
        border: none;
        border-radius: 3px;
    }

    ads--CDockAreaTitleBar QToolButton:hover,
    ads--CDockAreaTitleBar QPushButton:hover {
        background-color: #2E8BFF;
        color: #FFFFFF;
        border-color: #2E8BFF;
    }

    QScrollBar:vertical {
        background-color: palette(window);
        width: 12px;
        margin: 0px;
        border: none;
    }

    QScrollBar::handle:vertical {
        background-color: #43A0FF;
        min-height: 20px;
        border-radius: 4px;
    }

    QScrollBar::handle:vertical:hover {
        background-color: #2E8BFF;
    }

    QScrollBar:horizontal {
        background-color: palette(window);
        height: 12px;
        margin: 0px;
        border: none;
    }

    QScrollBar::handle:horizontal {
        background-color: #43A0FF;
        min-width: 20px;
        border-radius: 4px;
    }

    QScrollBar::handle:horizontal:hover {
        background-color: #2E8BFF;
    }

    QScrollBar::add-line,
    QScrollBar::sub-line,
    QScrollBar::add-page,
    QScrollBar::sub-page {
        background: transparent;
        border: none;
    }

    QTableCornerButton::section {
        background-color: palette(base);
        border: none;
    }
)";

// QSS_Buttons_Light 作用：
// - 欢迎页等浅色场景统一按钮风格；
// - 默认和悬停都不使用白色背景，避免高亮发白。
const QString QSS_Buttons_Light = R"(
    QPushButton {
        background-color: #43A0FF !important;
        color: #FFFFFF !important;
        border: 1px solid #43A0FF !important;
        padding: 6px 16px;
        border-radius: 3px;
        font-weight: 500;
        outline: none;
    }

    QPushButton:hover {
        background-color: #2E8BFF !important;
        color: #FFFFFF !important;
        border-color: #2E8BFF !important;
    }

    QPushButton:pressed {
        background-color: #1F78D0 !important;
        color: #FFFFFF !important;
        border-color: #1F78D0 !important;
    }

    QPushButton:disabled {
        background-color: #8DC4FF !important;
        color: #F3F8FF !important;
        border: 1px solid #8DC4FF !important;
        font-weight: normal;
    }
)";

// QSS_Buttons_Dark 作用：
// - 深色场景按钮样式保持主题蓝，悬停继续加深；
// - 避免出现白色 hover 背景。
const QString QSS_Buttons_Dark = R"(
    QPushButton {
        background-color: #2E8BFF !important;
        color: #FFFFFF !important;
        border: 1px solid #2E8BFF !important;
        padding: 6px 16px;
        border-radius: 3px;
        font-weight: 500;
        outline: none;
    }

    QPushButton:hover {
        background-color: #43A0FF !important;
        color: #FFFFFF !important;
        border-color: #43A0FF !important;
    }

    QPushButton:pressed {
        background-color: #1F78D0 !important;
        color: #FFFFFF !important;
        border-color: #1F78D0 !important;
    }

    QPushButton:disabled {
        background-color: #355A83 !important;
        color: #A9BFDA !important;
        border: 1px solid #355A83 !important;
        font-weight: normal;
    }
)";
