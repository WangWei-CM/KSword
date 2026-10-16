#include "../Framework.h"
#include "WindowControlInspection.h"
#include "WindowControlInspectionInput.h"
#include "WindowControlInspectionOverlay.h"
#include "WindowListInteraction.h"
#include "../Internationalization/LanguageManager.h"
#include "../UI/FlatButtonTheme.h"
#include "../UI/ThemeAccentIcon.h"
#include "../UI/ToolbarMetrics.h"
#include "../theme.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <limits>

namespace ks::control_inspection
{
    namespace
    {
        QString T(const char* source)
        { return ks::i18n::sourceText(QString::fromUtf8(source)); }
        HWND native(QWidget* widget) { return reinterpret_cast<HWND>(widget->winId()); }

        // SetActionIcon：保留原始 SVG 轮廓，绘制时从真实按钮状态读取当前主题色。
        // resourcePath 只在动作含义变化时替换，语言或色板刷新不重复生成图标。
        void SetActionIcon(QToolButton* button, const QString& resourcePath)
        {
            if (button->property("ksword_control_action_icon_resource").toString() == resourcePath)
            {
                return;
            }
            button->setProperty("ksword_control_action_icon_resource", resourcePath);
            button->setProperty("ksword_theme_icon_managed", true);
            button->setIcon(ks::ui::MakeThemeButtonAccentIcon(QIcon(resourcePath), QColor(), button));
        }

        bool editableProperty(const Node& node, const QString& property)
        {
            if (node.properties.contains(QStringLiteral("ControlId"))
                && node.properties.contains(QStringLiteral("Style")))
                return property == QStringLiteral("Name") || property == QStringLiteral("IsEnabled");
            if (property == QStringLiteral("Value.Value"))
                return (node.properties.value(QStringLiteral("Value")) == QStringLiteral("true")
                    && node.properties.value(QStringLiteral("Value.IsReadOnly")) != QStringLiteral("true"))
                    || node.window != nullptr;
            return property == QStringLiteral("RangeValue.Value")
                ? node.properties.value(QStringLiteral("RangeValue")) == QStringLiteral("true")
                : property == QStringLiteral("Toggle.State")
                ? node.properties.value(QStringLiteral("Toggle")) == QStringLiteral("true")
                : property == QStringLiteral("ExpandCollapse.State")
                ? node.properties.value(QStringLiteral("ExpandCollapse")) == QStringLiteral("true")
                : property == QStringLiteral("SelectionItem.IsSelected")
                ? node.properties.value(QStringLiteral("SelectionItem")) == QStringLiteral("true")
                : (property == QStringLiteral("Scroll.HorizontalPercent")
                    || property == QStringLiteral("Scroll.VerticalPercent"))
                && node.properties.value(QStringLiteral("Scroll")) == QStringLiteral("true");
        }
        bool booleanText(const QString& value)
        {
            return value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
                || value.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0
                || (value.size() == 1 && (value.at(0).toLower() == QLatin1Char('t')
                    || value.at(0).toLower() == QLatin1Char('f')));
        }

        class PropertyDelegate final : public QStyledItemDelegate
        {
        public:
            explicit PropertyDelegate(QObject* parent) : QStyledItemDelegate(parent) {}
            QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                const QModelIndex& index) const override
            {
                Q_UNUSED(option);
                if (index.column() != 1) return nullptr;
                const QString key = index.data(Qt::UserRole + 1).toString();
                const QString current = index.data(Qt::EditRole).toString();
                if (key == QStringLiteral("Value.Value") && booleanText(current))
                {
                    auto* combo = new QComboBox(parent);
                    combo->addItems({QStringLiteral("false"), QStringLiteral("true")});
                    return combo;
                }
                if (key == QStringLiteral("Value.Value") || key == QStringLiteral("Name"))
                    return new QLineEdit(parent);
                if (key == QStringLiteral("IsEnabled") || key == QStringLiteral("SelectionItem.IsSelected"))
                {
                    auto* combo = new QComboBox(parent);
                    combo->addItems({QStringLiteral("false"), QStringLiteral("true")});
                    return combo;
                }
                if (key == QStringLiteral("Toggle.State"))
                {
                    auto* combo = new QComboBox(parent);
                    combo->addItems({QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("2")});
                    return combo;
                }
                if (key == QStringLiteral("ExpandCollapse.State"))
                {
                    auto* combo = new QComboBox(parent);
                    combo->addItems({QStringLiteral("0"), QStringLiteral("1")});
                    return combo;
                }
                if (key == QStringLiteral("RangeValue.Value")
                    || key == QStringLiteral("Scroll.HorizontalPercent")
                    || key == QStringLiteral("Scroll.VerticalPercent"))
                {
                    auto* spin = new QDoubleSpinBox(parent);
                    spin->setDecimals(6); spin->setSingleStep(1.0);
                    bool minOk = false, maxOk = false;
                    const double minimum = index.data(Qt::UserRole + 3).toDouble(&minOk);
                    const double maximum = index.data(Qt::UserRole + 4).toDouble(&maxOk);
                    if (minOk && maxOk && minimum <= maximum) spin->setRange(minimum, maximum);
                    else if (key != QStringLiteral("RangeValue.Value")) spin->setRange(0.0, 100.0);
                    else spin->setRange(-std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
                    return spin;
                }
                return nullptr;
            }
            void setEditorData(QWidget* editor, const QModelIndex& index) const override
            {
                const QString value = index.data(Qt::EditRole).toString();
                if (auto* line = qobject_cast<QLineEdit*>(editor)) line->setText(value);
                else if (auto* combo = qobject_cast<QComboBox*>(editor))
                { const int row = combo->findText(value); if (row >= 0) combo->setCurrentIndex(row); }
                else if (auto* spin = qobject_cast<QDoubleSpinBox*>(editor)) spin->setValue(value.toDouble());
            }
            void setModelData(QWidget* editor, QAbstractItemModel* model,
                const QModelIndex& index) const override
            {
                QString value;
                if (auto* line = qobject_cast<QLineEdit*>(editor)) value = line->text();
                else if (auto* combo = qobject_cast<QComboBox*>(editor)) value = combo->currentText();
                else if (auto* spin = qobject_cast<QDoubleSpinBox*>(editor)) value = QString::number(spin->value(), 'g', 15);
                model->setData(index, value, Qt::EditRole);
            }
        };

        class Page final : public QWidget
        {
        public:
            Page(HWND target, DWORD pid, DWORD tid, quint64 created, QWidget* parent)
                : QWidget(parent), m_original(target), m_pid(pid), m_tid(tid), m_created(created)
            {
                setObjectName(QStringLiteral("ks_control_inspection_page"));
                setMinimumSize(0, 0); setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
                auto* layout = new QVBoxLayout(this);
                auto* toolbar = new QHBoxLayout;
                // 三个操作统一使用实底 SVG 按钮，不再由字体渲染细小且不稳定的 Unicode 符号。
                m_pick = new QToolButton(this);
                m_pick->setObjectName(QStringLiteral("ks_control_pick"));
                m_pick->setCheckable(true);
                SetActionIcon(m_pick, QStringLiteral(":/Icon/window_picker_aim.svg"));
                m_pause = new QToolButton(this);
                m_pause->setObjectName(QStringLiteral("ks_control_pause"));
                m_pause->setCheckable(true);
                m_refresh = new QToolButton(this);
                m_refresh->setObjectName(QStringLiteral("ks_control_refresh"));
                SetActionIcon(m_refresh, QStringLiteral(":/Icon/process_refresh.svg"));
                m_scope = new QComboBox(this); m_scope->setObjectName(QStringLiteral("ks_control_scope"));
                m_scope->addItems({T("所属顶层窗口"), T("原 HWND 子树")});
                m_view = new QComboBox(this); m_view->setObjectName(QStringLiteral("ks_control_view"));
                m_view->addItems({T("UIA 控件树"), T("UIA 原始树"), T("Win32 子窗口树")});
                for (auto* button : {m_pick, m_pause, m_refresh})
                {
                    // button 是当前登记动作；共享主题保留 checked/disabled 和原业务连接。
                    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
                    button->setAutoRaise(false);
                    button->setCursor(Qt::PointingHandCursor);
                    ks::ui::ApplyFlatButtonTheme(button, ks::ui::FlatButtonTone::Neutral,
                        ks::ui::FlatButtonAppearance::Solid);
                }
                toolbar->addWidget(m_pick); toolbar->addWidget(m_pause); toolbar->addWidget(m_refresh);
                toolbar->addWidget(m_scope); toolbar->addWidget(m_view); toolbar->addStretch(); layout->addLayout(toolbar);
                // 首次登记前准备暂停图标，让整行几何将全部纯图标动作统一为至少 28px。
                SetActionIcon(m_pause, QStringLiteral(":/Icon/process_pause.svg"));
                ks::ui::NormalizeToolbarRow(toolbar);
                auto* options = new QHBoxLayout;
                m_all = new QCheckBox(T("全部框选"), this); m_all->setChecked(true);
                m_tips = new QCheckBox(T("悬浮提示"), this); m_tips->setChecked(true);
                options->addWidget(m_all); options->addWidget(m_tips); options->addStretch(); layout->addLayout(options);
                auto* splitter = new QSplitter(Qt::Horizontal, this); splitter->setChildrenCollapsible(false);
                m_tree = new QTreeWidget(splitter); m_tree->setObjectName(QStringLiteral("ks_control_tree"));
                m_tree->setColumnCount(2); m_tree->setHeaderLabels({T("控件"), T("名称")});
                m_tree->setUniformRowHeights(true); m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
                m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
                m_tree->header()->setStretchLastSection(true);
                m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
                auto* right = new QWidget(splitter); auto* rightLayout = new QVBoxLayout(right);
                rightLayout->setContentsMargins(0, 0, 0, 0);
                m_origin = new QLabel(T("当前控件属性"), right); m_origin->setObjectName(QStringLiteral("ks_control_property_origin"));
                rightLayout->addWidget(m_origin);
                m_properties = new QTreeWidget(right); m_properties->setObjectName(QStringLiteral("ks_control_properties"));
                m_properties->setColumnCount(2); m_properties->setHeaderLabels({T("属性"), T("值")});
                m_properties->setRootIsDecorated(false);
                m_properties->setEditTriggers(QAbstractItemView::DoubleClicked
                    | QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
                m_properties->setItemDelegate(new PropertyDelegate(m_properties));
                m_properties->setSelectionMode(QAbstractItemView::ExtendedSelection);
                m_properties->header()->setStretchLastSection(true); rightLayout->addWidget(m_properties);
                m_properties->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
                splitter->setSizes({450, 550}); layout->addWidget(splitter, 1);
                m_status = new QLabel(T("等待首次检查"), this); m_status->setWordWrap(true); layout->addWidget(m_status);
                m_detail = window(); m_detail->installEventFilter(this);
                m_timer.setInterval(40); connect(&m_timer, &QTimer::timeout, this, [this] { tick(); }); m_timer.start();
                m_clock.start();
                connect(m_pick, &QToolButton::clicked, this, [this] { setPick(m_pick->isChecked()); });
                connect(m_pause, &QToolButton::clicked, this, [this] { setPaused(m_pause->isChecked()); });
                connect(m_refresh, &QToolButton::clicked, this, [this] { scan(); });
                connect(m_scope, &QComboBox::currentIndexChanged, this, [this] { changeTarget(); });
                connect(m_view, &QComboBox::currentIndexChanged, this, [this] { changeTarget(); });
                connect(m_properties, &QTreeWidget::itemChanged, this,
                    [this](QTreeWidgetItem* item, int column) { propertyChanged(item, column); });
                connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
                    if (m_rebuilding || !item) return;
                    const auto found = m_nodes.constFind(item->data(0, Qt::UserRole).toString());
                    if (found == m_nodes.cend()) return;
                    m_selected = *found; m_preserve = m_selected.id; m_hover = {};
                    showProperties(m_selected, false); updateOverlay();
                    if (!m_paused && !m_suspended) m_collector.details(request(), m_selected.id, ++m_serial);
                });
                connect(m_all, &QCheckBox::toggled, this, [this] { updateOverlay(); });
                connect(m_tips, &QCheckBox::toggled, this, [this] { updateOverlay(); });
                retranslate();
            }
            ~Page() override
            { ReleaseInput(this); m_collector.cancel(); delete m_overlay.data(); if (s_active == this) s_active.clear(); }
        protected:
            void showEvent(QShowEvent* event) override
            { QWidget::showEvent(event); if (!m_started) { m_started = true; activate(); } }
            bool eventFilter(QObject* watched, QEvent* event) override
            {
                if (watched == m_detail && (event->type() == QEvent::Close || event->type() == QEvent::Hide))
                { suspend(); }
                return QWidget::eventFilter(watched, event);
            }
            void changeEvent(QEvent* event) override
            {
                QWidget::changeEvent(event);
                if (event->type() == QEvent::LanguageChange) retranslate();
                if (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::PaletteChange)
                    if (m_overlay) { m_overlay->setPalette(palette()); m_overlay->update(); }
            }
        private:
            void retranslate()
            {
                m_pick->setToolTip(T("选择控件（Ctrl+Shift+C）；Esc 取消拾取"));
                // 暂停/继续图标随现有检查状态切换，热主题颜色由图标引擎独立读取。
                SetActionIcon(m_pause, m_paused ? QStringLiteral(":/Icon/process_start.svg")
                    : QStringLiteral(":/Icon/process_pause.svg"));
                m_pause->setToolTip(m_paused ? T("继续控件检查") : T("暂停控件检查"));
                m_refresh->setToolTip(T("刷新控件树"));
                m_all->setText(T("全部框选")); m_tips->setText(T("悬浮提示"));
                m_scope->setItemText(0, T("所属顶层窗口")); m_scope->setItemText(1, T("原 HWND 子树"));
                m_view->setItemText(0, T("UIA 控件树")); m_view->setItemText(1, T("UIA 原始树"));
                m_view->setItemText(2, T("Win32 子窗口树"));
                m_tree->setHeaderLabels({T("控件"), T("名称")}); m_properties->setHeaderLabels({T("属性"), T("值")});
            }
            Request request() const
            {
                Request request; request.generation = m_generation; request.root = m_root;
                request.excluded = native(m_detail); request.view = static_cast<View>(m_view->currentIndex());
                ::GetWindowThreadProcessId(m_root, &request.pid); request.tid = ::GetWindowThreadProcessId(m_root, nullptr);
                return request;
            }
            void configureInput()
            {
                ConfigureInput(this, m_root, native(m_detail),
                    [this] { setPick(!m_pick->isChecked()); }, [this] { setPick(false); },
                    [this](QPoint point) {
                        setPick(false); m_pickWaiting = true;
                        m_collector.hit(request(), point, ++m_serial, true);
                    });
                if (!PickerShortcutAvailable()) m_status->setText(T("拾取快捷键不可用，可使用瞄准按钮。"));
            }
            void activate()
            {
                if (s_active && s_active != this) s_active->setPaused(true);
                s_active = this; m_paused = false; m_pause->setChecked(false);
                m_root = m_scope->currentIndex() ? m_original : ::GetAncestor(m_original, GA_ROOT);
                m_suspended = false; configureInput(); scan(); retranslate();
            }
            void setPaused(bool paused)
            {
                if (!m_started) return;
                m_paused = paused; m_pause->setChecked(paused);
                if (paused) { suspend(); m_status->setText(T("检查已暂停，可继续浏览控件树。")); }
                else activate();
                retranslate();
            }
            void suspend()
            {
                m_suspended = true; setPick(false); ReleaseInput(this);
                m_collector.cancel(); ++m_generation; ++m_serial; m_pickWaiting = false;
                m_mutationPending = false; m_properties->setEnabled(true);
                m_hover = {}; if (m_overlay) m_overlay->hide();
            }
            void setPick(bool armed)
            {
                if (m_paused || m_suspended || !m_started) armed = false;
                if (s_active != this) armed = false;
                if (s_active == this && !ArmPicker(armed))
                { armed = false; m_status->setText(T("无法开启控件拾取，目标输入未被拦截。")); }
                const QSignalBlocker blocker(m_pick); m_pick->setChecked(armed);
            }
            void changeTarget()
            {
                setPick(false); m_selected = {}; m_hover = {}; m_mutationPending = false;
                m_properties->setEnabled(true); m_properties->clear();
                m_nodes.clear(); m_nodesDirty = true; m_items.clear(); m_tree->clear(); m_collector.cancel(); ++m_generation;
                if (m_started && !m_paused) activate();
            }
            void scan()
            {
                if (!m_root || !::IsWindow(m_root)) return;
                setPick(false);
                m_expanded.clear();
                for (auto it = m_items.cbegin(); it != m_items.cend(); ++it)
                    if (it.value()->isExpanded()) m_expanded.insert(it.key());
                m_preserve = m_selected.id;
                m_seen.clear();
                ++m_generation; ++m_serial; m_pickWaiting = false; m_scanDone = false; m_scanError = S_OK;
                m_lastScan = m_clock.elapsed(); m_collector.scan(request());
                m_status->setText(T("正在扫描控件…")); updateOverlay();
            }
            void showProperties(const Node& node, bool hovered)
            {
                m_propertyNode = node; m_propertyHovered = hovered;
                m_origin->setText(hovered ? T("鼠标悬停：%1").arg(node.name) : T("树节点选择：%1").arg(node.name));
                const QSignalBlocker blocker(m_properties);
                m_properties->clear();
                auto properties = node.properties;
                properties.insert(T("信息来源"), m_view->currentIndex() == 2 ? QStringLiteral("Win32") : QStringLiteral("UIA"));
                if (node.window && !properties.contains(QStringLiteral("Value.Value")))
                    properties.insert(QStringLiteral("Value.Value"), node.name);
                properties.insert(T("边界（屏幕物理像素）"), QStringLiteral("%1, %2, %3, %4")
                    .arg(node.bounds.x()).arg(node.bounds.y()).arg(node.bounds.width()).arg(node.bounds.height()));
                for (auto it = properties.cbegin(); it != properties.cend(); ++it)
                {
                    auto* item = new QTreeWidgetItem(m_properties, {it.key(), it.value()});
                    item->setData(0, Qt::UserRole, node.id);
                    item->setData(0, Qt::UserRole + 1, it.key());
                    item->setData(0, Qt::UserRole + 2, it.value());
                    if (it.key() == QStringLiteral("RangeValue.Value"))
                    {
                        item->setData(0, Qt::UserRole + 3, node.properties.value(QStringLiteral("RangeValue.Minimum")));
                        item->setData(0, Qt::UserRole + 4, node.properties.value(QStringLiteral("RangeValue.Maximum")));
                    }
                    else if (editableProperty(node, it.key()))
                    {
                        item->setFlags(item->flags() | Qt::ItemIsEditable);
                        item->setToolTip(1, T("修改值"));
                        QFont valueFont = item->font(1); valueFont.setUnderline(true); item->setFont(1, valueFont);
                    }
                }
            }
            void propertyChanged(QTreeWidgetItem* item, int column)
            {
                if (!item || column != 1 || m_rebuilding || m_mutationPending) return;
                const QString property = item->data(0, Qt::UserRole + 1).toString();
                if (!editableProperty(m_propertyNode, property)) return;
                const QString value = item->text(1);
                const QString previous = item->data(0, Qt::UserRole + 2).toString();
                if (value == previous) return;
                const QString id = item->data(0, Qt::UserRole).toString();
                if (id.isEmpty()) return;
                m_mutationPending = true; m_properties->setEnabled(false);
                m_status->setText(T("正在处理，请稍候…"));
                m_collector.mutate(request(), id, property, value, ++m_serial);
            }
            void consume(Reply reply)
            {
                if (reply.generation != m_generation || m_suspended || m_paused) return;
                if (reply.mutation)
                {
                    m_mutationPending = false; m_properties->setEnabled(true);
                    if (FAILED(reply.error))
                    {
                        showProperties(m_propertyNode, m_propertyHovered);
                        m_status->setText(T("操作失败"));
                    }
                    else if (!reply.hit.id.isEmpty())
                    {
                        m_nodes[reply.hit.id] = reply.hit;
                        if (m_selected.id == reply.hit.id) m_selected = reply.hit;
                        if (m_hover.id == reply.hit.id) m_hover = reply.hit;
                        showProperties(reply.hit, m_propertyHovered);
                        m_status->setText(T("操作成功"));
                    }
                    else m_status->setText(T("操作成功"));
                    return;
                }
                if (reply.hover)
                {
                    if (!reply.picked && reply.serial != m_serial) return;
                    if (reply.picked) m_pickWaiting = false;
                    if (reply.hit.id.isEmpty())
                    {
                        if (!reply.fromTree) m_hover = {};
                        if (reply.picked) m_status->setText(T("未读取到该位置的控件，已恢复正常操作。"));
                        else if (FAILED(reply.error)) m_status->setText(T("属性读取失败（0x%1），保留最近可用信息。")
                            .arg(static_cast<quint32>(reply.error), 8, 16, QLatin1Char('0')));
                        return;
                    }
                    if (!reply.fromTree) m_hover = reply.hit;
                    showProperties(reply.hit, !reply.fromTree);
                    if (reply.picked)
                    {
                        m_selected = reply.hit;
                        auto* item = m_items.value(reply.hit.id);
                        if (item) { const QSignalBlocker blocker(m_tree);
                            for (auto* ancestor = item->parent(); ancestor; ancestor = ancestor->parent()) ancestor->setExpanded(true);
                            m_tree->setCurrentItem(item); m_tree->scrollToItem(item); showProperties(reply.hit, true); }
                        else { m_reveal = reply.hit.id; scan(); }
                    }
                    updateOverlay(); return;
                }
                if (FAILED(reply.error)) m_scanError = reply.error;
                m_rebuilding = true;
                for (const auto& node : reply.nodes)
                {
                    if (node.id.isEmpty()) continue;
                    auto* parent = m_items.value(node.parent);
                    auto* item = m_items.value(node.id);
                    if (!item) item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
                    else if (item->parent() != parent)
                    {
                        if (item->parent()) item->parent()->takeChild(item->parent()->indexOfChild(item));
                        else m_tree->takeTopLevelItem(m_tree->indexOfTopLevelItem(item));
                        if (parent) parent->addChild(item); else m_tree->addTopLevelItem(item);
                    }
                    item->setText(0, node.type); item->setText(1, node.name); item->setData(0, Qt::UserRole, node.id);
                    item->setToolTip(0, node.id); m_items.insert(node.id, item); m_nodes.insert(node.id, node);
                    m_seen.insert(node.id);
                    if (node.id == m_preserve) { m_tree->setCurrentItem(item); m_selected = node; }
                    if (m_expanded.contains(node.id) || node.parent.isEmpty()) item->setExpanded(true);
                    if (node.id == m_reveal)
                    {
                        for (auto* ancestor = item->parent(); ancestor; ancestor = ancestor->parent()) ancestor->setExpanded(true);
                        m_tree->setCurrentItem(item); m_tree->scrollToItem(item); m_reveal.clear();
                    }
                }
                m_rebuilding = false;
                if (reply.done)
                {
                    m_scanDone = true;
                    QSet<QString> removed;
                    for (auto it = m_items.cbegin(); it != m_items.cend(); ++it) if (!m_seen.contains(it.key())) removed.insert(it.key());
                    QVector<QTreeWidgetItem*> deletions;
                    for (const auto& id : removed)
                    {
                        auto* item = m_items.take(id); m_nodes.remove(id);
                        if (!item->parent() || !removed.contains(item->parent()->data(0, Qt::UserRole).toString())) deletions.push_back(item);
                    }
                    m_rebuilding = true; for (auto* item : deletions) delete item; m_rebuilding = false;
                    if (!m_selected.id.isEmpty() && !m_nodes.contains(m_selected.id)) m_selected = {};
                }
                QString status = T("目标：%1　已识别：%2　%3").arg(QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(m_root), 0, 16))
                    .arg(m_nodes.size()).arg(m_scanDone ? T("扫描完成") : T("正在扫描"));
                if (FAILED(m_scanError)) status += T("；部分信息不可用（0x%1）").arg(static_cast<quint32>(m_scanError), 8, 16, QLatin1Char('0'));
                if (m_scanDone && m_nodes.size() <= 1 && m_view->currentIndex() != 2)
                    status += T("；目标未暴露更多 UIA 控件，可切换 Win32 子窗口树。" );
                m_status->setText(status); m_nodesDirty = true; updateOverlay();
            }
            void updateOverlay()
            {
                if (m_paused || m_suspended || !m_started) return;
                if (!m_overlay) m_overlay = CreateOverlay(m_detail);
                if (m_nodesDirty) { m_sceneNodes = m_nodes.values(); m_nodesDirty = false; }
                m_overlay->setPalette(palette());
                if (!m_overlay->isVisible()) { m_overlay->show(); FitOverlay(m_overlay); }
                UpdateOverlay(m_overlay, m_sceneNodes, m_hover, m_selected,
                    m_all->isChecked(), m_tips->isChecked(), m_cursor);
            }
            void tick()
            {
                if (!m_started || m_paused || s_active != this) return;
                if (!ks::window::windowIdentityMatches(reinterpret_cast<quintptr>(m_original), m_pid, m_tid, m_created))
                { setPaused(true); m_status->setText(T("目标窗口已失效，检查已结束。")); m_pick->setEnabled(false); m_pause->setEnabled(false); return; }
                if (m_scope->currentIndex() == 0 && ::GetAncestor(m_original, GA_ROOT) != m_root)
                { changeTarget(); return; }
                const bool hidden = !m_detail->isVisible() || m_detail->isMinimized()
                    || !::IsWindowVisible(m_root) || ::IsIconic(m_root);
                if (hidden) { if (!m_suspended) suspend(); return; }
                if (m_suspended) { m_suspended = false; configureInput(); scan(); }
                for (auto& reply : m_collector.take()) consume(std::move(reply));
                const qint64 now = m_clock.elapsed();
                if (m_overlay && now - m_lastClip >= 120)
                { m_dirty = ClipOverlay(m_overlay, m_root, native(m_detail)) || m_dirty; m_lastClip = now; }
                const ks::window::PhysicalCoordinateScope dpi;
                POINT point{}; ::GetCursorPos(&point); const QPoint cursor(point.x, point.y);
                HWND hit = ::WindowFromPhysicalPoint(point);
                if (!belongs(hit, m_root, native(m_detail)))
                {
                    if (!m_hover.id.isEmpty()) { m_hover = {}; ++m_serial; }
                }
                else if (!m_mutationPending && !m_pickWaiting && (cursor != m_cursor || now - m_lastHover >= 600))
                { m_collector.hit(request(), cursor, ++m_serial, false); m_lastHover = now; }
                m_cursor = cursor;
                const bool dirty = m_collector.takeDirty();
                m_dirty = m_dirty || dirty;
                if (!m_mutationPending && m_scanDone && !m_pickWaiting && !m_pick->isChecked()
                    && now - m_lastScan >= 400 && (m_dirty || now - m_lastScan >= 2000))
                { m_dirty = false; scan(); }
                updateOverlay();
            }
            inline static QPointer<Page> s_active;
            HWND m_original = nullptr, m_root = nullptr;
            DWORD m_pid = 0, m_tid = 0;
            quint64 m_created = 0, m_generation = 0, m_serial = 0;
            QWidget* m_detail = nullptr;
            QToolButton *m_pick = nullptr, *m_pause = nullptr, *m_refresh = nullptr;
            QComboBox *m_scope = nullptr, *m_view = nullptr;
            QCheckBox *m_all = nullptr, *m_tips = nullptr;
            QTreeWidget *m_tree = nullptr, *m_properties = nullptr;
            QLabel *m_origin = nullptr, *m_status = nullptr;
            QPointer<QWidget> m_overlay;
            QVector<Node> m_sceneNodes;
            bool m_nodesDirty = true;
            Collector m_collector;
            QTimer m_timer;
            QElapsedTimer m_clock;
            qint64 m_lastScan = 0, m_lastClip = 0, m_lastHover = 0;
            bool m_started = false, m_paused = false, m_suspended = true, m_rebuilding = false,
                m_scanDone = false, m_pickWaiting = false, m_dirty = false;
            HRESULT m_scanError = S_OK;
            QMap<QString, Node> m_nodes;
            QMap<QString, QTreeWidgetItem*> m_items;
            QSet<QString> m_expanded, m_seen;
            QString m_preserve, m_reveal;
            Node m_hover, m_selected, m_propertyNode;
            bool m_propertyHovered = false, m_mutationPending = false;
            QPoint m_cursor;
        };
    }
    QWidget* CreatePage(HWND target, DWORD pid, DWORD tid, quint64 creationTime, QWidget* parent)
    { return new Page(target, pid, tid, creationTime, parent); }
}
