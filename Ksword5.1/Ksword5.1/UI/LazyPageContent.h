#pragma once

#include "ThemeBinding.h"
#include <QPointer>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>
#include <utility>

namespace ks::ui
{
    // Keep the navigation page stable while deferring its one-shot child factory.
    // This is for optional child pages, not top-level/independent window lifetime.
    class LazyPageContent final : public QWidget
    {
    public:
        using Factory = std::function<QWidget*(QWidget*)>;
        explicit LazyPageContent(QWidget* parent, Factory factory)
            : QWidget(parent), m_factory(std::move(factory))
        {
            m_layout = new QVBoxLayout(this);
            m_layout->setContentsMargins(0, 0, 0, 0);
            m_layout->setSpacing(0);
        }

    protected:
        void showEvent(QShowEvent* event) override
        {
            QWidget::showEvent(event);
            if (!isVisible() || !m_factory)
            {
                return;
            }
            // Consume before calling user code to make nested Show idempotent.
            auto factory = std::move(m_factory);
            m_factory = {};
            const QPointer<LazyPageContent> guard(this);
            QWidget* const content = factory(this);
            if (guard.isNull() || content == nullptr)
            {
                return;
            }
            const ScopedWidgetThemePreparation preparation(this);
            m_layout->addWidget(content, 1);
            content->show();
        }

    private:
        Factory m_factory;
        QVBoxLayout* m_layout = nullptr;
    };
}
