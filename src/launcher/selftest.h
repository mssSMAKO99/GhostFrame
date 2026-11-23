#pragma once

namespace gf {

// Exercises the whole pipeline against tests/target: attach, hide, verify that
// the guard keeps the window excluded while the target keeps resetting the
// affinity, show, uninstall and verify the target is restored.
//
// Returns 0 on success, 1 on failure and 2 when the test target is not running.
int RunSelfTest();

}  // namespace gf
