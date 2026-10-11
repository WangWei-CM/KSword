#pragma once
#include <QWidget>

class QVBoxLayout;
class QHBoxLayout;
class QSplitter;
class QScrollArea;
class QGroupBox;

namespace ks::ui
{
    // 参数操作页的两栏骨架；参数独立滚动，执行区固定，结果保留全部可用高度。
    class ParameterWorkspace final : public QWidget
    {
    public:
        explicit ParameterWorkspace(QWidget* parent, int parameterWidth = 480);
        QVBoxLayout* parameters() const { return m_parameters; }
        QVBoxLayout* results() const { return m_results; }
        QHBoxLayout* actions() const { return m_actions; }
        // 页面完成构造后调用，只为该参数区内的独立输入控件补齐表面和行高。
        void finish();
    protected:
        void resizeEvent(QResizeEvent* event) override;
    private:
        QSplitter* m_splitter = nullptr; // 宽窗口左右排列，窄窗口上下排列。
        QScrollArea* m_scroll = nullptr; // 只有参数正文参与滚动，底部动作不滚走。
        QWidget* m_parameterPanel = nullptr;
        QWidget* m_resultPanel = nullptr;
        QVBoxLayout* m_parameters = nullptr;
        QVBoxLayout* m_results = nullptr;
        QHBoxLayout* m_actions = nullptr;
        int m_parameterWidth = 480;
        bool m_positioned = false; // 初次分配后保留用户拖动分隔条的位置。
    };

    // 仅由逐页布局明确登记；保留内容、验证器、业务信号与启用状态。
    void StyleParameterFields(QWidget* container);
    void StyleParameterSection(QGroupBox* group);
    // 将低频参数放入可展开分区，默认折叠，不改变子控件自身 enabled 状态。
    QWidget* FoldParameterSection(QGroupBox* group, bool expanded = false);
}
