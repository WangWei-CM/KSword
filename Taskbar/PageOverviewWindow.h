#pragma once
#include <QWidget>
#include <memory>

class QScreen;
class QShowEvent;
class QHideEvent;
class QMouseEvent;
class QKeyEvent;

// Taskbar 独立透明导航窗口：页面目录来自同目录 KSword，只有点击时才切换页面。
class PageOverviewWindow final : public QWidget
{
public:
    explicit PageOverviewWindow(QWidget* owner);
    ~PageOverviewWindow() override;
    void openOnScreen(QScreen* screen);
protected:
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
private:
    struct State;
    std::unique_ptr<State> m_state; // 连接、目录列、搜索与 RGB 动画只属于该窗口。
};
