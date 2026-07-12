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
#include <QRegularExpression>
#include <QSet>
#include <QTabWidget>
#include <QWidget>

#include <algorithm>

namespace
{
    constexpr auto kLanguagePackSchema = "ksword-language-pack";
    constexpr int kLanguagePackFormatVersion = 1;
    constexpr qint64 kMaximumLanguagePackBytes = 4 * 1024 * 1024;
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
    discoverLanguagePacks(&warningList);

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
