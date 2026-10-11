#pragma once
#include <memory>

class QWidget;

// 独立合成底层只模糊桌面；Qt 前景仍负责清晰文字、搜索与点击命中。
class PageOverviewBackdrop final
{
public:
    explicit PageOverviewBackdrop(QWidget* foreground);
    ~PageOverviewBackdrop();
    // 设置 0–40 的高斯强度，0 关闭；返回 false 表示系统合成不可用。
    bool setAmount(int amount);
    // 前景移动、缩放、改变层序时调用；所有坐标均从同一原生 HWND 读取。
    void synchronize();
    void hide();
private:
    struct State;
    std::unique_ptr<State> m_state; // 管理原生背景窗口与合成对象的共同生命周期。
};
