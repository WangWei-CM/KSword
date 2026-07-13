#pragma once

#include <QList>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class QComboBox;
class QLineEdit;
class QSpinBox;
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
        QString contextText(const QString& contextKey, const QString& sourceText) const;

        void bindText(QObject* object, const QString& key, const QString& fallbackText);
        void bindToolTip(QWidget* widget, const QString& key, const QString& fallbackText);
        void bindPlaceholder(QLineEdit* lineEdit, const QString& key, const QString& fallbackText);
        void bindSuffix(QSpinBox* spinBox, const QString& key, const QString& fallbackText);
        void bindWindowTitle(QWidget* widget, const QString& key, const QString& fallbackText);
        void bindTab(QTabWidget* tabWidget, QWidget* page, const QString& key, const QString& fallbackText);
        void bindTabToolTip(QTabWidget* tabWidget, QWidget* page, const QString& key, const QString& fallbackText);
        void bindComboBoxItem(
            QComboBox* comboBox,
            int itemIndex,
            const QString& key,
            const QString& fallbackText);
        void retranslateAll();

    private:
        LanguageManager() = default;
        Q_DISABLE_COPY_MOVE(LanguageManager)

        struct LanguagePack
        {
            LanguageInfo info;
            QString fallbackLanguageId;
            QHash<QString, QString> translations;
            QHash<QString, QString> contextTranslations;
        };

        void discoverLanguagePacks(QStringList* warningListOut);
        bool loadLanguagePack(const QString& filePath, LanguagePack* packOut, QString* errorTextOut) const;
        QString resolveText(
            const QString& languageId,
            const QString& key,
            const QString& fallbackText,
            QStringList* visitedLanguageIds) const;
        QString resolveContextText(
            const QString& languageId,
            const QString& contextKey,
            const QString& sourceText,
            QStringList* visitedLanguageIds) const;
        void applyBindings(QObject* object) const;
        void applyApplicationDirection() const;

        QList<LanguagePack> m_languagePacks;
        QString m_currentLanguageId;
    };

    inline QString text(const QString& key, const QString& fallbackText = QString())
    {
        return LanguageManager::instance().text(key, fallbackText);
    }

    inline QString contextText(const QString& contextKey, const QString& sourceText)
    {
        return LanguageManager::instance().contextText(contextKey, sourceText);
    }

}
