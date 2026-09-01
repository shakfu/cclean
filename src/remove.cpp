#include "cclean/remove.hpp"

#include <cerrno>
#include <cstring>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

bool remove_target(const Target& target, std::string& error) {
    const fs::path parent_path = target.path.parent_path();
    const std::string name = target.path.filename().string();

    if (name.empty() || name == "." || name == "..") {
        error = "Target is not a removable entry: " + target.path.string();
        return false;
    }

    // No O_NOFOLLOW on this one: ROOT itself may legitimately have been reached
    // through a symlinked path, and the user named it. Every open below ROOT,
    // where the names come from the scan rather than from the user, refuses to
    // follow one.
    const int parent = ::open(parent_path.empty() ? "." : parent_path.c_str(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (parent < 0) {
        error = errno == ENOENT
            ? "Target changed or disappeared: " + target.path.string()
            : errno_message(parent_path);
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

}  // namespace cclean
