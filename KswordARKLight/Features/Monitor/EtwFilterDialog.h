#pragma once

#include "../../Core/Win32Lean.h"
#include "EtwFilterModel.h"

namespace Ksword::Features::Monitor {

// EtwFilterDialog displays non-persistent ETW filtering controls. Inputs are a
// parent HWND and current filter state; processing creates a modal Win32 dialog-
// style window; output is true only when the user accepts changes.
class EtwFilterDialog final {
public:
    EtwFilterDialog(HWND parent, const EtwFilterState& initialState);

    // showModal runs the dialog's local message loop. Input is stateOut, filled
    // only on OK; output true means accepted, false means cancelled.
    bool showModal(EtwFilterState& stateOut);

    // WndProc dispatches the pure Win32 modal window. Inputs are standard Win32
    // message parameters; output is the handled LRESULT.
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void createControls();
    void layout();
    void loadStateToControls();
    bool saveControlsToState();
    void closeWithResult(bool accepted);

    HWND parent_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND providerList_ = nullptr;
    HWND pidEdit_ = nullptr;
    HWND levelCombo_ = nullptr;
    HWND currentProcessCheck_ = nullptr;
    HWND okButton_ = nullptr;
    HWND cancelButton_ = nullptr;
    EtwFilterState state_;
    bool accepted_ = false;
    bool finished_ = false;
};

} // namespace Ksword::Features::Monitor
