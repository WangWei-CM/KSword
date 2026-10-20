#pragma once
#include <QWidget>
#include "../UI/PageNavigationTabCatalog.h"
#include <QList>
#include <QPointer>

class QLabel;
class QVBoxLayout;
class QShowEvent;
class QTableWidget;

namespace ks::misc
{
    // 真实主程序中的标准主题检查页，示例控件不逐个调用样式适配函数。
    class ThemeCheckPage final : public QWidget
    {
    public:
        static ks::ui::PageNavigationCatalog navigationCatalog(const ThemeCheckPage* existing = nullptr);
        QWidget* activateNavigationDescriptor(const QString& id);

        explicit ThemeCheckPage(QWidget* parent = nullptr);
    protected:
        void showEvent(QShowEvent* event) override;
    private:
        ks::ui::PageNavigationTabCatalog m_navigationTabs;

        void addDynamicControls(); // 在已显示页面追加原生控件，验证运行时自动接入。
        void refreshChecks(); // 读取真实绑定/尺寸，不把这些检查当作像素视觉验收。
        QLabel* m_status = nullptr; // 检查结果与当前背景模式。
        QTableWidget* m_table = nullptr; // 显示样本真实接入与行高，不使用假业务结果占位。
        QVBoxLayout* m_dynamicLayout = nullptr; // 动态原生控件组。
        QList<QPointer<QWidget>> m_samples; // 只保存本页明确的独立单行样本。
        int m_dynamicCount = 0; // 限制示例组数量，避免检查页无限增长。
    };
}
