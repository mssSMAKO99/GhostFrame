// The UI's view boundary.
//
// The window is a thin shell (App): it owns the frame, the layout, the theme,
// the status/toast area and the shared state, and it routes input and paint to
// exactly one view.  A view owns one tab: its widget, its rows and everything
// that is painted inside the page body.
//
// Views never touch the capture pipeline directly.  Everything they need is
// reached through ViewHost, which the shell implements on top of RemoteCapture
// (the daemon's facade) and the settings document.  A view is therefore pure
// presentation plus the edits that belong to its tab.
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "ipc/remote_capture.h"
#include "settings.h"
#include "ui.h"
#include "window_list.h"

namespace gf {
class ViewHost;
class IView;

// Chrome geometry computed once per resize/DPI change.  The shell fills the
// frame fields and a base `body` rectangle; the active view fills the fields
// that belong to its page and positions its child controls.
struct LayoutInfo {
    RECT topBar = {};
    RECT sidebar = {};
    RECT content = {};
    RECT logoMark = {};
    RECT tabs[8] = {};
    int tabCount = 0;
    RECT windowButtons[3] = {};  // hide to tray, maximize, close
    RECT pageHeader = {};
    RECT pageIcon = {};
    RECT pageTitle = {};
    RECT pageSubtitle = {};
    RECT body = {};  // page body between the header and the footer
    RECT footer = {};
    RECT footerLine = {};
    RECT logLink = {};

    // Filled by the active view only.
    RECT cta = {};
    RECT secondaryCta = {};
    RECT searchBox = {};
    RECT toolbar = {};
    RECT list = {};
    RECT emptyCard = {};
    RECT emptyArt = {};
    RECT emptyText = {};
    RECT emptyCta = {};
    RECT settingsNav = {};
    RECT settingsNavItems[5] = {};
    RECT settingsArea = {};
    RECT settingsCard[3] = {};
    RECT settingsHotkeyList = {};
    RECT settingsRuleList = {};
    RECT settingsManage = {};
    RECT settingsActionRow = {};
    RECT settingsLogCta = {};
    RECT settingsRestoreCta = {};

    // Self-delete card (Advanced section).
    RECT settingsSelfModes[3] = {};
    RECT settingsSelfExit = {};
    RECT settingsSelfDeleteNow = {};

    // APPEARANCE section.
    RECT appearanceCard[6] = {};
    RECT appearancePresets[4] = {};
    RECT appearanceDensity[2] = {};
    RECT appearanceAccents[6] = {};
    RECT appearanceModes[2] = {};
    RECT appearanceSwatches[5] = {};
    RECT appearanceImage = {};
    RECT appearanceImageClear = {};
    RECT appearanceTabs = {};
    RECT appearanceReset = {};
    RECT appearanceEmojiPreview = {};
    RECT appearanceEmoji = {};
    RECT appearanceEmojiApply = {};
    RECT appearanceEmojiClear = {};

    // SHORTCUTS tab.
    RECT shortcutsToolbar = {};
    RECT shortcutsForm = {};
    RECT shortcutsListCard = {};
    RECT shortcutsList = {};
    RECT shortcutsDestination[3] = {};
    RECT shortcutsKind[3] = {};
    RECT shortcutsNameLabel = {};
    RECT shortcutsName = {};
    RECT shortcutsDescLabel = {};
    RECT shortcutsDesc = {};
    RECT shortcutsAppLabel = {};
    RECT shortcutsApp = {};
    RECT shortcutsBrowse = {};
    RECT shortcutsArgsLabel = {};
    RECT shortcutsArgs = {};
};

// Everything a view can say about itself that the shell needs for the tab
// strip and the page header.
struct ViewInfo {
    const wchar_t* id = L"";
    const wchar_t* label = L"";
    const wchar_t* title = L"";
    const wchar_t* subtitle = L"";
    ui::Icon icon = ui::Icon::None;
};

// The services the shell offers to a view.  Deliberately small: no capture or
// window-enumeration internals leak through it, only the facade and the
// high-level actions a tab needs.
class ViewHost {
public:
    virtual ~ViewHost() = default;

    virtual HWND Window() const = 0;
    virtual HINSTANCE Instance() const = 0;
    // Creates an owner-draw button (and its tooltip) in the shell window.
    virtual void AddButton(int id, const wchar_t* text, const wchar_t* tooltip) = 0;
    // Creates the search edit used by the windows view.
    virtual void AddSearchEdit(int id, const wchar_t* cue) = 0;
    // Creates a plain single-line edit (appearance emoji, shortcut fields).
    virtual void AddEdit(int id, const wchar_t* cue) = 0;
    virtual ui::Theme& Theme() = 0;
    virtual LayoutInfo& Layout() = 0;
    virtual gf::Settings& Settings() = 0;
    virtual gf::RemoteCapture& Capture() = 0;
    virtual gf::WindowEnumerator& Windows() = 0;

    // The registered views in registry order; used by the appearance editor.
    virtual const std::vector<gf::ViewInfo>& ViewCatalog() const = 0;
    // Re-applies theme, background, density and tab order from the settings.
    // Returns false when the configured emoji icon could not be rendered; the
    // previous icon is kept in that case.
    virtual bool ApplyAppearance() = 0;

    virtual void SetStatus(std::wstring text, bool error = false) = 0;
    virtual void ShowToast(std::wstring text, bool error) = 0;
    virtual void Invalidate() = 0;
    virtual void MarkSettingsDirty() = 0;
    // Persists the document and pushes it to the daemon (hotkeys, rules).
    virtual void SaveSettingsNow() = 0;
    virtual bool ApplyHotkeys(const gf::Settings& candidate, std::wstring& error) = 0;

    virtual void SelectView(const wchar_t* id) = 0;
    virtual void FocusWindowSearch() = 0;
    // Recomputes the chrome plus the active view's layout and child positions.
    virtual void Relayout() = 0;

    virtual gf::SendOutcome SendCommand(int index, gf::Cmd cmd, std::wstring& error) = 0;
    virtual void RestoreAllTargets() = 0;

    virtual bool AttachBusy() const = 0;
    virtual void StartAttachBatch(std::vector<HWND> targets, bool hideAfter) = 0;
    virtual void AttachForegroundTarget(bool hideAfter) = 0;
    virtual void CancelAttach() = 0;

    virtual void ApplySelfExclude() = 0;
    virtual void OpenLogFolder() = 0;
    // Confirms and starts the self-delete flow selected in the settings.
    virtual void RequestSelfDelete() = 0;
};

// One tab.  The shell calls SetActive on a switch, Build whenever the view's
// data may have changed, then Paint and the input hooks.
class IView {
public:
    virtual ~IView() = default;

    virtual const ViewInfo& Info() const = 0;

    // Creates the view's own child controls (buttons, search field).  The shell
    // only provides the helpers, so a view removed from the registry also
    // removes its controls.
    virtual void CreateControls(HWND parent, HINSTANCE instance) = 0;

    // Rebuilds the view's rows from the current host state.
    virtual void Build() = 0;

    // Activates/deactivates the view.  A view shows or hides its child controls
    // here and refreshes anything that is only interesting while visible.
    virtual void SetActive(bool active) = 0;

    // Fills the view-specific LayoutInfo fields and places child controls.
    virtual void Layout() = 0;

    // Page body and the header actions (CTA buttons).
    virtual void Paint(HDC dc) = 0;
    virtual void PaintHeader(HDC dc) = 0;

    // Input.  A hook returns true when it consumed the event.
    virtual bool OnLButtonDown(int x, int y, bool ctrl, bool shift) = 0;
    virtual bool OnLButtonDblClk(int x, int y) = 0;
    virtual bool OnRButtonDown(int x, int y) = 0;
    virtual bool OnKeyDown(int virtualKey) = 0;
    // A child control owned by the view changed (e.g. the window search field).
    virtual void OnControlChanged(int controlId) = 0;
    virtual void OnMouseMove(int x, int y, bool& changed) = 0;
    virtual void OnMouseLeave(bool& changed) = 0;
    virtual bool OnMouseWheel(int x, int y, int delta) = 0;
    virtual bool OnVScroll(int code, int position) = 0;
    virtual bool OnCommand(int id) = 0;

    // Shell queries that need a view's state.
    virtual bool HandCursorAt(int x, int y) const = 0;
    virtual bool ButtonActive(int id) const = 0;
    // True while a settings hotkey capture is in progress: Alt combinations must
    // reach the view instead of the system menu.
    virtual bool CapturesAlt() const = 0;

    // The capture snapshot is about to be refreshed / has been refreshed.
    virtual void OnBeforeCaptureRefresh() = 0;
    virtual void OnCaptureChanged() = 0;

    // An attach batch finished.  `active` tells the view whether it was the
    // visible tab when the completion arrived.
    virtual void OnAttachFinished(HWND lastHwnd, bool active) = 0;

    // Periodic refresh while this view is active; 0 disables the timer.
    virtual UINT RefreshIntervalMs() const = 0;
    virtual void OnTimer() = 0;
};

using ViewFactory = std::unique_ptr<IView> (*)(ViewHost& host);

// Compile-time plugin list.  Editing this one function is all it takes to add,
// remove or reorder a tab.
const std::vector<ViewFactory>& ViewFactories();

// Small shared drawing helper used by the header CTAs.
void PaintCtaButton(HDC dc, const ui::Theme& theme, const RECT& rect, const wchar_t* label,
                    ui::Icon icon, ui::Icon trailing, bool primary, bool hovered);

}  // namespace gf