#ifndef KSWORD_GUI_KSHOWCASEPAGES_H
#define KSWORD_GUI_KSHOWCASEPAGES_H

#include "Fl_Group.H"

#include <vector>

// KShowcasePages contains detached sample-panel factories for manual or mainline integration.
namespace KShowcasePages {

// Creates a navigation sample page; caller owns the returned FLTK group through normal FLTK parenting.
Fl_Group* CreateNavigationPage(int x, int y, int w, int h);

// Creates a container sample page; caller owns the returned FLTK group through normal FLTK parenting.
Fl_Group* CreateContainerPage(int x, int y, int w, int h);

// Creates an overlay sample page; caller owns the returned FLTK group through normal FLTK parenting.
Fl_Group* CreateOverlayPage(int x, int y, int w, int h);

// Creates a data-display sample page; caller owns the returned FLTK group through normal FLTK parenting.
Fl_Group* CreateDataPage(int x, int y, int w, int h);

// Creates a visualization sample page; caller owns the returned FLTK group through normal FLTK parenting.
Fl_Group* CreateVisualPage(int x, int y, int w, int h);

// Creates an input-helper sample page; caller owns the returned FLTK group through normal FLTK parenting.
Fl_Group* CreateInputPage(int x, int y, int w, int h);

// Creates all showcase pages in a stable navigation order and returns copied pointers.
std::vector<Fl_Group*> CreateAllPages(int x, int y, int w, int h);

} // namespace KShowcasePages

#endif // KSWORD_GUI_KSHOWCASEPAGES_H
