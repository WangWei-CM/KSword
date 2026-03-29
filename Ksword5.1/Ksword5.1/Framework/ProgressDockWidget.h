#pragma once

// ProgressDockWidget：当前操作面板中的“进度任务卡片列表”。
// 该控件职责：
// 1) 周期读取全局 kPro 的任务快照；
// 2) 把每个任务渲染为“任务名 + 步骤 + 进度条”卡片；
// 3) 自动隐藏已完成任务（由 kPro.set(..., progress=1.0) 控制）。

#include "../Framework.h"

#include <QString>
#include <QWidget>

class QLabel;
class QScrollArea;
class QTimer;
class QVBoxLayout;

class ProgressDockWidget final : public QWidget
{
public:
    // 构造函数作用：
    // - 初始化滚动区域与卡片容器；
    // - 启动定时刷新任务卡片。
    // 参数 parent：Qt 父对象。
    explicit ProgressDockWidget(QWidget* parent = nullptr);

private:
    // initializeUi 作用：
    // - 创建主布局、滚动容器与“空列表提示”。
    void initializeUi();

    // applyTransparentBackgroundPolicy 作用：
    // - 把“当前操作”面板根控件/滚动区域/viewport 全部改为透明；
    // - 避免 Dock 内容区域出现默认底色。
    void applyTransparentBackgroundPolicy();

    // initializeRefreshTimer 作用：
    // - 创建定时器；
    // - 通过 Revision 判断是否刷新。
    void initializeRefreshTimer();

    // refreshTaskCards 作用：
    // - 从 kPro 拉取快照并重建卡片视图；
    // - 支持按需刷新（forceRefresh）。
    // 参数 forceRefresh：
    // - true  强制刷新；
    // - false revision 不变时跳过刷新。
    void refreshTaskCards(bool forceRefresh);

    // clearCardLayout 作用：
    // - 删除旧卡片，避免内存泄漏并清空视图。
    void clearCardLayout();

    // createTaskCardWidget 作用：
    // - 根据单个任务数据创建卡片控件。
    // 参数 taskItem：任务快照数据。
    // 返回值：新建的卡片 QWidget 指针（父对象由布局接管）。
    QWidget* createTaskCardWidget(const kProgressTask& taskItem) const;

    // buildHighContrastTextHex 作用：
    // - 根据深浅主题返回高对比黑/白文字颜色；
    // - 避免透明背景下灰字看不清。
    // 返回值：可直接写入 QSS 的颜色字符串。
    QString buildHighContrastTextHex() const;

    // buildCardBackgroundHex 作用：
    // - 为无边框卡片提供轻量半透明底色；
    // - 在透明 Dock 上保持文字可读性。
    // 返回值：可直接写入 QSS 的 rgba 字符串。
    QString buildCardBackgroundHex() const;

    // buildProgressBarStyleSheet 作用：
    // - 统一生成进度条样式；
    // - 保证文字、底轨、进度块在深浅主题下都清晰。
    // 返回值：QProgressBar 样式表文本。
    QString buildProgressBarStyleSheet() const;

private:
    QVBoxLayout* m_rootLayout = nullptr;        // 根布局。
    QScrollArea* m_scrollArea = nullptr;        // 滚动容器。
    QWidget* m_scrollContent = nullptr;         // 滚动内容根控件。
    QVBoxLayout* m_cardLayout = nullptr;        // 卡片垂直布局。
    QLabel* m_emptyTipLabel = nullptr;          // “暂无任务”提示标签。
    QTimer* m_refreshTimer = nullptr;           // 定时刷新器。
    std::size_t m_lastRevision = 0;             // 上次刷新时 revision。
};
