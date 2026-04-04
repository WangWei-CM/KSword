#pragma once

// ============================================================
// HexEditorWidget.h
// 作用：
// - 提供统一的十六进制查看/编辑控件；
// - 支持查找、跳转、复制、导出等常用工具；
// - 后续所有“显示字节数据”的界面都应复用该组件。
// ============================================================

#include <QByteArray>
#include <QWidget>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

class QObject;
class QEvent;
class QPoint;
class QComboBox;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QShortcut;
class QTableWidget;
class QTableWidgetItem;
class QToolButton;
class QWidget;
class QVBoxLayout;

// HexEditorWidget：
// - 统一十六进制查看器；
// - 支持只读和可编辑两种模式；
// - 支持 Ctrl+F 异步查找、Ctrl+G 跳转。
class HexEditorWidget final : public QWidget
{
    Q_OBJECT

public:
    // SearchMode：
    // - 定义查找模式；
    // - HexBytes 支持“AA BB ??”格式。
    enum class SearchMode : int
    {
        HexBytes = 0,
        AsciiText,
        Utf16Text
    };

    // 构造函数：
    // - parent：Qt 父控件，可空。
    explicit HexEditorWidget(QWidget* parent = nullptr);

    // 析构函数：
    // - 组件析构时仅释放 Qt 子控件；
    // - 异步查找线程采用 ticket 机制自动丢弃过期结果。
    ~HexEditorWidget() override;

protected:
    // eventFilter：
    // - 作用：拦截十六进制表格视口鼠标事件；
    // - 用于实现“文本式线性拖拽选择”，避免默认矩形框选。
    // 调用方式：Qt 事件系统自动回调。
    // 入参 watched：被监听对象（主要是 m_hexTable->viewport()）。
    // 入参 event：事件对象。
    // 返回：true 表示事件已处理，false 交给基类继续处理。
    bool eventFilter(QObject* watched, QEvent* event) override;

public:

    // setRegionData：
    // - 作用：按“内存指针 + 长度”设置显示数据；
    // - regionPointer：输入内存区域指针，可为 nullptr；
    // - regionSize：输入内存区域长度（字节）；
    // - baseAddress：该区域在逻辑上的起始地址。
    void setRegionData(
        const void* regionPointer,
        std::size_t regionSize,
        std::uint64_t baseAddress = 0);

    // setByteArray：
    // - 作用：按 QByteArray 直接设置显示数据；
    // - bytes：输入字节数据；
    // - baseAddress：逻辑起始地址。
    void setByteArray(const QByteArray& bytes, std::uint64_t baseAddress = 0);

    // clearData：
    // - 作用：清空组件中的字节数据与查找高亮。
    void clearData();

    // setEditable：
    // - 作用：设置是否允许编辑十六进制单元格；
    // - editable：true=可编辑，false=只读。
    void setEditable(bool editable);

    // isEditable：
    // - 返回当前是否可编辑。
    bool isEditable() const;

    // setBytesPerRow：
    // - 作用：设置每行显示字节数（建议 8/16/32）；
    // - bytesPerRow：每行字节数，非法值会被夹取。
    void setBytesPerRow(int bytesPerRow);

    // bytesPerRow：
    // - 返回当前每行字节数。
    int bytesPerRow() const;

    // jumpToAbsoluteAddress：
    // - 作用：跳转到绝对地址；
    // - absoluteAddress：目标地址；
    // - 返回：true=成功定位，false=超范围。
    bool jumpToAbsoluteAddress(std::uint64_t absoluteAddress);

    // jumpToOffset：
    // - 作用：按“相对偏移”定位；
    // - offset：从 baseAddress 起算的字节偏移；
    // - 返回：true=成功定位，false=超范围。
    bool jumpToOffset(std::uint64_t offset);

    // jumpToRow：
    // - 作用：跳转到目标行；
    // - rowIndex：0 基行号；
    // - 返回：true=成功定位，false=超范围。
    bool jumpToRow(std::uint64_t rowIndex);

    // openFindPanel：
    // - 作用：显示查找面板并聚焦输入框。
    void openFindPanel();

    // openJumpPanel：
    // - 作用：显示跳转面板并聚焦输入框。
    void openJumpPanel();

    // setByteAtAbsoluteAddress：
    // - 作用：外部主动修改指定地址字节（例如写入失败时回滚）；
    // - absoluteAddress：绝对地址；
    // - byteValue：目标字节值；
    // - keepSelection：是否保留当前选择焦点；
    // - 返回：true=修改成功，false=地址越界。
    bool setByteAtAbsoluteAddress(
        std::uint64_t absoluteAddress,
        std::uint8_t byteValue,
        bool keepSelection = true);

    // data：
    // - 返回当前组件持有的数据副本。
    QByteArray data() const;

    // regionSize：
    // - 返回当前数据长度（字节）。
    std::size_t regionSize() const;

    // baseAddress：
    // - 返回当前逻辑基址。
    std::uint64_t baseAddress() const;

    // selectedAbsoluteAddress：
    // - 返回当前选中单元格对应的绝对地址；
    // - 未选中有效字节时返回 baseAddress。
    std::uint64_t selectedAbsoluteAddress() const;

    // selectedOffset：
    // - 返回当前选中单元格的相对偏移；
    // - 未选中有效字节时返回 0。
    std::uint64_t selectedOffset() const;

signals:
    // byteEdited：
    // - 作用：用户编辑字节后触发；
    // - absoluteAddress：被修改字节的绝对地址；
    // - oldValue：修改前字节；
    // - newValue：修改后字节。
    void byteEdited(
        std::uint64_t absoluteAddress,
        std::uint8_t oldValue,
        std::uint8_t newValue);

    // currentAddressChanged：
    // - 作用：用户切换选中单元格时触发。
    void currentAddressChanged(std::uint64_t absoluteAddress);

    // aboutToShowContextMenu：
    // - 作用：在弹出右键菜单前允许外部追加动作；
    // - menu：即将显示的菜单对象；
    // - absoluteAddress：当前菜单目标地址；
    // - hasByte：true 表示目标地址有有效字节。
    void aboutToShowContextMenu(
        QMenu* menu,
        std::uint64_t absoluteAddress,
        bool hasByte);

private:
    // SearchPattern：
    // - 作用：描述查找模式解析后的字节模板。
    struct SearchPattern
    {
        QByteArray patternBytes;
        QByteArray maskBytes;
        QString normalizeText;
    };

    // NavigateDirection：
    // - 作用：记录“异步查找完成后应如何自动定位”。
    enum class NavigateDirection : int
    {
        None = 0,
        Next,
        Previous
    };

private:
    // initializeUi：
    // - 作用：创建主布局、工具栏、查找面板、跳转面板和表格。
    void initializeUi();

    // initializeConnections：
    // - 作用：连接编辑、右键、快捷键和工具按钮。
    void initializeConnections();

    // rebuildTable：
    // - 作用：按 m_buffer 当前内容重建整个十六进制表格。
    void rebuildTable();

    // updateHeaderText：
    // - 作用：重建表头（地址 + 字节列 + ASCII）。
    void updateHeaderText();

    // updateSummaryLabel：
    // - 作用：刷新顶部摘要文本（长度、基址、模式）。
    void updateSummaryLabel();

    // updateStatusLabel：
    // - 作用：刷新底部状态文本。
    void updateStatusLabel(const QString& statusText);

    // initializeSelectionInspector：
    // - 作用：创建底部“选区检查器”面板；
    // - 展示起止地址、HEX/ASCII/UTF-16 和整数解释。
    void initializeSelectionInspector();

    // updateSelectionInspector：
    // - 作用：按当前选区刷新检查器文本；
    // - 无选区时回退显示当前字节或占位内容。
    void updateSelectionInspector();

    // buildSelectedByteArray：
    // - 作用：把当前选区转换为连续展示用字节数组；
    // - 按偏移升序输出，供各类预览格式复用。
    QByteArray buildSelectedByteArray() const;

    // formatSelectionHexPreview：
    // - 作用：把选中字节格式化为 HEX 预览文本；
    // - 过长时自动截断并标注省略。
    QString formatSelectionHexPreview(const QByteArray& selectedBytes) const;

    // formatSelectionAsciiPreview：
    // - 作用：把选中字节格式化为 ASCII 预览文本；
    // - 不可打印字符统一显示为 '.'。
    QString formatSelectionAsciiPreview(const QByteArray& selectedBytes) const;

    // formatSelectionUtf16Preview：
    // - 作用：按 UTF-16LE 尝试解码选区内容；
    // - 长度不足两个字节时显示不可用提示。
    QString formatSelectionUtf16Preview(const QByteArray& selectedBytes) const;

    // formatSelectionIntegerPreview：
    // - 作用：按首 1/2/4/8 字节解释整数与浮点；
    // - 便于快速判断选区像不像数值字段。
    QString formatSelectionIntegerPreview(const QByteArray& selectedBytes) const;

    // updateAsciiCellByRow：
    // - 作用：更新指定行的 ASCII 列显示。
    void updateAsciiCellByRow(int rowIndex);

    // updateRowHighlightByRow：
    // - 作用：根据查找命中掩码刷新指定行背景色。
    void updateRowHighlightByRow(int rowIndex);

    // updateSelectionHighlightRange：
    // - 作用：按“旧选区 + 新选区”的并集刷新受影响行；
    // - 避免每次拖拽都重建整张表。
    void updateSelectionHighlightRange(
        bool oldRangeValid,
        std::uint64_t oldStartOffset,
        std::uint64_t oldEndOffset,
        bool newRangeValid,
        std::uint64_t newStartOffset,
        std::uint64_t newEndOffset);

    // parseAddressNumber：
    // - 作用：解析十进制/十六进制数字文本。
    bool parseAddressNumber(const QString& text, std::uint64_t& valueOut) const;

    // parseSearchPattern：
    // - 作用：按查找模式把输入文本解析为可匹配字节模板。
    bool parseSearchPattern(
        SearchMode mode,
        const QString& text,
        SearchPattern& patternOut,
        QString& errorTextOut) const;

    // parseHexPattern：
    // - 作用：解析 HEX 模式输入（支持 ?? 通配）。
    bool parseHexPattern(
        const QString& text,
        SearchPattern& patternOut,
        QString& errorTextOut) const;

    // parseAsciiPattern：
    // - 作用：解析 ASCII/UTF-16 模式输入。
    bool parseAsciiPattern(
        SearchMode mode,
        const QString& text,
        SearchPattern& patternOut,
        QString& errorTextOut) const;

    // ensureSearchResultReady：
    // - 作用：确保当前输入对应的查找结果可用；
    // - direction：用户请求的导航方向；
    // - startOffset：导航起点偏移；
    // - 返回：true=结果可直接使用；false=已启动异步查找。
    bool ensureSearchResultReady(
        NavigateDirection direction,
        std::uint64_t startOffset);

    // startSearchAsync：
    // - 作用：异步扫描当前缓冲区，避免阻塞 UI。
    void startSearchAsync(
        const SearchPattern& pattern,
        SearchMode mode,
        const QString& searchText,
        NavigateDirection direction,
        std::uint64_t startOffset);

    // applySearchResult：
    // - 作用：应用异步查找结果并执行待处理导航。
    void applySearchResult(
        std::uint64_t ticket,
        const std::vector<std::uint64_t>& matchOffsets,
        int matchLength,
        SearchMode mode,
        const QString& searchText,
        const QString& normalizedText,
        std::uint64_t dataRevision);

    // clearSearchState：
    // - 作用：清空查找缓存与高亮。
    void clearSearchState();

    // gotoMatchByDirection：
    // - 作用：按方向跳转到上一个/下一个命中项。
    void gotoMatchByDirection(
        NavigateDirection direction,
        std::uint64_t startOffset);

    // setCurrentMatchIndex：
    // - 作用：设置当前命中索引并滚动/选中。
    void setCurrentMatchIndex(int matchIndex);

    // selectRangeByOffset：
    // - 作用：按偏移范围选中单元格。
    void selectRangeByOffset(
        std::uint64_t offset,
        int length,
        bool scrollToCenter);

    // selectLinearRange：
    // - 作用：按“文本式线性区间”选中字节；
    // - 选区起点和终点都包含，跨行时保持连续字节语义。
    void selectLinearRange(
        std::uint64_t anchorOffset,
        std::uint64_t currentOffset,
        bool scrollToCurrent);

    // rowColumnToOffset：
    // - 作用：把“行列”转换为字节偏移；
    // - 返回 true 表示该单元格对应有效字节。
    bool rowColumnToOffset(
        int row,
        int column,
        std::uint64_t& offsetOut) const;

    // offsetToRowColumn：
    // - 作用：把字节偏移转换为“行列”。
    bool offsetToRowColumn(
        std::uint64_t offset,
        int& rowOut,
        int& columnOut) const;

    // viewportPosToOffset：
    // - 作用：把视口坐标映射为字节偏移；
    // - 支持越界坐标夹取，便于拖拽到边缘时保持连续选择。
    bool viewportPosToOffset(
        const QPoint& viewportPos,
        std::uint64_t& offsetOut) const;

    // collectSelectedOffsets：
    // - 作用：收集当前选中的全部字节偏移。
    std::vector<std::uint64_t> collectSelectedOffsets() const;

    // copySelectedAsHex：
    // - 作用：复制选中字节为十六进制文本。
    void copySelectedAsHex();

    // copySelectedAsAscii：
    // - 作用：复制选中字节为 ASCII 文本。
    void copySelectedAsAscii();

    // copyCurrentAddress：
    // - 作用：复制当前单元格地址。
    void copyCurrentAddress();

    // copyCurrentRowDump：
    // - 作用：复制当前行“地址 + HEX + ASCII”文本。
    void copyCurrentRowDump();

    // exportBinaryFile：
    // - 作用：导出当前缓冲区为二进制文件。
    void exportBinaryFile();

    // exportHexTextFile：
    // - 作用：导出当前缓冲区为十六进制文本文件。
    void exportHexTextFile();

    // buildRowDumpText：
    // - 作用：构造指定行的转储文本。
    QString buildRowDumpText(int rowIndex) const;

    // buildFullDumpText：
    // - 作用：构造全部数据的转储文本。
    QString buildFullDumpText() const;

private:
    // m_rootLayout：根布局。
    QVBoxLayout* m_rootLayout = nullptr;

    // m_toolbarLayout：顶部工具区布局。
    QHBoxLayout* m_toolbarLayout = nullptr;

    // m_summaryLabel：显示当前数据长度、基址、行宽。
    QLabel* m_summaryLabel = nullptr;

    // m_bytesPerRowCombo：每行字节数选择框。
    QComboBox* m_bytesPerRowCombo = nullptr;

    // m_findButton：打开查找面板按钮。
    QToolButton* m_findButton = nullptr;

    // m_jumpButton：打开跳转面板按钮。
    QToolButton* m_jumpButton = nullptr;

    // m_exportButton：导出按钮。
    QToolButton* m_exportButton = nullptr;

    // m_findPanel：查找面板容器。
    QWidget* m_findPanel = nullptr;

    // m_findLayout：查找面板布局。
    QHBoxLayout* m_findLayout = nullptr;

    // m_findModeCombo：查找模式下拉框。
    QComboBox* m_findModeCombo = nullptr;

    // m_findEdit：查找输入框。
    QLineEdit* m_findEdit = nullptr;

    // m_findPrevButton：查找上一个按钮。
    QToolButton* m_findPrevButton = nullptr;

    // m_findNextButton：查找下一个按钮。
    QToolButton* m_findNextButton = nullptr;

    // m_findCloseButton：关闭查找面板按钮。
    QToolButton* m_findCloseButton = nullptr;

    // m_findResultLabel：显示查找结果统计。
    QLabel* m_findResultLabel = nullptr;

    // m_jumpPanel：跳转面板容器。
    QWidget* m_jumpPanel = nullptr;

    // m_jumpLayout：跳转面板布局。
    QHBoxLayout* m_jumpLayout = nullptr;

    // m_jumpModeCombo：跳转模式（绝对地址/偏移/行号）。
    QComboBox* m_jumpModeCombo = nullptr;

    // m_jumpEdit：跳转输入框。
    QLineEdit* m_jumpEdit = nullptr;

    // m_jumpApplyButton：执行跳转按钮。
    QToolButton* m_jumpApplyButton = nullptr;

    // m_jumpCloseButton：关闭跳转面板按钮。
    QToolButton* m_jumpCloseButton = nullptr;

    // m_hexTable：十六进制数据表格。
    QTableWidget* m_hexTable = nullptr;

    // m_selectionInspectorPanel：底部选区检查器容器。
    QWidget* m_selectionInspectorPanel = nullptr;

    // m_selectionInspectorLayout：选区检查器布局。
    QGridLayout* m_selectionInspectorLayout = nullptr;

    // m_selectionSummaryLabel：显示选区起止地址、长度等摘要。
    QLabel* m_selectionSummaryLabel = nullptr;

    // m_selectionHexPreviewLabel：显示 HEX 预览文本。
    QLabel* m_selectionHexPreviewLabel = nullptr;

    // m_selectionAsciiPreviewLabel：显示 ASCII 预览文本。
    QLabel* m_selectionAsciiPreviewLabel = nullptr;

    // m_selectionUtf16PreviewLabel：显示 UTF-16 预览文本。
    QLabel* m_selectionUtf16PreviewLabel = nullptr;

    // m_selectionIntegerPreviewLabel：显示整数/浮点解释文本。
    QLabel* m_selectionIntegerPreviewLabel = nullptr;

    // m_statusLabel：底部状态文本。
    QLabel* m_statusLabel = nullptr;

    // m_findShortcut：Ctrl+F 快捷键。
    QShortcut* m_findShortcut = nullptr;

    // m_jumpShortcut：Ctrl+G 快捷键。
    QShortcut* m_jumpShortcut = nullptr;

    // m_findNextShortcut：F3 快捷键。
    QShortcut* m_findNextShortcut = nullptr;

    // m_findPrevShortcut：Shift+F3 快捷键。
    QShortcut* m_findPrevShortcut = nullptr;

    // m_copyHexShortcut：Ctrl+C 复制 HEX。
    QShortcut* m_copyHexShortcut = nullptr;

    // m_copyAsciiShortcut：Ctrl+Shift+C 复制 ASCII。
    QShortcut* m_copyAsciiShortcut = nullptr;

    // m_buffer：当前显示的数据副本。
    QByteArray m_buffer;

    // m_baseAddress：当前数据基址。
    std::uint64_t m_baseAddress = 0;

    // m_editable：当前是否允许编辑字节。
    bool m_editable = false;

    // m_bytesPerRow：当前每行字节数。
    int m_bytesPerRow = 16;

    // m_ignoreItemChanged：程序内部回填时禁止触发编辑逻辑。
    bool m_ignoreItemChanged = false;

    // m_matchMask：查找高亮掩码（1=该字节命中）。
    QByteArray m_matchMask;

    // m_matchOffsets：查找命中的起始偏移列表。
    std::vector<std::uint64_t> m_matchOffsets;

    // m_currentMatchIndex：当前命中索引，-1 表示无命中。
    int m_currentMatchIndex = -1;

    // m_currentMatchLength：当前命中长度（字节）。
    int m_currentMatchLength = 0;

    // m_lastSearchMode：上次查找模式。
    SearchMode m_lastSearchMode = SearchMode::HexBytes;

    // m_lastSearchText：上次查找原始输入。
    QString m_lastSearchText;

    // m_lastSearchNormalizeText：上次查找标准化展示文本。
    QString m_lastSearchNormalizeText;

    // m_bufferRevision：数据版本号（数据变化时递增）。
    std::uint64_t m_bufferRevision = 0;

    // m_searchReadyRevision：查找结果对应的数据版本。
    std::uint64_t m_searchReadyRevision = 0;

    // m_searchTicket：异步查找票据（用于丢弃过期结果）。
    std::atomic<std::uint64_t> m_searchTicket{ 0 };

    // m_searchRunning：当前是否存在进行中的异步查找。
    bool m_searchRunning = false;

    // m_pendingDirection：查找完成后待执行导航方向。
    NavigateDirection m_pendingDirection = NavigateDirection::None;

    // m_pendingStartOffset：查找完成后待执行导航起点。
    std::uint64_t m_pendingStartOffset = 0;

    // m_linearSelectDragging：
    // - 作用：标记当前是否处于鼠标左键拖拽选择流程。
    bool m_linearSelectDragging = false;

    // m_linearSelectAnchorOffset：
    // - 作用：记录文本式拖拽选择锚点偏移。
    std::uint64_t m_linearSelectAnchorOffset = 0;

    // m_linearSelectAnchorValid：
    // - 作用：标记锚点偏移是否有效。
    bool m_linearSelectAnchorValid = false;

    // m_selectionRangeValid：
    // - 作用：标记当前是否存在自定义线性选区；
    // - 选区只覆盖十六进制字节区，不选 ASCII 列。
    bool m_selectionRangeValid = false;

    // m_selectionRangeStartOffset：
    // - 作用：记录当前选区起始偏移（包含）。
    std::uint64_t m_selectionRangeStartOffset = 0;

    // m_selectionRangeEndOffset：
    // - 作用：记录当前选区结束偏移（包含）。
    std::uint64_t m_selectionRangeEndOffset = 0;
};
