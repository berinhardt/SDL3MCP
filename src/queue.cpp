#include "queue.hpp"

#include <mutex>
#include <utility>

namespace sdl3mcp
{

bool InputQueue::isSettled(const Entry &entry) noexcept
{
    return entry.state == State::Completed || entry.state == State::Failed;
}

InputOutcome InputQueue::settledOutcome(const Entry &entry)
{
    const InputStatus status =
        entry.state == State::Completed ? InputStatus::Applied : InputStatus::Failed;
    return InputOutcome{status, entry.appliedFrame, entry.error};
}

void InputQueue::settlePosted(const Entry &entry, InputStatus status)
{
    if (!entry.posted) {
        return;
    }
    settled_[entry.seq] = InputOutcome{status, entry.appliedFrame, entry.error};
}

void InputQueue::submit(InputBatch batch, std::chrono::milliseconds timeout,
                        InputOutcome &outcome)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (stopped_) {
        outcome = InputOutcome{InputStatus::Stopped, 0, {}};
        return;
    }

    const EntryPtr entry = std::make_shared<Entry>();
    entry->seq = nextSeq_++;
    entry->batch = std::move(batch);
    pending_.emplace(entry->seq, entry);

    changed_.wait_for(lock, timeout, [this, &entry] {
        return stopped_ || isSettled(*entry);
    });

    if (isSettled(*entry)) {
        outcome = settledOutcome(*entry);
        return;
    }
    if (stopped_) {
        outcome = InputOutcome{InputStatus::Stopped, entry->appliedFrame, {}};
        return;
    }

    /* The app has not taken the batch, so it can still be withdrawn cleanly. */
    if (entry->state != State::Taken && entry->state != State::Applied) {
        entry->state = State::Cancelled;
        pending_.erase(entry->seq);
        outcome = InputOutcome{InputStatus::Cancelled, 0, {}};
        return;
    }

    /* The events are out; only the F+1 acknowledgement is missing. */
    outcome = InputOutcome{InputStatus::AppliedUnconfirmed, entry->appliedFrame, {}};
}

bool InputQueue::post(InputBatch batch, uint64_t &token, std::string &error)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
        error = "the input queue is stopped";
        return false;
    }

    const EntryPtr entry = std::make_shared<Entry>();
    entry->seq = nextSeq_++;
    entry->batch = std::move(batch);
    entry->posted = true;
    pending_.emplace(entry->seq, entry);

    token = entry->seq;
    error.clear();
    return true;
}

bool InputQueue::poll(uint64_t token, InputOutcome &outcome)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto settled = settled_.find(token);
    if (settled == settled_.end()) {
        return false;
    }

    outcome = settled->second;
    settled_.erase(settled);
    return true;
}

std::optional<TakenBatch> InputQueue::takeBatch()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || inflight_) {
        return std::nullopt;
    }

    while (!pending_.empty()) {
        auto front = pending_.begin();
        const EntryPtr &entry = front->second;
        if (entry->state != State::Queued) {
            pending_.erase(front);
            continue;
        }

        TakenBatch taken{entry->seq, std::move(entry->batch)};
        entry->state = State::Taken;
        inflight_ = entry;
        pending_.erase(front);
        return taken;
    }
    return std::nullopt;
}

void InputQueue::markApplied(uint64_t seq, uint64_t frame, std::optional<std::string> error)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!inflight_ || inflight_->seq != seq) {
        return;
    }

    inflight_->appliedFrame = frame;
    if (error.has_value()) {
        inflight_->state = State::Failed;
        inflight_->error = std::move(*error);
        lastError_ = inflight_->error;
        settlePosted(*inflight_, InputStatus::Failed);
        inflight_.reset();
    } else {
        inflight_->state = State::Applied;
        lastError_.clear();
    }
    changed_.notify_all();
}

void InputQueue::complete(uint64_t seq)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!inflight_ || inflight_->seq != seq) {
        return;
    }

    inflight_->state = State::Completed;
    lastAppliedFrame_ = inflight_->appliedFrame;
    ++completedCount_;
    settlePosted(*inflight_, InputStatus::Applied);
    inflight_.reset();
    changed_.notify_all();
}

void InputQueue::stop()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
        return;
    }

    stopped_ = true;
    for (auto &queued : pending_) {
        queued.second->state = State::Cancelled;
        /* A posted batch will never be taken, so its poller has to be told. */
        settlePosted(*queued.second, InputStatus::Stopped);
    }
    pending_.clear();
    changed_.notify_all();
}

uint64_t InputQueue::lastAppliedFrame() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return lastAppliedFrame_;
}

std::string InputQueue::lastError() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

uint64_t InputQueue::completedCount() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return completedCount_;
}

} // namespace sdl3mcp
