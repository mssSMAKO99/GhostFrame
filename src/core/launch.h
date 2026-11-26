// `--launch "<app.exe>"` wrapper.
//
// Starts an application and, when asked, captures its main window through the
// daemon.  This is the mode a shortcut can use: the target is opened and hidden
// from capture in one step, without ever showing the GhostFrame UI.
#pragma once

#include <string>

namespace gf {

// `capture` attaches the target's first top-level window; `hide` engages the
// capture exclusion right after a successful attach.  Returns 0 when the
// application was started (and, if requested, captured).
int RunLaunch(const std::wstring& application, bool capture, bool hide);

}  // namespace gf