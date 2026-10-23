#include "TablePresentation.h"
#include "VisibleTableWidget.h"
#include "./MainSurfaceStyle.h"
#include "./RoundedViewSurface.h"
#include "../theme.h"

#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QVBoxLayout>
#include <QPointer>
#include <QTableView>
#include <QTimer>
#include <QTreeView>
#include <QVariant>

namespace
{
    constexpr char kBegin[] = "/*KSWORD_TABLE_PRESENTATION_BEGIN*/";
    constexpr char kEnd[] = "/*KSWORD_TABLE_PRESENTATION_END*/";
    constexpr char kPreserve[] = "ksword_preserve_custom_table_presentation";
    constexpr char kDensity[] = "ksword_table_presentation_density";
    constexpr char kNormalizeHeader[] = "ksword_table_presentation_normalize_header";
    constexpr char kOriginalFrame[] = "ksword_table_presentation_original_frame";
    constexpr char kOriginalGrid[] = "ksword_table_presentation_original_grid";
    constexpr char kPending[] = "ksword_table_presentation_pending";
    constexpr char kInstalled[] = "ksword_table_presentation_installed";
    constexpr char kApplying[] = "ksword_table_presentation_applying";
    constexpr char kInitialized[] = "ksword_table_presentation_initialized";
    constexpr char kThemeDirty[] = "ksword_table_presentation_theme_dirty";

    // 本组件写入QSS产生的事件不回投自己；真实外部主题/字体通知仍由过滤器维护。
    class PresentationChange final
    {
    public:
        explicit PresentationChange(QAbstractItemView* view)
            : m_view(view), m_previous(view->property(kApplying).toBool())
        {
            view->setProperty(kApplying, true);
            view->setProperty(kInitialized, true);
        }

        ~PresentationChange()
        {
            if (m_view)
            {
                m_view->setProperty(kApplying, m_previous);
                m_view->setProperty(kThemeDirty, false);
            }
        }

    private:
        QPointer<QAbstractItemView> m_view;
        bool m_previous;
    };

    QString withoutPresentationBlock(QString style)
    {
        const qsizetype begin = style.indexOf(QLatin1String(kBegin));
        if (begin >= 0)
        {
            const qsizetype end = style.indexOf(QLatin1String(kEnd), begin);
            if (end >= 0) style.remove(begin, end + qstrlen(kEnd) - begin);
        }
        return style;
    }

    void replacePresentationBlock(QWidget* widget, const QString& block)
    {
        if (widget == nullptr) return;
        QString style = withoutPresentationBlock(widget->styleSheet());
        if (!block.isEmpty()) style += QLatin1String(kBegin) + block + QLatin1String(kEnd);
        if (style != widget->styleSheet()) widget->setStyleSheet(style);
    }

    QHeaderView* columnHeader(QAbstractItemView* view)
    {
        if (auto* table = qobject_cast<QTableView*>(view)) return table->horizontalHeader();
        if (auto* tree = qobject_cast<QTreeView*>(view)) return tree->header();
        return nullptr;
    }

    const QHeaderView* columnHeader(const QAbstractItemView* view)
    {
        if (const auto* table = qobject_cast<const QTableView*>(view)) return table->horizontalHeader();
        if (const auto* tree = qobject_cast<const QTreeView*>(view)) return tree->header();
        return nullptr;
    }

    bool supportedView(const QAbstractItemView* view)
    {
        return qobject_cast<const QTableView*>(view) != nullptr || qobject_cast<const QTreeView*>(view) != nullptr;
    }

    void schedulePresentation(QAbstractItemView* view, bool deferHidden = false)
    {
        if (!supportedView(view)) return;
        // 已初始化隐藏表只留原几何，Show时必然重算最新主题；首次Polish仍建立呈现基线。
        if (deferHidden && !view->isVisible() && view->property(kInitialized).toBool()) return;
        if (view->property(kPending).toBool()) return;
        const QPointer<QAbstractItemView> guardedView(view);
        view->setProperty(kPending, true);
        if (!guardedView) return; // 动态属性通知可同步销毁目标，不能再借原始 view。
        QTimer::singleShot(0, guardedView.data(), [guardedView]()
        {
            if (guardedView.isNull() || !guardedView->property(kPending).toBool()) return;
            guardedView->setProperty(kPending, false);
            if (!guardedView) return;
            const QVariant normalize = guardedView->property(kNormalizeHeader);
            ks::ui::ApplyTablePresentation(guardedView.data(), !normalize.isValid() || normalize.toBool());
        });
    }

    class TablePresentationFilter final : public QObject
    {
    public:
        explicit TablePresentationFilter(QObject* parent) : QObject(parent) {}

        bool eventFilter(QObject* source, QEvent* event) override
        {
            if (event == nullptr) return false;
            switch (event->type())
            {
            case QEvent::Polish:
            {
                const QPointer<QAbstractItemView> view(qobject_cast<QAbstractItemView*>(source));
                if (!supportedView(view.data())) break;
                schedulePresentation(view.data());
                if (!view) return true;
                break;
            }
            case QEvent::Show:
            {
                // Show 到达时视图构造已完成；先统一 frame/grid/表头，避免先绘旧尺寸再跳变。
                const QPointer<QAbstractItemView> view(qobject_cast<QAbstractItemView*>(source));
                if (!supportedView(view.data()) || view->property(kApplying).toBool()) break;
                if (view->property(kInitialized).toBool() &&
                    !view->property(kThemeDirty).toBool())
                {
                    break;
                }
                view->setProperty(kPending, false); // 消费旧首次请求，旧计时回调不再重复改样式。
                if (view.isNull()) return true;
                const QVariant normalize = view->property(kNormalizeHeader);
                ks::ui::ApplyTablePresentation(view.data(), !normalize.isValid() || normalize.toBool());
                if (view.isNull()) return true;
                break;
            }
            case QEvent::StyleChange:
            case QEvent::FontChange:
            case QEvent::PaletteChange:
            {
                // 全局过滤器只给支持的原表格/树标脏，不向普通控件或 Qt 内部视口写属性。
                const QPointer<QAbstractItemView> view(qobject_cast<QAbstractItemView*>(source));
                if (!supportedView(view.data())) break;
                // 仅忽略此表格自己的样式回声，不吞真正的应用主题广播。
                if (view->property(kApplying).toBool()) break;
                view->setProperty(kThemeDirty, true);
                if (!view) return true;
                schedulePresentation(view.data(), true);
                if (!view) return true;
                break;
            }
            case QEvent::ApplicationPaletteChange:
            {
                // 应用广播仍不受 kApplying 抑制；隐藏表只保留 dirty，沿原 Show 时机准备。
                const QPointer<QAbstractItemView> view(qobject_cast<QAbstractItemView*>(source));
                if (!supportedView(view.data())) break;
                view->setProperty(kThemeDirty, true);
                if (!view) return true;
                schedulePresentation(view.data(), true);
                if (!view) return true;
                break;
            }
            default:
                break;
            }
            return false;
        }
    };
}

namespace ks::ui
{
    QWidget* CreateTitledTablePanel(QTableView* table, const QString& title, QWidget* parent)
    {
        if (table == nullptr)
        {
            return nullptr;
        }
        auto* panel = new QWidget(parent); // 布局由 splitter 分配，表格与标题作为整体缩放。
        auto* layout = new QVBoxLayout(panel);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        auto* label = new QLabel(title, panel);
        label->setObjectName(QStringLiteral("ksword_table_panel_title"));
        label->setTextFormat(Qt::PlainText);
        label->setStyleSheet(QStringLiteral("QLabel{color:palette(text);font-weight:600;padding:0 3px;}"));
        layout->addWidget(label);
        layout->addWidget(table, 1);
        ApplyTablePresentation(table);
        return panel;
    }

    bool PreservesCustomTablePresentation(const QAbstractItemView* view)
    {
        return view != nullptr && (view->property(kPreserve).toBool()
            || view->property("ks_preserve_table_presentation").toBool());
    }

    void SetPreserveCustomTablePresentation(QAbstractItemView* view, const bool preserve)
    {
        if (view == nullptr) return;
        view->setProperty(kPreserve, preserve);
        const QVariant normalize = view->property(kNormalizeHeader);
        ApplyTablePresentation(view, !normalize.isValid() || normalize.toBool());
    }

    void SetTablePresentationDensity(QAbstractItemView* view, const TablePresentationDensity density)
    {
        if (view == nullptr) return;
        view->setProperty(kDensity, static_cast<int>(density));
        const QVariant normalize = view->property(kNormalizeHeader);
        ApplyTablePresentation(view, !normalize.isValid() || normalize.toBool());
    }

    void CopyTablePresentation(const QAbstractItemView* source, QAbstractItemView* target)
    {
        if (!supportedView(source) || !supportedView(target) || source == target) return;
        target->setProperty(kDensity, source->property(kDensity));
        target->setProperty(kPreserve, PreservesCustomTablePresentation(source));
        const auto* sourceTable = qobject_cast<const QTableView*>(source);
        auto* targetTable = qobject_cast<QTableView*>(target);
        const QVariant normalize = source->property(kNormalizeHeader);
        const bool preserveHeader = (sourceTable != nullptr && PreservesCustomTableHeaderStyle(sourceTable))
            || (normalize.isValid() && !normalize.toBool());
        if (targetTable != nullptr) SetPreserveCustomTableHeaderStyle(targetTable, preserveHeader);
        if (PreservesCustomTablePresentation(source) && sourceTable != nullptr && targetTable != nullptr)
        {
            targetTable->setProperty(kOriginalGrid, sourceTable->showGrid());
            targetTable->setShowGrid(sourceTable->showGrid());
        }
        if (PreservesCustomTablePresentation(source))
        {
            target->setProperty(kOriginalFrame, static_cast<int>(source->frameShape()));
            target->setFrameShape(source->frameShape());
            target->setStyleSheet(source->styleSheet());
        }
        if (PreservesCustomTablePresentation(source) || preserveHeader || (normalize.isValid() && !normalize.toBool()))
        {
            if (const QHeaderView* sourceHeader = columnHeader(source))
                if (QHeaderView* targetHeader = columnHeader(target)) targetHeader->setStyleSheet(sourceHeader->styleSheet());
            if (sourceTable != nullptr && targetTable != nullptr)
                targetTable->verticalHeader()->setStyleSheet(sourceTable->verticalHeader()->styleSheet());
        }
        ApplyTablePresentation(target, !normalize.isValid() || normalize.toBool());
    }

    void ApplyTablePresentation(QAbstractItemView* view, const bool normalizeHeader)
    {
        if (!supportedView(view)) return;
        const PresentationChange applying(view);
        view->setProperty(kNormalizeHeader, normalizeHeader);
        auto* table = qobject_cast<QTableView*>(view);
        if (PreservesCustomTablePresentation(view))
        {
            // 专业自绘保留原外壳，同时撤掉此前可能已安装的极角装饰。
            SetRoundedViewSurfaceEnabled(view, false);
            replacePresentationBlock(view, {});
            replacePresentationBlock(columnHeader(view), {});
            if (table != nullptr) replacePresentationBlock(table->verticalHeader(), {});
            if (view->property(kOriginalFrame).isValid())
                view->setFrameShape(static_cast<QFrame::Shape>(view->property(kOriginalFrame).toInt()));
            if (table != nullptr && table->property(kOriginalGrid).isValid())
                table->setShowGrid(table->property(kOriginalGrid).toBool());
            return;
        }

        // Never replace the delegate: semantic brushes, search marks, virtualized painting,
        // custom row heights, alternating-row policy and current selection remain business-owned.
        if (!view->property(kOriginalFrame).isValid()) view->setProperty(kOriginalFrame, static_cast<int>(view->frameShape()));
        if (table != nullptr && !table->property(kOriginalGrid).isValid()) table->setProperty(kOriginalGrid, table->showGrid());
        if (view->frameShape() != QFrame::NoFrame) view->setFrameShape(QFrame::NoFrame);
        if (table != nullptr && table->showGrid()) table->setShowGrid(false);
        const bool compact = view->property(kDensity).toInt() == static_cast<int>(TablePresentationDensity::Compact);
        const QString cellPadding = compact ? QStringLiteral("2px 6px") : QStringLiteral("4px 8px");
        const QString headerPadding = compact ? QStringLiteral("3px 6px") : QStringLiteral("5px 8px");
        // 主工作区把数据面、表头和页面底板分开；独立详情窗口继续使用原有角色。
        const bool transparentSurface = UsesTransparentMainSurface(view);
        const bool mainSurface = UsesMainSurfaceHierarchy(view) || transparentSurface;
        const MainSurfaceColors surfaces = MainWindowSurfaceColors();
        const QColor headerColor = mainSurface ? surfaces.header : KswordTheme::SurfaceMutedColor();
        const QString headerSurface = MainDataSurfaceStyleColor(headerColor, transparentSurface);
        const QString headerCanvas = transparentSurface ? QStringLiteral("transparent") : headerSurface;
        const int backdropWeight = transparentSurface ? 255 - MainDataSurfaceAlpha() : 0;
        const QColor headerBackgrounds[] = {
            KswordTheme::BlendColors(headerColor, QColor(Qt::black), backdropWeight),
            KswordTheme::BlendColors(headerColor, QColor(Qt::white), backdropWeight)};
        const QString headerText = KswordTheme::EnsureTextContrastForBackgrounds(
            KswordTheme::TextPrimaryColor(), headerBackgrounds, 2, 4.5).name();
        const QString headerBorder = mainSurface ? surfaces.border.name() : KswordTheme::BorderColor().name();
        // 外围轻描边区分并排表格；冻结辅助窗格是同一表格内容，不重复加圆角边界。
        const bool auxiliaryPane = view->property("KSWORD_TABLE_INTERACTION_FROZEN_PANE_AUXILIARY").toBool();
        const QString outerFrame = auxiliaryPane ? QStringLiteral("border:0;border-radius:0;")
            : QStringLiteral("border:1px solid %1;border-radius:8px;").arg(
                mainSurface ? surfaces.border.name()
                    : KswordTheme::BlendColors(KswordTheme::SurfaceColor(), KswordTheme::BorderColor(), 150).name());
        // 透明数据面同时按最暗/最亮背板估算前景，不能把带alpha底色直接当作纯色校准。
        const QColor rowBackgrounds[] = {
            KswordTheme::BlendColors(surfaces.content, QColor(Qt::black), backdropWeight),
            KswordTheme::BlendColors(surfaces.content, QColor(Qt::white), backdropWeight),
            KswordTheme::BlendColors(surfaces.alternate, QColor(Qt::black), backdropWeight),
            KswordTheme::BlendColors(surfaces.alternate, QColor(Qt::white), backdropWeight)};
        const QString dataSurface = mainSurface
            ? QStringLiteral("background-color:%1;color:%2;alternate-background-color:%3;")
                .arg(MainDataSurfaceStyleColor(surfaces.content, transparentSurface), KswordTheme::EnsureTextContrastForBackgrounds(
                    KswordTheme::TextPrimaryColor(), rowBackgrounds, 4, 4.5).name(),
                    MainDataSurfaceStyleColor(surfaces.alternate, transparentSurface))
            : QStringLiteral("color:palette(text);alternate-background-color:palette(alternate-base);");
        // item 显式 border:0 会让 Qt 样式表接管单元格底面并忽略模型 BackgroundRole。
        // 仅配置 padding，网格仍由 setShowGrid(false) 管理，保留热度/风险/差异语义刷子。
        replacePresentationBlock(view, QStringLiteral(
            "QTableView,QTableWidget,QTreeView,QTreeWidget{"
            "%4%5"
            "selection-background-color:palette(highlight);selection-color:palette(highlighted-text);}"
            "QTableView::item,QTableWidget::item,QTreeView::item,QTreeWidget::item{padding:%1;}"
            "QTableCornerButton::section{background:%2;border:0;border-bottom:1px solid %3;}")
            .arg(cellPadding, headerSurface, headerBorder, outerFrame, dataSurface));

        // QSS 圆角只约束外框，不能裁剪子表头/viewport；四个极角显式补父背景。
        // 冻结辅助窗格属于连体内部拼接，保持方角并让主表负责整个外壳。
        SetRoundedViewSurfaceEnabled(view, !auxiliaryPane);

        // normalizeHeader=false and the existing header-specific opt-out retain custom geometry.
        if (!normalizeHeader || (table != nullptr && PreservesCustomTableHeaderStyle(table)))
        {
            replacePresentationBlock(columnHeader(view), {});
            if (table != nullptr) replacePresentationBlock(table->verticalHeader(), {});
            return;
        }
        const QString headerBlock = QStringLiteral(
            "QHeaderView{background:%6;border:0;}"
            "QHeaderView::section{background-color:%2;color:%3;"
            "border:0;border-bottom:1px solid %4;padding:%1;font-weight:600;}"
            "QHeaderView::section:hover{background-color:%5;}")
            .arg(headerPadding, headerSurface, headerText, headerBorder,
                MainDataSurfaceStyleColor(KswordTheme::ControlInputHoverColor(headerColor), transparentSurface), headerCanvas);
        replacePresentationBlock(columnHeader(view), headerBlock);
        if (table != nullptr)
        {
            replacePresentationBlock(table->verticalHeader(), QStringLiteral(
                "QHeaderView{background:%5;border:0;}"
                "QHeaderView::section{background:%2;color:%3;border:0;"
                "border-right:1px solid %4;padding:%1;font-weight:400;}")
                .arg(headerPadding, headerSurface, headerText, headerBorder, headerCanvas));
        }
    }

    void InstallGlobalTablePresentation(QApplication* appInstance)
    {
        if (appInstance == nullptr || appInstance->property(kInstalled).toBool()) return;
        appInstance->setProperty(kInstalled, true);
        auto* filter = new TablePresentationFilter(appInstance);
        appInstance->installEventFilter(filter);
        for (QWidget* widget : appInstance->allWidgets())
            schedulePresentation(qobject_cast<QAbstractItemView*>(widget));
    }
}
