#pragma once

#include <string>
#include <vector>

#include "view.h"
#include "shortcuts.h"

namespace gf {

// SHORTCUTS tab: creates Desktop / Start Menu / Startup links and lists the
// manifest of links GhostFrame created.
class ShortcutsView final : public IView {
public:
    explicit ShortcutsView(ViewHost& host) : host_(host) {}

    const ViewInfo& Info() const override;
    void CreateControls(HWND parent, HINSTANCE instance) override;
    void Build() override;
    void SetActive(bool active) override;
    void Layout() override;
    void Paint(HDC dc) override;
    void PaintHeader(HDC dc) override;

    bool OnLButtonDown(int x, int y, bool ctrl, bool shift) override;
    bool OnLButtonDblClk(int x, int y) override { (void)x; (void)y; return false; }
    bool OnRButtonDown(int, int) override { return false; }
    bool OnKeyDown(int virtualKey) override;
    void OnControlChanged(int controlId) override { (void)controlId; }
    void OnMouseMove(int x, int y, bool& changed) override;
    void OnMouseLeave(bool& changed) override;
    bool OnMouseWheel(int x, int y, int delta) override;
    bool OnVScroll(int code, int position) override;
    bool OnCommand(int id) override;

    bool HandCursorAt(int x, int y) const override;
    bool ButtonActive(int id) const override { (void)id; return false; }
    bool CapturesAlt() const override { return false; }

    void OnBeforeCaptureRefresh() override {}
    void OnCaptureChanged() override {}
    void OnAttachFinished(HWND lastHwnd, bool active) override {
        (void)lastHwnd;
        (void)active;
    }
    UINT RefreshIntervalMs() const override { return 0; }
    void OnTimer() override {}

private:
    void ApplyControls();
    void LoadEntries();
    void SetKind(ShortcutKind kind);
    void CreateFromForm();
    void BrowseForApp();
    void RemoveEntry(int index);
    void RemoveAll();
    bool SaveEntriesWith(const ShortcutEntry& entry);
    int DestinationAt(int x, int y) const;
    int KindAt(int x, int y) const;
    std::wstring EditText(int id) const;
    void SetEditText(int id, const std::wstring& text);

    ViewHost& host_;
    ui::ListWidget list_;
    std::vector<ShortcutEntry> entries_;
    ShortcutKind kind_ = ShortcutKind::Ui;
    ShortcutLocation location_ = ShortcutLocation::Desktop;
    bool active_ = false;
    bool initialized_ = false;
    int hoverDestination_ = -1;
    int hoverKind_ = -1;
    int hoverCta_ = -1;
};

}  // namespace gf