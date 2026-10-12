#pragma once

#include "./ThemeBinding.h"
#include "./TabHierarchyTransition.h"
#include "../theme.h"
#include "../Internationalization/LanguageManager.h"
#include <QFontMetrics>
#include <QLineEdit>
#include <QPointer>
#include <QTabBar>
#include <QTabWidget>
#include <algorithm>

namespace ks::ui
{
    // 输入页面已确认的搜索字段；只统一搜索外观与单行高度，保留过滤连接和限定宽度。
    // 不扫描页面，不识别字段名，路径、地址、命令输入框必须由调用方明确排除。
    inline void StyleSearchField(QLineEdit* field, int compactHeight = 0)
    {
        if (field == nullptr || field->property("ksword_page_search_style").toBool())
        {
            return;
        }
        field->setProperty("ksword_page_search_style", true);
        // 标题栏双模式字段可明确传入原有紧凑高度；普通页面按字体统一高度。
        field->setFixedHeight(compactHeight > 0 ? compactHeight
            : std::max(28, QFontMetrics(field->font()).height() + 12));
        field->setClearButtonEnabled(true);
        if (field->placeholderText().isEmpty())
        {
            field->setPlaceholderText(ks::i18n::sourceText(QStringLiteral("搜索")));
        }
        // 几何在控件自身设置，颜色交给现有热主题绑定，避免再次引入白色描边。
        field->setStyleSheet(field->styleSheet() + QStringLiteral(
            "\nQLineEdit[ksword_page_search_style=\"true\"]{padding:0px 10px;border-radius:5px;}"));
        BindSearchFieldTheme(field);
    }

    // 普通内容页签共用状态颜色和几何；选中底面/细线平滑移动，原栏保留所有输入。
    void StylePageTabBar(QTabBar* bar);

    // 普通QTabWidget仅统一导航底面与动画；详情/属性窗口保留自身布局。
    void StylePageTabs(QTabWidget* tabs);

    // 明确页面选择等边留白；普通嵌入内容区不因此获得外角。
    // 接管直接页面或项目滚动壳内容的外围layout边距；内部卡片不动，纯嵌套Tab只计最内层。
    void StylePageTabsContentFrame(QTabWidget* tabs, int inset = 6);

    // 非直接父子导航的结果Tab与上方普通工具区连体；pane和结果页根不再重复内缩。
    // 调用页负责header到Tab的局部零间隔；专业header可传空以保留其独立主题绑定。
    void StyleJoinedResultTabs(QTabWidget* tabs, QWidget* header = nullptr);

    // 主导航明确承接方形工作区时，连接内部圆角与最外框；纯单子Tab可继承此资格。
    // 外半径=8+inset，无描边；有工具条或分隔区的复合页改在真正页根登记。
    void StyleSquarePageTabsFrame(QTabWidget* tabs, int inset = 6);

    // 复合主页面明确登记方形页根；挂到已有页面框架内时不再绘内部外角。
    void StyleWorkspaceContentFrame(QWidget* page, int inset = 6);

    // 子级保持轻底面与左缩进，顶部直接接父栏；原子栏方向动画继续使用原入口。
    void StyleChildPageTabs(QTabWidget* tabs);

    // 主窗口普通页签的最终回退样式，使用与明确登记页面相同的颜色和几何。
    // mainSurface 指定主工作区表面；独立设置/属性窗口仍保留其明确局部样式。
    QString BuildPageTabsStyleSheet(bool mainSurface);

    // 普通页签的动态图标按父/子栏实际常态、悬停与选中底面统一校准，返回可见前景。
    QColor PageTabIconContrastColor(const QColor& preferred);

    // 子栏快照动画读取当前栏真实底色；空栏回退普通导航底，不使用页面数据。
    QColor PageTabRailColor(const QTabBar* bar);
}
