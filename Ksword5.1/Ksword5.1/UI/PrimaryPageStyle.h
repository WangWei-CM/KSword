#pragma once

class QComboBox;
class QGroupBox;
class QLabel;
class QWidget;

namespace ks::ui
{
    // 页面显式登记组合框，完整维护中性底、文字、箭头与交互态；不改变选项或业务信号。
    void StylePrimaryCombo(QComboBox* combo);

    // 仅登记页面确认独立成块的输入框；组合控件内部编辑器和连体选择组不得调用。
    // 只对齐外轮廓圆角，保持颜色、命中区域和原业务逻辑。
    void StyleStandaloneInput(QWidget* input);

    // 已有功能分组保留完整细边界，只统一底面和标题，不创建卡片或改变内部布局。
    void StylePrimaryGroup(QGroupBox* group);

    // toolbar 是页面明确建立的操作区；轻底面和下分界区分操作与结果，不遍历子控件。
    void StylePrimaryToolbar(QWidget* toolbar);

    // 图表等独立数据面由页面明确登记，与表格使用同层底色和轻边界。
    void StylePrimaryContentPanel(QWidget* panel, bool outline = true);

    // 普通状态/列表/专业工作页的显式阅读基线；不扫描或重排业务控件。
    // page 保留原布局，仅统一正文下限、四周留白和主题画布。
    void StyleWorkspacePage(QWidget* page, int margin = 16);

    // 只为页面标题绑定可读次级颜色与字重，不接管状态提示或模型项的数据语义色。
    void StylePrimarySectionTitle(QLabel* title);
}
