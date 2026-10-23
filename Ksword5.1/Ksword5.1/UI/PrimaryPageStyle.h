#pragma once

#include <QString>

class QComboBox;
class QGroupBox;
class QLabel;
class QWidget;

namespace ks::ui
{
    // Pure QSS composition: preserve the existing standalone rule and position.
    QString WithStandaloneInputGeometry(QString style);
    QString WithParameterInputGeometry(QString style);

    // 页面显式登记组合框，完整维护中性底、文字、箭头与交互态；不改变选项或业务信号。
    void StylePrimaryCombo(QComboBox* combo);

    // 仅登记页面确认独立成块的输入框；组合控件内部编辑器和连体选择组不得调用。
    // 对齐外轮廓圆角；没有独立主题绑定时补齐输入角色，保留命中区域和业务逻辑。
    void StyleStandaloneInput(QWidget* input);

    // 普通分组采用无描边卡片与内部标题；参数分区复用标题/分隔线，不替换业务控件。
    void StylePrimaryGroup(QGroupBox* group);

    // toolbar 是页面明确建立的操作区；圆角轻底面区分操作与结果，不遍历子控件。
    void StylePrimaryToolbar(QWidget* toolbar);

    // 图表等独立数据面由页面明确登记，与表格使用同层底色和轻边界。
    void StylePrimaryContentPanel(QWidget* panel, bool outline = true);

    // 普通状态/列表/专业工作页的显式阅读基线；不扫描或重排业务控件。
    // page 保留原布局，仅统一正文下限、四周留白和主题画布。
    void StyleWorkspacePage(QWidget* page, int margin = 16);

    // 只为页面标题绑定可读次级颜色与字重，不接管状态提示或模型项的数据语义色。
    void StylePrimarySectionTitle(QLabel* title);
}
