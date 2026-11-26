// The compile-time view registry.
//
// This is the only place that knows which tabs exist.  Adding, removing or
// reordering a tab is a one-line change here; the shell builds the tab strip,
// the layout and the paint/input routing from this list.

#include "view.h"

#include "settings_view.h"
#include "shortcuts_view.h"
#include "targets_view.h"
#include "windows_view.h"

namespace gf {

const std::vector<ViewFactory>& ViewFactories() {
    static const std::vector<ViewFactory> factories = {
        [](ViewHost& host) -> std::unique_ptr<IView> {
            return std::make_unique<TargetsView>(host);
        },
        [](ViewHost& host) -> std::unique_ptr<IView> {
            return std::make_unique<WindowsView>(host);
        },
        [](ViewHost& host) -> std::unique_ptr<IView> {
            return std::make_unique<ShortcutsView>(host);
        },
        [](ViewHost& host) -> std::unique_ptr<IView> {
            return std::make_unique<SettingsView>(host);
        },
    };
    return factories;
}

}  // namespace gf