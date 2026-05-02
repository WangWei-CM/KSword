#include "GlobalDialogTheme.h"

#include "../theme.h"

#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QMessageBox>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QWidget>

namespace
{
    // kGlobalDialogThemePropertyName 作用：
    // - 标记某个 QDialog 已纳入全局普通弹窗主题；
    // - QSS 使用该动态属性作为选择器，避免污染非弹窗控件。
    constexpr const char* kGlobalDialogThemePropertyName = "ksword_global_dialog_theme";

    // kGlobalDialogPolishingPropertyName 作用：
    // - 标记当前弹窗正在应用主题；
    // - 避免 setStyleSheet 触发 StyleChange 后递归进入主题刷新。
    constexpr const char* kGlobalDialogPolishingPropertyName = "ksword_global_dialog_polishing";

    // kGlobalDialogDarkModePropertyName 作用：
    // - 缓存上次应用到弹窗的深浅色模式；
    // - 减少同一主题下重复刷新样式表的次数。
    constexpr const char* kGlobalDialogDarkModePropertyName = "ksword_global_dialog_dark_mode";

    // kOriginalDialogStyleSheetPropertyName 作用：
    // - 保存业务弹窗原有样式表；
    // - 全局主题只追加兜底规则，不直接丢弃业务自己的局部样式。
    constexpr const char* kOriginalDialogStyleSheetPropertyName = "ksword_global_dialog_original_style_sheet";

    // kGlobalDialogStyleMarker 作用：
    // - 写入合成样式表中的分隔标记；
    // - 后续刷新时据此区分“业务原样式”和“全局主题追加块”。
    constexpr const char* kGlobalDialogStyleMarker = "/* KSWORD_GLOBAL_DIALOG_THEME_BEGIN */";

    // pureDialogBackgroundColor 作用：
    // - 返回普通弹窗要求的纯色背景；
    // - 深色模式固定黑色，浅色模式固定白色。
    QColor pureDialogBackgroundColor(const bool darkModeEnabled)
    {
        return darkModeEnabled ? QColor(0, 0, 0) : QColor(255, 255, 255);
    }

    // pureDialogTextColor 作用：
    // - 返回与纯黑/纯白背景匹配的主文本色；
    // - 保证输入框、标签、按钮文字在两种主题下可读。
    QColor pureDialogTextColor(const bool darkModeEnabled)
    {
        return darkModeEnabled ? QColor(255, 255, 255) : QColor(0, 0, 0);
    }

    // pureDialogBorderColor 作用：
    // - 返回普通弹窗控件边框色；
    // - 纯黑/纯白背景下使用中性灰边框做层次区分。
    QColor pureDialogBorderColor(const bool darkModeEnabled)
    {
        return darkModeEnabled ? QColor(74, 74, 74) : QColor(208, 208, 208);
    }

    // buildDialogPalette 作用：
    // - 构建普通弹窗 palette；
    // - 与 QSS 配合处理未被样式表覆盖的原生绘制分支。
    QPalette buildDialogPalette(const QPalette& basePalette, const bool darkModeEnabled)
    {
        QPalette dialogPalette = basePalette;
        const QColor backgroundColor = pureDialogBackgroundColor(darkModeEnabled);
        const QColor textColor = pureDialogTextColor(darkModeEnabled);
        const QColor borderColor = pureDialogBorderColor(darkModeEnabled);

        dialogPalette.setColor(QPalette::Window, backgroundColor);
        dialogPalette.setColor(QPalette::Base, backgroundColor);
        dialogPalette.setColor(QPalette::AlternateBase, backgroundColor);
        dialogPalette.setColor(QPalette::Text, textColor);
        dialogPalette.setColor(QPalette::WindowText, textColor);
        dialogPalette.setColor(QPalette::Button, backgroundColor);
        dialogPalette.setColor(QPalette::ButtonText, textColor);
        dialogPalette.setColor(QPalette::Mid, borderColor);
        dialogPalette.setColor(QPalette::Highlight, KswordTheme::PrimaryBlueColor);
        dialogPalette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
        return dialogPalette;
    }

    // buildGlobalDialogStyleSheetBlock 作用：
    // - 生成普通弹窗全局追加样式；
    // - 背景、输入控件、下拉列表、滚动视口都显式填充纯黑/纯白。
    QString buildGlobalDialogStyleSheetBlock(const bool darkModeEnabled)
    {
        const QString backgroundText = pureDialogBackgroundColor(darkModeEnabled).name(QColor::HexRgb).toUpper();
        const QString textColorText = pureDialogTextColor(darkModeEnabled).name(QColor::HexRgb).toUpper();
        const QString borderColorText = pureDialogBorderColor(darkModeEnabled).name(QColor::HexRgb).toUpper();

        return QStringLiteral(
            "\n%1\n"
            "QDialog[%2=\"true\"]{"
            "  background-color:%3 !important;"
            "  color:%4 !important;"
            "}"
            "QDialog[%2=\"true\"] QWidget{"
            "  background-color:%3 !important;"
            "  color:%4 !important;"
            "}"
            "QDialog[%2=\"true\"] QLabel{"
            "  background-color:%3 !important;"
            "  color:%4 !important;"
            "}"
            "QDialog[%2=\"true\"] QLineEdit,"
            "QDialog[%2=\"true\"] QTextEdit,"
            "QDialog[%2=\"true\"] QPlainTextEdit,"
            "QDialog[%2=\"true\"] QSpinBox,"
            "QDialog[%2=\"true\"] QDoubleSpinBox,"
            "QDialog[%2=\"true\"] QComboBox{"
            "  background-color:%3 !important;"
            "  color:%4 !important;"
            "  border:1px solid %5;"
            "  border-radius:3px;"
            "  padding:3px 6px;"
            "  selection-background-color:%6;"
            "  selection-color:#FFFFFF;"
            "}"
            "QDialog[%2=\"true\"] QAbstractScrollArea,"
            "QDialog[%2=\"true\"] QAbstractScrollArea::viewport,"
            "QDialog[%2=\"true\"] QComboBox QAbstractItemView{"
            "  background-color:%3 !important;"
            "  color:%4 !important;"
            "  border:1px solid %5;"
            "  selection-background-color:%6;"
            "  selection-color:#FFFFFF;"
            "}"
            "QDialog[%2=\"true\"] QGroupBox{"
            "  background-color:%3 !important;"
            "  color:%4 !important;"
            "  border:1px solid %5;"
            "  border-radius:4px;"
            "  margin-top:8px;"
            "}"
            "QDialog[%2=\"true\"] QTabWidget::pane{"
            "  background-color:%3 !important;"
            "  border:1px solid %5;"
            "}"
            "QDialog[%2=\"true\"] QTabBar::tab{"
            "  background-color:%3 !important;"
            "  color:%4 !important;"
            "  border:1px solid %5;"
            "  padding:4px 10px;"
            "}"
            "QDialog[%2=\"true\"] QTabBar::tab:selected{"
            "  background-color:%6 !important;"
            "  color:#FFFFFF !important;"
            "}"
            "QDialog[%2=\"true\"] QMenu{"
            "  background-color:%3 !important;"
            "  color:%4 !important;"
            "  border:1px solid %5;"
            "}"
            "QDialog[%2=\"true\"] QMenu::item:selected{"
            "  background-color:%6 !important;"
            "  color:#FFFFFF !important;"
            "}"
            "%7")
            .arg(QString::fromLatin1(kGlobalDialogStyleMarker))
            .arg(QString::fromLatin1(kGlobalDialogThemePropertyName))
            .arg(backgroundText)
            .arg(textColorText)
            .arg(borderColorText)
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::ThemedButtonStyle());
    }

    // originalStyleSheetForDialog 作用：
    // - 获取并缓存业务弹窗原始样式；
    // - 如果当前样式已经含有全局标记，则沿用已缓存的原始样式。
    QString originalStyleSheetForDialog(QDialog* dialog)
    {
        if (dialog == nullptr)
        {
            return QString();
        }

        const QString currentStyleSheet = dialog->styleSheet();
        const bool currentStyleHasThemeBlock = currentStyleSheet.contains(QString::fromLatin1(kGlobalDialogStyleMarker));
        if (!currentStyleHasThemeBlock)
        {
            dialog->setProperty(kOriginalDialogStyleSheetPropertyName, currentStyleSheet);
            return currentStyleSheet;
        }

        return dialog->property(kOriginalDialogStyleSheetPropertyName).toString();
    }

    // shouldThemeDialog 作用：
    // - 判断某个 QDialog 是否应由普通弹窗主题器处理；
    // - QMessageBox 由 UI/ThemedMessageBox 专用逻辑处理，必须排除。
    bool shouldThemeDialog(QDialog* dialog)
    {
        if (dialog == nullptr)
        {
            return false;
        }
        if (qobject_cast<QMessageBox*>(dialog) != nullptr)
        {
            return false;
        }
        return true;
    }

    // GlobalDialogStyler 作用：
    // - QApplication 级事件过滤器；
    // - 在普通弹窗显示、换肤、样式变化时统一补齐背景和控件颜色。
    class GlobalDialogStyler final : public QObject
    {
    public:
        // 构造函数：
        // - 参数 parentObject：通常为 QApplication；
        // - 返回值：无，QObject 生命周期由父对象管理。
        explicit GlobalDialogStyler(QObject* parentObject)
            : QObject(parentObject)
        {
        }

        // eventFilter 作用：
        // - 监听普通弹窗的显示与样式相关事件；
        // - 触发时调用 polishDialog 统一补齐主题。
        bool eventFilter(QObject* watchedObject, QEvent* eventObject) override
        {
            QDialog* dialog = qobject_cast<QDialog*>(watchedObject);
            if (!shouldThemeDialog(dialog) || eventObject == nullptr)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            const QEvent::Type eventType = eventObject->type();
            if (eventType == QEvent::Polish ||
                eventType == QEvent::Show ||
                eventType == QEvent::PaletteChange ||
                eventType == QEvent::ApplicationPaletteChange ||
                eventType == QEvent::StyleChange)
            {
                if (!dialog->property(kGlobalDialogPolishingPropertyName).toBool())
                {
                    polishDialog(dialog);
                }
            }

            return QObject::eventFilter(watchedObject, eventObject);
        }

        // polishDialog 作用：
        // - 对单个普通弹窗应用当前主题；
        // - 不返回值，只修改控件 palette、属性和样式表。
        void polishDialog(QDialog* dialog) const
        {
            if (!shouldThemeDialog(dialog))
            {
                return;
            }
            if (dialog->property(kGlobalDialogPolishingPropertyName).toBool())
            {
                return;
            }

            struct PolishingResetter
            {
                QDialog* targetDialog = nullptr; // targetDialog：需要恢复重入标记的弹窗。
                ~PolishingResetter()
                {
                    if (targetDialog != nullptr)
                    {
                        targetDialog->setProperty(kGlobalDialogPolishingPropertyName, false);
                    }
                }
            };

            dialog->setProperty(kGlobalDialogPolishingPropertyName, true);
            PolishingResetter resetter{ dialog };

            const bool darkModeEnabled = KswordTheme::IsDarkModeEnabled();
            const QPalette sourcePalette = (qApp != nullptr) ? qApp->palette() : dialog->palette();
            const QString originalStyleSheet = originalStyleSheetForDialog(dialog);
            const QString targetStyleSheet = originalStyleSheet + buildGlobalDialogStyleSheetBlock(darkModeEnabled);

            dialog->setProperty(kGlobalDialogThemePropertyName, QStringLiteral("true"));
            dialog->setProperty(kGlobalDialogDarkModePropertyName, darkModeEnabled);
            dialog->setAttribute(Qt::WA_StyledBackground, true);
            dialog->setAutoFillBackground(true);
            dialog->setPalette(buildDialogPalette(sourcePalette, darkModeEnabled));

            if (dialog->styleSheet() != targetStyleSheet)
            {
                dialog->setStyleSheet(targetStyleSheet);
            }

            const QList<QPushButton*> buttonList = dialog->findChildren<QPushButton*>();
            for (QPushButton* button : buttonList)
            {
                if (button == nullptr)
                {
                    continue;
                }
                button->setCursor(Qt::PointingHandCursor);
                if (QStyle* buttonStyle = button->style())
                {
                    buttonStyle->unpolish(button);
                    buttonStyle->polish(button);
                }
            }
        }
    };

    // globalDialogStylerInstance 作用：
    // - 返回普通弹窗全局主题器单例；
    // - 单例父对象绑定 QApplication，避免手动释放。
    GlobalDialogStyler* globalDialogStylerInstance()
    {
        static QPointer<GlobalDialogStyler> stylerInstance;
        if (stylerInstance == nullptr && qApp != nullptr)
        {
            stylerInstance = new GlobalDialogStyler(qApp);
        }
        return stylerInstance.data();
    }
}

namespace ks::ui
{
    void InstallGlobalDialogTheme(QApplication* appInstance)
    {
        if (appInstance == nullptr)
        {
            return;
        }

        GlobalDialogStyler* stylerInstance = globalDialogStylerInstance();
        if (stylerInstance == nullptr)
        {
            return;
        }

        appInstance->installEventFilter(stylerInstance);
        RefreshGlobalDialogTheme();
    }

    void RefreshGlobalDialogTheme()
    {
        GlobalDialogStyler* stylerInstance = globalDialogStylerInstance();
        if (stylerInstance == nullptr || qApp == nullptr)
        {
            return;
        }

        const QWidgetList topLevelWidgetList = qApp->topLevelWidgets();
        for (QWidget* topLevelWidget : topLevelWidgetList)
        {
            QDialog* dialog = qobject_cast<QDialog*>(topLevelWidget);
            if (!shouldThemeDialog(dialog))
            {
                continue;
            }

            stylerInstance->polishDialog(dialog);
        }
    }
}
