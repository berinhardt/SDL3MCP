#include "capture.hpp"

#include <utility>

namespace sdl3mcp
{

bool CaptureState::begin(std::string &error)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stats_.pending) {
        error = "a capture request is already pending";
        return false;
    }

    stats_.pending = true;
    frame_.reset();
    failure_.clear();
    error.clear();
    return true;
}

bool CaptureState::wait(std::chrono::milliseconds timeout, Frame &frame, std::string &failure)
{
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock, timeout, [this] { return !stats_.pending; });

    if (frame_.has_value()) {
        frame = std::move(*frame_);
        frame_.reset();
        failure.clear();
        return true;
    }

    /* A timeout leaves pending set until here; cancel() already cleared it. */
    stats_.pending = false;
    failure = failure_;
    return false;
}

bool CaptureState::pending() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return stats_.pending;
}

void CaptureState::submit(Frame frame)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!stats_.pending) {
        return;
    }

    frame_ = std::move(frame);
    stats_.pending = false;
    changed_.notify_all();
}

void CaptureState::recordFailure(std::string reason)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!stats_.pending) {
        return;
    }

    if (failure_.empty()) {
        failure_ = std::move(reason);
        stats_.lastError = failure_;
    }
    ++stats_.dropped;
}

void CaptureState::recordError(std::string reason)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    stats_.lastError = std::move(reason);
}

void CaptureState::cancel()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    stats_.pending = false;
    frame_.reset();
    changed_.notify_all();
}

CaptureStats CaptureState::stats() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void CaptureState::updateSuccess(int preDownscaleWidth,
                                 int preDownscaleHeight,
                                 uint64_t pngBytes,
                                 double ms)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.count;
    stats_.lastWidth = preDownscaleWidth;
    stats_.lastHeight = preDownscaleHeight;
    stats_.lastBytes = pngBytes;
    stats_.lastMs = ms;
    stats_.lastError.clear();
}

} // namespace sdl3mcp
