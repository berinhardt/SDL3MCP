/*
  The queue's contract is a conversation between two threads, so every test runs
  the blocking submit on a worker and plays the app on the main thread.
*/

#include "queue.hpp"

#include "test_util.hpp"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace
{

using namespace std::chrono_literals;

using sdl3mcp::InputBatch;
using sdl3mcp::InputOutcome;
using sdl3mcp::InputQueue;
using sdl3mcp::InputStatus;
using sdl3mcp::ReleaseAllInput;
using sdl3mcp::TakenBatch;

/* The action count identifies the batch, so a take cannot be mistaken for another. */
InputBatch batchWith(std::size_t actionCount)
{
    InputBatch batch;
    for (std::size_t index = 0; index < actionCount; ++index) {
        batch.actions.emplace_back(ReleaseAllInput{});
    }
    return batch;
}

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds limit = 2000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

/*
  Runs one blocking submit on its own thread. The destructor joins, which also
  keeps the queue alive until the submit has returned.
*/
class SubmitWorker
{
public:
    SubmitWorker(InputQueue &queue, InputBatch batch, std::chrono::milliseconds timeout)
        : future_(promise_.get_future()),
          thread_([this, &queue, batch = std::move(batch), timeout]() mutable {
              InputOutcome outcome;
              queue.submit(std::move(batch), timeout, outcome);
              promise_.set_value(std::move(outcome));
          })
    {
    }

    ~SubmitWorker()
    {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    SubmitWorker(const SubmitWorker &) = delete;
    SubmitWorker &operator=(const SubmitWorker &) = delete;

    std::future<InputOutcome> &future() { return future_; }

private:
    std::promise<InputOutcome> promise_;
    std::future<InputOutcome> future_;
    std::thread thread_;
};

InputOutcome awaitResult(std::future<InputOutcome> &future,
                         std::chrono::milliseconds limit = 2000ms)
{
    CHECK(future.wait_for(limit) == std::future_status::ready);
    return future.get();
}

/* Polls until the submit has been enqueued; an empty queue cannot be asserted on. */
std::optional<TakenBatch> takeEventually(InputQueue &queue)
{
    std::optional<TakenBatch> taken;
    CHECK(waitUntil([&queue, &taken] {
        taken = queue.takeBatch();
        return taken.has_value();
    }));
    return taken;
}

void appliedOnlyAfterCompletion()
{
    InputQueue queue;
    SubmitWorker worker(queue, batchWith(2), 5000ms);

    const std::optional<TakenBatch> taken = takeEventually(queue);
    CHECK(taken->seq == 1);
    CHECK(taken->batch.actions.size() == 2);
    CHECK(queue.completedCount() == 0);

    queue.markApplied(taken->seq, 12, std::nullopt);
    /* Applied is not a settlement: the server waits for the F+1 publication. */
    CHECK(worker.future().wait_for(50ms) == std::future_status::timeout);
    CHECK(queue.completedCount() == 0);
    CHECK(queue.lastAppliedFrame() == 0);

    queue.complete(taken->seq);

    const InputOutcome outcome = awaitResult(worker.future());
    CHECK(outcome.status == InputStatus::Applied);
    CHECK(outcome.appliedFrame == 12);
    CHECK(outcome.error.empty());
    CHECK(queue.completedCount() == 1);
    CHECK(queue.lastAppliedFrame() == 12);
}

void timeoutBeforeTakeCancels()
{
    InputQueue queue;
    SubmitWorker worker(queue, batchWith(1), 50ms);

    const InputOutcome outcome = awaitResult(worker.future());
    CHECK(outcome.status == InputStatus::Cancelled);
    CHECK(!queue.takeBatch().has_value());
    CHECK(queue.completedCount() == 0);
    CHECK(queue.lastAppliedFrame() == 0);
}

void timeoutAfterApplyIsUnconfirmed()
{
    InputQueue queue;
    SubmitWorker worker(queue, batchWith(1), 300ms);

    const std::optional<TakenBatch> taken = takeEventually(queue);
    queue.markApplied(taken->seq, 4, std::nullopt);

    const InputOutcome outcome = awaitResult(worker.future());
    CHECK(outcome.status == InputStatus::AppliedUnconfirmed);
    CHECK(outcome.appliedFrame == 4);
    CHECK(queue.completedCount() == 0);

    /* The entry is still the app's to finish, so nothing new may be taken. */
    CHECK(!queue.takeBatch().has_value());

    queue.complete(taken->seq);
    CHECK(queue.completedCount() == 1);
    CHECK(queue.lastAppliedFrame() == 4);
}

void applyErrorFails()
{
    InputQueue queue;
    SubmitWorker worker(queue, batchWith(1), 5000ms);

    const std::optional<TakenBatch> taken = takeEventually(queue);
    queue.markApplied(taken->seq, 9, std::string("no window bound"));

    const InputOutcome outcome = awaitResult(worker.future());
    CHECK(outcome.status == InputStatus::Failed);
    CHECK(outcome.error == "no window bound");
    CHECK(outcome.appliedFrame == 9);
    CHECK(queue.lastError() == "no window bound");
    /* A failure needs no completion: the entry is already gone. */
    CHECK(queue.completedCount() == 0);
    CHECK(queue.lastAppliedFrame() == 0);
    CHECK(!queue.takeBatch().has_value());

    SubmitWorker next(queue, batchWith(1), 5000ms);
    const std::optional<TakenBatch> takenNext = takeEventually(queue);
    queue.markApplied(takenNext->seq, 2, std::nullopt);
    queue.complete(takenNext->seq);
    CHECK(awaitResult(next.future()).status == InputStatus::Applied);
    CHECK(queue.lastError().empty());
}

void takeWaitsForOutstandingBatch()
{
    InputQueue queue;
    SubmitWorker first(queue, batchWith(1), 5000ms);
    const std::optional<TakenBatch> takenFirst = takeEventually(queue);

    SubmitWorker second(queue, batchWith(2), 5000ms);
    /* Only the submit itself knows when the batch is queued; this just gives it time. */
    std::this_thread::sleep_for(20ms);
    CHECK(!queue.takeBatch().has_value());

    queue.markApplied(takenFirst->seq, 1, std::nullopt);
    queue.complete(takenFirst->seq);
    CHECK(awaitResult(first.future()).status == InputStatus::Applied);

    const std::optional<TakenBatch> takenSecond = takeEventually(queue);
    CHECK(takenSecond->seq == 2);
    CHECK(takenSecond->batch.actions.size() == 2);
    queue.markApplied(takenSecond->seq, 2, std::nullopt);
    queue.complete(takenSecond->seq);

    const InputOutcome outcome = awaitResult(second.future());
    CHECK(outcome.status == InputStatus::Applied);
    CHECK(outcome.appliedFrame == 2);
}

void cancelledBatchIsNeverTaken()
{
    InputQueue queue;
    SubmitWorker cancelled(queue, batchWith(1), 30ms);
    CHECK(awaitResult(cancelled.future()).status == InputStatus::Cancelled);

    SubmitWorker next(queue, batchWith(3), 5000ms);
    const std::optional<TakenBatch> taken = takeEventually(queue);
    CHECK(taken->seq == 2);
    CHECK(taken->batch.actions.size() == 3);

    queue.markApplied(taken->seq, 6, std::nullopt);
    queue.complete(taken->seq);
    CHECK(awaitResult(next.future()).status == InputStatus::Applied);
}

void nextBatchQueuesBehindUnconfirmed()
{
    InputQueue queue;
    SubmitWorker first(queue, batchWith(1), 300ms);
    const std::optional<TakenBatch> takenFirst = takeEventually(queue);
    queue.markApplied(takenFirst->seq, 3, std::nullopt);
    CHECK(awaitResult(first.future()).status == InputStatus::AppliedUnconfirmed);

    SubmitWorker second(queue, batchWith(2), 5000ms);
    std::this_thread::sleep_for(20ms);
    /* The next batch waits for the previous one's F+1 publication. */
    CHECK(!queue.takeBatch().has_value());

    queue.complete(takenFirst->seq);
    CHECK(queue.completedCount() == 1);

    const std::optional<TakenBatch> takenSecond = takeEventually(queue);
    CHECK(takenSecond->seq == 2);
    queue.markApplied(takenSecond->seq, 5, std::nullopt);
    queue.complete(takenSecond->seq);

    const InputOutcome outcome = awaitResult(second.future());
    CHECK(outcome.status == InputStatus::Applied);
    CHECK(outcome.appliedFrame == 5);
}

void stopWakesWaitersAndRefusesSubmits()
{
    InputQueue queue;
    SubmitWorker worker(queue, batchWith(1), 10000ms);
    std::this_thread::sleep_for(20ms);

    queue.stop();
    CHECK(awaitResult(worker.future()).status == InputStatus::Stopped);
    CHECK(!queue.takeBatch().has_value());

    InputOutcome outcome;
    queue.submit(batchWith(1), 1000ms, outcome);
    CHECK(outcome.status == InputStatus::Stopped);

    queue.stop();
    queue.submit(batchWith(1), 1000ms, outcome);
    CHECK(outcome.status == InputStatus::Stopped);
    CHECK(queue.completedCount() == 0);
}

void stopLeavesTakenBatchCompletable()
{
    InputQueue queue;
    SubmitWorker worker(queue, batchWith(1), 10000ms);
    const std::optional<TakenBatch> taken = takeEventually(queue);
    queue.markApplied(taken->seq, 8, std::nullopt);

    queue.stop();
    /* The waiter is released as Stopped even though the events are already out. */
    CHECK(awaitResult(worker.future()).status == InputStatus::Stopped);

    queue.complete(taken->seq);
    CHECK(queue.completedCount() == 1);
    CHECK(queue.lastAppliedFrame() == 8);
}

void countersAdvanceOnCompletion()
{
    InputQueue queue;
    SubmitWorker worker(queue, batchWith(1), 5000ms);

    const std::optional<TakenBatch> taken = takeEventually(queue);
    CHECK(queue.completedCount() == 0);
    CHECK(queue.lastAppliedFrame() == 0);

    queue.markApplied(taken->seq, 21, std::nullopt);
    CHECK(queue.completedCount() == 0);
    CHECK(queue.lastAppliedFrame() == 0);

    queue.complete(taken->seq);
    CHECK(queue.completedCount() == 1);
    CHECK(queue.lastAppliedFrame() == 21);
    CHECK(awaitResult(worker.future()).status == InputStatus::Applied);
}

} // namespace

int main()
{
    appliedOnlyAfterCompletion();
    timeoutBeforeTakeCancels();
    timeoutAfterApplyIsUnconfirmed();
    applyErrorFails();
    takeWaitsForOutstandingBatch();
    cancelledBatchIsNeverTaken();
    nextBatchQueuesBehindUnconfirmed();
    stopWakesWaitersAndRefusesSubmits();
    stopLeavesTakenBatchCompletable();
    countersAdvanceOnCompletion();

    std::printf("queue_tests passed\n");
    return 0;
}
