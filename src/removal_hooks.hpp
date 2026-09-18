#ifndef CCLEAN_SRC_REMOVAL_HOOKS_HPP
#define CCLEAN_SRC_REMOVAL_HOOKS_HPP

// Internal to the library. Not installed, and not part of the public API.
//
// Two points in remove_target() where a concurrent process changes what the
// next syscall finds, and where the suite has to stand in for that process: a
// thread cannot be aimed at an interval two syscalls wide. Both hooks are null
// unless a test sets one, and no library code sets either. They are compiled
// into every build rather than guarded by a macro so that the suite exercises
// the code that ships, at the cost of one null check per component walked and
// per target removed.
//
// Neither is synchronised. A caller sets one around a single remove_target()
// call and clears it afterwards; remove_targets() spreads its work across the
// pool, so a hook left installed would be read from several threads at once.

namespace cclean {
namespace testing {

// Fires between a reviewed target's identity check and the syscall that
// removes it by name. <cclean/remove.hpp> documents that interval as narrowed
// to two adjacent syscalls rather than closed, and what a replacement landing
// in it can reach is the bound that makes it tolerable.
using RemovalWindowHook = void (*)();

void set_removal_window_hook(RemovalWindowHook hook);

// Fires immediately before the openat() of `component`, once per interior
// component of the path from the root down. This is where the walk earns its
// keep: the entry the scan listed is reached only through descriptors the walk
// opened itself, so a component replaced here has to be refused rather than
// followed.
using WalkStepHook = void (*)(const char* component);

void set_walk_step_hook(WalkStepHook hook);

}  // namespace testing
}  // namespace cclean

#endif
