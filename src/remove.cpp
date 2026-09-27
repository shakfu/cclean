#include "cclean/remove.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "parallel.hpp"
#include "removal_hooks.hpp"

namespace cclean {

namespace {

std::string errno_message(const fs::path& path) {
    return path.string() + ": " + std::strerror(errno);
}

// Set only by the test suite, through "removal_hooks.hpp". Null everywhere
// else, so each costs one branch.
testing::RemovalWindowHook g_removal_window_hook = nullptr;
testing::WalkStepHook g_walk_step_hook = nullptr;

// Called where a reviewed target has been confirmed and the syscall that
// removes it by name has not run yet.
void enter_removal_window() {
    if (g_removal_window_hook != nullptr) {
        g_removal_window_hook();
    }
}

// Called before the openat() that descends into `component`.
void enter_walk_step(const std::string& component) {
    if (g_walk_step_hook != nullptr) {
        g_walk_step_hook(component.c_str());
    }
}

// Counts unlinked entries across workers. The atomic increment is per entry;
// the serialised callback runs once per 256, since a lock per unlink would
// contend on the path that dominates a run.
class Tally {
public:
    explicit Tally(const ProgressFn& progress) : progress_(progress) {}

    void add() {
        const std::uintmax_t done =
            count_.fetch_add(1, std::memory_order_relaxed) + 1;

        if (done % 256 == 0) {
            report(done);
        }
    }

    void finish() { report(count_.load(std::memory_order_relaxed)); }

private:
    const ProgressFn& progress_;
    std::atomic<std::uintmax_t> count_{0};
    std::mutex mutex_;
    std::uintmax_t reported_ = 0;

    // A worker can reach the lock after one holding a later count, so an
    // older count is dropped rather than drawn as a step backwards.
    void report(std::uintmax_t done) {
        const std::lock_guard<std::mutex> guard(mutex_);

        if (done > reported_ || done == 0) {
            reported_ = done;
            progress_("removing", done);
        }
    }
};

void count_removal(Tally* tally) {
    if (tally != nullptr) {
        tally->add();
    }
}

// The entries are read out in full before any of them is unlinked. Removing
// from a directory while its stream is still open leaves the fate of the
// not-yet-returned entries unspecified by POSIX.
//
// `directories`, when given, receives one flag per name: whether d_type said
// directory. It is a hint for where to split work, never a safety check.
bool read_directory(
    DIR* stream,
    const fs::path& shown,
    std::vector<std::string>& names,
    std::string& error,
    std::vector<bool>* directories = nullptr)
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

            if (directories != nullptr) {
                directories->push_back(entry->d_type == DT_DIR);
            }
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

        enter_walk_step(component);

        const int next = ::openat(parent, component.c_str(),
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                  O_CLOEXEC);

        if (next < 0) {
            const int open_errno = errno;

            if (open_errno == ENOENT) {
                error = "Target changed or disappeared: " +
                        (root / relative).string();
                ::close(parent);
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
    Tally* tally,
    std::string& error);

// Removes everything inside an open directory, through that descriptor rather
// than through its name. Takes ownership of `directory` either way: it is
// closed before this returns, whether the emptying succeeded or not.
bool empty_directory(
    int directory,
    const fs::path& shown,
    Tally* tally,
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
                                 tally, error)) {
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
    Tally* tally,
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
        count_removal(tally);
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

    if (!empty_directory(directory, shown, tally, error)) {
        return false;
    }

    if (::unlinkat(parent, name.c_str(), AT_REMOVEDIR) != 0) {
        error = errno_message(shown);
        return false;
    }

    count_removal(tally);
    return true;
}

// A directory opened by the split below, held until everything under it is
// gone. `parent` is the descriptor of the held directory containing it.
struct HeldDirectory {
    DIR* stream;
    int parent;
    std::string name;
    fs::path shown;
};

// Closes every held stream on every path out, an exception from the pool
// included.
struct HeldDirectories {
    std::vector<HeldDirectory> list;

    ~HeldDirectories() {
        for (auto it = list.rbegin(); it != list.rend(); ++it) {
            ::closedir(it->stream);
        }
    }
};

// One entry for a worker to remove, recursively if it is a directory.
struct SplitJob {
    int parent;
    std::string name;
    fs::path shown;
};

// empty_directory() spread across the worker pool, for one large target.
// Removing targets in parallel leaves a single large target on one thread.
//
// The top levels are opened here, breadth first, until there are enough
// subdirectories to keep the pool busy. Every entry of the last level opened
// becomes a job for remove_entry_at(), files included: a Cargo target/ holds
// gigabytes in flat directories like debug/deps. Workers descend only through
// descriptors held here, so the no-follow walk is unchanged. The held
// directories are removed deepest first once every job has finished, which
// keeps any directory from being removed while a worker is below it.
//
// Takes ownership of `directory`, as empty_directory() does.
bool empty_directory_split(
    int directory,
    const fs::path& shown,
    Tally* tally,
    std::string& error)
{
    // Enough subdirectories for 4 per worker; beyond that the pool balances
    // itself. The descriptor cap stays well inside the common limit of 1024.
    const std::size_t wanted = std::size_t{4} * worker_limit();
    constexpr std::size_t max_held = 256;
    constexpr int max_depth = 4;

    HeldDirectories held;
    DIR* const root_stream = ::fdopendir(directory);

    if (root_stream == nullptr) {
        error = errno_message(shown);
        ::close(directory);
        return false;
    }

    held.list.push_back({root_stream, -1, "", shown});

    std::vector<SplitJob> jobs;
    std::size_t level_begin = 0;

    for (int depth = 1;; ++depth) {
        const std::size_t level_end = held.list.size();
        std::vector<SplitJob> subdirectories;

        for (std::size_t i = level_begin; i < level_end; ++i) {
            // Copied out: the list grows below, which moves its elements.
            const int fd = ::dirfd(held.list[i].stream);
            const fs::path here = held.list[i].shown;

            std::vector<std::string> names;
            std::vector<bool> directories;

            if (!read_directory(held.list[i].stream, here, names, error,
                                &directories)) {
                return false;
            }

            for (std::size_t n = 0; n < names.size(); ++n) {
                SplitJob job{fd, names[n], here / names[n]};
                (directories[n] ? subdirectories : jobs)
                    .push_back(std::move(job));
            }
        }

        if (subdirectories.size() >= wanted || depth == max_depth ||
            held.list.size() + subdirectories.size() > max_held) {
            jobs.insert(jobs.end(),
                        std::make_move_iterator(subdirectories.begin()),
                        std::make_move_iterator(subdirectories.end()));
            break;
        }

        level_begin = level_end;

        for (SplitJob& candidate : subdirectories) {
            const int next = ::openat(candidate.parent, candidate.name.c_str(),
                                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                      O_CLOEXEC);
            DIR* const stream = next < 0 ? nullptr : ::fdopendir(next);

            // Not a directory after all, or not openable: the job's own
            // removal decides what it is and reports any error.
            if (stream == nullptr) {
                if (next >= 0) {
                    ::close(next);
                }

                jobs.push_back(std::move(candidate));
                continue;
            }

            held.list.push_back({stream, candidate.parent,
                                 std::move(candidate.name),
                                 std::move(candidate.shown)});
        }

        if (held.list.size() == level_end) {
            break;
        }
    }

    // One slot per job, written by one worker each. The first failure in job
    // order is reported, so the message does not depend on scheduling.
    std::vector<std::string> errors(jobs.size());
    std::vector<std::size_t> queue(jobs.size());
    std::iota(queue.rbegin(), queue.rend(), std::size_t{0});

    parallel_directories(
        std::move(queue),
        [&](std::size_t index, std::vector<std::size_t>&) {
            const SplitJob& job = jobs[index];
            std::string job_error;

            if (!remove_entry_at(job.parent, job.name, job.shown, tally,
                                 job_error)) {
                errors[index] = job_error.empty() ? job.shown.string()
                                                  : std::move(job_error);
            }
        });

    for (std::string& message : errors) {
        if (!message.empty()) {
            error = std::move(message);
            return false;
        }
    }

    for (std::size_t i = held.list.size(); i-- > 1;) {
        const HeldDirectory& entry = held.list[i];

        if (::unlinkat(entry.parent, entry.name.c_str(), AT_REMOVEDIR) != 0) {
            error = errno_message(entry.shown);
            return false;
        }

        count_removal(tally);
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
    bool split,
    Tally* tally,
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

    const bool emptied = split
        ? empty_directory_split(directory, target.path, tally, error)
        : empty_directory(directory, target.path, tally, error);

    if (!emptied) {
        return false;
    }

    // The name is resolved once more here, and what it finds need not be the
    // directory just emptied. AT_REMOVEDIR bounds what that can cost to an
    // empty directory somebody put there in the meantime: it refuses a
    // symlink, refuses a file, and refuses a directory with anything in it.
    enter_removal_window();

    if (::unlinkat(parent, name.c_str(), AT_REMOVEDIR) != 0) {
        error = errno_message(target.path);
        return false;
    }

    count_removal(tally);
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

bool remove_counted(
    const fs::path& root,
    const Target& target,
    bool split,
    Tally* tally,
    std::string& error);

}  // namespace

namespace testing {

void set_removal_window_hook(RemovalWindowHook hook) {
    g_removal_window_hook = hook;
}

void set_walk_step_hook(WalkStepHook hook) {
    g_walk_step_hook = hook;
}

}  // namespace testing

bool remove_target(
    const fs::path& root,
    const Target& target,
    std::string& error)
{
    return remove_counted(root, target, false, nullptr, error);
}

namespace {

bool remove_counted(
    const fs::path& root,
    const Target& target,
    bool split,
    Tally* tally,
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
        ok = remove_verified_directory(parent, name, target, split, tally,
                                       error);
    } else {
        // For a directory the descriptor carries the identity from here on.
        // For everything else there is no such handle: unlinkat() acts on the
        // name, and the check above was made a syscall earlier rather than as
        // part of the same operation. That narrows the window to the interval
        // between two adjacent syscalls instead of the interval a user spends
        // reading a prompt, which is the difference that matters, but it does
        // not close it.
        enter_removal_window();
        ok = remove_entry_at(parent, name, target.path, tally, error);
    }

    ::close(parent);
    return ok;
}

}  // namespace

std::vector<RemovalResult> remove_targets(
    const fs::path& root,
    const std::vector<Target>& targets,
    const ProgressFn& progress,
    bool split)
{
    // Below this, a target empties in a few milliseconds on one thread, and
    // starting a pool for it costs about 0.7 ms.
    constexpr std::uintmax_t split_threshold = 4096;

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
    std::vector<std::size_t> large;
    queue.reserve(targets.size());

    for (std::size_t i = targets.size(); i-- > 0;) {
        if (owner[i] != i) {
            continue;
        }

        const bool splits = split && targets[i].entries >= split_threshold;
        (splits ? large : queue).push_back(i);
    }

    // Each worker writes one distinct element of `results` and touches nothing
    // else, so there is no lock here and no merge afterwards. `children` stays
    // empty: a target is a leaf of this queue, whatever it holds underneath.
    Tally tally(progress);
    Tally* const counting = progress ? &tally : nullptr;

    parallel_directories(
        std::move(queue),
        [&](std::size_t index, std::vector<std::size_t>&) {
            RemovalResult& result = results[index];
            result.removed = remove_counted(root, targets[index], false,
                                            counting, result.error);
        });

    // One at a time, each spread across the pool by the split. Running them
    // inside the pool above would start a pool per worker.
    for (auto it = large.rbegin(); it != large.rend(); ++it) {
        RemovalResult& result = results[*it];
        result.removed =
            remove_counted(root, targets[*it], true, counting, result.error);
    }

    if (counting != nullptr) {
        tally.finish();
    }

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

std::vector<RemovalResult> remove_targets(
    const ScanResult& result,
    const ProgressFn& progress,
    bool split)
{
    return remove_targets(result.root, result.targets, progress, split);
}

}  // namespace cclean
