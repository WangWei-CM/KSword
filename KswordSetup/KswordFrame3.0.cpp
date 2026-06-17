#pragma execution_character_set("utf-8")

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Objbase.h>

#include "KswordGUI/KswordStyle.h"

#include "Fl.H"
#include "Fl_Window.H"

#include <string>
#include <thread>
#include <vector>

namespace {
constexpr int kTemporaryWindowHeight = 720;
constexpr int kTemporaryRightWindowWidth = 580;
constexpr int kTemporaryLayeredImageWidth = 405;

// CenteredWindowX computes the left edge for a window centered in the work area.
// Input is the work area origin/width and window width; output is a screen x.
int CenteredWindowX(int workX, int workWidth, int windowWidth) {
    return workX + (workWidth - windowWidth) / 2;
}

// CenteredWindowY computes the top edge for a window centered in the work area.
// Input is the work area origin/height and window height; output is a screen y.
int CenteredWindowY(int workY, int workHeight, int windowHeight) {
    return workY + (workHeight - windowHeight) / 2;
}
} // namespace

void GuiInitMain(const std::vector<std::string>& args, Fl_Window* MainWindow);
void GuiAfterShowMain(Fl_Window* MainWindow);
int AsyncMain(const std::vector<std::string>& args);

// main initializes the FLTK installer shell. Inputs are normal process
// arguments; processing initializes COM, theme state, the centered right-side
// window, and the transparent left-side PNG; return value is FLTK's event-loop
// exit code.
int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i] ? argv[i] : "");
    }

    // COM is required by the folder picker and shortcut creation helpers. The
    // installer runs on a single UI thread, so apartment threading is enough.
    const HRESULT comInitResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInitialized = SUCCEEDED(comInitResult);

    InitFlatThemeGlobal();

    int workX = 0;
    int workY = 0;
    int workWidth = 0;
    int workHeight = 0;
    Fl::screen_work_area(workX, workY, workWidth, workHeight);
    const int combinedWidth = kTemporaryLayeredImageWidth + kTemporaryRightWindowWidth;
    const int windowX = CenteredWindowX(workX, workWidth, combinedWidth) + kTemporaryLayeredImageWidth;
    const int windowY = CenteredWindowY(workY, workHeight, kTemporaryWindowHeight);

    Fl_Window* window = new Fl_Window(windowX, windowY, kTemporaryRightWindowWidth, kTemporaryWindowHeight, "PNG transparent-left window test");
    window->begin();

    std::thread(AsyncMain, args).detach();
    GuiInitMain(args, window);

    window->end();
    window->show(argc, argv);
    KApplyWindowIcon(window);
    GuiAfterShowMain(window);
    const int exitCode = Fl::run();
    if (comInitialized) {
        ::CoUninitialize();
    }
    return exitCode;
}
