// SETTINGS tab: the hotkey rebinding list, the auto-capture rule list and the
// helpers that edit them.

#include <windows.h>

#include <cwchar>

#include "app.h"
#include "resource.h"

void App::BuildSettingsRows() {
    std::vector<ui::ListRow> hotkeys;
    for (int i = 0; i < gf::kHotkeyCount; ++i) {
        const gf::HotkeyAction action = static_cast<gf::HotkeyAction>(i);
        ui::ListRow row;
        row.title = gf::HotkeyActionLabel(action);
        row.meta = rebinding_ && rebindAction_ == action
                       ? L"press a combination…"
                       : gf::FormatHotkey(settings_.hotkeys[i].modifiers,
                                          settings_.hotkeys[i].virtualKey);
        row.metaMono = true;
        row.data = i;
        hotkeys.push_back(std::move(row));
    }
    hotkeysList_.SetEmptyText(L"NO HOTKEYS");
    hotkeysList_.SetRows(std::move(hotkeys));

    std::vector<ui::ListRow> rules;
    for (size_t i = 0; i < settings_.autoAttach.size(); ++i) {
        const gf::AutoAttachRule& rule = settings_.autoAttach[i];
        ui::ListRow row;
        row.title = rule.value;
        row.meta = rule.match + (rule.hide ? L"  ·  hide on attach" : L"  ·  keep visible");
        row.data = static_cast<intptr_t>(i);
        row.badges.push_back({rule.enabled ? L"ENABLED" : L"DISABLED",
                              rule.enabled ? ui::kNeon : ui::kMuted});
        rules.push_back(std::move(row));
    }
    rulesList_.SetEmptyText(L"NO AUTO-CAPTURE RULES — ADD THE FOREGROUND APP");
    rulesList_.SetRows(std::move(rules));
    if (selectedRule_ >= static_cast<int>(settings_.autoAttach.size())) {
        selectedRule_ = settings_.autoAttach.empty() ? -1
                                                     : static_cast<int>(settings_.autoAttach.size()) - 1;
    }
}

void App::OnSettingsListAction(ui::ListWidget::Action action, bool hotkeyList) {
    if (action.kind == ui::ListWidget::ActionKind::None || action.index < 0) {
        return;
    }
    if (hotkeyList) {
        BeginRebind(static_cast<gf::HotkeyAction>(action.index));
        return;
    }
    selectedRule_ = action.index;
    rulesList_.Select(action.index);
    InvalidateAll();
}

void App::ResetHotkeysToDefaults() {
    gf::Settings defaults = gf::DefaultSettings();
    std::wstring error;
    if (ApplyHotkeys(defaults, error)) {
        BuildSettingsRows();
        SetStatus(L"HOTKEYS RESET TO DEFAULTS");
    } else {
        SetStatus(L"ERROR: " + error, true);
    }
}

void App::BeginRebind(gf::HotkeyAction action) {
    rebinding_ = true;
    rebindAction_ = action;
    SetStatus(std::wstring(L"PRESS A COMBINATION FOR ") + gf::HotkeyActionLabel(action) +
              L" — ESC TO CANCEL");
    ::SetFocus(hwnd_);
    BuildSettingsRows();
    InvalidateAll();
}

void App::FinishRebind(UINT modifiers, UINT virtualKey) {
    const gf::HotkeyAction action = rebindAction_;
    rebinding_ = false;
    rebindAction_ = gf::HotkeyAction::Count;

    if (action == gf::HotkeyAction::Count) {
        return;
    }

    gf::Settings candidate = settings_;
    candidate.hotkeys[static_cast<int>(action)] = gf::HotkeyBinding{modifiers, virtualKey};

    std::wstring error;
    if (ApplyHotkeys(candidate, error)) {
        BuildSettingsRows();
        SetStatus(std::wstring(gf::HotkeyActionLabel(action)) + L" = " +
                  gf::FormatHotkey(modifiers, virtualKey));
    } else {
        BuildSettingsRows();
        SetStatus(L"ERROR: " + error, true);
        ShowToast(L"HOTKEY REJECTED — " + error, true);
    }
    InvalidateAll();
}

void App::AddRuleForWindow(HWND hwnd) {
    if (hwnd == nullptr || !::IsWindow(hwnd) || hwnd == hwnd_) {
        SetStatus(L"NO FOREGROUND WINDOW TO ADD", true);
        return;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == ::GetCurrentProcessId()) {
        SetStatus(L"GHOSTFRAME CANNOT AUTO-CAPTURE ITSELF", true);
        return;
    }
    std::wstring path;
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process != nullptr) {
        wchar_t buffer[1024] = {};
        DWORD size = _countof(buffer);
        if (::QueryFullProcessImageNameW(process, 0, buffer, &size)) {
            path.assign(buffer, size);
        }
        ::CloseHandle(process);
    }
    if (path.empty()) {
        SetStatus(L"CANNOT READ THE PROCESS IMAGE NAME", true);
        return;
    }

    AddRuleForExePath(path);
}

void App::AddRuleForExePath(const std::wstring& path) {
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (name.empty()) {
        SetStatus(L"THAT FILE HAS NO NAME", true);
        return;
    }

    for (const gf::AutoAttachRule& rule : settings_.autoAttach) {
        if (_wcsicmp(rule.match.c_str(), L"exe") == 0 &&
            _wcsicmp(rule.value.c_str(), name.c_str()) == 0) {
            SetStatus(L"AN AUTO-CAPTURE RULE FOR " + name + L" ALREADY EXISTS", true);
            return;
        }
    }

    settings_.autoAttach.push_back(gf::AutoAttachRule{true, L"exe", name, true});
    MarkSettingsDirty();
    BuildSettingsRows();
    SetStatus(L"AUTO-CAPTURE RULE ADDED FOR " + name);
}

void App::ApplyAutoRulesNow() {
    if (attachThread_ != nullptr) {
        SetStatus(L"ATTACHING… — WAIT FOR THE CURRENT ATTACH TO FINISH", true);
        return;
    }
    autoLastScan_ = 0;  // bypass the throttle for an explicit request
    ScanAutoCapture();
    if (attachThread_ == nullptr) {
        SetStatus(L"AUTO-CAPTURE RULES APPLIED — NO NEW WINDOWS MATCHED");
    }
}
