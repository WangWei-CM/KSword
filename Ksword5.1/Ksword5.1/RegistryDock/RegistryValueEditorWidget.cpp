#include "RegistryValueEditorWidget.h"
namespace {
const ks::ui::PageNavigationTabDefinition kRegistryValueNavigation[] = {
    {"data", "", "", "数据", true},
    {"raw", "", "", "原始字节", false},
    {"compare", "", "", "修改对照", false}
};
}

#include "../UI/PageControlStyle.h"
#include "../UI/PrimaryPageStyle.h"
#include "../UI/SecondaryPageLayout.h"
#include "../UI/ToolbarMetrics.h"
#include "RegistryValueCodec.h"
#include "../UI/MemoryWorkbench/HexView.h"
#include "../UI/ThemeStatusRole.h"
#include "../UI/AsyncOperation.h"
#include "../Internationalization/LanguageManager.h"
#include "../theme.h"

#include <QColorDialog>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include "../UI/CodeTextEdit.h"
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QSplitter>
#include <QStackedWidget>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <Windows.h>

#include <string>
#include <vector>

namespace
{
    // Bounded replacement allocations. Existing captured values remain complete
    // and can be viewed/exported; an oversized import or typed edit is refused.
    constexpr qsizetype kMaximumReplacementBytes = 32 * 1024 * 1024;
    constexpr int kTextPage = 0;
    constexpr int kMultiPage = 1;
    constexpr int kNumberPage = 2;
    constexpr int kRawPage = 3;

    QString trText(const QString& source)
    {
        return ks::i18n::sourceText(source);
    }

    std::string_view byteView(const QByteArray& bytes)
    {
        return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
    }

    QByteArray qtBytes(const std::string& bytes)
    {
        return QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size()));
    }

    QString qtText(const std::u16string& text)
    {
        return QString::fromUtf16(text.data(), static_cast<qsizetype>(text.size()));
    }

    bool isNumberType(quint32 type)
    {
        return type == ks::registry::TypeDword || type == ks::registry::TypeDwordBigEndian
            || type == ks::registry::TypeQword;
    }

    QByteArray defaultData(quint32 type)
    {
        if (isNumberType(type))
            return QByteArray(type == ks::registry::TypeQword ? 8 : 4, '\0');
        if (type == ks::registry::TypeString || type == ks::registry::TypeExpandString)
            return QByteArray(2, '\0');
        if (type == ks::registry::TypeMultiString)
            return QByteArray(4, '\0');
        return {};
    }

    enum class RegistryTextError { None, Oversized, InvalidString, InvalidMultiString };
    struct RegistryTextRequest
    {
        quint64 revision = 0;
        quint32 type = 0;
        QString text;
        QString originalText;
        QByteArray originalBytes;
        bool restoreOriginal = false;
    };
    struct RegistryTextResult
    {
        quint64 revision = 0;
        QByteArray bytes;
        RegistryTextError error = RegistryTextError::None;
        QString expanded;
        bool expansionValid = false;
    };

    RegistryTextResult prepareRegistryText(const RegistryTextRequest& request)
    {
        RegistryTextResult result;
        result.revision = request.revision;
        const bool multi = request.type == ks::registry::TypeMultiString;
        if (request.restoreOriginal && request.text == request.originalText)
            result.bytes = request.originalBytes;
        else if (request.text.size() > (kMaximumReplacementBytes - (multi ? 4 : 2)) / 2)
            result.error = RegistryTextError::Oversized;
        else
        {
            std::string encoded;
            bool valid = false;
            if (multi)
            {
                std::vector<std::u16string> items;
                if (!request.text.isEmpty())
                    for (const auto& line : request.text.split(QLatin1Char('\n'), Qt::KeepEmptyParts))
                        items.push_back(line.toStdU16String());
                valid = ks::registry::EncodeMultiString(items, &encoded);
            }
            else
                valid = ks::registry::EncodeString(request.text.toStdU16String(), &encoded);
            if (!valid)
                result.error = multi ? RegistryTextError::InvalidMultiString : RegistryTextError::InvalidString;
            else if (encoded.size() > static_cast<std::size_t>(kMaximumReplacementBytes))
                result.error = RegistryTextError::Oversized;
            else
                result.bytes = qtBytes(encoded);
        }
        if (request.type == ks::registry::TypeExpandString && result.error == RegistryTextError::None)
        {
            const auto* source = reinterpret_cast<LPCWSTR>(request.text.utf16());
            const DWORD required = ExpandEnvironmentStringsW(source, nullptr, 0);
            if (required > 0 && required <= static_cast<DWORD>(kMaximumReplacementBytes / 2))
            {
                std::wstring expanded(required, L'\0');
                const DWORD copied = ExpandEnvironmentStringsW(source, expanded.data(), required);
                result.expansionValid = copied > 0 && copied <= required;
                if (result.expansionValid)
                    result.expanded = QString::fromWCharArray(expanded.data(), static_cast<qsizetype>(copied - 1));
            }
        }
        return result;
    }

    enum class RegistryReadError { None, Open, Oversized, Changed };
    struct RegistryReadResult
    {
        quint64 revision = 0;
        QByteArray bytes;
        QString error;
        RegistryReadError kind = RegistryReadError::None;
    };
    struct RegistryReadRequest { QString path; quint64 revision = 0; };
    RegistryReadResult readRegistryBytes(const RegistryReadRequest& request)
    {
        RegistryReadResult result;
        result.revision = request.revision;
        QFile file(request.path);
        if (!file.open(QIODevice::ReadOnly))
        { result.kind = RegistryReadError::Open; result.error = file.errorString(); return result; }
        const qint64 expected = file.size();
        if (expected > kMaximumReplacementBytes)
        { result.kind = RegistryReadError::Oversized; return result; }
        result.bytes = file.read(kMaximumReplacementBytes + 1);
        if (file.error() != QFileDevice::NoError || result.bytes.size() != expected || !file.atEnd())
        { result.kind = RegistryReadError::Changed; result.bytes.clear(); }
        return result;
    }
    struct RegistryWriteRequest { QString path; QByteArray bytes; quint64 revision = 0; };
    struct RegistryWriteResult { QString error; quint64 revision = 0; };
    RegistryWriteResult writeRegistryBytes(const RegistryWriteRequest& request)
    {
        QSaveFile file(request.path);
        if (!file.open(QIODevice::WriteOnly) || file.write(request.bytes) != request.bytes.size() || !file.commit())
            return {file.errorString(), request.revision};
        return {{}, request.revision};
    }

    QPlainTextEdit* plainEditor(QWidget* parent, bool readOnly = false)
    {
        auto* editor = new CodeTextEdit(parent);
        static_cast<CodeTextEdit*>(editor)->setSyntaxLanguage(CodeTextEdit::SyntaxLanguage::PlainText);
        editor->setReadOnly(readOnly);
        editor->setMinimumHeight(80);
        return editor;
    }
}

RegistryValueEditorWidget::RegistryValueEditorWidget(QWidget* parent) : QWidget(parent)
{
    m_textOperation = new ks::ui::AsyncOperation(this);
    m_importOperation = new ks::ui::AsyncOperation(this);
    m_exportOperation = new ks::ui::AsyncOperation(this);
    m_textPreparationTimer = new QTimer(this);
    m_textPreparationTimer->setSingleShot(true);
    m_textPreparationTimer->setInterval(120);
    connect(m_textPreparationTimer, &QTimer::timeout, this, &RegistryValueEditorWidget::prepareTextDraft);
    setObjectName(QStringLiteral("registry_value_editor"));
    setMinimumWidth(280);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);
    m_path = new QLabel(this);
    m_path->setProperty("ks_i18n_preserve_data_text", true);
    m_path->setWordWrap(true);
    m_path->setTextFormat(Qt::PlainText);
    m_path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_path);
    auto* fields = new QFormLayout;
    fields->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    fields->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_name = new QLineEdit(this);
    // 值编辑器可内嵌工作台或独立弹出，字段自身拥有主题，不能依赖弹窗祖先样式。
    ks::ui::StyleStandaloneInput(m_name);
    m_name->setObjectName(QStringLiteral("registry_value_name"));
    // QLineEdit's default 32767-character ceiling must not truncate a loaded
    // name or turn the displayed name into the submission identity.
    m_name->setMaxLength(1024 * 1024);
    m_name->setPlaceholderText(trText(QStringLiteral("留空表示（默认）值")));
    m_type = new QComboBox(this);
    ks::ui::StylePrimaryCombo(m_type);
    m_type->setObjectName(QStringLiteral("registry_value_type"));
    struct TypeChoice { quint32 type; const char* name; };
    const TypeChoice choices[] = {
        {ks::registry::TypeString, "REG_SZ"}, {ks::registry::TypeExpandString, "REG_EXPAND_SZ"},
        {ks::registry::TypeMultiString, "REG_MULTI_SZ"}, {ks::registry::TypeDword, "REG_DWORD"},
        {ks::registry::TypeQword, "REG_QWORD"}, {ks::registry::TypeBinary, "REG_BINARY"},
        {ks::registry::TypeNone, "REG_NONE"}, {ks::registry::TypeDwordBigEndian, "REG_DWORD_BIG_ENDIAN"}
    };
    for (const auto& choice : choices)
        m_type->addItem(QString::fromLatin1(choice.name), choice.type);
    m_type->setToolTip(trText(QStringLiteral("新建时切换类型会重置草稿数据；恢复原值可撤销。")));
    fields->addRow(trText(QStringLiteral("值名")), m_name);
    fields->addRow(trText(QStringLiteral("类型")), m_type);
    ks::ui::StyleSecondaryForm(fields, 120);
    root->addLayout(fields);
    m_metadata = new QLabel(this);
    m_metadata->setTextFormat(Qt::PlainText);
    m_metadata->setWordWrap(true);
    root->addWidget(m_metadata);

    m_tabs = new QTabWidget(this);
    ks::ui::StyleSecondaryTabs(m_tabs);
    m_tabs->setObjectName(QStringLiteral("registry_value_tabs"));
    m_tabs->setMinimumSize(0, 0);
    m_tabs->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_typedPages = new QStackedWidget(m_tabs);
    auto* textPage = new QWidget(m_typedPages);
    auto* textLayout = new QVBoxLayout(textPage);
    textLayout->setContentsMargins(0, 0, 0, 0);
    m_text = plainEditor(textPage);
    m_text->setObjectName(QStringLiteral("registry_value_text"));
    textLayout->addWidget(m_text, 1);
    m_expandedTitle = new QLabel(trText(QStringLiteral("展开预览（当前进程环境，只读）")), textPage);
    textLayout->addWidget(m_expandedTitle);
    m_expanded = plainEditor(textPage, true);
    m_expanded->setObjectName(QStringLiteral("registry_value_expanded"));
    m_expanded->setMaximumHeight(120);
    textLayout->addWidget(m_expanded);
    m_typedPages->addWidget(textPage);
    auto* multiPage = new QWidget(m_typedPages);
    auto* multiLayout = new QVBoxLayout(multiPage);
    multiLayout->setContentsMargins(0, 0, 0, 0);
    auto* multiHint = new QLabel(trText(QStringLiteral("每行一个字符串；空文档表示空列表，项目之间不能有空行。")), multiPage);
    multiHint->setWordWrap(true);
    multiLayout->addWidget(multiHint);
    m_multi = plainEditor(multiPage);
    m_multi->setObjectName(QStringLiteral("registry_value_multi"));
    m_multi->setLineWrapMode(QPlainTextEdit::NoWrap);
    multiLayout->addWidget(m_multi, 1);
    m_typedPages->addWidget(multiPage);
    auto* numberPage = new QWidget(m_typedPages);
    auto* numberLayout = new QVBoxLayout(numberPage);
    numberLayout->setContentsMargins(0, 0, 0, 0);
    auto* numberFields = new QFormLayout;
    numberFields->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_hexNumber = new QLineEdit(numberPage);
    m_hexNumber->setObjectName(QStringLiteral("registry_value_hex_number"));
    m_hexNumber->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    ks::ui::StyleStandaloneInput(m_hexNumber);
    m_decimalNumber = new QLineEdit(numberPage);
    m_decimalNumber->setObjectName(QStringLiteral("registry_value_decimal_number"));
    m_decimalNumber->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    ks::ui::StyleStandaloneInput(m_decimalNumber);
    numberFields->addRow(trText(QStringLiteral("十六进制")), m_hexNumber);
    numberFields->addRow(trText(QStringLiteral("十进制")), m_decimalNumber);
    ks::ui::StyleSecondaryForm(numberFields, 120);
    numberLayout->addLayout(numberFields);
    m_range = new QLabel(numberPage);
    m_range->setWordWrap(true);
    numberLayout->addWidget(m_range);
    m_consoleColor = new QPushButton(trText(QStringLiteral("选择控制台颜色")), numberPage);
    m_consoleColor->setObjectName(QStringLiteral("registry_value_console_color"));
    m_consoleColor->setProperty("registry_value_button", true);
    numberLayout->addWidget(m_consoleColor);
    numberLayout->addStretch();
    m_typedPages->addWidget(numberPage);
    auto* rawHint = new QLabel(trText(QStringLiteral("此类型使用完整原始字节编辑器。")), m_typedPages);
    rawHint->setWordWrap(true);
    m_typedPages->addWidget(rawHint);
    m_navigationTabs.add(kRegistryValueNavigation[0], m_tabs, m_typedPages);

    auto* rawPage = new QWidget(m_tabs);
    auto* rawLayout = new QVBoxLayout(rawPage);
    rawLayout->setContentsMargins(0, 0, 0, 0);
    auto* fileTools = new QHBoxLayout;
    auto* importButton = new QPushButton(trText(QStringLiteral("导入字节")), rawPage);
    auto* exportButton = new QPushButton(trText(QStringLiteral("导出字节")), rawPage);
    importButton->setProperty("registry_value_button", true);
    exportButton->setProperty("registry_value_button", true);
    fileTools->addWidget(importButton);
    fileTools->addWidget(exportButton);
    fileTools->addStretch();
    ks::ui::NormalizeToolbarRow(fileTools);
    rawLayout->addLayout(fileTools);
    auto* lengthTools = new QHBoxLayout;
    m_resizeSize = new QLineEdit(rawPage);
    ks::ui::StyleStandaloneInput(m_resizeSize);
    m_resizeSize->setObjectName(QStringLiteral("registry_value_byte_length"));
    m_resizeSize->setPlaceholderText(trText(QStringLiteral("字节数")));
    auto* resizeButton = new QPushButton(trText(QStringLiteral("调整长度")), rawPage);
    resizeButton->setProperty("registry_value_button", true);
    resizeButton->setToolTip(trText(QStringLiteral("扩展补零，缩短移除草稿末尾字节；恢复原值可撤销。导入和调整长度上限为 32 MiB。")));
    lengthTools->addWidget(m_resizeSize, 1);
    lengthTools->addWidget(resizeButton);
    ks::ui::NormalizeToolbarRow(lengthTools);
    rawLayout->addLayout(lengthTools);
    m_hex = new ks::ui::HexView(rawPage);
    m_hex->setObjectName(QStringLiteral("registry_value_raw_hex"));
    m_hex->setEditable(true);
    rawLayout->addWidget(m_hex, 1);
    m_navigationTabs.add(kRegistryValueNavigation[1], m_tabs, rawPage);

    auto* comparePage = new QWidget(m_tabs);
    auto* compareLayout = new QVBoxLayout(comparePage);
    compareLayout->setContentsMargins(0, 0, 0, 0);
    auto* compareSplit = new QSplitter(Qt::Vertical, comparePage);
    compareSplit->setChildrenCollapsible(false);
    auto* beforePage = new QWidget(compareSplit);
    auto* beforeLayout = new QVBoxLayout(beforePage);
    beforeLayout->setContentsMargins(0, 0, 0, 0);
    beforeLayout->addWidget(new QLabel(trText(QStringLiteral("原值（读取基线）")), beforePage));
    m_before = new ks::ui::HexView(beforePage);
    m_before->setStatusBarVisible(false);
    beforeLayout->addWidget(m_before, 1);
    auto* afterPage = new QWidget(compareSplit);
    auto* afterLayout = new QVBoxLayout(afterPage);
    afterLayout->setContentsMargins(0, 0, 0, 0);
    afterLayout->addWidget(new QLabel(trText(QStringLiteral("新值（尚未提交）")), afterPage));
    m_after = new ks::ui::HexView(afterPage);
    m_after->setStatusBarVisible(false);
    afterLayout->addWidget(m_after, 1);
    compareLayout->addWidget(compareSplit, 1);
    m_navigationTabs.add(kRegistryValueNavigation[2], m_tabs, comparePage);
    root->addWidget(m_tabs, 1);
    m_state = new QLabel(this);
    m_state->setTextFormat(Qt::PlainText);
    m_state->setWordWrap(true);
    m_state->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_state);
    m_discard = new QPushButton(trText(QStringLiteral("恢复原值")), this);
    m_discard->setProperty("registry_value_button", true);
    root->addWidget(m_discard);

    connect(m_name, &QLineEdit::textChanged, this, [this] {
        if (m_syncing) return;
        if (m_textPreparationPending) { editText(); return; }
        invalidateDraftPreparation();
        m_operationError.clear();
        updateState();
        emit draftChanged();
    });
    connect(m_type, &QComboBox::currentIndexChanged, this, &RegistryValueEditorWidget::changeType);
    connect(m_text, &QPlainTextEdit::textChanged, this, &RegistryValueEditorWidget::editText);
    connect(m_multi, &QPlainTextEdit::textChanged, this, &RegistryValueEditorWidget::editText);
    connect(m_hexNumber, &QLineEdit::textChanged, this, [this] { editNumber(true); });
    connect(m_decimalNumber, &QLineEdit::textChanged, this, [this] { editNumber(false); });
    connect(m_hex, &ks::ui::HexView::byteEdited, this, [this] {
        if (m_syncing) return;
        invalidateDraftPreparation();
        m_working = m_hex->buffer();
        m_comparisonNeedsRefresh = true;
        m_dataError.clear();
        m_operationError.clear();
        m_typedNeedsRefresh = true;
        m_resizeSize->setText(QString::number(m_working.size()));
        updateState();
        emit draftChanged();
    });
    connect(m_tabs, &QTabWidget::currentChanged, this, &RegistryValueEditorWidget::changeTab);
    connect(m_discard, &QPushButton::clicked, this, &RegistryValueEditorWidget::discardChanges);
    connect(importButton, &QPushButton::clicked, this, &RegistryValueEditorWidget::importBytes);
    connect(exportButton, &QPushButton::clicked, this, &RegistryValueEditorWidget::exportBytes);
    connect(resizeButton, &QPushButton::clicked, this, &RegistryValueEditorWidget::resizeBytes);
    connect(m_consoleColor, &QPushButton::clicked, this, &RegistryValueEditorWidget::chooseConsoleColor);
    m_exportOperation->setStateChangedCallback([this, exportButton] {
        exportButton->setEnabled(!m_exportOperation->isInFlight());
    });
    m_textOperation->setStateChangedCallback([this] {
        if (!m_textPreparationPending || m_textOperation->isInFlight() || m_textPreparationTimer->isActive()) return;
        m_textPreparationPending = false;
        m_tabs->setTabEnabled(1, true);
        m_tabs->setTabEnabled(2, true);
        m_dataError = trText(QStringLiteral("文本草稿准备失败；请重试编辑或恢复原值。"));
        updateState();
        emit draftChanged();
    });
    m_importOperation->setStateChangedCallback([this] {
        if (!m_importPending || m_importOperation->isInFlight()) return;
        m_importPending = false;
        m_operationError = trText(QStringLiteral("文件读取失败或长度发生变化；草稿未改变。"));
        updateState();
        emit draftChanged();
    });
    applyControlStyles();
    setValue({}, {}, ks::registry::TypeString, QByteArray(2, '\0'), true);
}

RegistryValueEditorWidget::~RegistryValueEditorWidget()
{
    m_textOperation->close();
    m_importOperation->close();
    m_exportOperation->close();
}

void RegistryValueEditorWidget::invalidateDraftPreparation()
{
    ++m_revision;
    m_textPreparationTimer->stop();
    m_textPreparationPending = false;
    m_importPending = false;
    m_tabs->setTabEnabled(1, true);
    m_tabs->setTabEnabled(2, true);
}

quint32 RegistryValueEditorWidget::currentType() const
{
    return m_createMode ? m_type->currentData().toUInt() : m_originalType;
}

void RegistryValueEditorWidget::setValue(const QString& keyPath, const QString& name,
    quint32 type, const QByteArray& raw, bool createMode)
{
    invalidateDraftPreparation();
    m_textOperation->cancel();
    m_importOperation->cancel();
    m_syncing = true;
    m_keyPath = keyPath;
    m_originalName = name;
    m_originalType = type;
    m_original = raw;
    m_working = raw;
    m_comparisonNeedsRefresh = true;
    m_comparisonBaselineNeedsRefresh = true;
    m_createMode = createMode;
    m_originalTypedValid = false;
    m_originalTypedText.clear();
    m_dataError.clear();
    m_operationError.clear();
    m_path->setText(keyPath);
    m_path->setToolTip(keyPath);
    m_name->setText(name);
    m_name->setReadOnly(!createMode);
    int typeIndex = m_type->findData(type);
    if (typeIndex < 0)
    {
        m_type->addItem(trText(QStringLiteral("类型 %1（原始字节）")).arg(type), type);
        typeIndex = m_type->count() - 1;
    }
    m_type->setCurrentIndex(typeIndex);
    m_type->setEnabled(createMode);
    m_typedNeedsRefresh = true;
    m_hexNeedsRefresh = true;
    loadTypedEditor();
    if (type == ks::registry::TypeString || type == ks::registry::TypeExpandString)
    {
        m_originalTypedValid = !m_text->isReadOnly();
        m_originalTypedText = m_text->toPlainText();
    }
    else if (type == ks::registry::TypeMultiString)
    {
        m_originalTypedValid = !m_multi->isReadOnly();
        m_originalTypedText = m_multi->toPlainText();
    }
    const bool typed = m_typedPages->currentIndex() != kRawPage;
    m_tabs->setTabEnabled(0, typed);
    m_tabs->setCurrentIndex(typed ? 0 : 1);
    if (m_tabs->currentIndex() == 1) loadHexEditor();
    if (m_tabs->currentIndex() == 2) updateComparison();
    m_syncing = false;
    updateState();
    emit draftChanged();
}

bool RegistryValueEditorWidget::value(RegistryValueDraft* out, QString* errorOut) const
{
    auto fail = [errorOut](const QString& message) {
        if (errorOut) *errorOut = message;
        return false;
    };
    if (!out)
        return fail(trText(QStringLiteral("缺少注册表草稿输出。")));
    const QString name = m_createMode ? m_name->text() : m_originalName;
    if (name.contains(QChar(0)))
        return fail(trText(QStringLiteral("值名不能包含 NUL 字符。")));
    if (name.size() > 16383)
        return fail(trText(QStringLiteral("值名超过 16383 个 UTF-16 字符；名称未截断。")));
    if (m_textPreparationPending)
        return fail(trText(QStringLiteral("正在准备当前文本草稿，请稍候…")));
    if (m_importPending)
        return fail(trText(QStringLiteral("正在读取原始字节，请稍候…")));
    if (!m_dataError.isEmpty())
        return fail(m_dataError);
    *out = {name, currentType(), m_working};
    if (errorOut) errorOut->clear();
    return true;
}

bool RegistryValueEditorWidget::isModified() const
{
    return (m_createMode && (m_name->text() != m_originalName || currentType() != m_originalType))
        || m_textPreparationPending || !m_dataError.isEmpty() || m_working != m_original;
}

void RegistryValueEditorWidget::discardChanges()
{
    setValue(m_keyPath, m_originalName, m_originalType, m_original, m_createMode);
}

void RegistryValueEditorWidget::loadTypedEditor()
{
    const bool previousSync = m_syncing;
    m_syncing = true;
    m_typedNeedsRefresh = false;
    m_formatWarning.clear();
    const quint32 type = currentType();
    const bool stringType = type == ks::registry::TypeString || type == ks::registry::TypeExpandString;
    if (stringType || type == ks::registry::TypeMultiString)
    {
        m_typedPages->setCurrentIndex(stringType ? kTextPage : kMultiPage);
        auto* edit = stringType ? m_text : m_multi;
        QString text;
        bool canonical = false;
        bool decoded = false;
        if (m_working.size() <= kMaximumReplacementBytes)
        {
            if (stringType)
            {
                std::u16string decodedText;
                decoded = ks::registry::DecodeString(byteView(m_working), &decodedText, &canonical);
                if (decoded) text = qtText(decodedText);
            }
            else
            {
                std::vector<std::u16string> items;
                decoded = ks::registry::DecodeMultiString(byteView(m_working), &items, &canonical);
                QStringList lines;
                for (const auto& item : items)
                {
                    const QString line = qtText(item);
                    if (line.contains(QLatin1Char('\n')) || line.contains(QLatin1Char('\r')))
                        decoded = false;
                    lines.push_back(line);
                }
                if (decoded) text = lines.join(QLatin1Char('\n'));
            }
        }
        edit->setReadOnly(!decoded);
        edit->setPlainText(text);
        if (!decoded)
            m_formatWarning = trText(QStringLiteral("数据无法无损显示为文本或超过 32 MiB；请使用原始字节。原始数据保持完整。"));
        else if (!canonical || text.contains(QLatin1Char('\r')))
            m_formatWarning = trText(QStringLiteral("原始终止符或换行格式不规范；未修改时保留原字节，文本修改后使用规范 UTF-16LE 编码。"));
        m_expandedTitle->setVisible(type == ks::registry::TypeExpandString);
        m_expanded->setVisible(type == ks::registry::TypeExpandString);
        if (type == ks::registry::TypeExpandString) updateExpandPreview();
    }
    else if (isNumberType(type))
    {
        m_typedPages->setCurrentIndex(kNumberPage);
        std::uint64_t number = 0;
        const bool valid = ks::registry::DecodeUnsigned(byteView(m_working),
            type == ks::registry::TypeQword ? 8 : 4, &number, type == ks::registry::TypeDwordBigEndian);
        m_hexNumber->setText(valid ? QStringLiteral("0x%1").arg(number, 0, 16).toUpper() : QString());
        m_decimalNumber->setText(valid ? QString::number(number) : QString());
        m_range->setText(type == ks::registry::TypeQword
            ? trText(QStringLiteral("无符号 64 位范围：0 ～ 18446744073709551615"))
            : trText(QStringLiteral("无符号 32 位范围：0 ～ 4294967295")));
        if (!valid)
            m_formatWarning = trText(QStringLiteral("原始整数长度不符合类型；输入整数会生成完整规范字节，也可使用原始字节修复。"));
        m_consoleColor->setVisible(isConsoleColor());
        updateConsoleColor();
    }
    else
        m_typedPages->setCurrentIndex(kRawPage);
    m_syncing = previousSync;
}

void RegistryValueEditorWidget::loadHexEditor()
{
    const bool previousSync = m_syncing;
    m_syncing = true;
    m_hexNeedsRefresh = false;
    m_hex->setBuffer(0, m_working);
    m_hex->setReference(m_original);
    m_resizeSize->setText(QString::number(m_working.size()));
    m_syncing = previousSync;
}

void RegistryValueEditorWidget::updateComparison()
{
    if (!m_comparisonNeedsRefresh) return;
    m_comparisonNeedsRefresh = false;
    if (m_comparisonBaselineNeedsRefresh)
    {
        m_comparisonBaselineNeedsRefresh = false;
        m_before->setBuffer(0, m_original);
    }
    m_after->setBuffer(0, m_working);
    m_after->setReference(m_original);
}

void RegistryValueEditorWidget::stageBytes(const QByteArray& bytes)
{
    invalidateDraftPreparation();
    m_working = bytes;
    m_comparisonNeedsRefresh = true;
    if (bytes != m_original) m_formatWarning.clear();
    m_dataError.clear();
    m_operationError.clear();
    m_hexNeedsRefresh = true;
    m_resizeSize->setText(QString::number(m_working.size()));
    if (m_tabs->currentIndex() == 1) loadHexEditor();
    if (m_tabs->currentIndex() == 2) updateComparison();
    updateState();
    emit draftChanged();
}

void RegistryValueEditorWidget::editText()
{
    if (m_syncing) return;
    const quint32 type = currentType();
    if (type != ks::registry::TypeMultiString && type != ks::registry::TypeString
        && type != ks::registry::TypeExpandString) return;
    // The signal path performs no full-document read, comparison or encoding.
    ++m_revision;
    m_textPreparationPending = false;
    m_textOperation->cancel();
    m_importPending = false;
    m_textPreparationPending = true;
    m_dataError.clear();
    m_operationError.clear();
    m_tabs->setTabEnabled(1, false);
    m_tabs->setTabEnabled(2, false);
    m_textPreparationTimer->start();
    updateState();
    emit draftChanged();
}

void RegistryValueEditorWidget::prepareTextDraft()
{
    if (!m_textPreparationPending) return;
    const quint32 type = currentType();
    RegistryTextRequest request{m_revision, type,
        (type == ks::registry::TypeMultiString ? m_multi : m_text)->toPlainText(),
        m_originalTypedText, m_original, m_originalTypedValid && type == m_originalType};
    m_textOperation->submit<RegistryTextRequest, RegistryTextResult>(std::move(request),
        [](const RegistryTextRequest& value, const ks::ui::AsyncOperationToken&) {
            return prepareRegistryText(value);
        }, [this, type](const RegistryTextResult& result, std::uint64_t) {
            if (result.revision != m_revision || !m_textPreparationPending || type != currentType()) return true;
            m_textPreparationPending = false;
            m_tabs->setTabEnabled(1, true);
            m_tabs->setTabEnabled(2, true);
            if (result.error == RegistryTextError::None)
            {
                if (type == ks::registry::TypeExpandString)
                    m_expanded->setPlainText(result.expansionValid ? result.expanded
                        : trText(QStringLiteral("环境变量展开失败或预览超过上限；原文不变。")));
                stageBytes(result.bytes); // draftChanged may synchronously close the editor.
                return true;
            }
            if (result.error == RegistryTextError::Oversized)
                m_dataError = trText(QStringLiteral("文本编码后超过 32 MiB 编辑上限；数据未截断。"));
            else if (result.error == RegistryTextError::InvalidMultiString)
                m_dataError = trText(QStringLiteral("多字符串项目不能为空或包含 NUL、无效 UTF-16；请移除空行。"));
            else
                m_dataError = trText(QStringLiteral("字符串不能包含 NUL 或无效 UTF-16；原始格式可在字节页编辑。"));
            updateState();
            emit draftChanged();
            return true;
        }, [this, revision = m_revision](std::exception_ptr) {
            if (revision != m_revision) return;
            m_textPreparationPending = false;
            m_tabs->setTabEnabled(1, true);
            m_tabs->setTabEnabled(2, true);
            m_dataError = trText(QStringLiteral("文本草稿准备失败；请重试编辑或恢复原值。"));
            updateState();
            emit draftChanged();
        });
}

void RegistryValueEditorWidget::editNumber(bool hexadecimal)
{
    if (m_syncing || !isNumberType(currentType())) return;
    invalidateDraftPreparation();
    const quint32 type = currentType();
    const unsigned bits = type == ks::registry::TypeQword ? 64 : 32;
    const QByteArray input = (hexadecimal ? m_hexNumber : m_decimalNumber)->text().trimmed().toLatin1();
    std::uint64_t number = 0;
    if (!ks::registry::ParseUnsigned(byteView(input), hexadecimal ? 16 : 10, bits, &number))
    {
        m_dataError = trText(QStringLiteral("请输入有效的无符号 %1 位整数；不接受负数、溢出或无效数字。")).arg(bits);
        updateState();
        emit draftChanged();
        return;
    }
    m_syncing = true;
    if (hexadecimal) m_decimalNumber->setText(QString::number(number));
    else m_hexNumber->setText(QStringLiteral("0x%1").arg(number, 0, 16).toUpper());
    m_syncing = false;
    std::string encoded;
    if (!ks::registry::EncodeUnsigned(number, bits / 8, &encoded, type == ks::registry::TypeDwordBigEndian)) return;
    stageBytes(qtBytes(encoded));
    updateConsoleColor();
}

void RegistryValueEditorWidget::changeType()
{
    if (m_syncing || !m_createMode) return;
    invalidateDraftPreparation();
    const bool previousSync = m_syncing;
    m_syncing = true;
    m_working = currentType() == m_originalType ? m_original : defaultData(currentType());
    m_comparisonNeedsRefresh = true;
    m_dataError.clear();
    m_operationError.clear();
    m_typedNeedsRefresh = true;
    m_hexNeedsRefresh = true;
    loadTypedEditor();
    const bool typed = m_typedPages->currentIndex() != kRawPage;
    m_tabs->setTabEnabled(0, typed);
    m_tabs->setCurrentIndex(typed ? 0 : 1);
    loadHexEditor();
    m_syncing = previousSync;
    updateState();
    emit draftChanged();
}

void RegistryValueEditorWidget::changeTab(int index)
{
    if (m_syncing) return;
    if (index == 0 && m_typedNeedsRefresh) loadTypedEditor();
    if (index == 1 && m_hexNeedsRefresh) loadHexEditor();
    if (index == 2) updateComparison();
    updateState();
}

void RegistryValueEditorWidget::updateState()
{
    m_metadata->setText(trText(QStringLiteral("原始 %1 字节 · 当前 %2 字节 · %3"))
        .arg(m_original.size()).arg(m_working.size())
        .arg(isModified() ? trText(QStringLiteral("草稿已修改")) : trText(QStringLiteral("与原值一致"))));
    m_discard->setEnabled(isModified());
    m_consoleColor->setVisible(isConsoleColor());
    updateConsoleColor();
    QString error;
    RegistryValueDraft draft;
    value(&draft, &error);
    if (!error.isEmpty() || !m_operationError.isEmpty())
    {
        m_state->setText(error.isEmpty() ? m_operationError : error);
        ks::ui::ApplyStatusRole(m_state, ks::ui::StatusRole::Error);
    }
    else if (!m_formatWarning.isEmpty())
    {
        m_state->setText(m_formatWarning);
        ks::ui::ApplyStatusRole(m_state, ks::ui::StatusRole::Warning);
    }
    else
    {
        m_state->setText(trText(QStringLiteral("修改仅保存在草稿中，由注册表工作台提交并回读验证。")));
        ks::ui::ApplyStatusRole(m_state, ks::ui::StatusRole::Info);
    }
}

void RegistryValueEditorWidget::importBytes()
{
    const quint64 revision = m_revision;
    const QPointer<RegistryValueEditorWidget> guard(this);
    const QString path = QFileDialog::getOpenFileName(this, trText(QStringLiteral("导入注册表原始字节")));
    if (!guard || revision != m_revision || path.isEmpty()) return;
    invalidateDraftPreparation();
    m_importPending = true;
    const quint64 importRevision = m_revision;
    updateState();
    emit draftChanged();
    if (!guard || importRevision != m_revision || !m_importPending) return;
    m_importOperation->submit<RegistryReadRequest, RegistryReadResult>({path, importRevision},
        [](const RegistryReadRequest& value, const ks::ui::AsyncOperationToken&) {
            return readRegistryBytes(value);
        }, [this](const RegistryReadResult& result, std::uint64_t) {
            if (result.revision != m_revision || !m_importPending) return true;
            m_importPending = false;
            if (result.kind == RegistryReadError::None)
            {
                m_typedNeedsRefresh = true;
                stageBytes(result.bytes);
                return true;
            }
            if (result.kind == RegistryReadError::Open) m_operationError = result.error;
            else if (result.kind == RegistryReadError::Oversized)
                m_operationError = trText(QStringLiteral("导入文件超过 32 MiB 编辑上限；草稿未改变。"));
            else
                m_operationError = trText(QStringLiteral("文件读取失败或长度发生变化；草稿未改变。"));
            updateState();
            emit draftChanged();
            return true;
        }, [this, importRevision](std::exception_ptr) {
            if (importRevision != m_revision) return;
            m_importPending = false;
            m_operationError = trText(QStringLiteral("文件读取失败或长度发生变化；草稿未改变。"));
            updateState();
            emit draftChanged();
        });
}

void RegistryValueEditorWidget::exportBytes()
{
    if (m_exportOperation->isBusy()) return;
    RegistryValueDraft draft;
    QString error;
    if (!value(&draft, &error))
    {
        m_operationError = error;
        updateState();
        return;
    }
    const quint64 revision = m_revision;
    const QPointer<RegistryValueEditorWidget> guard(this);
    const QString path = QFileDialog::getSaveFileName(this, trText(QStringLiteral("导出注册表原始字节")));
    if (!guard || revision != m_revision || path.isEmpty() || m_exportOperation->isBusy()) return;
    const auto accepted = m_exportOperation->submitTransaction<RegistryWriteRequest, RegistryWriteResult>({path, draft.data, revision},
        [](const RegistryWriteRequest& value) { return writeRegistryBytes(value); },
        [this](const RegistryWriteResult& result, std::uint64_t) {
            if (result.revision != m_revision) return true;
            m_operationError = result.error.isEmpty() ? QString()
                : trText(QStringLiteral("字节导出失败：%1")).arg(result.error);
            updateState();
            return true;
        }, [this, revision](std::exception_ptr) {
            if (revision != m_revision) return;
            m_operationError = trText(QStringLiteral("字节导出失败：%1"))
                .arg(trText(QStringLiteral("后台任务失败")));
            updateState();
        });
    if (accepted == 0)
    {
        m_operationError = trText(QStringLiteral("字节导出失败：%1"))
            .arg(trText(QStringLiteral("后台任务失败")));
        updateState();
    }
}

void RegistryValueEditorWidget::resizeBytes()
{
    const QByteArray input = m_resizeSize->text().trimmed().toLatin1();
    std::uint64_t length = 0;
    if (!ks::registry::ParseUnsigned(byteView(input), 10, 32, &length)
        || length > static_cast<std::uint64_t>(kMaximumReplacementBytes))
    {
        m_operationError = trText(QStringLiteral("字节数必须在 0 ～ 33554432 范围内；草稿未改变。"));
        updateState();
        return;
    }
    QByteArray bytes = m_working;
    const qsizetype size = static_cast<qsizetype>(length);
    if (size < bytes.size()) bytes.truncate(size);
    else if (size > bytes.size()) bytes.append(QByteArray(size - bytes.size(), '\0'));
    m_typedNeedsRefresh = true;
    stageBytes(bytes);
}

void RegistryValueEditorWidget::updateExpandPreview()
{
    const QString text = m_text->toPlainText();
    const DWORD required = ExpandEnvironmentStringsW(reinterpret_cast<LPCWSTR>(text.utf16()), nullptr, 0);
    if (required == 0 || required > static_cast<DWORD>(kMaximumReplacementBytes / 2))
    {
        m_expanded->setPlainText(trText(QStringLiteral("环境变量展开失败或预览超过上限；原文不变。")));
        return;
    }
    std::wstring expanded(required, L'\0');
    const DWORD copied = ExpandEnvironmentStringsW(reinterpret_cast<LPCWSTR>(text.utf16()), expanded.data(), required);
    m_expanded->setPlainText(copied > 0 && copied <= required
        ? QString::fromWCharArray(expanded.data(), static_cast<qsizetype>(copied - 1))
        : trText(QStringLiteral("环境变量展开失败或预览超过上限；原文不变。")));
}

bool RegistryValueEditorWidget::isConsoleColor() const
{
    QString path = m_keyPath;
    if (path.startsWith(QStringLiteral("HKCU\\"), Qt::CaseInsensitive))
        path.replace(0, 4, QStringLiteral("HKEY_CURRENT_USER"));
    if (path.compare(QStringLiteral("HKEY_CURRENT_USER\\Console"), Qt::CaseInsensitive) != 0
        || currentType() != ks::registry::TypeDword)
        return false;
    const QString name = m_createMode ? m_name->text() : m_originalName;
    if (!name.startsWith(QStringLiteral("ColorTable"), Qt::CaseInsensitive) || name.size() != 12)
        return false;
    bool valid = false;
    const int index = name.mid(10).toInt(&valid, 10);
    return valid && name[10].isDigit() && name[11].isDigit() && index >= 0 && index < 16;
}

void RegistryValueEditorWidget::updateConsoleColor()
{
    if (!isConsoleColor()) return;
    std::uint64_t number = 0;
    const bool valid = ks::registry::DecodeUnsigned(byteView(m_working), 4, &number);
    m_consoleColor->setEnabled(valid && m_dataError.isEmpty());
    if (!valid) return;
    const QColor color(static_cast<int>(number & 0xff), static_cast<int>((number >> 8) & 0xff),
        static_cast<int>((number >> 16) & 0xff));
    QPixmap swatch(20, 20);
    swatch.fill(color);
    QPainter painter(&swatch);
    painter.setPen(palette().color(QPalette::Mid));
    painter.drawRect(0, 0, 19, 19);
    m_consoleColor->setIcon(QIcon(swatch));
    m_consoleColor->setToolTip(trText(QStringLiteral("控制台 COLORREF 色值：%1；色块表示数据本身。")).arg(color.name()));
}

void RegistryValueEditorWidget::chooseConsoleColor()
{
    std::uint64_t number = 0;
    if (!isConsoleColor() || !ks::registry::DecodeUnsigned(byteView(m_working), 4, &number)) return;
    const QColor initial(static_cast<int>(number & 0xff), static_cast<int>((number >> 8) & 0xff),
        static_cast<int>((number >> 16) & 0xff));
    const quint64 revision = m_revision;
    const QPointer<RegistryValueEditorWidget> guard(this);
    const QColor color = QColorDialog::getColor(initial, this, trText(QStringLiteral("选择控制台颜色")));
    if (!guard || revision != m_revision || !color.isValid()) return;
    const std::uint64_t selected = (number & UINT64_C(0xff000000)) | static_cast<unsigned>(color.red())
        | (static_cast<std::uint64_t>(color.green()) << 8) | (static_cast<std::uint64_t>(color.blue()) << 16);
    std::string encoded;
    if (!ks::registry::EncodeUnsigned(selected, 4, &encoded)) return;
    stageBytes(qtBytes(encoded));
    loadTypedEditor();
}

void RegistryValueEditorWidget::applyControlStyles()
{
    const QString buttonStyle = KswordTheme::ThemedButtonStyle();
    for (auto* button : findChildren<QPushButton*>())
    {
        if (button->property("registry_value_button").toBool())
        {
            button->setStyleSheet(buttonStyle);
            // 主题重建局部颜色后仍恢复登记的统一按钮几何。
            ks::ui::NormalizeToolbarControl(button);
        }
    }
    updateConsoleColor();
}

void RegistryValueEditorWidget::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::PaletteChange)
        applyControlStyles();
}

ks::ui::PageNavigationCatalog RegistryValueEditorWidget::navigationCatalog(const RegistryValueEditorWidget* existing)
{
    ks::ui::PageNavigationCatalog schema;
    for (const auto& definition : kRegistryValueNavigation)
        schema.append(ks::ui::PageNavigationTabDescriptor(definition));
    return existing ? existing->m_navigationTabs.describe(std::move(schema)) : schema;
}
QWidget* RegistryValueEditorWidget::activateNavigationDescriptor(const QString& id)
{
    return m_navigationTabs.activate(id, navigationCatalog(this));
}
namespace { const bool kRegistryValueNavigationRegistered =
    ks::ui::RegisterTypedPageNavigationCatalog<RegistryValueEditorWidget>(QStringLiteral("registry.value_editor")); }
