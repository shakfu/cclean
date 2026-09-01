#ifndef CCLEAN_REMOVE_HPP
#define CCLEAN_REMOVE_HPP

#include <string>

#include "cclean/target.hpp"

namespace cclean {

// Removes one reviewed target, recursively if it is a directory. Returns false
// with `error` set; the message already names the path.
//
// Checking a path and then removing it by that same path is two lookups of the
// same name, and between them a concurrent process can put something else
// there. A type check narrows that but does not close it: a regular file can be
// replaced by another regular file, and fs::remove_all() re-resolves every
// component of every path in the subtree as it walks, so the race repeats at
// each level. In a tree another user can write to, that is enough to delete an
// object the user never reviewed.
//
// This is descriptor-relative instead. The parent directory is opened once, the
// identity check runs against that descriptor, and the removal names the entry
// within it, so a component swapped after the check cannot be reached by name
// at all. O_NOFOLLOW at every open below the parent means a symlink substituted
// for a directory is an error rather than a way out of the tree.
bool remove_target(const Target& target, std::string& error);

}  // namespace cclean

#endif  // CCLEAN_REMOVE_HPP
