// ============================================================
// WorkbenchHexPane.Panels.cpp
// 作用：
// - 搭建界面（buildUi）：分割条 + 画布 + 解释器面板 + 查找条的布局，画布信号到
//   本类私有槛/公开信号的接线，查找条的数据/选区回调与命中高亮接线，Esc/F3
//   两个快捷键（照抄 HexView.Panels.cpp 的规则，见下方小节）。
// - openFind / closeFindBar：查找条显隐，与 Esc 快捷键的启用状态保持同步。
// - 右键菜单扩展点与编辑拒绝原因的转发（画布信号直接转发成本类同名信号，见
//   buildUi 里的 connect(canvas_, &HexCanvas::xxx, this, &WorkbenchHexPane::xxx)——
//   Qt 允许把一个信号接到另一个"槛"位置恰好是同签名的信号，效果就是纯转发，
//   不需要手写一个只做 emit 的槛函数）。
//
// ------------------------------------------------------------
// Esc / F3 规则（照抄 HexView.Panels.cpp，见该文件的文件头注释）
// ------------------------------------------------------------
// - F3 / Shift+F3：查找条没打开就先打开；打开后如果输入框是空的，停在"打开"
//   这一步，不往下找（让用户先输入）；否则按方向触发 findNext/findPrevious。
//   两个快捷键恒启用，不随查找条显隐切换。
// - Esc：只在查找条可见时启用（本类没有跳转条，不需要 HexView 那套"先关哪个"
//   的两条分支判断）；查找条不可见时 Esc 快捷键整体禁用，按键事件会落到画布
//   自己的 keyPressEvent（取消未完成的半字节编辑 / 折叠选区），这正是"为什么
//   要动态启用禁用，不能让它常驻"的原因——常驻会在查找条关闭时抢在画布之前
//   吞掉每一次 Escape 按键。
// - 本类头文件已冻结，不能新增一个"QShortcut* m_escapeShortcut"这样的私有
//   成员来记住这个快捷键对象；改用 objectName + findChild 在需要切换启用状态
//   的两个时点（openFind/closeFindBar）重新找到它，语义与"存一个成员指针"
//   完全等价，只是查找方式不同。
// ------------------------------------------------------------

#include "WorkbenchHexPane.h"

#include "../../theme.h"

#include <QApplication>
#include <QKeySequence>
#include <QResizeEvent>
#include <QShortcut>
#include <QShowEvent>
#include <QSplitter>
#include <QVBoxLayout>

namespace ks::ui
{
    namespace
    {
        // kFindHighlightLayer：查找命中的高亮层号，ux.md §4.1 定死"地址簿 10、
        // int3 30、查找 100 最上"，与 HexView.Panels.cpp 的 kFindHighlightLayer
        // 取同一个数值（两边各自独立画布实例，层号互不冲突，不需要共享常量）。
        constexpr int kFindHighlightLayer = 100;

        // kEscapeShortcutObjectName：Esc 快捷键的对象名，供 openFind/closeFindBar
        // 之后用 findChild 重新找到它并切换启用状态（见文件头说明：头文件冻结，
        // 不能为它新增一个私有成员指针）。
        const QString kEscapeShortcutObjectName = QStringLiteral("WorkbenchHexPaneEscapeShortcut");

        // RefreshEscapeShortcut：按查找条当前是否可见，重新设置 Esc 快捷键的
        // 启用状态——可见才启用，否则禁用（见文件头"为什么要动态启用禁用"）。
        // 传入：pane 本类实例（用于 findChild）；findBar 查找条。
        // 找不到快捷键对象（理论上不会发生，buildUi 恒会建好）时安静地什么都
        // 不做，不崩溃。
        void RefreshEscapeShortcut(QWidget* pane, HexFindBar* findBar)
        {
            if (pane == nullptr || findBar == nullptr)
            {
                return;
            }
            QShortcut* escapeShortcut = pane->findChild<QShortcut*>(kEscapeShortcutObjectName);
            if (escapeShortcut != nullptr)
            {
                escapeShortcut->setEnabled(findBar->isVisibleTo(pane));
            }
        }
    }

    // buildUi：构造函数体内调用一次，搭好全部子控件与接线。
    void WorkbenchHexPane::buildUi()
    {
        // ---- 布局：查找条在最上方（初始隐藏），下面是左右分割的画布/解释器。
        root_ = new QVBoxLayout(this);
        root_->setContentsMargins(0, 0, 0, 0);
        root_->setSpacing(0);

        findBar_ = new HexFindBar(this);
        findBar_->hide();
        root_->addWidget(findBar_);

        splitter_ = new QSplitter(Qt::Horizontal, this);
        splitter_->setChildrenCollapsible(false);

        canvas_ = new HexCanvas(splitter_);
        // 画布本身不拥有叠加层（HexCanvas::setOverlay 只接受非拥有指针，见
        // HexCanvas.h"一、模型分工"）；overlay_ 是本类的值成员，生命周期覆盖
        // 画布全程，这里一次性接好，不需要等到 setAddressSpace——没有它，画布的
        // stageBytes/ChangeKind 着色从构造起就形同只读查看器，这是"本类是
        // MemoryDiffOverlay 唯一一份的持有者"这条文件头承诺真正落地的地方。
        canvas_->setOverlay(&overlay_);
        splitter_->addWidget(canvas_);

        inspector_ = new HexInspectorPanel(splitter_);
        inspector_->setCanvas(canvas_);
        splitter_->addWidget(inspector_);

        // 画布占满剩余空间，解释器面板按内容固定宽度（与 HexView::ensureInspector
        // 的拉伸因子设置一致）。
        splitter_->setStretchFactor(0, 1);
        splitter_->setStretchFactor(1, 0);

        root_->addWidget(splitter_, 1);

        // ---- 分割条比例：splitterMoved 只在手柄被真正拖拽时发出（代码调用
        // setSizes 不会触发它），据此标记"用户已经手动摆过比例"，之后
        // showEvent/resizeEvent 驱动的 applyInitialSplitterSizes() 永久不再
        // 自动改写（见头文件增量④的注释与该函数实现处的详细算法）。
        connect(splitter_, &QSplitter::splitterMoved, this, &WorkbenchHexPane::onSplitterMoved);

        // ---- 画布信号接线（Direct，同线程，见装配接口文档 §3）----
        // visibleRangeChanged/caretMoved 驱动基线喂入器，定义在 WorkbenchHexPane.cpp。
        connect(canvas_, &HexCanvas::visibleRangeChanged, this, &WorkbenchHexPane::onCanvasVisibleRangeChanged);
        connect(canvas_, &HexCanvas::caretMoved, this, &WorkbenchHexPane::onCanvasCaretMoved);
        connect(canvas_, &HexCanvas::contentChanged, this, &WorkbenchHexPane::onCanvasContentChanged);

        // editRejected / contextMenuAboutToShow：参数签名与本类同名信号完全
        // 一致，直接信号转信号（Qt 允许 connect 的"槛"位置是另一个信号，效果
        // 就是纯转发，不需要再写一个只做 emit 的槛函数）。
        connect(canvas_, &HexCanvas::editRejected, this, &WorkbenchHexPane::editRejected);
        connect(canvas_, &HexCanvas::contextMenuAboutToShow, this, &WorkbenchHexPane::contextMenuAboutToShow);

        // rowWidthModeChanged：行宽或自适应/手动模式变化。手动选择（automatic 为假）时记下这个值作为
        // "用户上次手选的行宽"（保存设置时自适应状态不得覆盖它），再原样转发给装配层刷新菜单按钮。
        connect(canvas_, &HexCanvas::rowWidthModeChanged, this, [this](int bytesPerRow, bool automatic) {
            if (!automatic)
            {
                manualBytesPerRow_ = bytesPerRow;
            }
            emit rowWidthModeChanged(bytesPerRow, automatic);
        });

        // ---- 查找条：数据/选区回调 ----
        // 查找范围 = 基线窗口（ux.md §4.1："查找范围=已读窗口"），取 overlay_
        // 当前基线的 Materialize 结果（含暂存补丁的"所见值"，与画布显示一致；
        // 没有基线或基线长度为 0 时返回空数据，HexFindBar 自己会在结果文字里
        // 报"没有数据"一类的提示，不需要本类特殊处理）。
        findBar_->setSourceGetter([this]() {
            HexFindBar::SourceView view;
            if (overlay_.HasBaseline())
            {
                const std::uint64_t length = overlay_.BaselineSize();
                if (length > 0)
                {
                    const ksword::memwb::MaterializedBytes materialized =
                        overlay_.Materialize(overlay_.BaseAddress(), length);
                    if (materialized.ok)
                    {
                        view.data = QByteArray(
                            reinterpret_cast<const char*>(materialized.bytes.data()),
                            static_cast<int>(materialized.bytes.size()));
                        view.base = overlay_.BaseAddress();
                        // 不可读字节的占位 00 不是数据；搜索与高亮都需要同一份真实有效性快照。
                        view.validMask = QByteArray(
                            reinterpret_cast<const char*>(materialized.validMask.data()),
                            static_cast<qsizetype>(materialized.validMask.size()));
                    }
                }
            }
            return view;
        });
        findBar_->setRangeSourceGetter([this](std::uint64_t first, std::uint64_t last) {
            HexFindBar::SourceView view;
            if (!overlay_.HasBaseline() || !overlay_.BaselineSize() || first > last) return view;
            const auto base = overlay_.BaseAddress();
            const auto size = overlay_.BaselineSize();
            const auto baselineLast = size - 1 > UINT64_MAX - base ? UINT64_MAX : base + size - 1;
            first = std::max(first, base); last = std::min(last, baselineLast);
            if (first > last) return view;
            const auto materialized = overlay_.Materialize(first, last - first + 1);
            if (!materialized.ok) return view;
            view.base = first;
            view.data = QByteArray(reinterpret_cast<const char*>(materialized.bytes.data()),
                static_cast<qsizetype>(materialized.bytes.size()));
            view.validMask = QByteArray(reinterpret_cast<const char*>(materialized.validMask.data()),
                static_cast<qsizetype>(materialized.validMask.size()));
            return view;
        });
        findBar_->setSelectionGetter([this](std::uint64_t* firstOut, std::uint64_t* lastOut) {
            const std::optional<HexCanvas::AddressRange> range = canvas_->selectedRange();
            if (!range.has_value())
            {
                return false;
            }
            *firstOut = range->first;
            *lastOut = range->last;
            return true;
        });

        // 命中：选中命中范围（插入点在起点、锚点在末字节——与
        // HexView::onFindMatch 同一手法），不在视口内才居中滚动。
        connect(findBar_, &HexFindBar::matchFound, this, [this](quint64 first, quint64 length, bool wrapped) {
            Q_UNUSED(wrapped);
            if (length == 0)
            {
                return;
            }
            const std::uint64_t last = first + (length - 1ULL);
            canvas_->setCaretAddress(last, false, false);
            canvas_->setCaretAddress(first, true, false);
            canvas_->scrollToAddress(first, HexCanvas::ScrollAlign::Nearest);
        });

        // 可见高亮：交给画布的查找高亮层，颜色每次现取（主题切换后画布自己会
        // 在下一次 paintEvent 重新取色，本类不需要额外监听 PaletteChange）。
        connect(findBar_, &HexFindBar::highlightsChanged, this,
            [this](const std::vector<HexFindBar::AddressRange>& ranges) {
                if (ranges.empty())
                {
                    canvas_->clearHighlightRanges(kFindHighlightLayer);
                    return;
                }
                canvas_->setHighlightRanges(
                    kFindHighlightLayer,
                    ranges,
                    KswordTheme::AccentColor(KswordTheme::AccentRole::Yellow),
                    QStringLiteral("查找命中"));
            });

        connect(findBar_, &HexFindBar::closeRequested, this, [this]() { closeFindBar(); });

        // ---- 快捷键：Esc（默认禁用，查找条打开时才启用）与 F3/Shift+F3 ----
        QShortcut* escapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        escapeShortcut->setObjectName(kEscapeShortcutObjectName);
        escapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        escapeShortcut->setEnabled(false);
        connect(escapeShortcut, &QShortcut::activated, this, [this]() { closeFindBar(); });

        // triggerFind：F3（forward=true）/ Shift+F3（forward=false）共用的逻辑，
        // 照抄 HexView::onFindNextShortcut：没打开先打开，输入框为空就停在
        // "打开"这一步（让用户先输入），否则按方向触发查找。
        const auto triggerFind = [this](bool forward) {
            if (!findBar_->isVisibleTo(this))
            {
                openFind();
                if (findBar_->patternText().isEmpty())
                {
                    return;
                }
            }
            if (forward)
            {
                findBar_->findNext();
            }
            else
            {
                findBar_->findPrevious();
            }
        };

        QShortcut* nextShortcut = new QShortcut(QKeySequence(Qt::Key_F3), this);
        nextShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(nextShortcut, &QShortcut::activated, this, [triggerFind]() { triggerFind(true); });

        QShortcut* previousShortcut = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3), this);
        previousShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(previousShortcut, &QShortcut::activated, this, [triggerFind]() { triggerFind(false); });
    }

    // openFind：显示查找条并让 Esc 生效。
    void WorkbenchHexPane::openFind()
    {
        findBar_->open();
        RefreshEscapeShortcut(this, findBar_);
    }

    // closeFindBar：取消在途搜索、清高亮、隐藏；焦点原先在查找条内就还给画布
    // （查找条隐藏后 Qt 会把焦点丢给别处，不一定落回画布，需要显式要回来，
    // 与 HexView::closeFindBar 同一手法）。
    void WorkbenchHexPane::closeFindBar()
    {
        QWidget* focus = QApplication::focusWidget();
        const bool hadFocus = (focus != nullptr) && findBar_->isAncestorOf(focus);
        findBar_->deactivate();
        findBar_->hide();
        RefreshEscapeShortcut(this, findBar_);
        if (hadFocus && canvas_ != nullptr)
        {
            canvas_->setFocus(Qt::OtherFocusReason);
        }
    }

    // ------------------------------------------------------------
    // 分割条初始比例（Wave 3 布局缺陷修复，任务书增量①）
    // ------------------------------------------------------------
    // 根因（见任务书原文）：本类构造时只给画布设了拉伸因子 1、解释器面板设了
    // 拉伸因子 0，从未显式调用过 splitter_->setSizes(...)——HexView.Panels.cpp
    // 的 applyInspectorVisible 对同一问题有现成解法（显示解释器面板时按
    // "min(面板 sizeHint 宽, 分割器宽 45%)"显式摆一次宽度），本类当初漏抄了
    // 这一步。Qt 的 QSplitter 在"从未 setSizes 过"时，首次布局会按各子控件
    // 自己的 sizeHint 分配空间——解释器面板的 sizeHint 会随其内容（已解码的
    // 数值行）变宽，在真实数据下可以轻松超过画布的 sizeHint，于是画布反而被
    // 挤到很窄（900px 宽窗口里画布只剩约 270px，解释器占约 620px，比例彻底
    // 反了）。

    // applyInitialSplitterSizes：按公式"解释器宽度=clamp(面板 sizeHint 宽,
    // 240, 分割器宽×36%)，画布占其余"重新摆一次分割条宽度；窄窗口
    // （分割器宽<600）时额外保证画布不被压成一条缝。splitterSizesUserAdjusted_
    // 为真（用户已经手动拖过分割条）时整体跳过，不再覆盖用户自己选的比例。
    void WorkbenchHexPane::applyInitialSplitterSizes()
    {
        if (splitterSizesUserAdjusted_)
        {
            return;
        }
        if (splitter_ == nullptr || inspector_ == nullptr || canvas_ == nullptr)
        {
            // 防御：三者均在 buildUi 里一次性建好，正常运行不会走到这里。
            return;
        }

        // 分割条自身宽度是第一选择；showEvent 可能在布局引擎真正把分割条的
        // 宽度同步成最终值之前就先触发一次（取决于 Qt 内部 polish 的时序），
        // 这种情况下退回本控件（splitter 的直接父级、root_ 的外边距为 0，
        // 两者理应同宽）自己的 width() 兜底——resize() 会立即更新这个值，
        // 不需要等布局真正跑完。
        int totalWidth = splitter_->width();
        if (totalWidth <= 0)
        {
            totalWidth = width();
        }
        if (totalWidth <= 0)
        {
            // 两边都还是 0：这一刻确实没有任何可用的宽度信息（典型：窗口
            // 还从未被 resize 过），什么都不做，等下一次 showEvent/resizeEvent
            // 真正带着非零宽度到来时再算，不用定时器去猜这一刻何时发生。
            return;
        }

        // ---- 第一步：按公式求解释器宽度的"一般情况"初值 ----
        // 下界恒为 240（文档数值，也与 HexInspectorPanel::minimumSizeHint 的
        // 260px 留有余量）；上界是分割条宽度的 36%。当分割条本身很窄
        // （<667px 左右）时 36% 会小于 240，出现"上界比下界还小"的颠倒区间——
        // 这里先把上界抬到下界，让后面的 clamp 运算保持良定义，真正的"别把
        // 画布压扁"收紧留给第二步的窄窗口分支去做。
        const int lowerBoundWidth = 240;
        int upperBoundWidth = totalWidth * 36 / 100;
        if (upperBoundWidth < lowerBoundWidth)
        {
            upperBoundWidth = lowerBoundWidth;
        }

        int inspectorWidth = inspector_->sizeHint().width();
        if (inspectorWidth < lowerBoundWidth)
        {
            inspectorWidth = lowerBoundWidth;
        }
        if (inspectorWidth > upperBoundWidth)
        {
            inspectorWidth = upperBoundWidth;
        }

        // ---- 关键修正：把面板自身的硬性最小宽度也算进来 ----
        // QSplitter::setSizes 不是"你给多少它就用多少"：任何子控件都不会被
        // 压到它自己 minimumSizeHint() 以下（HexInspectorPanel::
        // minimumSizeHint 固定约 260px，见该类注释），哪怕我们这里传的
        // inspectorWidth 更小，Qt 也会在内部把它拉回这个硬下限、从画布那边
        // 补差——如果本函数不预先把这条硬下限算进来，下面第二步"窄窗口画布
        // 下限保护"算出的 canvasWidth 就是一个 Qt 实际上根本不会采用的虚假
        // 值，保护逻辑名存实亡（本包夹具的窄窗口断言第一次跑就实测抓到了
        // 这个缺口：canvasWidth 预期 284 其实只拿到 156）。
        const int inspectorHardMinWidth = inspector_->minimumSizeHint().width();
        if (inspectorWidth < inspectorHardMinWidth)
        {
            inspectorWidth = inspectorHardMinWidth;
        }

        int canvasWidth = totalWidth - inspectorWidth;

        // ---- 第二步：窄窗口（分割器宽<600）下的画布下限保护 ----
        // "一行所需最小宽度"取画布当前 sizeHint 的宽度——HexCanvas::sizeHint
        // 按当前每行字节数给出"完整显示一整行十六进制+ASCII、不需要横向滚动"
        // 的建议宽度（见 HexCanvas.h 对 sizeHint 的说明）。画布本身支持横向
        // 滚动，所以不要求整行都露出来，只要求不小于半行——半行仍然是"一条
        // 缝"与"看得清内容"的合理分界线，恰好对应任务书原文的措辞。
        if (totalWidth < 600)
        {
            const int oneRowWidth = canvas_->sizeHint().width();
            const int minCanvasWidth = oneRowWidth / 2;
            if (canvasWidth < minCanvasWidth)
            {
                // 画布能拿到的空间最多到"总宽度减去解释器的硬性最小宽度"——
                // 这是 inspectorHardMinWidth 本身带来的物理上限，本类无法
                // （也无权）突破 HexInspectorPanel 自己声明的最小可用宽度。
                // 窗口本身窄到两边硬性需求之和都超过总宽度时（典型：420px
                // 窗口配一个 260px 硬下限的解释器面板，半行还要 280+px），
                // 只能退而求其次，把"尽力而为"之后仍然剩下的空间全部给画布，
                // 不强行索取一个 Qt 根本不会批准的数字。
                const int bestEffortCanvasWidth = totalWidth - inspectorHardMinWidth;
                canvasWidth = (bestEffortCanvasWidth < minCanvasWidth) ? bestEffortCanvasWidth : minCanvasWidth;
                if (canvasWidth < 0)
                {
                    canvasWidth = 0;
                }
                inspectorWidth = totalWidth - canvasWidth;
            }
        }

        // 两个分量都不允许低于 1（QSplitter::setSizes 接受 0，但 0 宽度的
        // 子控件会让该面板彻底不可见，不是本函数任何一条分支想要的结果；
        // 实际运行中上面两步的推导不会让它们变成 0 或负数，这里只是最后的
        // 安全兜底）。
        if (canvasWidth < 1)
        {
            canvasWidth = 1;
        }
        if (inspectorWidth < 1)
        {
            inspectorWidth = 1;
        }

        splitter_->setSizes(QList<int>{ canvasWidth, inspectorWidth });
    }

    // onSplitterMoved：用户真的拖动了分割条手柄——见本函数声明处与
    // applyInitialSplitterSizes() 文件头的说明，之后彻底放弃自动摆放。
    void WorkbenchHexPane::onSplitterMoved(int pos, int index)
    {
        Q_UNUSED(pos);
        Q_UNUSED(index);
        splitterSizesUserAdjusted_ = true;
    }

    // minimumSizeHint：见头文件声明处的详细说明——故意返回一个很小的固定值，
    // 不让 root_ 布局（splitter_ 两个面板各自最小宽度之和）向上传播成宿主
    // 顶层窗口的硬性最小尺寸。返回 QSize(1,1) 而不是 QSize(0,0)：0 在某些
    // Qt 内部路径会被当成"这条维度没有限制"的特殊值处理，1 则是一个确切、
    // 恒为正的极小值，语义更明确（"几乎不限制，但不是无限制"）。
    QSize WorkbenchHexPane::minimumSizeHint() const
    {
        return QSize(1, 1);
    }

    // showEvent：首次显示（以及之后每一次重新显示）时，分割条此刻已经有了
    // 真实的布局宽度，按公式重新摆一次比例（用户已手动调整过则整体跳过，见
    // applyInitialSplitterSizes 内部判断）。
    void WorkbenchHexPane::showEvent(QShowEvent* event)
    {
        QWidget::showEvent(event);
        applyInitialSplitterSizes();
    }

    // resizeEvent：窗口大小变化时同样重新摆一次——否则"先在宽窗口里摆好比例，
    // 再把窗口拖窄"这条路径会让画布继续停留在宽窗口时分到的绝对像素宽度，
    // 窄到一定程度就会被压成一条缝，这正是任务书要求的"窄窗口下限保护"必须
    // 在 resize 时同样生效的原因。
    void WorkbenchHexPane::resizeEvent(QResizeEvent* event)
    {
        QWidget::resizeEvent(event);
        applyInitialSplitterSizes();
    }
}
