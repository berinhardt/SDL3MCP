/*
  The input queue: the only shared state between sdl_input on the server thread
  and SDL3MCP_AppIterate on the app thread.

  A batch travels Queued -> Taken -> Applied -> Completed on the happy path. The
  server blocks in submit() until the batch is Completed, which the app signals
  at frame F+1, the frame after the one that pushed the events. That extra frame
  is what makes Applied honest: by then the app has consumed the events, and the
  text buffers those events borrow may be freed.

  Timeouts have two shapes, because the queue may or may not still own the
  batch:

  - Before takeBatch() the queue still owns it, so submit() marks it Cancelled,
    removes the entry, and returns Cancelled. The app must never see it.
  - After takeBatch() the app owns the batch and may already have pushed its
    events; rolling back is impossible. submit() therefore returns
    AppliedUnconfirmed with whatever frame the app has reported so far and
    leaves the entry for the app to complete(). The input was delivered; only
    the acknowledgement is missing.

  Ownership: takeBatch() moves the InputBatch out of the queue. From then until
  complete() the app holds it and must keep it alive, because SDL text events
  borrow the string. The queue never frees a batch it has handed out. On a
  failed apply the app may free the batch immediately, since no events were
  pushed.

  One mutex and one condition variable guard everything. Nothing here calls SDL,
  so every member is legal on either thread, and no member may be read without
  the mutex. Entries are shared_ptr-owned so a waiter that times out can still
  read its outcome after the queue drops the entry. Submit callers are
  serialized by the server; the class does not enforce that because a single
  waiter at a time is a transport-level property. The queue stays alive for the
  whole session: destroying it while a submit is blocked is undefined, exactly
  as destroying a condition variable under a waiter would be.
*/

#ifndef SDL3MCP_QUEUE_HPP
#define SDL3MCP_QUEUE_HPP

#include "input.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace sdl3mcp
{

enum class InputStatus
{
    /* The app applied the batch and published it at F+1. */
    Applied,
    /* The app took the batch but the waiter timed out before completion. */
    AppliedUnconfirmed,
    /* The waiter timed out before the app took the batch; nothing was pushed. */
    Cancelled,
    /* The app could not apply the batch; `error` says why. */
    Failed,
    /* The queue was stopped; the batch will never be applied. */
    Stopped
};

/*
  Cancelled is the default so a caller that forgets to run a submit never reads
  an indeterminate status.
*/
struct InputOutcome
{
    InputStatus status = InputStatus::Cancelled;
    uint64_t appliedFrame = 0;
    std::string error;
};

/* The batch plus the sequence number needed to mark it applied and complete it. */
struct TakenBatch
{
    uint64_t seq = 0;
    InputBatch batch;
};

class InputQueue
{
public:
    /*
      Queues the batch and blocks until the app completes it, the app fails it,
      the queue is stopped, or `timeout` expires. Server thread only, one waiter
      at a time. On return `outcome` describes what happened.
    */
    void submit(InputBatch batch, std::chrono::milliseconds timeout, InputOutcome &outcome);

    /*
      Hands the next queued batch to the app thread and moves its ownership
      there. Non-blocking. Returns nullopt when the queue is stopped, when no
      batch is queued, or while a previously taken batch is still awaiting
      completion: the app publishes one batch per frame at most.
    */
    std::optional<TakenBatch> takeBatch();

    /*
      Reports the frame the batch was applied at. A non-empty `error` fails the
      entry, wakes the waiter, and drops it: nothing was pushed, so the app may
      free the batch at once. Success keeps the entry until complete(). App
      thread only.
    */
    void markApplied(uint64_t seq, uint64_t frame, std::optional<std::string> error);

    /*
      Publishes the batch as done at F+1. The waiter returns Applied, and the app
      may free the batch and its text from here on. Unknown or already removed
      sequence numbers are ignored. App thread only.
    */
    void complete(uint64_t seq);

    /*
      Idempotent. Cancels every queued batch, releases waiters with Stopped, and
      refuses new submits. Batches already taken stay valid until the app
      completes them.
    */
    void stop();

    /* The frame of the most recently completed batch, 0 before the first one. */
    uint64_t lastAppliedFrame() const;

    /* The most recent apply error; cleared by the next successful apply. */
    std::string lastError() const;

    /* How many batches reached completion; failed ones never count. */
    uint64_t completedCount() const;

private:
    enum class State
    {
        Queued,
        Taken,
        Applied,
        Completed,
        Failed,
        Cancelled
    };

    struct Entry
    {
        uint64_t seq = 0;
        State state = State::Queued;
        InputBatch batch;
        uint64_t appliedFrame = 0;
        std::string error;
    };

    using EntryPtr = std::shared_ptr<Entry>;

    static bool isSettled(const Entry &entry) noexcept;

    mutable std::mutex mutex_;
    std::condition_variable changed_;

    /* Entries the queue still owns, in submission order. */
    std::map<uint64_t, EntryPtr> pending_;

    /* The one batch the app holds; null when nothing is awaiting completion. */
    EntryPtr inflight_;

    uint64_t nextSeq_ = 1;
    uint64_t lastAppliedFrame_ = 0;
    std::string lastError_;
    uint64_t completedCount_ = 0;
    bool stopped_ = false;
};

} // namespace sdl3mcp

#endif /* SDL3MCP_QUEUE_HPP */
