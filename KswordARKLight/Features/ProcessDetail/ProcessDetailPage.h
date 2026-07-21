#pragma once

#include "ProcessDetailTypes.h"

#include "../../Core/Win32Lean.h"

#include <commctrl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Ksword::Features::ProcessDetail {

// ProcessDetailPage is the native Win32 conversion of the full process-detail
// layout. It owns native tab pages and does not load foreign UI sources,
// resources, or binaries.
class ProcessDetailPage final {
public:
    static HWND Create(HWND parent, DWORD processId, const RECT& bounds);
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    enum class TabIndex : std::size_t {
        Detail = 0,
        Threads,
        Actions,
        Modules,
        Token,
        TokenSwitch,
        Evidence,
        Peb,
        Count
    };

    enum ControlId : int {
        TabControl = 1000,

        DetailTitle = 1100,
        DetailPath,
        DetailCopyPath,
        DetailOpenFolder,
        DetailCommandLine,
        DetailCopyCommand,
        DetailParentText,
        DetailOpenHandles,
        DetailGotoParent,
        DetailStartTime,
        DetailUser,
        DetailAdmin,
        DetailArchitecture,
        DetailPriority,
        DetailSession,
        DetailThreadCount,
        DetailHandleCount,
        DetailCpu,
        DetailRam,
        DetailDisk,
        DetailSignature,

        ThreadRefresh = 1200,
        ThreadSample,
        ThreadStack,
        ThreadStatus,
        ThreadList,
        ThreadRuntimeOutput,

        ActionTerminateMode = 1300,
        ActionTerminate,
        ActionSuspend,
        ActionResume,
        ActionSetCritical,
        ActionClearCritical,
        ActionPriority,
        ActionApplyPriority,
        ActionOpenFolder,
        ActionRefreshPpl,
        ActionEfficiencyOn,
        ActionEfficiencyOff,
        ActionR0Terminate,
        ActionR0Suspend,
        ActionR0Ppl,
        ActionR0Hide,
        ActionR0Danger,
        ActionInjectionMode,
        ActionDllPath,
        ActionBrowseDll,
        ActionInjectDll,
        ActionShellcodePath,
        ActionBrowseShellcode,
        ActionInjectShellcode,

        ModuleRefresh = 1400,
        ModuleVerifySignature,
        ModuleStatus,
        ModuleList,

        TokenRefresh = 1500,
        TokenStatus,
        TokenEditorToolbar,
        TokenCopy,
        TokenFind,
        TokenGoto,
        TokenWrap,
        TokenOutput,
        TokenEditorStatus,

        TokenSwitchRefresh = 1600,
        TokenSwitchApply,
        TokenSwitchRefreshAll,
        TokenSwitchStatus,
        TokenSandboxInert,
        TokenVirtualizationAllowed,
        TokenVirtualizationEnabled,
        TokenUiAccess,
        TokenMandatoryNoWriteUp,
        TokenMandatoryNewProcessMin,
        TokenHasRestrictions,
        TokenIsAppContainer,
        TokenIsRestricted,
        TokenIsLessPrivilegedAppContainer,
        TokenIsSandboxed,
        TokenIsAppSilo,
        TokenRawInfoClass,
        TokenRawInputMode,
        TokenRawPayload,
        TokenRawApply,

        EvidenceR0Status = 1700,
        EvidenceCapability,
        EvidenceImagePath,
        EvidenceHandleTable,
        EvidenceSectionObject,
        EvidenceProtection,
        EvidenceSignature,
        EvidenceSectionSignature,
        EvidenceSessionSource,
        EvidenceImagePathSource,
        EvidenceProtectionSource,
        EvidenceSignatureSource,
        EvidenceSectionSignatureSource,
        EvidenceObjectTableSource,
        EvidenceSectionObjectSource,
        EvidenceProtectionOffset,
        EvidenceSignatureOffset,
        EvidenceSectionSignatureOffset,
        EvidenceObjectTableOffset,
        EvidenceSectionObjectOffset,
        EvidenceRefreshSection,
        EvidenceSectionStatus,
        EvidenceSectionOutput,

        PebRefresh = 2100,
        PebApply,
        PebStatus,
        PebTarget,
        PebCommandLine,
        PebImagePath,
        PebCurrentDirectory,
        PebEnvironmentName,
        PebEnvironmentValue,
        PebImageBase,
        PebAffinity,
        PebPriority,
        PebOutput,
        PebReadonlyReason
    };

    struct Placement {
        HWND hwnd = nullptr;
        int x = 0;
        int y = 0;
        int width = 0;  // Negative values mean right margin and stretch.
        int height = 0; // Negative values mean bottom margin and stretch.
    };

    struct PageState {
        HWND hwnd = nullptr;
        std::vector<Placement> placements;
    };

    explicit ProcessDetailPage(DWORD processId);
    ~ProcessDetailPage();

    ProcessDetailPage(const ProcessDetailPage&) = delete;
    ProcessDetailPage& operator=(const ProcessDetailPage&) = delete;

    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK PageSubclassProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR subclassId,
        DWORD_PTR referenceData);
    LRESULT HandlePageMessage(TabIndex tab, HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool Initialize(HWND hwnd);
    void Layout();
    void LayoutPage(TabIndex tab);
    void UpdateVisiblePage();
    bool EnsurePage(TabIndex tab);
    bool CreatePageHost(TabIndex tab);
    void DestroyPageHost(TabIndex tab);
    bool CreateTabControls(TabIndex tab);
    void PopulateTab(TabIndex tab);
    void ResetTabRuntimeState(TabIndex tab);
    void RedrawTabClient();
    void OnTabActivated(TabIndex tab);
    void RefreshAll();

    bool CreateDetailTab();
    bool CreateThreadTab();
    bool CreateActionTab();
    bool CreateModuleTab();
    bool CreateTokenTab();
    bool CreateTokenSwitchTab();
    bool CreateEvidenceTab();
    bool CreatePebTab();

    HWND AddControl(
        TabIndex tab,
        DWORD exStyle,
        const wchar_t* className,
        const wchar_t* text,
        DWORD style,
        int controlId,
        int x,
        int y,
        int width,
        int height);
    HWND AddLabel(TabIndex tab, int controlId, const wchar_t* text, int x, int y, int width, int height);
    HWND AddButton(TabIndex tab, int controlId, const wchar_t* text, int x, int y, int width, int height);
    HWND AddEdit(TabIndex tab, int controlId, const wchar_t* text, bool readOnly, bool multiline, int x, int y, int width, int height);
    HWND AddCombo(TabIndex tab, int controlId, int x, int y, int width, int height);
    HWND AddCheck(TabIndex tab, int controlId, const wchar_t* text, int x, int y, int width, int height);
    HWND AddGroup(TabIndex tab, const wchar_t* text, int x, int y, int width, int height);
    HWND AddList(TabIndex tab, int controlId, int x, int y, int width, int height);
    HWND Control(TabIndex tab, int controlId) const;
    void SetControlText(TabIndex tab, int controlId, const std::wstring& text);
    std::wstring ControlText(TabIndex tab, int controlId) const;
    void SetPageStatus(TabIndex tab, int controlId, const std::wstring& text);

    static void AddListColumn(HWND list, int index, const wchar_t* title, int width);
    static void ClearList(HWND list);
    static void AddListRow(HWND list, int row, const std::vector<std::wstring>& values, LPARAM data = 0);
    static std::wstring ListCell(HWND list, int row, int column);
    static bool CopyText(HWND owner, const std::wstring& text);
    static std::wstring ReadWindowText(HWND hwnd);
    static void ApplyFont(HWND hwnd, HFONT font = nullptr);

    bool HandleGenericContextMenu(HWND source, POINT screenPoint);
    bool HandleThreadContextMenu(POINT screenPoint);
    bool HandleModuleContextMenu(POINT screenPoint);
    void ShowModuleDetailDialog();
    void CopyListCell(HWND list);
    void CopyListRow(HWND list);
    void CopyListAll(HWND list);
    int SelectedListRow(HWND list) const;

    void PopulateDetailTab();
    void PopulateThreadTab();
    void PopulateModuleTab();
    void PopulateTokenTab();
    void PopulateTokenSwitchTab();
    void PopulateEvidenceTab();
    void PopulatePebTab();

    bool HandleDetailCommand(int controlId);
    bool HandleThreadCommand(int controlId);
    bool HandleActionCommand(int controlId);
    bool HandleModuleCommand(int controlId);
    bool HandleTokenCommand(int controlId);
    bool HandleTokenSwitchCommand(int controlId);
    bool HandleEvidenceCommand(int controlId);
    bool HandlePebCommand(int controlId);
    bool HandlePageNotify(TabIndex tab, NMHDR* header, LRESULT& result);

    void SuspendSelectedThread();
    void ResumeSelectedThread();
    void TerminateSelectedThread();
    void TerminateAllThreadsByR0();
    void ShowSelectedThreadSummary();
    void OpenSelectedModuleFolder();
    void UnloadSelectedModule();
    void SuspendSelectedModuleThread();
    void ResumeSelectedModuleThread();
    void TerminateSelectedModuleThread();
    void ExecuteProcessAction(int actionId);
    void BrowseForPayload(bool dllMode);
    void ApplyTokenSwitches();
    void ApplyRawTokenValue();
    void RefreshTokenReport();
    void RefreshTokenSwitches();
    void RefreshSectionReport();
    void RefreshPebReport();
    void ApplyPebEdits();

private:
    DWORD processId_ = 0;
    HWND hwnd_ = nullptr;
    HWND tab_ = nullptr;
    HFONT titleFont_ = nullptr;
    TabIndex currentTab_ = TabIndex::Count;
    std::array<PageState, static_cast<std::size_t>(TabIndex::Count)> pages_{};
    std::unordered_map<HWND, int> listColumnCounts_;
    std::unordered_map<HWND, int> listContextColumns_;
    ProcessDetailSnapshot snapshot_{};
    bool tokenLoaded_ = false;
    bool tokenSwitchLoaded_ = false;
    bool sectionLoaded_ = false;
    bool pebLoaded_ = false;
};

} // namespace Ksword::Features::ProcessDetail
