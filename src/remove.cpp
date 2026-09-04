#include "cclean/remove.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "parallel.hpp"

namespace cclean {

namespace {

std::string errno_message(const fs::path& path) {
    return path.string() + ": " + std::strerror(errno);
}

// The entries are read out in full before any of them is unlinked. Removing
// from a directory while its stream is still open leaves the fate of the
// not-yet-returned entries unspecified by POSIX.
bool read_directory(
    DIR* stream,
    const fs::path& shown,
    std::vector<std::string>& names,
    std::string& error)
{
    bool ok = true;

    while (true) {
        errno = 0;
        const dirent* entry = ::readdir(stream);

        if (entry == nullptr) {
            if (errno != 0) {
                error = errno_message(shown);
                ok = false;
            }
            break;
        }

        const std::string name = entry->d_name;

        if (name != "." && name != "..") {
            names.push_back(name);
        }
    }

    return ok;
}

// Opens the directory holding `relative`, one component at a time from `root`,
// refusing to follow a symlink at any of them. Returns -1 with `error` set.
//
// Resolving the parent's whole path in one open() instead is a second lookup of
// every component, made after the scan listed them and before the identity
// check below can mean anything: an interior directory replaced by a symlink in
// that window sent the removal wherever the link pointed, which was reachable
// even though every open beneath the parent already refused to follow one. The
// walk closes that by descending only through descriptors it has opened itself.
//
// `root` is the one component opened by name and followed, because the user
// typed it; everything below it came from the scan.
int open_parent(
    const fs::path& root,
    const fs::path& relative,
    std::string& error)
{
    int parent = ::open(root.empty() ? "." : root.c_str(),
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (parent < 0) {
        error = errno_message(root.empty() ? fs::path(".") : root);
        return -1;
    }

    fs::path walked = root;

    // Everything but the final component, which is the target itself.
    for (auto it = relative.begin(), last = --relative.end();
         it != last; ++it) {
        const std::string component = it->string();

        // lexically_relative() produces ".." for a target that is not under
        // root at all, and "." for a trailing separator. Neither can be walked
        // through safely, and both mean the caller passed a root the target
        // does not belong to.
        if (component.empty() || component == "." || component == "..") {
            error = "Target is not below the root it was found in: " +
                    (root / relative).string();
            ::close(parent);
            return -1;
        }

        walked /= component;

        const int next = ::openat(parent, component.c_str(),
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                  O_CLOEXEC);

        if (next < 0) {
            const int open_errno = errno;

            if (open_errno == ENOENT) {
                error = "Target changed or disappeared: " +
                        (root / relative).string();
                return -1;
            }

            // O_NOFOLLOW reports a symlink as ELOOP or, with O_DIRECTORY, as
            // ENOTDIR, which is also what an ordinary file gives. Which of the
            // two it was is the whole point of the refusal, so it is worth one
            // more syscall to say so: a component that was a directory during
            // the scan and is a symlink now is the case this walk exists for.
            struct stat info;

            if ((open_errno == ELOOP || open_errno == ENOTDIR) &&
                ::fstatat(parent, component.c_str(), &info,
                          AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISLNK(info.st_mode)) {
                error = "Path component is now a symlink, refusing to follow "
                        "it: " + walked.string();
            } else {
                errno = open_errno;
                error = errno_message(walked);
            }

            ::close(parent);
            return -1;
        }

        ::close(parent);
        parent = next;
    }

    return parent;
}

// True when the entry the descriptor or the stat refers to is the same object
// the scan recorded. A Target a caller built by hand carries no identity and
// was never reviewed against a displayed list, so it is checked by type alone.
bool identity_matches(const Target& target, const struct stat& info) {
    return !target.has_identity ||
           (static_cast<std::uint64_t>(info.st_dev) == target.device &&
            static_cast<std::uint64_t>(info.st_ino) == target.inode);
}

bool remove_entry_at(
    int parent,
    const std::string& name,
    const fs::path& shown,
    std::string& error);

// Removes everything inside an open directory, through that descriptor rather
// than through its name. Takes ownership of `directory` either way: it is
// closed before this returns, whether the emptying succeeded or not.
bool empty_directory(
    int directory,
    const fs::path& shown,
    std::string& error)
{
    // fdopendir() takes ownership of the descriptor, and closedir() closes it,
    // so the stream is kept open until the children are gone and dirfd() is
    // what the recursion descends through.
    DIR* stream = ::fdopendir(directory);

    if (stream == nullptr) {
        error = errno_message(shown);
        ::close(directory);
        return false;
    }

    std::vector<std::string> names;
    bool ok = read_directory(stream, shown, names, error);

    if (ok) {
        for (const std::string& child : names) {
            if (!remove_entry_at(::dirfd(stream), child, shown / child,
                                 error)) {
                ok = false;
                break;
            }
        }
    }

    ::closedir(stream);
    return ok;
}

// One descriptor and one stack frame are held per level, for as long as the
// level below is being emptied. That is the price of the property: the
// descriptor is what makes the next step relative to a directory that has
// already been checked rather than to a name that can be re-pointed. A tree
// deep enough to exhaust either limit fails with a reported error on the
// openat() rather than removing the wrong thing, and cannot be built through
// the ordinary filesystem calls in the first place.
bool remove_entry_at(
    int parent,
    const std::string& name,
    const fs::path& shown,
    std::string& error)
{
    // The unlink is attempted before anything is known about the entry, rather
    // than after a stat that says what it is. Files outnumber directories in
    // the trees this removes, so it is one syscall where a stat first was two
    // -- but the reason is that the kernel then makes the decision atomically.
    // unlinkat() without AT_REMOVEDIR removes a symlink and never its target,
    // and refuses a directory outright, so there is no window between deciding
    // and acting for the two to disagree about.
    if (::unlinkat(parent, name.c_str(), 0) == 0) {
        return true;
    }

    // Linux reports a directory here as EISDIR, the BSDs as EPERM. Either can
    // also be a genuine failure on something that is not a directory, which is
    // what the openat below separates.
    const int unlink_errno = errno;

    if (unlink_errno != EISDIR && unlink_errno != EPERM) {
        error = errno_message(shown);
        return false;
    }

    const int directory = ::openat(parent, name.c_str(),
                                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                   O_CLOEXEC);

    if (directory < 0) {
        // Not a directory after all, so the unlink was the real attempt and
        // its error is the one worth reporting.
        if (errno == ENOTDIR || errno == ELOOP) {
            errno = unlink_errno;
        }

        error = errno_message(shown);
        return false;
    }

    if (!empty_directory(directory, shown, error)) {
        return false;
    }

    if (::unlinkat(parent, name.c_str(), AT_REMOVEDIR) != 0) {
        error = errno_message(shown);
        return false;
    }

    return true;
}

// The directory form of a reviewed target. fstatat() answered for the name;
// this opens the name and asks the object, so a directory replaced by another
// directory between the two is refused here rather than emptied. Everything
// below is then removed through that verified descriptor, which is the same
// discipline the walk down from `root` already follows.
bool remove_verified_directory(
    int parent,
    const std::string& name,
    const Target& target,
    std::string& error)
{
    const int directory = ::openat(parent, name.c_str(),
                                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                   O_CLOEXEC);

    if (directory < 0) {
        if (errno == ENOENT) {
            error = "Target changed or disappeared: " + target.path.string();
        } else if (errno == ENOTDIR || errno == ELOOP) {
            error = "Target changed type: " + target.path.string();
        } else {
            error = errno_message(target.path);
        }

        return false;
    }

    struct stat info;

    if (::fstat(directory, &info) != 0) {
        error = errno_message(target.path);
        ::close(directory);
        return false;
    }

    if (!S_ISDIR(info.st_mode) || !identity_matches(target, info)) {
        error = "Target was replaced since it was scanned: " +
                target.path.string();
        ::close(directory);
        return false;
    }

    if (!empty_directory(directory, target.path, error)) {
        return false;
    }

    // The name is resolved once more here, and what it finds need not be the
    // directory just emptied. AT_REMOVEDIR bounds what that can cost to an
    // empty directory somebody put there in the meantime: it refuses a
    // symlink, refuses a file, and refuses a directory with anything in it.
    if (::unlinkat(parent, name.c_str(), AT_REMOVEDIR) != 0) {
        error = errno_message(target.path);
        return false;
    }

    return true;
}

// True when normalising `text` would not change it: no empty component, no
// "." or ".." component, and no trailing separator.
bool is_lexically_normal(const std::string& text) {
    if (text.size() > 1 && text.back() == '/') {
        return false;
    }

    for (std::size_t start = 0; start <= text.size();) {
        const std::size_t separator = text.find('/', start);
        const std::size_t stop =
            separator == std::string::npos ? text.size() : separator;
        const std::size_t length = stop - start;

        // An empty component is a doubled separator, except for the leading
        // one of an absolute path. "." and ".." are what normalising removes.
        if ((length == 0 && start != 0) ||
            (length == 1 && text[start] == '.') ||
            (length == 2 && text[start] == '.' && text[start + 1] == '.')) {
            return false;
        }

        if (separator == std::string::npos) {
            break;
        }

        start = separator + 1;
    }

    return true;
}

// A comparable form of a target's path: lexically normalised, with any
// trailing separator dropped, so that "a/b", "a/./b" and "a/b/" are one path
// and anything below it is this key followed by a separator.
//
// lexically_normal() rebuilds the path component by component and is most of
// the cost of this pass over a long list, while a path that came from the scan
// is already normal. Deciding that is one read of the string.
std::string coverage_key(const fs::path& path) {
    std::string key = path.generic_string();

    if (is_lexically_normal(key)) {
        return key;
    }

    key = path.lexically_normal().generic_string();

    while (key.size() > 1 && key.back() == '/') {
        key.pop_back();
    }

    return key;
}

// True when `outer` is `inner` or names a directory above it.
bool covers(const std::string& outer, const std::string& inner) {
    if (outer == inner) {
        return true;
    }

    if (inner.size() <= outer.size() ||
        inner.compare(0, outer.size(), outer) != 0) {
        return false;
    }

    // A root key already ends in the separator; every other key needs one at
    // the join, so that "/a" does not come out as covering "/ab".
    return outer == "/" || inner[outer.size()] == '/';
}

}  // namespace

bool remove_target(
    const fs::path& root,
    const Target& target,
    std::string& error)
{
    const fs::path relative = target.path.lexically_relative(root);
    const std::string name = target.path.filename().string();

    if (name.empty() || name == "." || name == ".." || relative.empty() ||
        relative == ".") {
        error = "Target is not a removable entry: " + target.path.string();
        return false;
    }

    const int parent = open_parent(root, relative, error);

    if (parent < 0) {
        return false;
    }

    struct stat info;

    if (::fstatat(parent, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        error = errno == ENOENT
            ? "Target changed or disappeared: " + target.path.string()
            : errno_message(target.path);
        ::close(parent);
        return false;
    }

    const bool is_symlink = S_ISLNK(info.st_mode);
    const bool is_directory = S_ISDIR(info.st_mode);

    if (is_symlink != target.is_symlink ||
        (!target.is_symlink && is_directory != target.is_directory)) {
        error = "Target changed type: " + target.path.string();
        ::close(parent);
        return false;
    }

    // A type is not an identity. The run displayed this list and then waited
    // for the user, and a directory replaced by another directory in that
    // window matches every check above; so does a file replaced by another
    // file. What the scan recorded was the object, and that is what has to
    // still be there.
    if (!identity_matches(target, info)) {
        error = "Target was replaced since it was scanned: " +
                target.path.string();
        ::close(parent);
        return false;
    }

    bool ok;

    if (is_directory) {
        ok = remove_verified_directory(parent, name, target, error);
    } else {
        // For a directory the descriptor carries the identity from here on.
        // For everything else there is no such handle: unlinkat() acts on the
        // name, and the check above was made a syscall earlier rather than as
        // part of the same operation. That narrows the window to the interval
        // between two adjacent syscalls instead of the interval a user spends
        // reading a prompt, which is the difference that matters, but it does
        // not close it.
        ok = remove_entry_at(parent, name, target.path, error);
    }

    ::close(parent);
    return ok;
}

std::vector<RemovalResult> remove_targets(
    const fs::path& root,
    const std::vector<Target>& targets)
{
    std::vector<RemovalResult> results(targets.size());

    if (targets.empty()) {
        return results;
    }

    // scan() never lists a descendant of a matched directory, but this
    // overload takes a list the caller built or filtered, which can hold one
    // path twice or hold both a directory and something inside it. Dispatched
    // as they stand, two workers would descend the same subtree: whichever
    // reached a name second would find it already gone and report a failure
    // for a target that was in fact removed, and which of the two that was
    // would vary between runs. So every target is first attributed to the
    // outermost target that covers it, and only those are dispatched.
    std::vector<std::string> keys;
    keys.reserve(targets.size());

    for (const Target& target : targets) {
        keys.push_back(coverage_key(target.path));
    }

    const auto before = [&](std::size_t a, std::size_t b) {
        return keys[a] != keys[b] ? keys[a] < keys[b] : a < b;
    };

    std::vector<std::size_t> order(targets.size());
    std::iota(order.begin(), order.end(), std::size_t{0});

    // scan() returns its targets sorted by path, which is the list this
    // normally gets, and sorting a sorted range still costs its full n log n
    // of string comparisons. Checking for it first costs n of them.
    if (!std::is_sorted(order.begin(), order.end(), before)) {
        std::sort(order.begin(), order.end(), before);
    }

    // Sorted, a covering key precedes everything it covers, but not always
    // immediately: "a/b.txt" sorts between "a/b" and "a/b/c". `open` therefore
    // holds the chain of dispatched targets still above the current one, and
    // the ones that no longer are get popped first.
    std::vector<std::size_t> owner(targets.size());
    std::vector<std::size_t> open;

    for (const std::size_t index : order) {
        while (!open.empty() && !covers(keys[open.back()], keys[index])) {
            open.pop_back();
        }

        // A repeat of the same path always shares an outcome. A path below
        // another target shares one only when that target is a directory: a
        // symlink is unlinked without what it points at being touched, so
        // something named below it is not removed with it -- and the walk down
        // would refuse to follow it in any case, which is an error worth
        // reporting on its own target rather than hiding behind another's.
        const bool covered = !open.empty() &&
            (keys[open.back()] == keys[index] ||
             (targets[open.back()].is_directory &&
              !targets[open.back()].is_symlink));

        if (covered) {
            owner[index] = open.back();
        } else {
            owner[index] = index;
            open.push_back(index);
        }
    }

    // The queue is popped from the back, so the indices go in reversed: a run
    // that ends up on one thread then removes in the order the list was
    // reviewed in, which is what a --verbose log reads best in.
    std::vector<std::size_t> queue;
    queue.reserve(targets.size());

    for (std::size_t i = targets.size(); i-- > 0;) {
        if (owner[i] == i) {
            queue.push_back(i);
        }
    }

    // Each worker writes one distinct element of `results` and touches nothing
    // else, so there is no lock here and no merge afterwards. `children` stays
    // empty: a target is a leaf of this queue, whatever it holds underneath.
    parallel_directories(
        std::move(queue),
        [&](std::size_t index, std::vector<std::size_t>&) {
            RemovalResult& result = results[index];
            result.removed =
                remove_target(root, targets[index], result.error);
        });

    // A covered target went with the one covering it, so it reports that
    // target's outcome: removed when the covering removal succeeded, and the
    // same error when it did not. Reporting it as a failure of its own would
    // say the wrong thing about a path that is no longer there.
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (owner[i] != i) {
            results[i] = results[owner[i]];
        }
    }

    return results;
}

std::vector<RemovalResult> remove_targets(const ScanResult& result) {
    return remove_targets(result.root, result.targets);
}

}  // namespace cclean
