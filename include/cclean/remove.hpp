#ifndef CCLEAN_REMOVE_HPP
#define CCLEAN_REMOVE_HPP

#include <string>
#include <vector>

#include "cclean/scan.hpp"
#include "cclean/target.hpp"

namespace cclean {

// Removes one reviewed target, recursively if it is a directory. `root` is the
// root the scan was given, and `target.path` must be at or below it. Returns
// false with `error` set; the message already names the path.
//
// Checking a path and then removing it by that same path is two lookups of the
// same name, and between them a concurrent process can put something else
// there. A type check narrows that but does not close it: a regular file can be
// replaced by another regular file, and fs::remove_all() re-resolves every
// component of every path in the subtree as it walks, so the race repeats at
// each level. In a tree another user can write to, that is enough to delete an
// object the user never reviewed.
//
// This is descriptor-relative instead. Every component from `root` down is
// opened with O_NOFOLLOW, one at a time, through the descriptor the component
// above it returned; the identity check runs against the descriptor the walk
// ends on, and the removal names the entry within it. So a component swapped
// after the scan cannot be reached by name at all, and a symlink substituted
// for a directory anywhere on the way down is an error rather than a way out
// of the tree.
//
// `root` itself is opened by name and followed, because the user typed it and
// may legitimately have reached it through a symlink. Every name below it came
// from the scan instead, and none of them is followed.
//
// The descriptor the walk ends on is checked for the type the scan saw and for
// the device and inode it recorded, because a type is not an identity: between
// the list being displayed and the user answering the prompt, a directory can
// be replaced by another directory, and a file by another file. A directory is
// then opened, confirmed a second time through fstat() on the descriptor
// itself, and emptied through it. A non-directory is unlinked by name one
// syscall after its identity was confirmed -- narrowed to that interval rather
// than closed, since there is no handle to unlink through.
//
// A Target built by hand carries no identity (`has_identity` false) and is
// checked by type alone: it was never reviewed against a displayed list, so
// there is nothing for a replacement to have been substituted for.
bool remove_target(
    const fs::path& root,
    const Target& target,
    std::string& error);

// The outcome of one removal, in the same position as its target.
struct RemovalResult {
    bool removed = false;
    // Set only when `removed` is false; the message already names the path.
    std::string error;
};

// Removes every target, spreading them across the same worker pool the scan
// uses. Removal is unlinkat per entry and was the whole cost of a run: on a
// 134,400-file tree with 2,400 targets the walk, the sizing and the sort took
// 30 ms together and the serial removal took 940 ms. The same run now takes
// 270 ms end to end, which is level with what `xargs -P8 rm -rf` reaches over
// the same targets.
//
// The unit of work is one target, not one directory level as it is when
// sizing, so a run whose targets are one enormous node_modules parallelises no
// better than the serial loop did. Emptying a single target across threads
// means handing a directory descriptor between them and unlinking the
// directory only once every worker below it has finished, which is a
// dependency the flat work queue cannot express -- and this is the deletion
// path, where the descriptor discipline is the safety property. Removing
// several targets at once needs none of that: each opens its own parent and
// shares nothing.
//
// Results are returned in the order the targets were given, so what a caller
// prints does not depend on which worker drew which target.
//
// The list may overlap: scan() never emits a descendant of a matched
// directory, but a caller that built or filtered its own list can pass the
// same path twice, or pass both `cache` and `cache/item`, or two spellings of
// one path. Removing those concurrently would race two workers down one
// subtree and turn the loser's "already gone" into a reported failure, so a
// target that another target covers is not dispatched: it is removed as part
// of that one and reports its outcome, error included, which then names the
// covering path rather than its own. A path below a symlink target is not
// covered by it, since unlinking a symlink removes nothing underneath.
std::vector<RemovalResult> remove_targets(
    const fs::path& root,
    const std::vector<Target>& targets);

// The whole of a scan, which carries the root it was given: the form to prefer,
// since a result and a root cannot be mismatched. The overload above is for a
// caller removing a list it has filtered or built itself, which then has to
// name the root those targets were found under.
std::vector<RemovalResult> remove_targets(const ScanResult& result);

}  // namespace cclean

#endif  // CCLEAN_REMOVE_HPP
