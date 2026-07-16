#include "LanguageManager.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QChildEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSet>
#include <QTabWidget>
#include <QSpinBox>
#include <QTableView>
#include <QTextEdit>
#include <QTimer>
#include <QToolBox>
#include <QTreeView>
#include <QVariant>
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
    constexpr auto kSystemLanguagePreferenceId = "system";
    constexpr auto kEnglishFallbackLanguageId = "en-US";
    constexpr auto kProductFallbackLanguageId = "zh-CN";
    constexpr int kComboKeyRole = Qt::UserRole + 91;
    constexpr int kComboFallbackRole = Qt::UserRole + 92;

    constexpr auto kTextKeyProperty = "ks_i18n_text_key";
    constexpr auto kTextFallbackProperty = "ks_i18n_text_fallback";
    constexpr auto kToolTipKeyProperty = "ks_i18n_tooltip_key";
    constexpr auto kToolTipFallbackProperty = "ks_i18n_tooltip_fallback";
    constexpr auto kPlaceholderKeyProperty = "ks_i18n_placeholder_key";
    constexpr auto kPlaceholderFallbackProperty = "ks_i18n_placeholder_fallback";
    constexpr auto kSuffixKeyProperty = "ks_i18n_suffix_key";
    constexpr auto kSuffixFallbackProperty = "ks_i18n_suffix_fallback";
    constexpr auto kWindowTitleKeyProperty = "ks_i18n_window_title_key";
    constexpr auto kWindowTitleFallbackProperty = "ks_i18n_window_title_fallback";
    constexpr auto kTabKeyProperty = "ks_i18n_tab_key";
    constexpr auto kTabFallbackProperty = "ks_i18n_tab_fallback";
    constexpr auto kTabToolTipKeyProperty = "ks_i18n_tab_tooltip_key";
    constexpr auto kTabToolTipFallbackProperty = "ks_i18n_tab_tooltip_fallback";

    constexpr auto kRuntimeRefreshPendingProperty = "ks_i18n_runtime_refresh_pending";
    constexpr auto kRuntimeWindowTitleSourceProperty = "ks_i18n_runtime_window_title_source";
    constexpr auto kRuntimeWindowTitleAppliedProperty = "ks_i18n_runtime_window_title_applied";
    constexpr auto kRuntimeToolTipSourceProperty = "ks_i18n_runtime_tooltip_source";
    constexpr auto kRuntimeToolTipAppliedProperty = "ks_i18n_runtime_tooltip_applied";
    constexpr auto kRuntimeStatusTipSourceProperty = "ks_i18n_runtime_status_tip_source";
    constexpr auto kRuntimeStatusTipAppliedProperty = "ks_i18n_runtime_status_tip_applied";
    constexpr auto kRuntimeWhatsThisSourceProperty = "ks_i18n_runtime_whats_this_source";
    constexpr auto kRuntimeWhatsThisAppliedProperty = "ks_i18n_runtime_whats_this_applied";
    constexpr auto kRuntimeAccessibleNameSourceProperty = "ks_i18n_runtime_accessible_name_source";
    constexpr auto kRuntimeAccessibleNameAppliedProperty = "ks_i18n_runtime_accessible_name_applied";
    constexpr auto kRuntimeAccessibleDescriptionSourceProperty = "ks_i18n_runtime_accessible_description_source";
    constexpr auto kRuntimeAccessibleDescriptionAppliedProperty = "ks_i18n_runtime_accessible_description_applied";
    constexpr auto kRuntimeTextSourceProperty = "ks_i18n_runtime_text_source";
    constexpr auto kRuntimeTextAppliedProperty = "ks_i18n_runtime_text_applied";
    constexpr auto kRuntimeTitleSourceProperty = "ks_i18n_runtime_title_source";
    constexpr auto kRuntimeTitleAppliedProperty = "ks_i18n_runtime_title_applied";
    constexpr auto kRuntimePlaceholderSourceProperty = "ks_i18n_runtime_placeholder_source";
    constexpr auto kRuntimePlaceholderAppliedProperty = "ks_i18n_runtime_placeholder_applied";
    constexpr auto kRuntimePrefixSourceProperty = "ks_i18n_runtime_prefix_source";
    constexpr auto kRuntimePrefixAppliedProperty = "ks_i18n_runtime_prefix_applied";
    constexpr auto kRuntimeSuffixSourceProperty = "ks_i18n_runtime_suffix_source";
    constexpr auto kRuntimeSuffixAppliedProperty = "ks_i18n_runtime_suffix_applied";
    constexpr auto kRuntimeSpecialValueSourceProperty = "ks_i18n_runtime_special_value_source";
    constexpr auto kRuntimeSpecialValueAppliedProperty = "ks_i18n_runtime_special_value_applied";
    constexpr auto kRuntimeTabSourceProperty = "ks_i18n_runtime_tab_source";
    constexpr auto kRuntimeTabAppliedProperty = "ks_i18n_runtime_tab_applied";
    constexpr auto kRuntimeTabToolTipSourceProperty = "ks_i18n_runtime_tab_tooltip_source";
    constexpr auto kRuntimeTabToolTipAppliedProperty = "ks_i18n_runtime_tab_tooltip_applied";
    constexpr int kRuntimeComboSourceRole = Qt::UserRole + 1517;
    constexpr int kRuntimeComboAppliedRole = Qt::UserRole + 1518;
    constexpr int kRuntimeHeaderSourceRole = Qt::UserRole + 1519;
    constexpr int kRuntimeHeaderAppliedRole = Qt::UserRole + 1520;
    constexpr int kRuntimeModelSourceRole = Qt::UserRole + 1521;
    constexpr int kRuntimeModelAppliedRole = Qt::UserRole + 1522;

    const QRegularExpression& sourcePlaceholderExpression()
    {
        static const QRegularExpression expression(
            QStringLiteral("%(?:L?\\d+|n)|\\{\\d+\\}"));
        return expression;
    }

    bool containsHanCharacters(const QString& text)
    {
        static const QRegularExpression expression(
            QStringLiteral("[\\x{3400}-\\x{4DBF}\\x{4E00}-\\x{9FFF}]"));
        return expression.match(text).hasMatch();
    }

    struct ManagedTextResult
    {
        QString sourceText;
        QString appliedText;
        bool setText = false;
        bool updateMetadata = false;
        bool clearMetadata = false;
    };

    ManagedTextResult resolveManagedText(
        const ks::i18n::LanguageManager& languageManager,
        const QString& currentText,
        const QVariant& sourceValue,
        const QVariant& appliedValue,
        const bool allowRenderedSource = true)
    {
        ManagedTextResult result;
        const bool hasSource = sourceValue.isValid();
        const bool isLastAppliedValue = appliedValue.isValid()
            && currentText == appliedValue.toString();

        QString sourceText;
        if (!hasSource)
        {
            if (containsHanCharacters(currentText))
            {
                sourceText = currentText;
            }
            else if (allowRenderedSource)
            {
                sourceText = languageManager.sourceForRenderedText(currentText);
                if (sourceText.isEmpty())
                {
                    return result;
                }
            }
            else
            {
                return result;
            }
        }
        else if (isLastAppliedValue)
        {
            sourceText = sourceValue.toString();
        }
        else
        {
            // A caller replaced a previously translated property. Treat a new
            // Chinese value as the next source string; non-Chinese content is
            // user/runtime data and must no longer be managed by this fallback.
            if (!containsHanCharacters(currentText))
            {
                result.clearMetadata = true;
                return result;
            }
            sourceText = currentText;
        }

        result.sourceText = sourceText;
        result.appliedText = languageManager.sourceText(sourceText);
        if (!hasSource && result.appliedText == sourceText)
        {
            // Unknown Chinese text can be user data (for example a path or a
            // file name). Only manage text for which the active language pack
            // supplies an actual translation.
            return ManagedTextResult{};
        }
        result.setText = currentText != result.appliedText;
        result.updateMetadata = result.setText
            || !sourceValue.isValid()
            || sourceValue.toString() != result.sourceText
            || !appliedValue.isValid()
            || appliedValue.toString() != result.appliedText;
        return result;
    }

    template <typename Setter>
    void applyManagedObjectText(
        const ks::i18n::LanguageManager& languageManager,
        QObject* storageObject,
        const char* sourceProperty,
        const char* appliedProperty,
        const QString& currentText,
        Setter&& setter)
    {
        if (storageObject == nullptr)
        {
            return;
        }

        const ManagedTextResult result = resolveManagedText(
            languageManager,
            currentText,
            storageObject->property(sourceProperty),
            storageObject->property(appliedProperty));
        if (result.clearMetadata)
        {
            storageObject->setProperty(sourceProperty, QVariant());
            storageObject->setProperty(appliedProperty, QVariant());
            return;
        }
        if (!result.updateMetadata)
        {
            return;
        }
        if (result.setText)
        {
            setter(result.appliedText);
        }
        storageObject->setProperty(sourceProperty, result.sourceText);
        storageObject->setProperty(appliedProperty, result.appliedText);
    }

    QObject* runtimeTranslationRoot(QObject* object)
    {
        if (QWidget* widget = qobject_cast<QWidget*>(object))
        {
            return widget->window() != nullptr ? widget->window() : widget;
        }

        QObject* currentObject = object;
        while (currentObject != nullptr)
        {
            if (QWidget* parentWidget = qobject_cast<QWidget*>(currentObject))
            {
                return parentWidget->window() != nullptr ? parentWidget->window() : parentWidget;
            }
            currentObject = currentObject->parent();
        }
        return object;
    }

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

    bool isHistoricalChineseLanguage(const QString& languageId)
    {
        // zh-CN is the checked-in source-language baseline. Other zh-* packs
        // remain extensible and must be allowed to provide their own values.
        return languageId.compare(
            QString::fromLatin1(kProductFallbackLanguageId),
            Qt::CaseInsensitive) == 0;
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

    if (m_languagePacks.isEmpty())
    {
        m_currentLanguageId = QString::fromLatin1(kProductFallbackLanguageId);
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("No valid language packs were found. Built-in text will be used.");
        }
        applyApplicationDirection();
        return false;
    }

    const bool applied = setLanguage(preferredLanguageId, errorTextOut);
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
    const QString resolvedLanguageId = resolvePreferredLanguageId(languageId);
    const auto packIterator = std::find_if(
        m_languagePacks.cbegin(),
        m_languagePacks.cend(),
        [&resolvedLanguageId](const LanguagePack& pack) {
            return pack.info.id.compare(resolvedLanguageId, Qt::CaseInsensitive) == 0;
        });
    if (packIterator == m_languagePacks.cend())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("Language pack not found: %1 (resolved from %2)")
                .arg(resolvedLanguageId, languageId);
        }
        return false;
    }

    const bool languageWillChange = !m_currentLanguageId.isEmpty()
        && m_currentLanguageId.compare(packIterator->info.id, Qt::CaseInsensitive) != 0;
    if (languageWillChange)
    {
        // Capture the canonical source for controls that were constructed from
        // an already-rendered English context translation before changing the
        // active pack. Their runtime metadata then makes the switch reversible.
        QApplication* application = qobject_cast<QApplication*>(QCoreApplication::instance());
        if (application != nullptr)
        {
            const QWidgetList topLevelWidgetList = application->topLevelWidgets();
            for (QWidget* widget : topLevelWidgetList)
            {
                applyRuntimeTranslations(widget);
            }
        }
    }

    m_currentLanguageId = packIterator->info.id;
    ensureApplicationEventFilter();
    applyApplicationDirection();
    retranslateAll();
    if (errorTextOut != nullptr)
    {
        errorTextOut->clear();
    }
    return true;
}

QString ks::i18n::LanguageManager::resolvePreferredLanguageId(
    const QString& preferredLanguageId) const
{
    QString requestedLanguageId = preferredLanguageId.trimmed();
    if (requestedLanguageId.isEmpty()
        || requestedLanguageId.compare(
            QString::fromLatin1(kSystemLanguagePreferenceId),
            Qt::CaseInsensitive) == 0)
    {
        requestedLanguageId = QLocale::system().name();
    }
    requestedLanguageId.replace('_', '-');

    const auto findExactLanguage = [this](const QString& candidateLanguageId) {
        return std::find_if(
            m_languagePacks.cbegin(),
            m_languagePacks.cend(),
            [&candidateLanguageId](const LanguagePack& pack) {
                return pack.info.id.compare(candidateLanguageId, Qt::CaseInsensitive) == 0;
            });
    };

    // Locale negotiation is deterministic:
    // exact region -> base language -> en-US -> KSword's product fallback.
    auto languageMatch = findExactLanguage(requestedLanguageId);
    if (languageMatch != m_languagePacks.cend())
    {
        return languageMatch->info.id;
    }

    const QString baseLanguageId = requestedLanguageId.section('-', 0, 0);
    languageMatch = findExactLanguage(baseLanguageId);
    if (languageMatch == m_languagePacks.cend() && !baseLanguageId.isEmpty())
    {
        languageMatch = std::find_if(
            m_languagePacks.cbegin(),
            m_languagePacks.cend(),
            [&baseLanguageId](const LanguagePack& pack) {
                return pack.info.id.section('-', 0, 0).compare(
                    baseLanguageId,
                    Qt::CaseInsensitive) == 0;
            });
    }
    if (languageMatch != m_languagePacks.cend())
    {
        return languageMatch->info.id;
    }

    languageMatch = findExactLanguage(QString::fromLatin1(kEnglishFallbackLanguageId));
    if (languageMatch != m_languagePacks.cend())
    {
        return languageMatch->info.id;
    }

    languageMatch = findExactLanguage(QString::fromLatin1(kProductFallbackLanguageId));
    if (languageMatch != m_languagePacks.cend())
    {
        return languageMatch->info.id;
    }

    // A damaged/custom installation may omit both required fallback packs.
    // Keep the UI usable with the first validated pack; normal builds always
    // stop at the product fallback above.
    return m_languagePacks.isEmpty()
        ? QString::fromLatin1(kProductFallbackLanguageId)
        : m_languagePacks.constFirst().info.id;
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

QString ks::i18n::LanguageManager::contextText(
    const QString& contextKey,
    const QString& sourceText) const
{
    if (contextKey.trimmed().isEmpty() || sourceText.isEmpty())
    {
        return sourceText;
    }

    // Chinese is the historical source language. Returning the call-site
    // fallback makes a language switch incapable of rewriting the old UI.
    if (isHistoricalChineseLanguage(m_currentLanguageId))
    {
        return sourceText;
    }

    QStringList visitedLanguageIds;
    return resolveContextText(m_currentLanguageId, contextKey, sourceText, &visitedLanguageIds);
}

QString ks::i18n::LanguageManager::sourceText(const QString& sourceText) const
{
    if (sourceText.isEmpty() || isHistoricalChineseLanguage(m_currentLanguageId))
    {
        return sourceText;
    }

    QStringList visitedLanguageIds;
    return resolveSourceText(m_currentLanguageId, sourceText, &visitedLanguageIds);
}

QString ks::i18n::LanguageManager::sourceForRenderedText(const QString& renderedText) const
{
    if (renderedText.isEmpty() || containsHanCharacters(renderedText))
    {
        return {};
    }

    QString resolvedSource;
    for (const LanguagePack& pack : m_languagePacks)
    {
        const auto iterator = pack.renderedSources.constFind(renderedText);
        if (iterator == pack.renderedSources.constEnd())
        {
            continue;
        }
        if (!resolvedSource.isEmpty() && resolvedSource != iterator.value())
        {
            return {};
        }
        resolvedSource = iterator.value();
    }
    return resolvedSource;
}

QString ks::i18n::LanguageManager::displayText(const QString& renderedOrSourceText) const
{
    if (renderedOrSourceText.isEmpty())
    {
        return renderedOrSourceText;
    }
    if (containsHanCharacters(renderedOrSourceText))
    {
        return sourceText(renderedOrSourceText);
    }

    const QString canonicalSource = sourceForRenderedText(renderedOrSourceText);
    return canonicalSource.isEmpty() ? renderedOrSourceText : sourceText(canonicalSource);
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

void ks::i18n::LanguageManager::bindSuffix(
    QSpinBox* spinBox,
    const QString& key,
    const QString& fallbackText)
{
    if (spinBox == nullptr)
    {
        return;
    }
    spinBox->setProperty(kSuffixKeyProperty, key);
    spinBox->setProperty(kSuffixFallbackProperty, fallbackText);
    applyBindings(spinBox);
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

void ks::i18n::LanguageManager::bindTabToolTip(
    QTabWidget* tabWidget,
    QWidget* page,
    const QString& key,
    const QString& fallbackText)
{
    if (tabWidget == nullptr || page == nullptr || tabWidget->indexOf(page) < 0)
    {
        return;
    }
    page->setProperty(kTabToolTipKeyProperty, key);
    page->setProperty(kTabToolTipFallbackProperty, fallbackText);
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
        QList<QPointer<QWidget>> widgetList;
        widgetList.append(QPointer<QWidget>(widget));
        const QList<QWidget*> childWidgetList = widget->findChildren<QWidget*>();
        widgetList.reserve(widgetList.size() + childWidgetList.size());
        for (QWidget* childWidget : childWidgetList)
        {
            widgetList.append(QPointer<QWidget>(childWidget));
        }

        // Qt normally delivers LanguageChange to top-level widgets. KSword
        // contains many nested, independently implemented pages, so deliver the
        // event to every live widget as well. This keeps already-created lazy
        // pages, tables, charts, and dialogs in sync without a restart.
        for (const QPointer<QWidget>& targetWidget : widgetList)
        {
            if (targetWidget.isNull())
            {
                continue;
            }
            QEvent languageChangeEvent(QEvent::LanguageChange);
            QCoreApplication::sendEvent(targetWidget.data(), &languageChangeEvent);
        }
        applyBindings(widget);
        applyRuntimeTranslations(widget);
    }
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
    const QJsonObject contextTranslationObject = rootObject.value(QStringLiteral("context_translations")).toObject();
    const QJsonValue sourceTranslationValue = rootObject.value(QStringLiteral("source_translations"));
    const QJsonObject sourceTranslationObject = sourceTranslationValue.toObject();

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
    if (!sourceTranslationValue.isUndefined() && !sourceTranslationValue.isObject())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("Language pack source_translations is invalid: %1").arg(filePath);
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

    for (auto iterator = contextTranslationObject.constBegin(); iterator != contextTranslationObject.constEnd(); ++iterator)
    {
        if (!iterator.value().isString() || iterator.key().trimmed().isEmpty())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("Language pack contains an invalid context translation: %1")
                    .arg(filePath);
            }
            return false;
        }
        loadedPack.contextTranslations.insert(iterator.key(), iterator.value().toString());
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

    // Keep only unambiguous rendered-text -> Chinese-source relationships.
    // This allows already-created English controls to switch back immediately
    // without guessing when two source strings share the same English text.
    QSet<QString> ambiguousRenderedTexts;
    for (auto iterator = loadedPack.sourceTranslations.constBegin();
         iterator != loadedPack.sourceTranslations.constEnd();
         ++iterator)
    {
        const QString renderedText = iterator.value();
        if (renderedText.isEmpty() || renderedText == iterator.key())
        {
            continue;
        }
        const auto existing = loadedPack.renderedSources.constFind(renderedText);
        if (existing != loadedPack.renderedSources.constEnd() && existing.value() != iterator.key())
        {
            ambiguousRenderedTexts.insert(renderedText);
            continue;
        }
        loadedPack.renderedSources.insert(renderedText, iterator.key());
    }
    for (const QString& ambiguousText : ambiguousRenderedTexts)
    {
        loadedPack.renderedSources.remove(ambiguousText);
    }

    // Compile rendered-text fallbacks for source strings that contain Qt/.NET
    // placeholders. This lets a label such as "状态：3 项" switch immediately
    // even when the original call site formatted the Chinese text before it
    // reached the widget.
    for (auto iterator = loadedPack.sourceTranslations.constBegin();
         iterator != loadedPack.sourceTranslations.constEnd();
         ++iterator)
    {
        const QString sourcePattern = iterator.key();
        if (!containsHanCharacters(sourcePattern))
        {
            continue;
        }

        QRegularExpressionMatchIterator placeholderIterator =
            sourcePlaceholderExpression().globalMatch(sourcePattern);
        if (!placeholderIterator.hasNext())
        {
            continue;
        }

        LanguagePack::SourceTemplate sourceTemplate;
        sourceTemplate.translatedPattern = iterator.value();
        QString expressionText = QStringLiteral("^");
        int previousEnd = 0;
        int captureGroup = 1;
        int firstPlaceholderStart = -1;
        int lastPlaceholderEnd = -1;
        while (placeholderIterator.hasNext())
        {
            const QRegularExpressionMatch placeholderMatch = placeholderIterator.next();
            if (firstPlaceholderStart < 0)
            {
                firstPlaceholderStart = placeholderMatch.capturedStart();
            }
            lastPlaceholderEnd = placeholderMatch.capturedEnd();

            const QString literalPart = sourcePattern.mid(
                previousEnd,
                placeholderMatch.capturedStart() - previousEnd);
            expressionText += QRegularExpression::escape(literalPart);
            sourceTemplate.literalLength += literalPart.size();
            expressionText += QStringLiteral("([\\s\\S]*?)");

            const QString placeholder = placeholderMatch.captured();
            if (!sourceTemplate.placeholderCaptureGroups.contains(placeholder))
            {
                sourceTemplate.placeholderCaptureGroups.insert(placeholder, captureGroup);
            }
            ++captureGroup;
            previousEnd = placeholderMatch.capturedEnd();
        }

        const QString trailingLiteral = sourcePattern.mid(previousEnd);
        expressionText += QRegularExpression::escape(trailingLiteral);
        sourceTemplate.literalLength += trailingLiteral.size();
        expressionText += QStringLiteral("$");
        sourceTemplate.requiredPrefix = sourcePattern.left(firstPlaceholderStart);
        sourceTemplate.requiredSuffix = sourcePattern.mid(lastPlaceholderEnd);
        sourceTemplate.expression = QRegularExpression(expressionText);
        if (sourceTemplate.expression.isValid())
        {
            loadedPack.sourceTemplates.append(std::move(sourceTemplate));
        }
    }

    std::sort(
        loadedPack.sourceTemplates.begin(),
        loadedPack.sourceTemplates.end(),
        [](const LanguagePack::SourceTemplate& left, const LanguagePack::SourceTemplate& right) {
            return left.literalLength > right.literalLength;
        });

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

    // zh-CN is the source-language baseline. The fallback at the original
    // call site is authoritative so a generated/edited pack cannot alter it.
    if (isHistoricalChineseLanguage(normalizedLanguageId) && !fallbackText.isEmpty())
    {
        return fallbackText;
    }

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
    const auto contextIterator = packIterator->contextTranslations.constFind(key);
    if (contextIterator != packIterator->contextTranslations.constEnd())
    {
        return contextIterator.value();
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

QString ks::i18n::LanguageManager::resolveContextText(
    const QString& languageId,
    const QString& contextKey,
    const QString& sourceText,
    QStringList* visitedLanguageIds) const
{
    if (visitedLanguageIds == nullptr || languageId.trimmed().isEmpty())
    {
        return sourceText;
    }

    const QString normalizedLanguageId = languageId.trimmed().toLower();
    if (visitedLanguageIds->contains(normalizedLanguageId))
    {
        return sourceText;
    }
    visitedLanguageIds->append(normalizedLanguageId);

    // Keep the historical Chinese fallback authoritative even when a third
    // language reaches zh-CN through its fallback chain.
    if (isHistoricalChineseLanguage(normalizedLanguageId))
    {
        return sourceText;
    }

    const auto packIterator = std::find_if(
        m_languagePacks.cbegin(),
        m_languagePacks.cend(),
        [&normalizedLanguageId](const LanguagePack& pack) {
            return pack.info.id.compare(normalizedLanguageId, Qt::CaseInsensitive) == 0;
        });
    if (packIterator == m_languagePacks.cend())
    {
        return sourceText;
    }

    const auto translationIterator = packIterator->contextTranslations.constFind(contextKey);
    if (translationIterator != packIterator->contextTranslations.constEnd())
    {
        return translationIterator.value();
    }
    if (!packIterator->fallbackLanguageId.isEmpty())
    {
        return resolveContextText(
            packIterator->fallbackLanguageId,
            contextKey,
            sourceText,
            visitedLanguageIds);
    }
    return sourceText;
}

QString ks::i18n::LanguageManager::resolveSourceText(
    const QString& languageId,
    const QString& sourceText,
    QStringList* visitedLanguageIds) const
{
    if (visitedLanguageIds == nullptr || languageId.trimmed().isEmpty() || sourceText.isEmpty())
    {
        return sourceText;
    }

    const QString normalizedLanguageId = languageId.trimmed().toLower();
    if (visitedLanguageIds->contains(normalizedLanguageId))
    {
        return sourceText;
    }
    visitedLanguageIds->append(normalizedLanguageId);

    if (isHistoricalChineseLanguage(normalizedLanguageId))
    {
        return sourceText;
    }

    const auto packIterator = std::find_if(
        m_languagePacks.cbegin(),
        m_languagePacks.cend(),
        [&normalizedLanguageId](const LanguagePack& pack) {
            return pack.info.id.compare(normalizedLanguageId, Qt::CaseInsensitive) == 0;
        });
    if (packIterator == m_languagePacks.cend())
    {
        return sourceText;
    }

    const auto translationIterator = packIterator->sourceTranslations.constFind(sourceText);
    if (translationIterator != packIterator->sourceTranslations.constEnd())
    {
        return translationIterator.value();
    }

    if (containsHanCharacters(sourceText))
    {
        for (const LanguagePack::SourceTemplate& sourceTemplate : packIterator->sourceTemplates)
        {
            if (!sourceTemplate.requiredPrefix.isEmpty()
                && !sourceText.startsWith(sourceTemplate.requiredPrefix))
            {
                continue;
            }
            if (!sourceTemplate.requiredSuffix.isEmpty()
                && !sourceText.endsWith(sourceTemplate.requiredSuffix))
            {
                continue;
            }

            const QRegularExpressionMatch sourceMatch = sourceTemplate.expression.match(sourceText);
            if (!sourceMatch.hasMatch())
            {
                continue;
            }

            QString translatedText;
            int previousEnd = 0;
            QRegularExpressionMatchIterator translatedPlaceholderIterator =
                sourcePlaceholderExpression().globalMatch(sourceTemplate.translatedPattern);
            while (translatedPlaceholderIterator.hasNext())
            {
                const QRegularExpressionMatch placeholderMatch = translatedPlaceholderIterator.next();
                translatedText += sourceTemplate.translatedPattern.mid(
                    previousEnd,
                    placeholderMatch.capturedStart() - previousEnd);
                const int captureGroup = sourceTemplate.placeholderCaptureGroups.value(
                    placeholderMatch.captured(),
                    -1);
                if (captureGroup < 0)
                {
                    translatedText.clear();
                    break;
                }
                translatedText += sourceMatch.captured(captureGroup);
                previousEnd = placeholderMatch.capturedEnd();
            }
            if (!translatedText.isNull())
            {
                translatedText += sourceTemplate.translatedPattern.mid(previousEnd);
                return translatedText;
            }
        }
    }

    if (!packIterator->fallbackLanguageId.isEmpty())
    {
        return resolveSourceText(
            packIterator->fallbackLanguageId,
            sourceText,
            visitedLanguageIds);
    }
    return sourceText;
}

void ks::i18n::LanguageManager::ensureApplicationEventFilter()
{
    if (m_applicationEventFilterInstalled)
    {
        return;
    }

    QApplication* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (application == nullptr)
    {
        return;
    }
    application->installEventFilter(this);
    m_applicationEventFilterInstalled = true;
}

void ks::i18n::LanguageManager::scheduleRuntimeTranslation(QObject* object)
{
    if (object == nullptr || m_applyingRuntimeTranslations
        || object->property(kRuntimeRefreshPendingProperty).toBool())
    {
        return;
    }

    object->setProperty(kRuntimeRefreshPendingProperty, true);
    const QPointer<QObject> guardedObject(object);
    QTimer::singleShot(0, this, [this, guardedObject]() {
        if (guardedObject.isNull())
        {
            return;
        }
        guardedObject->setProperty(kRuntimeRefreshPendingProperty, false);
        applyRuntimeTranslations(guardedObject.data());
    });
}

bool ks::i18n::LanguageManager::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == nullptr || event == nullptr || m_applyingRuntimeTranslations
        || isHistoricalChineseLanguage(m_currentLanguageId))
    {
        return QObject::eventFilter(watched, event);
    }

    switch (event->type())
    {
    case QEvent::ChildAdded:
    {
        QChildEvent* childEvent = static_cast<QChildEvent*>(event);
        QObject* targetObject = childEvent->child() != nullptr ? childEvent->child() : watched;
        scheduleRuntimeTranslation(runtimeTranslationRoot(targetObject));
        break;
    }
    case QEvent::Show:
    case QEvent::PolishRequest:
    case QEvent::ActionAdded:
    case QEvent::ActionChanged:
        scheduleRuntimeTranslation(runtimeTranslationRoot(watched));
        break;
    case QEvent::LayoutRequest:
    case QEvent::UpdateRequest:
    {
        QObject* targetObject = watched;
        if (qobject_cast<QHeaderView*>(watched) != nullptr)
        {
            QObject* parentObject = watched->parent();
            while (parentObject != nullptr && qobject_cast<QAbstractItemView*>(parentObject) == nullptr)
            {
                parentObject = parentObject->parent();
            }
            if (parentObject != nullptr)
            {
                targetObject = parentObject;
            }
        }
        scheduleRuntimeTranslation(targetObject);
        break;
    }
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void ks::i18n::LanguageManager::applyRuntimeTranslations(QObject* object)
{
    if (object == nullptr || m_applyingRuntimeTranslations)
    {
        return;
    }

    QScopedValueRollback<bool> translationGuard(m_applyingRuntimeTranslations, true);
    std::function<void(QAbstractItemModel*, const QModelIndex&, int&)> translateModelItems;
    translateModelItems = [this, &translateModelItems](
                              QAbstractItemModel* model,
                              const QModelIndex& parentIndex,
                              int& remainingIndexBudget) {
        if (model == nullptr || remainingIndexBudget <= 0)
        {
            return;
        }

        const int rowCount = model->rowCount(parentIndex);
        const int columnCount = model->columnCount(parentIndex);
        for (int row = 0; row < rowCount && remainingIndexBudget > 0; ++row)
        {
            for (int column = 0; column < columnCount && remainingIndexBudget > 0; ++column)
            {
                const QModelIndex index = model->index(row, column, parentIndex);
                --remainingIndexBudget;
                if (!index.isValid())
                {
                    continue;
                }

                const QVariant sourceValue = model->data(index, kRuntimeModelSourceRole);
                const QVariant appliedValue = model->data(index, kRuntimeModelAppliedRole);
                const ManagedTextResult result = resolveManagedText(
                    *this,
                    model->data(index, Qt::DisplayRole).toString(),
                    sourceValue,
                    appliedValue,
                    false);
                if (result.clearMetadata)
                {
                    model->setData(index, QVariant(), kRuntimeModelSourceRole);
                    model->setData(index, QVariant(), kRuntimeModelAppliedRole);
                    continue;
                }
                if (!result.updateMetadata)
                {
                    continue;
                }

                // Do not mutate read-only/custom models that reject our
                // private roles. This keeps runtime/user data untouched.
                if (!model->setData(index, result.sourceText, kRuntimeModelSourceRole))
                {
                    continue;
                }
                if (!model->setData(index, result.appliedText, kRuntimeModelAppliedRole))
                {
                    model->setData(index, QVariant(), kRuntimeModelSourceRole);
                    continue;
                }
                if (result.setText
                    && !model->setData(index, result.appliedText, Qt::DisplayRole))
                {
                    model->setData(index, QVariant(), kRuntimeModelSourceRole);
                    model->setData(index, QVariant(), kRuntimeModelAppliedRole);
                }
            }

            const QModelIndex childParent = model->index(row, 0, parentIndex);
            if (childParent.isValid() && model->hasChildren(childParent))
            {
                translateModelItems(model, childParent, remainingIndexBudget);
            }
        }
    };

    std::function<void(QObject*)> visitObject;
    visitObject = [this, &translateModelItems, &visitObject](QObject* currentObject) {
        if (currentObject == nullptr)
        {
            return;
        }

        if (QAction* action = qobject_cast<QAction*>(currentObject))
        {
            applyManagedObjectText(
                *this,
                action,
                kRuntimeTextSourceProperty,
                kRuntimeTextAppliedProperty,
                action->text(),
                [action](const QString& value) { action->setText(value); });
            applyManagedObjectText(
                *this,
                action,
                kRuntimeToolTipSourceProperty,
                kRuntimeToolTipAppliedProperty,
                action->toolTip(),
                [action](const QString& value) { action->setToolTip(value); });
            applyManagedObjectText(
                *this,
                action,
                kRuntimeStatusTipSourceProperty,
                kRuntimeStatusTipAppliedProperty,
                action->statusTip(),
                [action](const QString& value) { action->setStatusTip(value); });
            applyManagedObjectText(
                *this,
                action,
                kRuntimeWhatsThisSourceProperty,
                kRuntimeWhatsThisAppliedProperty,
                action->whatsThis(),
                [action](const QString& value) { action->setWhatsThis(value); });
        }

        if (QWidget* widget = qobject_cast<QWidget*>(currentObject))
        {
            applyManagedObjectText(
                *this,
                widget,
                kRuntimeWindowTitleSourceProperty,
                kRuntimeWindowTitleAppliedProperty,
                widget->windowTitle(),
                [widget](const QString& value) { widget->setWindowTitle(value); });
            applyManagedObjectText(
                *this,
                widget,
                kRuntimeToolTipSourceProperty,
                kRuntimeToolTipAppliedProperty,
                widget->toolTip(),
                [widget](const QString& value) { widget->setToolTip(value); });
            applyManagedObjectText(
                *this,
                widget,
                kRuntimeStatusTipSourceProperty,
                kRuntimeStatusTipAppliedProperty,
                widget->statusTip(),
                [widget](const QString& value) { widget->setStatusTip(value); });
            applyManagedObjectText(
                *this,
                widget,
                kRuntimeWhatsThisSourceProperty,
                kRuntimeWhatsThisAppliedProperty,
                widget->whatsThis(),
                [widget](const QString& value) { widget->setWhatsThis(value); });
            applyManagedObjectText(
                *this,
                widget,
                kRuntimeAccessibleNameSourceProperty,
                kRuntimeAccessibleNameAppliedProperty,
                widget->accessibleName(),
                [widget](const QString& value) { widget->setAccessibleName(value); });
            applyManagedObjectText(
                *this,
                widget,
                kRuntimeAccessibleDescriptionSourceProperty,
                kRuntimeAccessibleDescriptionAppliedProperty,
                widget->accessibleDescription(),
                [widget](const QString& value) { widget->setAccessibleDescription(value); });
        }

        if (QLabel* label = qobject_cast<QLabel*>(currentObject))
        {
            applyManagedObjectText(
                *this,
                label,
                kRuntimeTextSourceProperty,
                kRuntimeTextAppliedProperty,
                label->text(),
                [label](const QString& value) { label->setText(value); });
        }
        else if (QAbstractButton* button = qobject_cast<QAbstractButton*>(currentObject))
        {
            applyManagedObjectText(
                *this,
                button,
                kRuntimeTextSourceProperty,
                kRuntimeTextAppliedProperty,
                button->text(),
                [button](const QString& value) { button->setText(value); });
        }

        if (QGroupBox* groupBox = qobject_cast<QGroupBox*>(currentObject))
        {
            applyManagedObjectText(
                *this,
                groupBox,
                kRuntimeTitleSourceProperty,
                kRuntimeTitleAppliedProperty,
                groupBox->title(),
                [groupBox](const QString& value) { groupBox->setTitle(value); });
        }
        if (QMenu* menu = qobject_cast<QMenu*>(currentObject))
        {
            applyManagedObjectText(
                *this,
                menu,
                kRuntimeTitleSourceProperty,
                kRuntimeTitleAppliedProperty,
                menu->title(),
                [menu](const QString& value) { menu->setTitle(value); });
        }
        if (QLineEdit* lineEdit = qobject_cast<QLineEdit*>(currentObject))
        {
            applyManagedObjectText(
                *this,
                lineEdit,
                kRuntimePlaceholderSourceProperty,
                kRuntimePlaceholderAppliedProperty,
                lineEdit->placeholderText(),
                [lineEdit](const QString& value) { lineEdit->setPlaceholderText(value); });
        }
        if (QPlainTextEdit* plainTextEdit = qobject_cast<QPlainTextEdit*>(currentObject))
        {
            applyManagedObjectText(
                *this,
                plainTextEdit,
                kRuntimePlaceholderSourceProperty,
                kRuntimePlaceholderAppliedProperty,
                plainTextEdit->placeholderText(),
                [plainTextEdit](const QString& value) { plainTextEdit->setPlaceholderText(value); });
        }
        if (QTextEdit* textEdit = qobject_cast<QTextEdit*>(currentObject))
        {
            applyManagedObjectText(
                *this,
                textEdit,
                kRuntimePlaceholderSourceProperty,
                kRuntimePlaceholderAppliedProperty,
                textEdit->placeholderText(),
                [textEdit](const QString& value) { textEdit->setPlaceholderText(value); });
        }

        const auto translateSpinBox = [this](auto* spinBox) {
            applyManagedObjectText(
                *this,
                spinBox,
                kRuntimePrefixSourceProperty,
                kRuntimePrefixAppliedProperty,
                spinBox->prefix(),
                [spinBox](const QString& value) { spinBox->setPrefix(value); });
            applyManagedObjectText(
                *this,
                spinBox,
                kRuntimeSuffixSourceProperty,
                kRuntimeSuffixAppliedProperty,
                spinBox->suffix(),
                [spinBox](const QString& value) { spinBox->setSuffix(value); });
            applyManagedObjectText(
                *this,
                spinBox,
                kRuntimeSpecialValueSourceProperty,
                kRuntimeSpecialValueAppliedProperty,
                spinBox->specialValueText(),
                [spinBox](const QString& value) { spinBox->setSpecialValueText(value); });
        };
        if (QSpinBox* spinBox = qobject_cast<QSpinBox*>(currentObject))
        {
            translateSpinBox(spinBox);
        }
        else if (QDoubleSpinBox* doubleSpinBox = qobject_cast<QDoubleSpinBox*>(currentObject))
        {
            translateSpinBox(doubleSpinBox);
        }

        if (QComboBox* comboBox = qobject_cast<QComboBox*>(currentObject))
        {
            applyManagedObjectText(
                *this,
                comboBox,
                kRuntimePlaceholderSourceProperty,
                kRuntimePlaceholderAppliedProperty,
                comboBox->placeholderText(),
                [comboBox](const QString& value) { comboBox->setPlaceholderText(value); });
            for (int itemIndex = 0; itemIndex < comboBox->count(); ++itemIndex)
            {
                const ManagedTextResult result = resolveManagedText(
                    *this,
                    comboBox->itemText(itemIndex),
                    comboBox->itemData(itemIndex, kRuntimeComboSourceRole),
                    comboBox->itemData(itemIndex, kRuntimeComboAppliedRole));
                if (result.clearMetadata)
                {
                    comboBox->setItemData(itemIndex, QVariant(), kRuntimeComboSourceRole);
                    comboBox->setItemData(itemIndex, QVariant(), kRuntimeComboAppliedRole);
                    continue;
                }
                if (!result.updateMetadata)
                {
                    continue;
                }
                if (result.setText)
                {
                    comboBox->setItemText(itemIndex, result.appliedText);
                }
                comboBox->setItemData(itemIndex, result.sourceText, kRuntimeComboSourceRole);
                comboBox->setItemData(itemIndex, result.appliedText, kRuntimeComboAppliedRole);
            }
        }

        if (QTabWidget* tabWidget = qobject_cast<QTabWidget*>(currentObject))
        {
            for (int tabIndex = 0; tabIndex < tabWidget->count(); ++tabIndex)
            {
                QWidget* page = tabWidget->widget(tabIndex);
                if (page == nullptr)
                {
                    continue;
                }
                applyManagedObjectText(
                    *this,
                    page,
                    kRuntimeTabSourceProperty,
                    kRuntimeTabAppliedProperty,
                    tabWidget->tabText(tabIndex),
                    [tabWidget, tabIndex](const QString& value) { tabWidget->setTabText(tabIndex, value); });
                applyManagedObjectText(
                    *this,
                    page,
                    kRuntimeTabToolTipSourceProperty,
                    kRuntimeTabToolTipAppliedProperty,
                    tabWidget->tabToolTip(tabIndex),
                    [tabWidget, tabIndex](const QString& value) { tabWidget->setTabToolTip(tabIndex, value); });
            }
        }
        if (QToolBox* toolBox = qobject_cast<QToolBox*>(currentObject))
        {
            for (int itemIndex = 0; itemIndex < toolBox->count(); ++itemIndex)
            {
                QWidget* page = toolBox->widget(itemIndex);
                if (page == nullptr)
                {
                    continue;
                }
                applyManagedObjectText(
                    *this,
                    page,
                    kRuntimeTabSourceProperty,
                    kRuntimeTabAppliedProperty,
                    toolBox->itemText(itemIndex),
                    [toolBox, itemIndex](const QString& value) { toolBox->setItemText(itemIndex, value); });
                applyManagedObjectText(
                    *this,
                    page,
                    kRuntimeTabToolTipSourceProperty,
                    kRuntimeTabToolTipAppliedProperty,
                    toolBox->itemToolTip(itemIndex),
                    [toolBox, itemIndex](const QString& value) { toolBox->setItemToolTip(itemIndex, value); });
            }
        }

        if (QTableView* tableView = qobject_cast<QTableView*>(currentObject))
        {
            QAbstractItemModel* model = tableView->model();
            if (model != nullptr)
            {
                const int columnCount = model->columnCount(tableView->rootIndex());
                for (int section = 0; section < columnCount; ++section)
                {
                    const QString currentHeader = model->headerData(
                        section,
                        Qt::Horizontal,
                        Qt::DisplayRole).toString();
                    const ManagedTextResult result = resolveManagedText(
                        *this,
                        currentHeader,
                        model->headerData(section, Qt::Horizontal, kRuntimeHeaderSourceRole),
                        model->headerData(section, Qt::Horizontal, kRuntimeHeaderAppliedRole));
                    if (result.clearMetadata)
                    {
                        model->setHeaderData(section, Qt::Horizontal, QVariant(), kRuntimeHeaderSourceRole);
                        model->setHeaderData(section, Qt::Horizontal, QVariant(), kRuntimeHeaderAppliedRole);
                        continue;
                    }
                    if (!result.updateMetadata)
                    {
                        continue;
                    }
                    if (result.setText
                        && !model->setHeaderData(section, Qt::Horizontal, result.appliedText, Qt::DisplayRole))
                    {
                        continue;
                    }
                    model->setHeaderData(section, Qt::Horizontal, result.sourceText, kRuntimeHeaderSourceRole);
                    model->setHeaderData(section, Qt::Horizontal, result.appliedText, kRuntimeHeaderAppliedRole);
                }
                int remainingIndexBudget = 50000;
                translateModelItems(model, tableView->rootIndex(), remainingIndexBudget);
            }
        }
        else if (QTreeView* treeView = qobject_cast<QTreeView*>(currentObject))
        {
            QAbstractItemModel* model = treeView->model();
            if (model != nullptr)
            {
                const int columnCount = model->columnCount(treeView->rootIndex());
                for (int section = 0; section < columnCount; ++section)
                {
                    const QString currentHeader = model->headerData(
                        section,
                        Qt::Horizontal,
                        Qt::DisplayRole).toString();
                    const ManagedTextResult result = resolveManagedText(
                        *this,
                        currentHeader,
                        model->headerData(section, Qt::Horizontal, kRuntimeHeaderSourceRole),
                        model->headerData(section, Qt::Horizontal, kRuntimeHeaderAppliedRole));
                    if (result.clearMetadata)
                    {
                        model->setHeaderData(section, Qt::Horizontal, QVariant(), kRuntimeHeaderSourceRole);
                        model->setHeaderData(section, Qt::Horizontal, QVariant(), kRuntimeHeaderAppliedRole);
                        continue;
                    }
                    if (!result.updateMetadata)
                    {
                        continue;
                    }
                    if (result.setText
                        && !model->setHeaderData(section, Qt::Horizontal, result.appliedText, Qt::DisplayRole))
                    {
                        continue;
                    }
                    model->setHeaderData(section, Qt::Horizontal, result.sourceText, kRuntimeHeaderSourceRole);
                    model->setHeaderData(section, Qt::Horizontal, result.appliedText, kRuntimeHeaderAppliedRole);
                }
                int remainingIndexBudget = 50000;
                translateModelItems(model, treeView->rootIndex(), remainingIndexBudget);
            }
        }

        const QObjectList childList = currentObject->children();
        for (QObject* childObject : childList)
        {
            visitObject(childObject);
        }
    };

    visitObject(object);
}

void ks::i18n::LanguageManager::applyBindings(QObject* object) const
{
    if (object == nullptr)
    {
        return;
    }

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

    if (QSpinBox* spinBox = qobject_cast<QSpinBox*>(object))
    {
        const QString suffixKey = spinBox->property(kSuffixKeyProperty).toString();
        if (!suffixKey.isEmpty())
        {
            spinBox->setSuffix(text(
                suffixKey,
                spinBox->property(kSuffixFallbackProperty).toString()));
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
            const QString tabToolTipKey = page->property(kTabToolTipKeyProperty).toString();
            if (!tabToolTipKey.isEmpty())
            {
                tabWidget->setTabToolTip(
                    index,
                    text(tabToolTipKey, page->property(kTabToolTipFallbackProperty).toString()));
            }
        }
    }

    const QObjectList childList = object->children();
    for (QObject* childObject : childList)
    {
        applyBindings(childObject);
    }
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
