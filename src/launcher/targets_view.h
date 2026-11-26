#pragma once

#include "view.h"

namespace gf {

// TARGETS tab: the tracked windows, their row actions and the commands that
// operate on the current selection.
class TargetsView final : public IView {
public:
    explicit TargetsView(ViewHost& host) : host_(host) {}

    const ViewInfo& Info() const override;
    void CreateControls(HWND parent, HINSTANCE instance) override;
    void Build() override;
    void SetActive(bool active) override;
    void Layout() override;
    void Paint(HDC dc) override;
    void PaintHeader(HDC dc) override;

    bool OnLButtonDown(int x, int y, bool ctrl, bool shift) override;
    bool OnLButtonDblClk(int x, int y) override;
    bool OnRButtonDown(int x, int y) override;
    bool OnKeyDown(int virtualKey) override;
    void OnControlChanged(int controlId) override;
    void OnMouseMove(int x, int y, bool& changed) override;
    void OnMouseLeave(bool& changed) override;
    bool OnMouseWheel(int x, int y, int delta) override;
    bool OnVScroll(int code, int position) override;
    bool OnCommand(int id) override;

    bool HandCursorAt(int x, int y) const override;
    bool ButtonActive(int id) const override;
    bool CapturesAlt() const override { return false; }

    void OnBeforeCaptureRefresh() override;
    void OnCaptureChanged() override;
    void OnAttachFinished(HWND lastHwnd, bool active) override;
    UINT RefreshIntervalMs() const override { return 0; }
    void OnTimer() override {}

private:
    void ApplyControls();
    void PaintEmpty(HDC dc);
    void ApplyListAction(ui::ListWidget::Action action);
    void ApplyRowAction(int row, int actionId);
    void SelectIndex(int index);
    bool SelectedValid() const;
    HWND SelectedHwnd() const;
    std::vector<size_t> SelectedIndices() const;
    void ToggleStealth(int index);
    void ToggleSelection();
    void RestoreAll();
    void ShowSelected(int command);
    void CloseSelected();
    void RemoveSelected();

    ViewHost& host_;
    ui::ListWidget list_;
    int selected_ = -1;
    HWND selectedHwnd_ = nullptr;
    HWND refreshKeep_ = nullptr;
    bool active_ = false;
    bool hoverCta_ = false;
};

}  // namespace gf