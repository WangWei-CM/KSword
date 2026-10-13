#pragma once

#include <QString>

class CodeEditorWidget;
class QVBoxLayout;
class QWidget;

namespace ks::misc
{
    // PopulateCksResearchPage：向既有参数画布添加只读候选，返回共享结果区的记录模板。
    // page/parameters 为第三个 Tab 的正文；resultParent 为现有工作区，不创建第二套布局。
    CodeEditorWidget* PopulateCksResearchPage(
        QWidget* page, QVBoxLayout* parameters, QWidget* resultParent);

    // CksResearchVerificationTemplate：按当前语言生成完整记录模板，供显示、复制与导出共用。
    QString CksResearchVerificationTemplate();
}
