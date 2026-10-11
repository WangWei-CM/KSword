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

    // 普通内容页签共用状态颜色和几何；只登记明确传入的栏，保留页序和导航行为。
    void StylePageTabBar(QTabBar* bar);

    // 已确认的 QTabWidget 连接全宽导航底板与页面圆角；保留内容页自己的内边距。
    void StylePageTabs(QTabWidget* tabs);

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
