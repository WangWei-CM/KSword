#pragma once

#include <QList>
#include <QHash>
#include <QObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

class QComboBox;
class QLineEdit;
class QTabWidget;
class QWidget;

namespace ks::i18n
{
    struct LanguageInfo
    {
        QString id;
        QString name;
        QString nativeName;
        QString author;
        QString filePath;
        bool rightToLeft = false;
    };

    class LanguageManager final : public QObject
    {
    public:
        static LanguageManager& instance();

        bool initialize(const QString& preferredLanguageId, QString* errorTextOut = nullptr);
        bool setLanguage(const QString& languageId, QString* errorTextOut = nullptr);

        QString currentLanguageId() const;
        QList<LanguageInfo> availableLanguages() const;
        QString text(const QString& key, const QString& fallbackText = QString()) const;
        QString translateSource(const QString& sourceText) const;

        void bindText(QObject* object, const QString& key, const QString& fallbackText);
        void bindToolTip(QWidget* widget, const QString& key, const QString& fallbackText);
        void bindPlaceholder(QLineEdit* lineEdit, const QString& key, const QString& fallbackText);
        void bindWindowTitle(QWidget* widget, const QString& key, const QString& fallbackText);
        void bindTab(QTabWidget* tabWidget, QWidget* page, const QString& key, const QString& fallbackText);
        void bindComboBoxItem(
            QComboBox* comboBox,
            int itemIndex,
            const QString& key,
            const QString& fallbackText);
        void retranslateAll();

    protected:
        bool eventFilter(QObject* watchedObject, QEvent* event) override;

    private:
        LanguageManager() = default;
        Q_DISABLE_COPY_MOVE(LanguageManager)

        struct LanguagePack
        {
            struct SourceTemplate
            {
                QRegularExpression expression;
                QString translatedTemplate;
                QStringList placeholders;
            };

            LanguageInfo info;
            QString fallbackLanguageId;
            QHash<QString, QString> translations;
            QHash<QString, QString> sourceTranslations;
            QList<SourceTemplate> sourceTemplates;
        };

        void discoverLanguagePacks(QStringList* warningListOut);
        bool loadLanguagePack(const QString& filePath, LanguagePack* packOut, QString* errorTextOut) const;
        QString resolveText(
            const QString& languageId,
            const QString& key,
            const QString& fallbackText,
            QStringList* visitedLanguageIds) const;
        QString resolveSourceText(const LanguagePack& pack, const QString& sourceText) const;
        void rebuildSourceIndexes(LanguagePack* pack) const;
        void applyBindings(QObject* object) const;
        void applyAutomaticTranslation(QObject* object) const;
        void applyApplicationDirection() const;

        QList<LanguagePack> m_languagePacks;
        QString m_currentLanguageId;
        mutable QHash<QString, QString> m_runtimeSourceCache;
        bool m_eventFilterInstalled = false;
        mutable bool m_applyingAutomaticTranslation = false;
    };

    inline QString text(const QString& key, const QString& fallbackText = QString())
    {
        return LanguageManager::instance().text(key, fallbackText);
    }

    inline QString source(const QString& sourceText)
    {
        return LanguageManager::instance().translateSource(sourceText);
    }
}
