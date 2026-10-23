#include "DesktopDrawingPage.h"
#include "../../UI/PrimaryPageStyle.h"
#include "../../UI/ParameterWorkspace.h"
#include "../../UI/ToolbarMetrics.h"

#include "../../Internationalization/LanguageManager.h"
#include "../../UI/ThemeStatusRole.h"

#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
    enum class DesktopDrawingBodyText
    {
        Display, Refresh, Pattern, Cross, Circle, Rectangle, Diamond, Star,
        X, Y, Center, Size, LineWidth, Rate, Color, Start, Stop, Hotkey, Idle, DisplayChanged
    };
    struct DesktopDrawingBodyEntry
    {
        const char* target;
        QString key;
        QString source;
        bool mnemonic;
    };
    const DesktopDrawingBodyEntry& desktopDrawingBodyEntry(DesktopDrawingBodyText id)
    {
        static const DesktopDrawingBodyEntry entries[] = {
            {"ks_desktop_drawing_display", QStringLiteral("misc.desktop_drawing.display"), QStringLiteral("绘制显示器"), false},
            {"ks_desktop_drawing_refresh", QStringLiteral("misc.desktop_drawing.refresh"), QStringLiteral("刷新显示器"), true},
            {"ks_desktop_drawing_pattern", QStringLiteral("misc.desktop_drawing.pattern"), QStringLiteral("绘制图案"), false},
            {"ks_desktop_drawing_pattern_combo", QStringLiteral("misc.desktop_drawing.cross"), QStringLiteral("十字准星"), false},
            {"ks_desktop_drawing_pattern_combo", QStringLiteral("misc.desktop_drawing.circle"), QStringLiteral("圆环"), false},
            {"ks_desktop_drawing_pattern_combo", QStringLiteral("misc.desktop_drawing.rectangle"), QStringLiteral("矩形边框"), false},
            {"ks_desktop_drawing_pattern_combo", QStringLiteral("misc.desktop_drawing.diamond"), QStringLiteral("菱形"), false},
            {"ks_desktop_drawing_pattern_combo", QStringLiteral("misc.desktop_drawing.star"), QStringLiteral("五角星"), false},
            {"ks_desktop_drawing_x", QStringLiteral("misc.desktop_drawing.x"), QStringLiteral("中心横坐标（物理像素）"), false},
            {"ks_desktop_drawing_y", QStringLiteral("misc.desktop_drawing.y"), QStringLiteral("中心纵坐标（物理像素）"), false},
            {"ks_desktop_drawing_center", QStringLiteral("misc.desktop_drawing.center"), QStringLiteral("居中到所选显示器"), true},
            {"ks_desktop_drawing_size", QStringLiteral("misc.desktop_drawing.size"), QStringLiteral("图案尺寸（物理像素）"), false},
            {"ks_desktop_drawing_line_width", QStringLiteral("misc.desktop_drawing.line_width"), QStringLiteral("线条宽度（物理像素）"), false},
            {"ks_desktop_drawing_rate", QStringLiteral("misc.desktop_drawing.rate"), QStringLiteral("每秒重绘次数"), false},
            {"ks_desktop_drawing_color", QStringLiteral("misc.desktop_drawing.color"), QStringLiteral("图案颜色"), false},
            {"ks_desktop_drawing_start", QStringLiteral("misc.desktop_drawing.start"), QStringLiteral("开始绘制"), true},
            {"ks_desktop_drawing_stop", QStringLiteral("misc.desktop_drawing.stop"), QStringLiteral("停止并刷新"), true},
            {"ks_desktop_drawing_hotkey", QStringLiteral("misc.desktop_drawing.hotkey"), QStringLiteral("按 Ctrl+Alt+F10 停止绘制。"), false},
            {"ks_desktop_drawing_status", QStringLiteral("misc.desktop_drawing.idle"), QStringLiteral("尚未开始绘制。"), false},
            {"ks_desktop_drawing_status", QStringLiteral("misc.desktop_drawing.display_changed"), QStringLiteral("显示器不可用或布局已变化。请刷新显示器后重新开始。"), false},
        };
        return entries[static_cast<int>(id)];
    }
    void bindDesktopDrawingBody(ks::i18n::LanguageManager& language, QWidget* widget,
        DesktopDrawingBodyText id)
    {
        const auto& entry = desktopDrawingBodyEntry(id);
        widget->setObjectName(QString::fromLatin1(entry.target));
        language.bindText(widget, entry.key, entry.source);
    }
    QString desktopDrawingDisplayText(const ks::misc::desktop_drawing::Display& display)
    {
        const QString name = QString::fromStdWString(display.name);
        return QStringLiteral("%1  (%2 x %3)").arg(name)
            .arg(display.bounds.right - display.bounds.left)
            .arg(display.bounds.bottom - display.bounds.top);
    }

    constexpr int kStopHotkeyId = 0x4B44;

    QSpinBox* makeSpin(QWidget* parent, int minimum, int maximum, int value)
    {
        auto* spin = new QSpinBox(parent);
        spin->setRange(minimum, maximum);
        spin->setValue(value);
        spin->setKeyboardTracking(false);
        return spin;
    }
}

namespace ks::misc
{
    QColor DesktopDrawingPage::initialDrawingColor()
    {
        return QColor(255, 80, 80);
    }

    QVector<ks::ui::PageSearchBodyText> DesktopDrawingPage::coldSearchBody(
        const std::vector<desktop_drawing::Display>& initialDisplays)
    {
        // The caller must provide the actual fresh enumeration snapshot. An
        // assumed display/default status cannot represent the initial page.
        QVector<ks::ui::PageSearchBodyText> body;
        body.reserve(20 + static_cast<qsizetype>(initialDisplays.size()));
        const auto append = [&body](DesktopDrawingBodyText id)
        {
            const auto& entry = desktopDrawingBodyEntry(id);
            body.append({ks::i18n::text(entry.key, entry.source),
                QString::fromLatin1(entry.target), {}, entry.mnemonic});
        };
        // Real QObject creation order: combo before its label, and the color
        // button before the color label. Spin values are not search text.
        // GlobalUiSearch skips all combo items when the real count exceeds 60.
        if (initialDisplays.size() <= 60)
            for (const auto& display : initialDisplays)
                body.append({ks::i18n::sourceText(desktopDrawingDisplayText(display)),
                    QStringLiteral("ks_desktop_drawing_display_combo"), {}, false});
        append(DesktopDrawingBodyText::Display);
        append(DesktopDrawingBodyText::Refresh);
        for (const auto id : {DesktopDrawingBodyText::Cross, DesktopDrawingBodyText::Circle,
            DesktopDrawingBodyText::Rectangle, DesktopDrawingBodyText::Diamond, DesktopDrawingBodyText::Star}) append(id);
        for (const auto id : {DesktopDrawingBodyText::Pattern, DesktopDrawingBodyText::X, DesktopDrawingBodyText::Y,
            DesktopDrawingBodyText::Center, DesktopDrawingBodyText::Size, DesktopDrawingBodyText::LineWidth,
            DesktopDrawingBodyText::Rate}) append(id);
        body.append({initialDrawingColor().name(QColor::HexRgb),
            QStringLiteral("ks_desktop_drawing_color_button"), {}, true});
        for (const auto id : {DesktopDrawingBodyText::Color, DesktopDrawingBodyText::Start,
            DesktopDrawingBodyText::Stop, DesktopDrawingBodyText::Hotkey}) append(id);
        append(initialDisplays.empty() ? DesktopDrawingBodyText::DisplayChanged : DesktopDrawingBodyText::Idle);
        return body;
    }

    DesktopDrawingPage::DesktopDrawingPage(QWidget* parent) : QWidget(parent)
    {
        initializeUi();
        m_timer = new QTimer(this);
        m_timer->setTimerType(Qt::PreciseTimer);
        connect(m_timer, &QTimer::timeout, this, &DesktopDrawingPage::drawFrame);
        connect(qApp, &QCoreApplication::aboutToQuit, this, &DesktopDrawingPage::stopDrawing);
        qApp->installNativeEventFilter(this);
        refreshDisplays();
    }

    DesktopDrawingPage::~DesktopDrawingPage()
    {
        stopDrawing();
        if (qApp)
        {
            qApp->removeNativeEventFilter(this);
        }
    }

    void DesktopDrawingPage::initializeUi()
    {
        auto& language = ks::i18n::LanguageManager::instance();
        auto* root = new QVBoxLayout(this);
        root->setSpacing(10);
        ks::ui::StyleWorkspacePage(this);

        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        m_settings = new QWidget(scroll);
        auto* form = new QFormLayout(m_settings);
        form->setContentsMargins(16, 16, 16, 16);
        ks::ui::StylePrimaryContentPanel(m_settings, false);
        // 简短表单保持一屏可读，标签与字段使用固定间距。
        form->setHorizontalSpacing(16);
        form->setVerticalSpacing(8);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        auto addRow = [&](DesktopDrawingBodyText id, QWidget* control)
            {
                auto* label = new QLabel(m_settings);
                bindDesktopDrawingBody(language, label, id);
                label->setBuddy(control);
                form->addRow(label, control);
            };

        m_displayCombo = new QComboBox(m_settings);
        m_displayCombo->setObjectName(QStringLiteral("ks_desktop_drawing_display_combo"));
        ks::ui::StylePrimaryCombo(m_displayCombo);
        m_displayCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_displayCombo->setMinimumContentsLength(20);
        addRow(DesktopDrawingBodyText::Display, m_displayCombo);
        auto* refresh = new QPushButton(m_settings);
        bindDesktopDrawingBody(language, refresh, DesktopDrawingBodyText::Refresh);
        form->addRow(QString(), refresh);

        m_patternCombo = new QComboBox(m_settings);
        m_patternCombo->setObjectName(QStringLiteral("ks_desktop_drawing_pattern_combo"));
        ks::ui::StylePrimaryCombo(m_patternCombo);
        auto addPattern = [&](desktop_drawing::Pattern pattern, DesktopDrawingBodyText id)
            {
                const int index = m_patternCombo->count();
                const auto& entry = desktopDrawingBodyEntry(id);
                m_patternCombo->addItem(entry.source, static_cast<int>(pattern));
                language.bindComboBoxItem(m_patternCombo, index, entry.key, entry.source);
            };
        addPattern(desktop_drawing::Pattern::Cross, DesktopDrawingBodyText::Cross);
        addPattern(desktop_drawing::Pattern::Circle, DesktopDrawingBodyText::Circle);
        addPattern(desktop_drawing::Pattern::Rectangle, DesktopDrawingBodyText::Rectangle);
        addPattern(desktop_drawing::Pattern::Diamond, DesktopDrawingBodyText::Diamond);
        addPattern(desktop_drawing::Pattern::Star, DesktopDrawingBodyText::Star);
        m_patternCombo->setCurrentIndex(4);
        addRow(DesktopDrawingBodyText::Pattern, m_patternCombo);

        m_xSpin = makeSpin(m_settings, 0, 0, 0);
        m_ySpin = makeSpin(m_settings, 0, 0, 0);
        m_sizeSpin = makeSpin(m_settings, 16, 1024, 160);
        m_lineWidthSpin = makeSpin(m_settings, 1, 32, 3);
        m_rateSpin = makeSpin(m_settings, 5, 120, 30);
        addRow(DesktopDrawingBodyText::X, m_xSpin);
        addRow(DesktopDrawingBodyText::Y, m_ySpin);
        auto* center = new QPushButton(m_settings);
        bindDesktopDrawingBody(language, center, DesktopDrawingBodyText::Center);
        form->addRow(QString(), center);
        addRow(DesktopDrawingBodyText::Size, m_sizeSpin);
        addRow(DesktopDrawingBodyText::LineWidth, m_lineWidthSpin);
        addRow(DesktopDrawingBodyText::Rate, m_rateSpin);
        m_colorButton = new QPushButton(m_settings);
        m_colorButton->setObjectName(QStringLiteral("ks_desktop_drawing_color_button"));
        updateColorButton();
        addRow(DesktopDrawingBodyText::Color, m_colorButton);
        // 绘制结果在桌面上，此页保留单列表单，不创建空的结果面板。
        ks::ui::StyleParameterFields(m_settings);
        scroll->setWidget(m_settings);
        root->addWidget(scroll, 1);

        auto* actions = new QHBoxLayout;
        m_startButton = new QPushButton(this);
        m_stopButton = new QPushButton(this);
        bindDesktopDrawingBody(language, m_startButton, DesktopDrawingBodyText::Start);
        bindDesktopDrawingBody(language, m_stopButton, DesktopDrawingBodyText::Stop);
        m_stopButton->setEnabled(false);
        actions->addWidget(m_startButton);
        actions->addWidget(m_stopButton);
        actions->addStretch();
        ks::ui::NormalizeToolbarRow(actions);
        root->addLayout(actions);
        m_hotkeyLabel = new QLabel(this);
        m_hotkeyLabel->setWordWrap(true);
        bindDesktopDrawingBody(language, m_hotkeyLabel, DesktopDrawingBodyText::Hotkey);
        root->addWidget(m_hotkeyLabel);
        m_statusLabel = new QLabel(this);
        m_statusLabel->setWordWrap(true);
        bindDesktopDrawingBody(language, m_statusLabel, DesktopDrawingBodyText::Idle);
        ks::ui::ApplyStatusRole(m_statusLabel, ks::ui::StatusRole::Idle);
        root->addWidget(m_statusLabel);

        connect(refresh, &QPushButton::clicked, this, &DesktopDrawingPage::refreshDisplays);
        connect(center, &QPushButton::clicked, this, &DesktopDrawingPage::updateCoordinates);
        connect(m_displayCombo, &QComboBox::currentIndexChanged, this, &DesktopDrawingPage::updateCoordinates);
        connect(m_colorButton, &QPushButton::clicked, this, &DesktopDrawingPage::chooseColor);
        connect(m_startButton, &QPushButton::clicked, this, &DesktopDrawingPage::startDrawing);
        connect(m_stopButton, &QPushButton::clicked, this, &DesktopDrawingPage::stopDrawing);
    }

    void DesktopDrawingPage::refreshDisplays()
    {
        stopDrawing();
        const QString previous = m_displayCombo->currentData().toString();
        const QSignalBlocker blocker(m_displayCombo);
        m_displays = desktop_drawing::enumerateDisplays();
        m_displayCombo->clear();
        for (const auto& display : m_displays)
        {
            const QString name = QString::fromStdWString(display.name);
            m_displayCombo->addItem(desktopDrawingDisplayText(display), name);
        }
        const int previousIndex = m_displayCombo->findData(previous);
        if (previousIndex >= 0)
        {
            m_displayCombo->setCurrentIndex(previousIndex);
        }
        updateCoordinates();
        m_startButton->setEnabled(!m_displays.empty());
        if (m_displays.empty())
        {
            showFailure(desktop_drawing::DrawResult::DisplayChanged);
        }
    }

    void DesktopDrawingPage::updateCoordinates()
    {
        const int index = m_displayCombo->currentIndex();
        if (index < 0 || index >= static_cast<int>(m_displays.size()))
        {
            return;
        }
        const RECT& bounds = m_displays[index].bounds;
        m_xSpin->setRange(0, bounds.right - bounds.left - 1);
        m_ySpin->setRange(0, bounds.bottom - bounds.top - 1);
        m_xSpin->setValue((bounds.right - bounds.left) / 2);
        m_ySpin->setValue((bounds.bottom - bounds.top) / 2);
    }

    void DesktopDrawingPage::chooseColor()
    {
        const QColor color = QColorDialog::getColor(m_color, this,
            ks::i18n::text(QStringLiteral("misc.desktop_drawing.color"), QStringLiteral("图案颜色")));
        if (color.isValid())
        {
            m_color = color;
            updateColorButton();
        }
    }

    void DesktopDrawingPage::updateColorButton()
    {
        QPixmap swatch(20, 20);
        swatch.fill(m_color);
        m_colorButton->setIcon(QIcon(swatch));
        m_colorButton->setText(m_color.name(QColor::HexRgb));
    }

    void DesktopDrawingPage::startDrawing()
    {
        if (m_timer->isActive())
        {
            return;
        }
        const int index = m_displayCombo->currentIndex();
        if (index < 0 || index >= static_cast<int>(m_displays.size()))
        {
            showFailure(desktop_drawing::DrawResult::DisplayChanged);
            return;
        }
        desktop_drawing::Options options;
        options.display = m_displays[index];
        options.pattern = static_cast<desktop_drawing::Pattern>(m_patternCombo->currentData().toInt());
        options.x = m_xSpin->value();
        options.y = m_ySpin->value();
        options.size = m_sizeSpin->value();
        options.lineWidth = m_lineWidthSpin->value();
        options.color = RGB(m_color.red(), m_color.green(), m_color.blue());
        const auto result = m_renderer.start(options);
        if (result != desktop_drawing::DrawResult::Success)
        {
            showFailure(result);
            return;
        }
        // nullptr 将热键关联到当前线程消息队列，不需要为绘制创建窗口。
        m_hotkeyRegistered = ::RegisterHotKey(nullptr, kStopHotkeyId,
            MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F10) != FALSE;
        auto& language = ks::i18n::LanguageManager::instance();
        if (m_hotkeyRegistered)
        {
            language.bindText(m_hotkeyLabel, QStringLiteral("misc.desktop_drawing.hotkey"),
                QStringLiteral("按 Ctrl+Alt+F10 停止绘制。"));
        }
        else
        {
            language.bindText(m_hotkeyLabel, QStringLiteral("misc.desktop_drawing.hotkey_unavailable"),
                QStringLiteral("快捷键不可用，请使用“停止并刷新”。"));
        }
        ks::ui::ApplyStatusRole(m_hotkeyLabel, m_hotkeyRegistered ? ks::ui::StatusRole::None : ks::ui::StatusRole::Warning);
        m_timer->start((1000 + m_rateSpin->value() - 1) / m_rateSpin->value());
        setRunning(true);
        language.bindText(m_statusLabel, QStringLiteral("misc.desktop_drawing.running"),
            QStringLiteral("正在绘制。"));
        ks::ui::ApplyStatusRole(m_statusLabel, ks::ui::StatusRole::Info);
    }

    void DesktopDrawingPage::stopDrawing()
    {
        const bool wasRunning = m_timer && m_timer->isActive();
        if (m_timer)
        {
            m_timer->stop();
        }
        if (m_hotkeyRegistered)
        {
            ::UnregisterHotKey(nullptr, kStopHotkeyId);
            m_hotkeyRegistered = false;
        }
        m_renderer.stop();
        setRunning(false);
        if (wasRunning)
        {
            ks::i18n::LanguageManager::instance().bindText(m_statusLabel,
                QStringLiteral("misc.desktop_drawing.stopped"), QStringLiteral("已停止绘制。"));
            ks::ui::ApplyStatusRole(m_statusLabel, ks::ui::StatusRole::Idle);
        }
    }

    void DesktopDrawingPage::setRunning(bool running)
    {
        m_settings->setEnabled(!running);
        m_startButton->setEnabled(!running && !m_displays.empty());
        m_stopButton->setEnabled(running);
    }

    void DesktopDrawingPage::drawFrame()
    {
        const auto result = m_renderer.drawFrame();
        if (result != desktop_drawing::DrawResult::Success)
        {
            stopDrawing();
            showFailure(result);
        }
    }

    void DesktopDrawingPage::showFailure(desktop_drawing::DrawResult result)
    {
        auto& language = ks::i18n::LanguageManager::instance();
        if (result == desktop_drawing::DrawResult::DesktopUnavailable)
        {
            language.bindText(m_statusLabel, QStringLiteral("misc.desktop_drawing.desktop_unavailable"),
                QStringLiteral("桌面已切换，绘制已停止。"));
        }
        else if (result == desktop_drawing::DrawResult::DisplayChanged)
        {
            bindDesktopDrawingBody(language, m_statusLabel, DesktopDrawingBodyText::DisplayChanged);
        }
        else
        {
            language.bindText(m_statusLabel, QStringLiteral("misc.desktop_drawing.failed"),
                QStringLiteral("绘制失败，请重新开始。"));
        }
        ks::ui::ApplyStatusRole(m_statusLabel, ks::ui::StatusRole::Warning);
    }

    bool DesktopDrawingPage::nativeEventFilter(const QByteArray&, void* message, qintptr* result)
    {
        const auto* native = static_cast<const MSG*>(message);
        if (!native)
        {
            return false;
        }
        if (native->message == WM_HOTKEY && native->wParam == kStopHotkeyId && m_hotkeyRegistered)
        {
            stopDrawing();
            if (result)
            {
                *result = 0;
            }
            return true;
        }
        if (native->message == WM_DISPLAYCHANGE)
        {
            const bool wasRunning = m_timer->isActive();
            stopDrawing();
            if (wasRunning)
            {
                showFailure(desktop_drawing::DrawResult::DisplayChanged);
            }
        }
        return false;
    }
}
