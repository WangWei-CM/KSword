#include "LanguageManager.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QProxyStyle>
#include <QRegularExpression>
#include <QSet>
#include <QStyleOption>
#include <QTabWidget>
#include <QTextEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QWidget>

#include <algorithm>
#include <functional>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace
{
    constexpr auto kLanguagePackSchema = "ksword-language-pack";
    constexpr int kLanguagePackFormatVersion = 1;
    constexpr qint64 kMaximumLanguagePackBytes = 32 * 1024 * 1024;
    constexpr int kComboKeyRole = Qt::UserRole + 91;
    constexpr int kComboFallbackRole = Qt::UserRole + 92;

    constexpr auto kTextKeyProperty = "ks_i18n_text_key";
    constexpr auto kTextFallbackProperty = "ks_i18n_text_fallback";
    constexpr auto kToolTipKeyProperty = "ks_i18n_tooltip_key";
    constexpr auto kToolTipFallbackProperty = "ks_i18n_tooltip_fallback";
    constexpr auto kPlaceholderKeyProperty = "ks_i18n_placeholder_key";
    constexpr auto kPlaceholderFallbackProperty = "ks_i18n_placeholder_fallback";
    constexpr auto kWindowTitleKeyProperty = "ks_i18n_window_title_key";
    constexpr auto kWindowTitleFallbackProperty = "ks_i18n_window_title_fallback";
    constexpr auto kTabKeyProperty = "ks_i18n_tab_key";
    constexpr auto kTabFallbackProperty = "ks_i18n_tab_fallback";
    constexpr auto kAutoTextSourceProperty = "ks_i18n_auto_text_source";
    constexpr auto kAutoTextLastProperty = "ks_i18n_auto_text_last";
    constexpr auto kAutoToolTipSourceProperty = "ks_i18n_auto_tooltip_source";
    constexpr auto kAutoToolTipLastProperty = "ks_i18n_auto_tooltip_last";
    constexpr auto kAutoPlaceholderSourceProperty = "ks_i18n_auto_placeholder_source";
    constexpr auto kAutoPlaceholderLastProperty = "ks_i18n_auto_placeholder_last";
    constexpr auto kAutoWindowTitleSourceProperty = "ks_i18n_auto_window_title_source";
    constexpr auto kAutoWindowTitleLastProperty = "ks_i18n_auto_window_title_last";
    constexpr auto kAutoTabSourceProperty = "ks_i18n_auto_tab_source";
    constexpr auto kAutoTabLastProperty = "ks_i18n_auto_tab_last";
    constexpr auto kAutoPrefixSourceProperty = "ks_i18n_auto_prefix_source";
    constexpr auto kAutoPrefixLastProperty = "ks_i18n_auto_prefix_last";
    constexpr auto kAutoSuffixSourceProperty = "ks_i18n_auto_suffix_source";
    constexpr auto kAutoSuffixLastProperty = "ks_i18n_auto_suffix_last";
    constexpr auto kAutoSpecialSourceProperty = "ks_i18n_auto_special_source";
    constexpr auto kAutoSpecialLastProperty = "ks_i18n_auto_special_last";
    constexpr auto kAutoDocumentSourceProperty = "ks_i18n_auto_document_source";
    constexpr auto kAutoDocumentLastProperty = "ks_i18n_auto_document_last";
    constexpr int kAutoComboSourceRole = Qt::UserRole + 0x4B50;
    constexpr int kAutoComboLastRole = Qt::UserRole + 0x4B51;

    bool containsHanText(const QString& text)
    {
        static const QRegularExpression hanExpression(QStringLiteral("[\\x{3400}-\\x{4DBF}\\x{4E00}-\\x{9FFF}]"));
        return hanExpression.match(text).hasMatch();
    }

    class LanguageProxyStyle final : public QProxyStyle
    {
    public:
        explicit LanguageProxyStyle(QStyle* baseStyle)
            : QProxyStyle(baseStyle)
        {
        }

        void drawControl(
            const ControlElement element,
            const QStyleOption* option,
            QPainter* painter,
            const QWidget* widget = nullptr) const override
        {
            if (element == CE_ItemViewItem)
            {
                if (const QStyleOptionViewItem* itemOption = qstyleoption_cast<const QStyleOptionViewItem*>(option))
                {
                    QStyleOptionViewItem translatedOption(*itemOption);
                    translatedOption.text = ks::i18n::LanguageManager::instance().translateSource(itemOption->text);
                    QProxyStyle::drawControl(element, &translatedOption, painter, widget);
                    return;
                }
            }
            else if (element == CE_HeaderLabel)
            {
                if (const QStyleOptionHeader* headerOption = qstyleoption_cast<const QStyleOptionHeader*>(option))
                {
                    QStyleOptionHeader translatedOption(*headerOption);
                    translatedOption.text = ks::i18n::LanguageManager::instance().translateSource(headerOption->text);
                    QProxyStyle::drawControl(element, &translatedOption, painter, widget);
                    return;
                }
            }
            QProxyStyle::drawControl(element, option, painter, widget);
        }
    };

    void appendUniqueDirectory(QStringList* directoryList, const QString& directoryPath)
    {
        if (directoryList == nullptr)
        {
            return;
        }

        const QString normalizedPath = QDir::cleanPath(directoryPath.trimmed());
        if (!normalizedPath.isEmpty() && !directoryList->contains(normalizedPath, Qt::CaseInsensitive))
        {
            directoryList->append(normalizedPath);
        }
    }

    QStringList languageDirectoryCandidates()
    {
        QStringList directoryList;
        const QString applicationDirectory = QCoreApplication::applicationDirPath();
        appendUniqueDirectory(&directoryList, QDir(applicationDirectory).absoluteFilePath(QStringLiteral("languages")));
#ifdef Q_OS_WIN
        wchar_t executablePathBuffer[32768] = {};
        constexpr DWORD executablePathCapacity = static_cast<DWORD>(
            sizeof(executablePathBuffer) / sizeof(executablePathBuffer[0]));
        const DWORD executablePathLength = ::GetModuleFileNameW(
            nullptr,
            executablePathBuffer,
            executablePathCapacity);
        if (executablePathLength > 0 && executablePathLength < executablePathCapacity)
        {
            const QString executableDirectory = QFileInfo(
                QString::fromWCharArray(executablePathBuffer, static_cast<int>(executablePathLength))).absolutePath();
            appendUniqueDirectory(
                &directoryList,
                QDir(executableDirectory).absoluteFilePath(QStringLiteral("languages")));
        }
#endif

        QString walkingPath = QDir::currentPath();
        for (int depth = 0; depth < 8; ++depth)
        {
            const QDir walkingDirectory(walkingPath);
            appendUniqueDirectory(&directoryList, walkingDirectory.absoluteFilePath(QStringLiteral("languages")));
            appendUniqueDirectory(
                &directoryList,
                walkingDirectory.absoluteFilePath(QStringLiteral("Ksword5.1/Ksword5.1/languages")));

            QDir parentDirectory(walkingPath);
            if (!parentDirectory.cdUp())
            {
                break;
            }
            walkingPath = parentDirectory.absolutePath();
        }
        return directoryList;
    }

    bool isValidLanguageId(const QString& languageId)
    {
        static const QRegularExpression languageIdExpression(
            QStringLiteral("^[A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,8})*$"));
        return languageIdExpression.match(languageId).hasMatch();
    }
}

ks::i18n::LanguageManager& ks::i18n::LanguageManager::instance()
{
    static LanguageManager manager;
    return manager;
}

bool ks::i18n::LanguageManager::initialize(
    const QString& preferredLanguageId,
    QString* errorTextOut)
{
    QStringList warningList;
    if (m_languagePacks.isEmpty())
    {
        discoverLanguagePacks(&warningList);
    }

    if (QApplication* application = qobject_cast<QApplication*>(QCoreApplication::instance()))
    {
        if (!m_eventFilterInstalled)
        {
            application->installEventFilter(this);
            m_eventFilterInstalled = true;
        }
        if (!application->property("ks_i18n_proxy_style_installed").toBool())
        {
            application->setStyle(new LanguageProxyStyle(application->style()));
            application->setProperty("ks_i18n_proxy_style_installed", true);
        }
    }

    if (m_languagePacks.isEmpty())
    {
        m_currentLanguageId = QStringLiteral("zh-CN");
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("No valid language packs were found. Built-in text will be used.");
        }
        applyApplicationDirection();
        return false;
    }

    QString requestedId = preferredLanguageId.trimmed();
    if (requestedId.compare(QStringLiteral("system"), Qt::CaseInsensitive) == 0 || requestedId.isEmpty())
    {
        requestedId = QLocale::system().name().replace('_', '-');
    }

    auto hasLanguage = [this](const QString& languageId) {
        return std::any_of(
            m_languagePacks.cbegin(),
            m_languagePacks.cend(),
            [&languageId](const LanguagePack& pack) {
                return pack.info.id.compare(languageId, Qt::CaseInsensitive) == 0;
            });
    };

    if (!hasLanguage(requestedId))
    {
        const QString baseLanguage = requestedId.section('-', 0, 0);
        const auto baseMatch = std::find_if(
            m_languagePacks.cbegin(),
            m_languagePacks.cend(),
            [&baseLanguage](const LanguagePack& pack) {
                return pack.info.id.section('-', 0, 0).compare(baseLanguage, Qt::CaseInsensitive) == 0;
            });
        if (baseMatch != m_languagePacks.cend())
        {
            requestedId = baseMatch->info.id;
        }
        else if (hasLanguage(QStringLiteral("zh-CN")))
        {
            requestedId = QStringLiteral("zh-CN");
        }
        else if (hasLanguage(QStringLiteral("en-US")))
        {
            requestedId = QStringLiteral("en-US");
        }
        else
        {
            requestedId = m_languagePacks.constFirst().info.id;
        }
    }

    const bool applied = setLanguage(requestedId, errorTextOut);
    if (errorTextOut != nullptr && !warningList.isEmpty())
    {
        const QString warningText = warningList.join(QStringLiteral("\n"));
        *errorTextOut = errorTextOut->isEmpty()
            ? warningText
            : (*errorTextOut + QStringLiteral("\n") + warningText);
    }
    return applied;
}

bool ks::i18n::LanguageManager::setLanguage(
    const QString& languageId,
    QString* errorTextOut)
{
    const auto packIterator = std::find_if(
        m_languagePacks.cbegin(),
        m_languagePacks.cend(),
        [&languageId](const LanguagePack& pack) {
            return pack.info.id.compare(languageId.trimmed(), Qt::CaseInsensitive) == 0;
        });
    if (packIterator == m_languagePacks.cend())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("Language pack not found: %1").arg(languageId);
        }
        return false;
    }

    m_currentLanguageId = packIterator->info.id;
    m_runtimeSourceCache.clear();
    applyApplicationDirection();
    retranslateAll();
    if (errorTextOut != nullptr)
    {
        errorTextOut->clear();
    }
    return true;
}

QString ks::i18n::LanguageManager::currentLanguageId() const
{
    return m_currentLanguageId;
}

QList<ks::i18n::LanguageInfo> ks::i18n::LanguageManager::availableLanguages() const
{
    QList<LanguageInfo> languageList;
    languageList.reserve(m_languagePacks.size());
    for (const LanguagePack& pack : m_languagePacks)
    {
        languageList.append(pack.info);
    }
    return languageList;
}

QString ks::i18n::LanguageManager::text(
    const QString& key,
    const QString& fallbackText) const
{
    QStringList visitedLanguageIds;
    return resolveText(m_currentLanguageId, key, fallbackText, &visitedLanguageIds);
}

QString ks::i18n::LanguageManager::translateSource(const QString& sourceText) const
{
    if (sourceText.isEmpty() || m_currentLanguageId.isEmpty())
    {
        return sourceText;
    }

    const QString cacheKey = m_currentLanguageId + QChar(0x001F) + sourceText;
    const auto cachedIterator = m_runtimeSourceCache.constFind(cacheKey);
    if (cachedIterator != m_runtimeSourceCache.constEnd())
    {
        return cachedIterator.value();
    }

    const auto packIterator = std::find_if(
        m_languagePacks.cbegin(),
        m_languagePacks.cend(),
        [this](const LanguagePack& pack) {
            return pack.info.id.compare(m_currentLanguageId, Qt::CaseInsensitive) == 0;
        });
    if (packIterator == m_languagePacks.cend())
    {
        return sourceText;
    }

    QString translatedText = resolveSourceText(*packIterator, sourceText);
    if (translatedText == sourceText && !packIterator->fallbackLanguageId.isEmpty())
    {
        const auto fallbackIterator = std::find_if(
            m_languagePacks.cbegin(),
            m_languagePacks.cend(),
            [&packIterator](const LanguagePack& pack) {
                return pack.info.id.compare(packIterator->fallbackLanguageId, Qt::CaseInsensitive) == 0;
            });
        if (fallbackIterator != m_languagePacks.cend())
        {
            translatedText = resolveSourceText(*fallbackIterator, sourceText);
        }
    }
    m_runtimeSourceCache.insert(cacheKey, translatedText);
    return translatedText;
}

void ks::i18n::LanguageManager::bindText(
    QObject* object,
    const QString& key,
    const QString& fallbackText)
{
    if (object == nullptr)
    {
        return;
    }
    object->setProperty(kTextKeyProperty, key);
    object->setProperty(kTextFallbackProperty, fallbackText);
    applyBindings(object);
}

void ks::i18n::LanguageManager::bindToolTip(
    QWidget* widget,
    const QString& key,
    const QString& fallbackText)
{
    if (widget == nullptr)
    {
        return;
    }
    widget->setProperty(kToolTipKeyProperty, key);
    widget->setProperty(kToolTipFallbackProperty, fallbackText);
    applyBindings(widget);
}

void ks::i18n::LanguageManager::bindPlaceholder(
    QLineEdit* lineEdit,
    const QString& key,
    const QString& fallbackText)
{
    if (lineEdit == nullptr)
    {
        return;
    }
    lineEdit->setProperty(kPlaceholderKeyProperty, key);
    lineEdit->setProperty(kPlaceholderFallbackProperty, fallbackText);
    applyBindings(lineEdit);
}

void ks::i18n::LanguageManager::bindWindowTitle(
    QWidget* widget,
    const QString& key,
    const QString& fallbackText)
{
    if (widget == nullptr)
    {
        return;
    }
    widget->setProperty(kWindowTitleKeyProperty, key);
    widget->setProperty(kWindowTitleFallbackProperty, fallbackText);
    applyBindings(widget);
}

void ks::i18n::LanguageManager::bindTab(
    QTabWidget* tabWidget,
    QWidget* page,
    const QString& key,
    const QString& fallbackText)
{
    if (tabWidget == nullptr || page == nullptr)
    {
        return;
    }
    page->setProperty(kTabKeyProperty, key);
    page->setProperty(kTabFallbackProperty, fallbackText);
    applyBindings(tabWidget);
}

void ks::i18n::LanguageManager::bindComboBoxItem(
    QComboBox* comboBox,
    const int itemIndex,
    const QString& key,
    const QString& fallbackText)
{
    if (comboBox == nullptr || itemIndex < 0 || itemIndex >= comboBox->count())
    {
        return;
    }
    comboBox->setItemData(itemIndex, key, kComboKeyRole);
    comboBox->setItemData(itemIndex, fallbackText, kComboFallbackRole);
    applyBindings(comboBox);
}

void ks::i18n::LanguageManager::retranslateAll()
{
    QApplication* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (application == nullptr)
    {
        return;
    }

    const QWidgetList topLevelWidgetList = application->topLevelWidgets();
    for (QWidget* widget : topLevelWidgetList)
    {
        if (widget == nullptr)
        {
            continue;
        }
        QEvent languageChangeEvent(QEvent::LanguageChange);
        QCoreApplication::sendEvent(widget, &languageChangeEvent);
        applyBindings(widget);
    }
}

bool ks::i18n::LanguageManager::eventFilter(QObject* watchedObject, QEvent* event)
{
    if (watchedObject == nullptr || event == nullptr || m_applyingAutomaticTranslation)
    {
        return QObject::eventFilter(watchedObject, event);
    }

    switch (event->type())
    {
    case QEvent::Show:
    case QEvent::Polish:
    case QEvent::LanguageChange:
        applyBindings(watchedObject);
        break;
    case QEvent::ActionChanged:
    case QEvent::ToolTipChange:
    case QEvent::WindowTitleChange:
        applyAutomaticTranslation(watchedObject);
        break;
    case QEvent::Paint:
        applyAutomaticTranslation(watchedObject);
        // QAbstractScrollArea and QTabWidget paint through child widgets. Translating
        // the direct parent keeps dynamically replaced document/tab text current.
        applyAutomaticTranslation(watchedObject->parent());
        break;
    default:
        break;
    }
    return QObject::eventFilter(watchedObject, event);
}

void ks::i18n::LanguageManager::discoverLanguagePacks(QStringList* warningListOut)
{
    m_languagePacks.clear();
    QSet<QString> loadedLanguageIds;

    const QStringList directoryList = languageDirectoryCandidates();
    for (const QString& directoryPath : directoryList)
    {
        QDir languageDirectory(directoryPath);
        if (!languageDirectory.exists())
        {
            continue;
        }

        const QFileInfoList fileInfoList = languageDirectory.entryInfoList(
            QStringList{ QStringLiteral("*.json") },
            QDir::Files | QDir::Readable,
            QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo& fileInfo : fileInfoList)
        {
            LanguagePack pack;
            QString loadErrorText;
            if (!loadLanguagePack(fileInfo.absoluteFilePath(), &pack, &loadErrorText))
            {
                if (warningListOut != nullptr)
                {
                    warningListOut->append(loadErrorText);
                }
                continue;
            }

            const QString normalizedId = pack.info.id.toLower();
            if (loadedLanguageIds.contains(normalizedId))
            {
                continue;
            }
            loadedLanguageIds.insert(normalizedId);
            m_languagePacks.append(std::move(pack));
        }
    }

    std::sort(
        m_languagePacks.begin(),
        m_languagePacks.end(),
        [](const LanguagePack& left, const LanguagePack& right) {
            return left.info.nativeName.localeAwareCompare(right.info.nativeName) < 0;
        });
}

bool ks::i18n::LanguageManager::loadLanguagePack(
    const QString& filePath,
    LanguagePack* packOut,
    QString* errorTextOut) const
{
    if (packOut == nullptr)
    {
        return false;
    }

    const QFileInfo fileInfo(filePath);
    if (!fileInfo.exists() || !fileInfo.isFile() || fileInfo.size() <= 0 || fileInfo.size() > kMaximumLanguagePackBytes)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("Invalid language pack size: %1").arg(filePath);
        }
        return false;
    }

    QFile packFile(filePath);
    if (!packFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("Cannot open language pack: %1").arg(filePath);
        }
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(packFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("Invalid language pack JSON (%1): %2")
                .arg(filePath, parseError.errorString());
        }
        return false;
    }

    const QJsonObject rootObject = document.object();
    const QString schema = rootObject.value(QStringLiteral("schema")).toString();
    const int formatVersion = rootObject.value(QStringLiteral("format_version")).toInt(-1);
    const QString languageId = rootObject.value(QStringLiteral("id")).toString().trimmed();
    const QString name = rootObject.value(QStringLiteral("name")).toString().trimmed();
    const QString nativeName = rootObject.value(QStringLiteral("native_name")).toString().trimmed();
    const QJsonObject translationObject = rootObject.value(QStringLiteral("translations")).toObject();
    const QJsonObject sourceTranslationObject = rootObject.value(QStringLiteral("source_translations")).toObject();

    if (schema != QString::fromLatin1(kLanguagePackSchema)
        || formatVersion != kLanguagePackFormatVersion
        || !isValidLanguageId(languageId)
        || name.isEmpty()
        || nativeName.isEmpty()
        || translationObject.isEmpty())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("Language pack metadata is invalid: %1").arg(filePath);
        }
        return false;
    }

    LanguagePack loadedPack;
    loadedPack.info.id = languageId;
    loadedPack.info.name = name;
    loadedPack.info.nativeName = nativeName;
    loadedPack.info.author = rootObject.value(QStringLiteral("author")).toString().trimmed();
    loadedPack.info.filePath = QDir::cleanPath(filePath);
    loadedPack.info.rightToLeft = rootObject.value(QStringLiteral("text_direction"))
        .toString(QStringLiteral("ltr"))
        .compare(QStringLiteral("rtl"), Qt::CaseInsensitive) == 0;
    loadedPack.fallbackLanguageId = rootObject.value(QStringLiteral("fallback")).toString().trimmed();
    if (!loadedPack.fallbackLanguageId.isEmpty() && !isValidLanguageId(loadedPack.fallbackLanguageId))
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("Language pack fallback id is invalid: %1").arg(filePath);
        }
        return false;
    }

    for (auto iterator = translationObject.constBegin(); iterator != translationObject.constEnd(); ++iterator)
    {
        if (!iterator.value().isString() || iterator.key().trimmed().isEmpty())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("Language pack contains a non-string translation: %1")
                    .arg(filePath);
            }
            return false;
        }
        loadedPack.translations.insert(iterator.key(), iterator.value().toString());
    }

    for (auto iterator = sourceTranslationObject.constBegin(); iterator != sourceTranslationObject.constEnd(); ++iterator)
    {
        if (!iterator.value().isString() || iterator.key().isEmpty())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("Language pack contains an invalid source translation: %1")
                    .arg(filePath);
            }
            return false;
        }
        loadedPack.sourceTranslations.insert(iterator.key(), iterator.value().toString());
    }
    rebuildSourceIndexes(&loadedPack);

    *packOut = std::move(loadedPack);
    return true;
}

QString ks::i18n::LanguageManager::resolveText(
    const QString& languageId,
    const QString& key,
    const QString& fallbackText,
    QStringList* visitedLanguageIds) const
{
    if (visitedLanguageIds == nullptr || languageId.trimmed().isEmpty())
    {
        return fallbackText.isEmpty() ? key : fallbackText;
    }

    const QString normalizedLanguageId = languageId.trimmed().toLower();
    if (visitedLanguageIds->contains(normalizedLanguageId))
    {
        return fallbackText.isEmpty() ? key : fallbackText;
    }
    visitedLanguageIds->append(normalizedLanguageId);

    const auto packIterator = std::find_if(
        m_languagePacks.cbegin(),
        m_languagePacks.cend(),
        [&normalizedLanguageId](const LanguagePack& pack) {
            return pack.info.id.compare(normalizedLanguageId, Qt::CaseInsensitive) == 0;
        });
    if (packIterator == m_languagePacks.cend())
    {
        return fallbackText.isEmpty() ? key : fallbackText;
    }

    const auto translationIterator = packIterator->translations.constFind(key);
    if (translationIterator != packIterator->translations.constEnd())
    {
        return translationIterator.value();
    }
    if (!packIterator->fallbackLanguageId.isEmpty())
    {
        return resolveText(
            packIterator->fallbackLanguageId,
            key,
            fallbackText,
            visitedLanguageIds);
    }
    return fallbackText.isEmpty() ? key : fallbackText;
}

void ks::i18n::LanguageManager::rebuildSourceIndexes(LanguagePack* pack) const
{
    if (pack == nullptr)
    {
        return;
    }

    pack->sourceTemplates.clear();
    static const QRegularExpression placeholderExpression(QStringLiteral("%(?:L?\\d+|n)"));

    for (auto iterator = pack->sourceTranslations.constBegin(); iterator != pack->sourceTranslations.constEnd(); ++iterator)
    {
        const QString& sourceText = iterator.key();
        QRegularExpressionMatchIterator matchIterator = placeholderExpression.globalMatch(sourceText);
        int cursor = 0;
        QString patternText = QStringLiteral("^");
        QStringList placeholderList;
        while (matchIterator.hasNext())
        {
            const QRegularExpressionMatch placeholderMatch = matchIterator.next();
            patternText += QRegularExpression::escape(sourceText.mid(cursor, placeholderMatch.capturedStart() - cursor));
            patternText += QStringLiteral("(.*?)");
            placeholderList.append(placeholderMatch.captured());
            cursor = placeholderMatch.capturedEnd();
        }

        if (!placeholderList.isEmpty())
        {
            patternText += QRegularExpression::escape(sourceText.mid(cursor));
            patternText += QStringLiteral("$");
            LanguagePack::SourceTemplate sourceTemplate;
            sourceTemplate.expression = QRegularExpression(
                patternText,
                QRegularExpression::DotMatchesEverythingOption);
            sourceTemplate.translatedTemplate = iterator.value();
            sourceTemplate.placeholders = placeholderList;
            if (sourceTemplate.expression.isValid())
            {
                pack->sourceTemplates.append(std::move(sourceTemplate));
            }
        }
    }
}

QString ks::i18n::LanguageManager::resolveSourceText(
    const LanguagePack& pack,
    const QString& sourceText) const
{
    const auto exactIterator = pack.sourceTranslations.constFind(sourceText);
    if (exactIterator != pack.sourceTranslations.constEnd())
    {
        return exactIterator.value();
    }

    for (const LanguagePack::SourceTemplate& sourceTemplate : pack.sourceTemplates)
    {
        const QRegularExpressionMatch match = sourceTemplate.expression.match(sourceText);
        if (!match.hasMatch())
        {
            continue;
        }

        QHash<QString, QString> capturedValues;
        for (int index = 0; index < sourceTemplate.placeholders.size(); ++index)
        {
            const QString& placeholder = sourceTemplate.placeholders.at(index);
            if (!capturedValues.contains(placeholder))
            {
                capturedValues.insert(placeholder, match.captured(index + 1));
            }
        }
        QStringList placeholderKeys = capturedValues.keys();
        std::sort(
            placeholderKeys.begin(),
            placeholderKeys.end(),
            [](const QString& left, const QString& right) {
                return left.size() > right.size();
            });
        QString translatedText = sourceTemplate.translatedTemplate;
        for (const QString& placeholder : placeholderKeys)
        {
            translatedText.replace(placeholder, capturedValues.value(placeholder));
        }
        return translatedText;
    }

    return sourceText;
}

void ks::i18n::LanguageManager::applyBindings(QObject* object) const
{
    if (object == nullptr)
    {
        return;
    }

    applyAutomaticTranslation(object);

    const QString textKey = object->property(kTextKeyProperty).toString();
    if (!textKey.isEmpty())
    {
        const QString translatedText = text(textKey, object->property(kTextFallbackProperty).toString());
        if (QAbstractButton* button = qobject_cast<QAbstractButton*>(object))
        {
            button->setText(translatedText);
        }
        else if (QLabel* label = qobject_cast<QLabel*>(object))
        {
            label->setText(translatedText);
        }
        else if (QGroupBox* groupBox = qobject_cast<QGroupBox*>(object))
        {
            groupBox->setTitle(translatedText);
        }
        else if (QAction* action = qobject_cast<QAction*>(object))
        {
            action->setText(translatedText);
        }
    }

    if (QWidget* widget = qobject_cast<QWidget*>(object))
    {
        const QString toolTipKey = widget->property(kToolTipKeyProperty).toString();
        if (!toolTipKey.isEmpty())
        {
            widget->setToolTip(text(toolTipKey, widget->property(kToolTipFallbackProperty).toString()));
        }

        const QString windowTitleKey = widget->property(kWindowTitleKeyProperty).toString();
        if (!windowTitleKey.isEmpty())
        {
            widget->setWindowTitle(text(
                windowTitleKey,
                widget->property(kWindowTitleFallbackProperty).toString()));
        }
    }

    if (QLineEdit* lineEdit = qobject_cast<QLineEdit*>(object))
    {
        const QString placeholderKey = lineEdit->property(kPlaceholderKeyProperty).toString();
        if (!placeholderKey.isEmpty())
        {
            lineEdit->setPlaceholderText(text(
                placeholderKey,
                lineEdit->property(kPlaceholderFallbackProperty).toString()));
        }
    }

    if (QComboBox* comboBox = qobject_cast<QComboBox*>(object))
    {
        for (int index = 0; index < comboBox->count(); ++index)
        {
            const QString itemKey = comboBox->itemData(index, kComboKeyRole).toString();
            if (!itemKey.isEmpty())
            {
                comboBox->setItemText(index, text(itemKey, comboBox->itemData(index, kComboFallbackRole).toString()));
            }
        }
    }

    if (QTabWidget* tabWidget = qobject_cast<QTabWidget*>(object))
    {
        for (int index = 0; index < tabWidget->count(); ++index)
        {
            QWidget* page = tabWidget->widget(index);
            if (page == nullptr)
            {
                continue;
            }
            const QString tabKey = page->property(kTabKeyProperty).toString();
            if (!tabKey.isEmpty())
            {
                tabWidget->setTabText(index, text(tabKey, page->property(kTabFallbackProperty).toString()));
            }
        }
    }

    const QObjectList childList = object->children();
    for (QObject* childObject : childList)
    {
        applyBindings(childObject);
    }
}

void ks::i18n::LanguageManager::applyAutomaticTranslation(QObject* object) const
{
    if (object == nullptr || m_applyingAutomaticTranslation)
    {
        return;
    }

    m_applyingAutomaticTranslation = true;
    const auto isSourceTextForCurrentLanguage = [this](const QString& value) {
        if (value.isEmpty())
        {
            return false;
        }
        const bool targetIsChinese = m_currentLanguageId.startsWith(QStringLiteral("zh"), Qt::CaseInsensitive);
        return targetIsChinese ? !containsHanText(value) : containsHanText(value);
    };
    const auto applyTrackedText = [this, object, &isSourceTextForCurrentLanguage](
        const QString& currentText,
        const char* sourcePropertyName,
        const char* lastPropertyName,
        const std::function<void(const QString&)>& setter) {
        QString sourceText = object->property(sourcePropertyName).toString();
        const QString lastTranslatedText = object->property(lastPropertyName).toString();

        if (sourceText.isEmpty())
        {
            if (!isSourceTextForCurrentLanguage(currentText))
            {
                return;
            }
            sourceText = currentText;
        }
        else if (currentText != lastTranslatedText && isSourceTextForCurrentLanguage(currentText))
        {
            sourceText = currentText;
        }
        else if (currentText != lastTranslatedText && !isSourceTextForCurrentLanguage(currentText))
        {
            return;
        }

        const QString translatedText = translateSource(sourceText);
        object->setProperty(sourcePropertyName, sourceText);
        object->setProperty(lastPropertyName, translatedText);
        if (translatedText != currentText)
        {
            setter(translatedText);
        }
    };

    if (object->property(kTextKeyProperty).toString().isEmpty())
    {
        if (QAbstractButton* button = qobject_cast<QAbstractButton*>(object))
        {
            applyTrackedText(
                button->text(),
                kAutoTextSourceProperty,
                kAutoTextLastProperty,
                [button](const QString& value) { button->setText(value); });
        }
        else if (QLabel* label = qobject_cast<QLabel*>(object))
        {
            applyTrackedText(
                label->text(),
                kAutoTextSourceProperty,
                kAutoTextLastProperty,
                [label](const QString& value) { label->setText(value); });
        }
        else if (QGroupBox* groupBox = qobject_cast<QGroupBox*>(object))
        {
            applyTrackedText(
                groupBox->title(),
                kAutoTextSourceProperty,
                kAutoTextLastProperty,
                [groupBox](const QString& value) { groupBox->setTitle(value); });
        }
        else if (QAction* action = qobject_cast<QAction*>(object))
        {
            applyTrackedText(
                action->text(),
                kAutoTextSourceProperty,
                kAutoTextLastProperty,
                [action](const QString& value) { action->setText(value); });
        }
    }

    if (QWidget* widget = qobject_cast<QWidget*>(object))
    {
        if (widget->property(kToolTipKeyProperty).toString().isEmpty())
        {
            applyTrackedText(
                widget->toolTip(),
                kAutoToolTipSourceProperty,
                kAutoToolTipLastProperty,
                [widget](const QString& value) { widget->setToolTip(value); });
        }
        if (widget->property(kWindowTitleKeyProperty).toString().isEmpty())
        {
            applyTrackedText(
                widget->windowTitle(),
                kAutoWindowTitleSourceProperty,
                kAutoWindowTitleLastProperty,
                [widget](const QString& value) { widget->setWindowTitle(value); });
        }
    }

    if (QLineEdit* lineEdit = qobject_cast<QLineEdit*>(object))
    {
        if (lineEdit->property(kPlaceholderKeyProperty).toString().isEmpty())
        {
            applyTrackedText(
                lineEdit->placeholderText(),
                kAutoPlaceholderSourceProperty,
                kAutoPlaceholderLastProperty,
                [lineEdit](const QString& value) { lineEdit->setPlaceholderText(value); });
        }
    }

    if (QAction* action = qobject_cast<QAction*>(object))
    {
        applyTrackedText(
            action->toolTip(),
            kAutoToolTipSourceProperty,
            kAutoToolTipLastProperty,
            [action](const QString& value) { action->setToolTip(value); });
    }

    if (QPlainTextEdit* plainTextEdit = qobject_cast<QPlainTextEdit*>(object))
    {
        applyTrackedText(
            plainTextEdit->placeholderText(),
            kAutoPlaceholderSourceProperty,
            kAutoPlaceholderLastProperty,
            [plainTextEdit](const QString& value) { plainTextEdit->setPlaceholderText(value); });
        if (plainTextEdit->isReadOnly())
        {
            applyTrackedText(
                plainTextEdit->toPlainText(),
                kAutoDocumentSourceProperty,
                kAutoDocumentLastProperty,
                [plainTextEdit](const QString& value) { plainTextEdit->setPlainText(value); });
        }
    }

    if (QTextEdit* textEdit = qobject_cast<QTextEdit*>(object))
    {
        applyTrackedText(
            textEdit->placeholderText(),
            kAutoPlaceholderSourceProperty,
            kAutoPlaceholderLastProperty,
            [textEdit](const QString& value) { textEdit->setPlaceholderText(value); });
        if (textEdit->isReadOnly())
        {
            applyTrackedText(
                textEdit->toHtml(),
                kAutoDocumentSourceProperty,
                kAutoDocumentLastProperty,
                [textEdit](const QString& value) { textEdit->setHtml(value); });
        }
    }

    if (QSpinBox* spinBox = qobject_cast<QSpinBox*>(object))
    {
        applyTrackedText(spinBox->prefix(), kAutoPrefixSourceProperty, kAutoPrefixLastProperty,
            [spinBox](const QString& value) { spinBox->setPrefix(value); });
        applyTrackedText(spinBox->suffix(), kAutoSuffixSourceProperty, kAutoSuffixLastProperty,
            [spinBox](const QString& value) { spinBox->setSuffix(value); });
        applyTrackedText(spinBox->specialValueText(), kAutoSpecialSourceProperty, kAutoSpecialLastProperty,
            [spinBox](const QString& value) { spinBox->setSpecialValueText(value); });
    }
    else if (QDoubleSpinBox* spinBox = qobject_cast<QDoubleSpinBox*>(object))
    {
        applyTrackedText(spinBox->prefix(), kAutoPrefixSourceProperty, kAutoPrefixLastProperty,
            [spinBox](const QString& value) { spinBox->setPrefix(value); });
        applyTrackedText(spinBox->suffix(), kAutoSuffixSourceProperty, kAutoSuffixLastProperty,
            [spinBox](const QString& value) { spinBox->setSuffix(value); });
        applyTrackedText(spinBox->specialValueText(), kAutoSpecialSourceProperty, kAutoSpecialLastProperty,
            [spinBox](const QString& value) { spinBox->setSpecialValueText(value); });
    }

    if (QProgressBar* progressBar = qobject_cast<QProgressBar*>(object))
    {
        applyTrackedText(
            progressBar->format(),
            kAutoTextSourceProperty,
            kAutoTextLastProperty,
            [progressBar](const QString& value) { progressBar->setFormat(value); });
    }

    if (QMenu* menu = qobject_cast<QMenu*>(object))
    {
        applyTrackedText(
            menu->title(),
            kAutoTextSourceProperty,
            kAutoTextLastProperty,
            [menu](const QString& value) { menu->setTitle(value); });
    }

    if (QComboBox* comboBox = qobject_cast<QComboBox*>(object))
    {
        for (int index = 0; index < comboBox->count(); ++index)
        {
            if (!comboBox->itemData(index, kComboKeyRole).toString().isEmpty())
            {
                continue;
            }
            const QString currentText = comboBox->itemText(index);
            QString sourceText = comboBox->itemData(index, kAutoComboSourceRole).toString();
            const QString lastText = comboBox->itemData(index, kAutoComboLastRole).toString();
            if (sourceText.isEmpty() && isSourceTextForCurrentLanguage(currentText))
            {
                sourceText = currentText;
            }
            else if (!sourceText.isEmpty() && currentText != lastText && isSourceTextForCurrentLanguage(currentText))
            {
                sourceText = currentText;
            }
            if (sourceText.isEmpty())
            {
                continue;
            }
            const QString translatedText = translateSource(sourceText);
            comboBox->setItemData(index, sourceText, kAutoComboSourceRole);
            comboBox->setItemData(index, translatedText, kAutoComboLastRole);
            if (currentText == lastText || isSourceTextForCurrentLanguage(currentText))
            {
                comboBox->setItemText(index, translatedText);
            }
        }
    }

    if (QTabWidget* tabWidget = qobject_cast<QTabWidget*>(object))
    {
        for (int index = 0; index < tabWidget->count(); ++index)
        {
            QWidget* page = tabWidget->widget(index);
            if (page == nullptr || !page->property(kTabKeyProperty).toString().isEmpty())
            {
                continue;
            }
            const QString currentText = tabWidget->tabText(index);
            QString sourceText = page->property(kAutoTabSourceProperty).toString();
            const QString lastText = page->property(kAutoTabLastProperty).toString();
            if (sourceText.isEmpty() && isSourceTextForCurrentLanguage(currentText))
            {
                sourceText = currentText;
            }
            else if (!sourceText.isEmpty() && currentText != lastText && isSourceTextForCurrentLanguage(currentText))
            {
                sourceText = currentText;
            }
            if (sourceText.isEmpty())
            {
                continue;
            }
            const QString translatedText = translateSource(sourceText);
            page->setProperty(kAutoTabSourceProperty, sourceText);
            page->setProperty(kAutoTabLastProperty, translatedText);
            if (currentText == lastText || isSourceTextForCurrentLanguage(currentText))
            {
                tabWidget->setTabText(index, translatedText);
            }
        }
    }
    m_applyingAutomaticTranslation = false;
}

void ks::i18n::LanguageManager::applyApplicationDirection() const
{
    QApplication* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (application == nullptr)
    {
        return;
    }

    const auto packIterator = std::find_if(
        m_languagePacks.cbegin(),
        m_languagePacks.cend(),
        [this](const LanguagePack& pack) {
            return pack.info.id.compare(m_currentLanguageId, Qt::CaseInsensitive) == 0;
        });
    const bool rightToLeft = packIterator != m_languagePacks.cend() && packIterator->info.rightToLeft;
    application->setLayoutDirection(rightToLeft ? Qt::RightToLeft : Qt::LeftToRight);
}
