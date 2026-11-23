// SETTINGS tab: the hotkey rebinding list, the auto-capture rule list and the
// helpers that edit them.

#include <windows.h>

#include <cwchar>

#include "app.h"
#include "resource.h"

void App::BuildSettingsRows() {
    std::vector<ui::ListRow> hotkeys;
    const int hotkeyCount = settingsSection_ == SettingsSection::General ? 3 : gf::kHotkeyCount;
    for (int i = 0; i < hotkeyCount; ++i) {
        const gf::HotkeyAction action = static_cast<gf::HotkeyAction>(i);
        ui::ListRow row;
        row.title = gf::HotkeyActionLabel(action);
        row.meta = rebinding_ && rebindAction_ == action
                       ? L"press a combination…"
                       : gf::FormatHotkey(settings_.hotkeys[i].modifiers,
                                          settings_.hotkeys[i].virtualKey);
        row.metaMono = true;
        row.metaChip = true;
        row.data = i;
        hotkeys.push_back(std::move(row));
    }
    hotkeysList_.SetEmptyText(L"No hotkeys");
    hotkeysList_.SetRows(std::move(hotkeys));

    std::vector<ui::ListRow> rules;
    for (size_t i = 0; i < settings_.autoAttach.size(); ++i) {
        const gf::AutoAttachRule& rule = settings_.autoAttach[i];
        ui::ListRow row;
        row.title = rule.value;
        row.meta = (rule.match == L"exe" ? L"Executable" : L"Window title") +
                   std::wstring(L"  ·  ") +
                   (rule.hide ? L"hide on attach" : L"keep visible");
        row.data = static_cast<intptr_t>(i);
        row.actions.push_back(ui::ListAction{ID_BTN_SET_ENABLE, rule.enabled ? L"On" : L"Off",
                                             ui::kNeon, rule.enabled, false});
        row.actions.push_back(ui::ListAction{ID_BTN_SET_HIDE, rule.hide ? L"Hide" : L"Show",
                                             ui::kNeon, rule.hide, false});
        row.actions.push_back(ui::ListAction{ID_BTN_SET_REMOVE, L"", ui::kWarn, false, true,
                                             ui::Icon::Trash, true});
        rules.push_back(std::move(row));
    }
    rulesList_.SetEmptyText(L"No auto-capture rules yet — add the foreground app below");
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
        const int index = static_cast<int>(hotkeysList_.DataAt(action.index));
        if (index >= 0 && index < gf::kHotkeyCount) {
            BeginRebind(static_cast<gf::HotkeyAction>(index));
        }
        return;
    }

    const int rule = static_cast<int>(rulesList_.DataAt(action.index));
    if (rule < 0 || rule >= static_cast<int>(settings_.autoAttach.size())) {
        return;
    }
    selectedRule_ = rule;
    rulesList_.Select(action.index);

    gf::AutoAttachRule& entry = settings_.autoAttach[static_cast<size_t>(rule)];
    if (action.kind == ui::ListWidget::ActionKind::RowAction) {
        if (action.actionId == ID_BTN_SET_ENABLE) {
            entry.enabled = !entry.enabled;
            SetStatus(std::wstring(L"Auto-capture rule ") +
                      (entry.enabled ? L"enabled: " : L"disabled: ") + entry.value);
        } else if (action.actionId == ID_BTN_SET_HIDE) {
            entry.hide = !entry.hide;
            SetStatus(std::wstring(L"Rule ") + entry.value +
                      (entry.hide ? L" will hide new windows" : L" will keep new windows visible"));
        } else if (action.actionId == ID_BTN_SET_REMOVE) {
            const std::wstring removed = entry.value;
            settings_.autoAttach.erase(settings_.autoAttach.begin() + rule);
            selectedRule_ = -1;
            MarkSettingsDirty();
            BuildSettingsRows();
            SetStatus(L"Auto-capture rule removed: " + removed);
            InvalidateAll();
            return;
        }
        MarkSettingsDirty();
        BuildSettingsRows();
        if (selectedRule_ >= 0) {
            rulesList_.Select(selectedRule_);
        }
        InvalidateAll();
        return;
    }
    InvalidateAll();
}

void App::ResetHotkeysToDefaults() {
    gf::Settings defaults = gf::DefaultSettings();
    std::wstring error;
    if (ApplyHotkeys(defaults, error)) {
        BuildSettingsRows();
        SetStatus(L"Hotkeys reset to defaults");
    } else {
        SetStatus(L"Error: " + error, true);
    }
}

void App::BeginRebind(gf::HotkeyAction action) {
    rebinding_ = true;
    rebindAction_ = action;
    SetStatus(std::wstring(L"Press a combination for ") + gf::HotkeyActionLabel(action) +
              L" — Esc to cancel");
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
        SetStatus(L"Error: " + error, true);
        ShowToast(L"Hotkey rejected — " + error, true);
    }
    InvalidateAll();
}

void App::AddRuleForWindow(HWND hwnd) {
    if (hwnd == nullptr || !::IsWindow(hwnd) || hwnd == hwnd_) {
        SetStatus(L"No foreground window to add", true);
        return;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == ::GetCurrentProcessId()) {
        SetStatus(L"GhostFrame cannot auto-capture itself", true);
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
        SetStatus(L"Cannot read the process image name", true);
        return;
    }

    AddRuleForExePath(path);
}

void App::AddRuleForExePath(const std::wstring& path) {
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (name.empty()) {
        SetStatus(L"That file has no name", true);
        return;
    }

    for (const gf::AutoAttachRule& rule : settings_.autoAttach) {
        if (_wcsicmp(rule.match.c_str(), L"exe") == 0 &&
            _wcsicmp(rule.value.c_str(), name.c_str()) == 0) {
            SetStatus(L"An auto-capture rule for " + name + L" already exists", true);
            return;
        }
    }

    settings_.autoAttach.push_back(gf::AutoAttachRule{true, L"exe", name, true});
    MarkSettingsDirty();
    BuildSettingsRows();
    SetStatus(L"Auto-capture rule added for " + name);
}

void App::ApplyAutoRulesNow() {
    if (attachThread_ != nullptr) {
        SetStatus(L"ATTACHING… — WAIT FOR THE CURRENT ATTACH TO FINISH", true);
        return;
    }
    autoLastScan_ = 0;  // bypass the throttle for an explicit request
    ScanAutoCapture();
    if (attachThread_ == nullptr) {
        SetStatus(L"Auto-capture rules applied — no new windows matched");
    }
}
