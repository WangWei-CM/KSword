#pragma once
#include <QSlider>

namespace ks::ui
{
    // 标准QSlider默认也使用同一轨道/圆把手外观；主题重建时重新生成，不要求替换控件类型。
    QString BuildStandardSliderStyle();

    // 连续数值控件：仍使用 QSlider 的输入、范围、步长、跟踪与信号，仅替换绘制。
    // 原 QSlider* 成员可直接持有此类型；不另建配置值或滚轮算法。
    class ThemedSlider final : public QSlider
    {
    public:
        explicit ThemedSlider(Qt::Orientation orientation, QWidget* parent = nullptr);
    protected:
        void paintEvent(QPaintEvent* event) override;
    };
}
