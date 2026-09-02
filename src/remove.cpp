#include "cclean/remove.hpp"

#include <cerrno>
#include <cstring>
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

    if (!ok) {
        return false;
    }

    if (::unlinkat(parent, name.c_str(), AT_REMOVEDIR) != 0) {
        error = errno_message(shown);
        return false;
    }

    return true;
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

    const bool ok = remove_entry_at(parent, name, target.path, error);
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

    // The queue is popped from the back, so the indices go in reversed: a run
    // that ends up on one thread then removes in the order the list was
    // reviewed in, which is what a --verbose log reads best in.
    std::vector<std::size_t> queue;
    queue.reserve(targets.size());

    for (std::size_t i = targets.size(); i-- > 0;) {
        queue.push_back(i);
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

    return results;
}

std::vector<RemovalResult> remove_targets(const ScanResult& result) {
    return remove_targets(result.root, result.targets);
}

}  // namespace cclean
