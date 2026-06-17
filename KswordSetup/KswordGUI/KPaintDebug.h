#ifndef KSWORD_GUI_KPAINTDEBUG_H
#define KSWORD_GUI_KPAINTDEBUG_H

#include "Fl_Widget.H"

#include <cstdio>

// KPaintDebugTraceDraw is a compile-time opt-in paint diagnostic helper.
// To reproduce the disappearing-area issue, temporarily define
// KSWORD_ENABLE_PAINT_DEBUG for a local debug run, open a page containing a
// KPanel with KTable/KInput/KTextBox/KTextDisplay children, then click, select,
// type, scroll, and open a table context menu. Parent containers should report
// FL_DAMAGE_CHILD for child-only updates; those updates must not clear the
// parent surface because FLTK will usually redraw only the damaged child.
inline void KPaintDebugTraceDraw(const char* widget_type, const Fl_Widget* widget) {
#ifdef KSWORD_ENABLE_PAINT_DEBUG
    // Input is the widget type name plus a live widget pointer; output is one
    // stderr line per draw call and there is no return value.
    if (!widget) {
        return;
    }

    // The log includes geometry and raw damage bits so a repro can distinguish
    // full repaint from child-only repaint without adding a test framework.
    std::fprintf(stderr,
        "[KPaintDebug] draw %-16s xywh=(%d,%d,%d,%d) damage=0x%02x label=%s\n",
        widget_type ? widget_type : "(unknown)",
        widget->x(),
        widget->y(),
        widget->w(),
        widget->h(),
        static_cast<unsigned int>(widget->damage()),
        widget->label() ? widget->label() : "");
#else
    // Release/default builds pay only the cost of discarded arguments; this
    // keeps the helper safe to leave in hot draw paths.
    (void)widget_type;
    (void)widget;
#endif
}

#endif // KSWORD_GUI_KPAINTDEBUG_H
