#pragma once

// Cancellation for blocking backend work.
//
// The API client parks callers on rate-limit windows, Downloads polls a cancel
// flag between chunks, and a Runner shutdown has to be able to pull a worker out
// of all of that promptly. A scattered std::function parameter cannot express
// that: the blocking waits live in shared backend code that a caller reaches
// through three layers of domain objects.
//
// The scope is thread-local, so a unit of work posts it on entry and every
// backend wait it performs observes it without an extra parameter on every call.
// It is a stack rather than a single flag because a runner thread executes
// nested units of work, each of which may carry its own cancellation state, and
// the inner one must go away when it finishes without clearing the outer one.
//
// The scope only borrows the flag: an atomic_bool owns the state and outlives
// every thread reading it. Nothing here allocates, so it is safe to enter and
// leave on a code path that runs while the process is shutting down.

#include <atomic>

namespace mikai::backend {

namespace detail {

// A node of the innermost-to-outermost chain on the calling thread, or nullptr.
// A raw chain rather than a container, because pushing and popping must not
// allocate; the node itself lives inside the scope object on that stack frame.
struct CancellationNode {
    const std::atomic_bool* state = nullptr;
    const CancellationNode* previous = nullptr;
};

inline thread_local const CancellationNode* cancellationChain = nullptr;

}  // namespace detail

class WorkCancellationScope {
public:
    explicit WorkCancellationScope(const std::atomic_bool* state) noexcept
        : node_{state, detail::cancellationChain} {
        detail::cancellationChain = &node_;
    }

    ~WorkCancellationScope() { detail::cancellationChain = node_.previous; }

    WorkCancellationScope(const WorkCancellationScope&) = delete;
    WorkCancellationScope& operator=(const WorkCancellationScope&) = delete;

    /// True when any scope installed on the calling thread has been cancelled.
    /// Every enclosing scope is consulted, so an uncancelled inner unit of work
    /// cannot mask a cancelled outer one. False when no scope is installed.
    static bool cancellationRequested() noexcept {
        for (const detail::CancellationNode* node = detail::cancellationChain; node != nullptr;
             node = node->previous) {
            const std::atomic_bool* state = node->state;
            if (state != nullptr && state->load(std::memory_order_relaxed)) return true;
        }
        return false;
    }

private:
    detail::CancellationNode node_;
};

}  // namespace mikai::backend
