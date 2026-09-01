#ifndef CCLEAN_SRC_PARALLEL_HPP
#define CCLEAN_SRC_PARALLEL_HPP

// Internal to the library. Not installed, and not part of the public API.

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <iterator>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace cclean {

// Spreads a queue of directories across all cores. A worker pops one, runs
// `scan` on it holding no lock, and merges whatever subdirectories that turned
// up back into the queue. `scan` is responsible for its own results and their
// synchronisation; this only distributes the work.
//
// A throwing `scan` is caught rather than allowed to escape. Every scan in the
// library takes the `error_code` overloads and so does not throw today, but the
// consequences of one that did are out of proportion to the mistake: the worker
// would skip the `--active` below, leaving a count that can never reach zero and
// every other worker parked on it forever, and the exception would then escape a
// thread function and call std::terminate. Catching keeps the bookkeeping on one
// path, and the first exception is rethrown to the caller once the pool is
// joined, so the failure surfaces as it would have serially.
//
//   scan(const Job&, std::vector<Job>& children)
template <typename Job, typename Scan>
void parallel_directories(std::vector<Job> queue, Scan scan) {
    if (queue.empty()) {
        return;
    }

    std::mutex mutex;
    std::condition_variable ready;
    std::size_t active = 0;
    std::exception_ptr failure;

    // Past this many workers the queue is the bottleneck rather than the
    // filesystem, and on a 128-core host the count-per-core pool meant 127
    // threads for a scan with work for a handful: stacks, scheduler pressure,
    // and thread creation itself as a fresh way for a small run to fail.
    constexpr unsigned int ceiling = 32;

    unsigned int limit = std::thread::hardware_concurrency();

    if (limit == 0) {
        limit = 1;
    }

    limit = std::min(limit, ceiling);

    std::vector<std::thread> pool;

    const auto worker = [&] {
        std::unique_lock<std::mutex> lock(mutex);

        while (true) {
            ready.wait(lock, [&] { return !queue.empty() || active == 0; });

            if (queue.empty()) {
                // Nothing queued and nobody left who could queue more.
                ready.notify_all();
                return;
            }

            const Job job = std::move(queue.back());
            queue.pop_back();
            ++active;

            lock.unlock();

            std::vector<Job> children;
            std::exception_ptr caught;

            try {
                scan(job, children);
            } catch (...) {
                caught = std::current_exception();
                children.clear();
            }

            lock.lock();

            if (caught && !failure) {
                failure = caught;
            }

            if (failure) {
                // Abandon the walk. Workers still inside `scan` rejoin through
                // this same path, and those already waiting wake once `active`
                // reaches zero.
                queue.clear();
            } else {
                queue.insert(queue.end(),
                             std::make_move_iterator(children.begin()),
                             std::make_move_iterator(children.end()));
            }

            --active;

            if (!queue.empty() || active == 0) {
                ready.notify_all();
            }
        }
    };

    // Sized to the machine, not to the work: the queue often starts as a
    // single directory, and only grows as children are found. Growing the pool
    // from inside the worker loop to match, which is the obvious answer, was
    // measured and rejected -- in every form tried (an atomic flag or a
    // thread-local one, the spawn inlined or out of line, lazily or with the
    // pool pre-filled) it cost 6-8% of a scan of a real tree, because the
    // state the growth step needs has to stay live across a very tight loop.
    // Paying that on every real run to save the 0.7 ms of thread creation on a
    // run with nothing to do is the wrong way round.
    pool.reserve(limit - 1);

    for (unsigned int i = 0; i + 1 < limit; ++i) {
        try {
            pool.emplace_back(worker);
        } catch (const std::system_error&) {
            // The pool is an optimisation: if the process cannot create
            // another thread the walk still finishes on the ones that did
            // start, down to this thread alone.
            break;
        }
    }

    worker();

    for (std::thread& thread : pool) {
        thread.join();
    }

    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace cclean

#endif  // CCLEAN_SRC_PARALLEL_HPP
