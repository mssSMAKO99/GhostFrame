#pragma once

namespace gf {

// Runs the background daemon: takes the single-instance mutex, creates a hidden
// message window and the named-pipe server, then pumps messages until a Shutdown
// request arrives.  Returns the process exit code.  A second instance finds the
// mutex taken and returns 0 without disturbing the first.
int RunDaemon();

// Local, scriptable exercise of the daemon protocol: ensures a daemon is running
// and performs a Hello / GetSnapshot / Send / Shutdown round trip, reporting the
// result to stdout and to `daemon-check.log` next to the executable.  Returns 0
// on success.  Driven by the hidden `--daemon-check` flag.
int RunDaemonCheck();

}  // namespace gf
