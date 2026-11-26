#pragma once

#include "view.h"

namespace gf {

// SETTINGS tab: hotkeys, auto-capture rules, appearance, behavior and
// maintenance.
class SettingsView final : public IView {
public:
    explicit SettingsView(ViewHost& host) : host_(host) {}

    const ViewInfo& Info() const override;
    void CreateControls(HWND parent, HINSTANCE instance) override;
    void Build() override;
    void SetActive(bool active) override;
    void Layout() override;
    void Paint(HDC dc) override;
    void PaintHeader(HDC) override {}

    bool OnLButtonDown(int x, int y, bool ctrl, bool shift) override;
    bool OnLButtonDblClk(int x, int y) override;
    bool OnRButtonDown(int, int) override { return false; }
    bool OnKeyDown(int virtualKey) override;
    void OnControlChanged(int controlId) override;
    void OnMouseMove(int x, int y, bool& changed) override;
    void OnMouseLeave(bool& changed) override;
    bool OnMouseWheel(int x, int y, int delta) override;
    bool OnVScroll(int code, int position) override;
    bool OnCommand(int id) override;

    bool HandCursorAt(int x, int y) const override;
    bool ButtonActive(int id) const override { (void)id; return false; }
    bool CapturesAlt() const override { return rebinding_; }

    void OnBeforeCaptureRefresh() override {}
    void OnCaptureChanged() override {}
    void OnAttachFinished(HWND lastHwnd, bool active) override {
        (void)lastHwnd;
        (void)active;
    }
    UINT RefreshIntervalMs() const override { return 0; }
    void OnTimer() override {}

private:
    // The order is the order of the secondary navigation rail.
    enum class Section { General, Appearance, Hotkeys, AutoCapture, Advanced };

    void ApplyControls();
    void SetSection(Section section);
    int MaxScroll() const;
    void ScrollBy(int amount);

    // True when the child (in client coordinates) lies fully inside the
    // scrollable settings area.  Controls that scroll past the area edge are
    // hidden so a child window can never draw over the nav or the footer.
    bool ChildInsideArea(int id) const;
    void LoadEmojiField();
    void ApplyEmoji();
    void ClearEmojiIcon();
    RECT SwitchRowRect(const RECT& card, int index) const;
    int BehaviorSwitchAt(int x, int y) const;
    int NavAt(int x, int y) const;
    int ActionAt(int x, int y) const;
    int AppearancePresetAt(int x, int y) const;
    int AppearanceAccentAt(int x, int y) const;
    int AppearanceModeAt(int x, int y) const;
    int AppearanceDensityAt(int x, int y) const;
    int AppearanceSwatchAt(int x, int y) const;
    int SelfDeleteModeAt(int x, int y) const;
    bool SelfDeleteExitAt(int x, int y) const;
    void PaintCard(HDC dc, const RECT& card, ui::Icon icon, const wchar_t* title,
                   const wchar_t* subtitle);
    void PaintSwitch(HDC dc, const RECT& rect, const wchar_t* title, const wchar_t* subtitle,
                     bool on, bool hovered);
    void PaintPill(HDC dc, const RECT& rect, const wchar_t* label, bool selected, bool hovered);
    void PaintSwatch(HDC dc, const RECT& rect, COLORREF color, bool selected, bool hovered,
                     const wchar_t* label);
    void PaintAppearance(HDC dc);
    void PaintTabsCard(HDC dc);
    void ApplyListAction(ui::ListWidget::Action action, bool hotkeyList);
    void ApplyTabRowAction(int row, int actionId);
    bool ApplyAppearanceChange();
    void ResetAppearance();
    void ChooseBackgroundImage();
    void ResetHotkeys();
    void BeginRebind(HotkeyAction action);
    void FinishRebind(UINT modifiers, UINT virtualKey);
    void AddRuleForWindow(HWND hwnd);
    void AddRuleForExePath(const std::wstring& path);
    void ApplyAutoRulesNow();
    ui::ListWidget* ActiveList();
    const ui::ListWidget* ActiveList() const;

    ViewHost& host_;
    ui::ListWidget hotkeys_;
    ui::ListWidget rules_;
    ui::ListWidget tabs_;
    Section section_ = Section::General;
    int selectedRule_ = -1;
    bool rebinding_ = false;
    HotkeyAction rebindAction_ = HotkeyAction::Count;
    bool active_ = false;
    bool emojiUpdating_ = false;
    int hoverNav_ = -1;
    int hoverSwitch_ = -1;
    int hoverAction_ = -1;
    int hoverPreset_ = -1;
    int hoverAccent_ = -1;
    int hoverMode_ = -1;
    int hoverDensity_ = -1;
    int hoverSwatch_ = -1;
    int hoverSelfMode_ = -1;
    bool hoverSelfExit_ = false;

    // Vertical page scrolling for sections that are taller than the available
    // body (Appearance, Advanced, and Hotkeys on very short screens).
    int scroll_ = 0;
    int contentHeight_ = 0;
};

}  // namespace gf