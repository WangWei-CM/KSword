#pragma once
#include <QString>
#include <qstring.h>

const QString QSS_MainWindow_TabWidget = R"(
        /* 标签页容器样式 - 白色背景 */
        QTabWidget::pane {
            border: 1px solid #E0E0E0;  /* 浅色边框增强层次感 */
            background-color: white;    /* 白色背景 */
            top: -1px;                  /* 消除边框间隙 */
            border-radius: 0px;         /* 全方角 */
        }

        /* 标签栏样式 */
        QTabBar {
            background-color: white;    /* 标签栏白色背景 */
            border-bottom: 1px solid #E0E0E0;
        }

        /* 未选中标签样式 */
        QTabBar::tab {
            background-color: white;    /* 标签白色背景 */
            color: #666666;             /* 灰色文字 */
            padding: 8px 16px;          /* 内边距 */
            margin-right: 1px;          /* 标签间间隙 */
            border: 1px solid transparent;  /* 透明边框 */
            border-radius: 0px;         /* 全方角 */
        }

        /* 选中标签样式 - 主题色#43A0FF */
        QTabBar::tab:selected {
            color: #43A0FF;             /* 主题色文字 */
            border-bottom: 2px solid #43A0FF;  /* 底部主题色高亮条 */
        }

        /* 标签悬停效果 */
        QTabBar::tab:hover:!selected {
            color: #43A0FF;             /* 悬停时主题色文字 */
            border-bottom: 1px solid #E0E0E0;
        }

        /* 关闭按钮样式 */
        QTabBar::close-button {
            image: url(:/icons/close_normal.png);  /* 可替换为自定义关闭图标 */
            subcontrol-position: right;
            subcontrol-origin: margin;
            width: 16px;
            height: 16px;
            margin: 0 4px 0 4px;
        }

        QTabBar::close-button:hover {
            image: url(:/icons/close_hover.png);   /* 悬停状态关闭图标 */
        }
    )"
    ;
const QString QSS_MainWindow_dockStyle = R"(
    /* 停靠窗口整体样式 */
    QDockWidget {
        border: 1px solid #E0E0E0;
        border-radius: 2px;
        margin: 1px;
        background-color: white;
    }

    /* 标题栏基础样式 - 与主题色协调 */
    QDockWidget::title {
        background-color: white;
        color: #666666; /* 未选中时与标签页保持一致 */
        padding: 6px 10px;
        border-bottom: 1px solid #E0E0E0;
        font-weight: 500;
    }

    /* 聚集时选中的标题栏 - 突出主题色 */
    QDockWidget::title:checked {
        color: #43A0FF; /* 主题色 */
        border-bottom: 2px solid #43A0FF; /* 加粗主题色下划线 */
    }

    /* 聚集标签栏整体样式 */
    QDockWidget::tab-bar {
        background-color: white;
        border-bottom: 1px solid #E0E0E0;
    }

    /* 聚集时未选中标签 */
    QDockWidget::tab {
        color: #666666;
        padding: 6px 16px;
        margin-right: 2px;
        border-bottom: 2px solid transparent;
    }

    /* 聚集时选中标签 - 强化主题色 */
    QDockWidget::tab:selected {
        color: #43A0FF; /* 主题色文字 */
        border-bottom: 2px solid #43A0FF; /* 主题色下划线 */
        background-color: #F0F7FF; /* 主题色浅色背景 */
    }

    /* 标签悬停效果 */
    QDockWidget::tab:hover:!selected {
        color: #43A0FF;
        background-color: #F5F9FF;
    }

    /* 内容区域样式 */
    QDockWidget > QWidget {
        background-color: white;
        border: none;
    }

    /* 手柄样式 - 加入主题色反馈 */
    QDockWidget::handle {
        background-color: #F5F5F5;
    }
    QDockWidget::handle:hover {
        background-color: #E8F0FF; /* 主题色浅色变体 */
    }

    /* 按钮样式 - 主题色交互反馈 */
    QDockWidget::close-button,
    QDockWidget::float-button {
        width: 16px;
        height: 16px;
        subcontrol-origin: padding;
        subcontrol-position: right;
        margin-right: 6px;
        border-radius: 3px;
    }


    /* 聚集标签栏整体样式 */
    QDockWidget::tab-bar {
        background-color: white;
        border-bottom: 1px solid #E0E0E0; /* 与中央标签栏底部边框一致 */
    }

    /* 聚集时未选中标签 */
    QDockWidget::tab {
        color: #666666;
        padding: 6px 16px; /* 与中央标签页内边距匹配 */
        margin-right: 1px;
        border-bottom: 2px solid transparent;
    }

    /* 聚集时选中标签 */
    QDockWidget::tab:selected {
        color: #43A0FF; /* 主题色文字 */
        border-bottom: 2px solid #43A0FF; /* 主题色下划线 */
        background-color: #F0F7FF; /* 主题色浅色背景，增强区分度 */
    }

    /* 标签悬停效果（未选中时） */
    QDockWidget::tab:hover:!selected {
        color: #43A0FF;
        background-color: #F5F9FF; /* 比选中状态更浅的背景 */
    }

    /* 内容区域样式 */
    QDockWidget > QWidget {
        background-color: white;
        border: none;
    }

    /* 手柄样式 */
    QDockWidget::handle {
        background-color: #F5F5F5;
    }
    QDockWidget::handle:hover {
        background-color: #E8F0FF; /* 主题色衍生色，增强交互反馈 */
    }

    /* 关闭/浮动按钮样式 */
    QDockWidget::close-button,
    QDockWidget::float-button {
        width: 16px;
        height: 16px;
        subcontrol-origin: padding;
        subcontrol-position: right;
        margin-right: 6px;
        border-radius: 3px; /* 轻微圆角，增强点击感 */
    }

    QDockWidget::close-button:hover,
    QDockWidget::float-button:hover {
        background-color: #E8F0FF; /* 主题色背景反馈 */
    }

    QDockWidget::close-button:hover,
    QDockWidget::float-button:hover {
        background-color: #E8F0FF; /* 主题色背景反馈 */
    }
QMainWindow QTabBar {  /* 限定作用于主窗口内的 QTabBar（包括停靠聚集标签） */
    background-color: white;
    border-bottom: 1px solid #E0E0E0;
}

QMainWindow QTabBar::tab {  /* 停靠聚集的未选中标签 */
    color: #666666;
    padding: 6px 16px;
    margin-right: 1px;
    border-bottom: 2px solid transparent;
}

QMainWindow QTabBar::tab:selected {  /* 停靠聚集的选中标签 */
    color: #43A0FF;
    border-bottom: 2px solid #43A0FF;
    background-color: #F0F7FF;
}

QMainWindow QTabBar::tab:hover:!selected {  /* 停靠聚集标签的悬停效果 */
    color: #43A0FF;
    background-color: #F5F9FF;
}
)";