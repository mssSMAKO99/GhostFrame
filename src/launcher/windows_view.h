#pragma once

#include "view.h"

namespace gf {

// WINDOWS tab: the live enumerator, its filter and the attach actions.
class WindowsView final : public IView {
public:
    explicit WindowsView(ViewHost& host) : host_(host) {}

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

    void OnBeforeCaptureRefresh() override {}
    void OnCaptureChanged() override {}
    void OnAttachFinished(HWND lastHwnd, bool active) override;
    UINT RefreshIntervalMs() const override { return 2000; }
    void OnTimer() override;

private:
    void Refresh();
    void ApplyControls();
    void ApplyListAction(ui::ListWidget::Action action);
    void AttachSelected();
    void AttachProcess(HWND sample);

    ViewHost& host_;
    ui::ListWidget list_;
    std::wstring filter_;
    bool active_ = false;
    bool hoverCta_ = false;
    bool hoverRefresh_ = false;
};

}  // namespace gf