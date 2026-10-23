#pragma once
#include "WindowInputClient.h"
#include "../UI/PageNavigationCatalog.h"
#include <vector>

class QWidget;
namespace ks::window_input
{
    struct InjectionWindowChoice
    {
        dwm_order::WindowIdentity identity;
        std::wstring title;
    };
    struct InjectionInitialSnapshot
    {
        std::vector<InjectionWindowChoice> choices;
        std::uint32_t error = 0;
    };

    // The same read-only enumeration used by the page's refresh worker. Call
    // off the UI thread; no widget, agent or input control is created or queried.
    InjectionInitialSnapshot ReadInitialInjectionSnapshot();
    // Complete initial body only with the actual completed enumeration above.
    // Projection is pure value formatting; it performs no native query.
    QVector<ks::ui::PageSearchBodyText> coldSearchBody(const InjectionInitialSnapshot& initial);

    QWidget* CreateControl(const dwm_order::WindowIdentity& identity, QWidget* parent);
    QWidget* CreateInjectionPage(QWidget* parent);
}
