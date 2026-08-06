//
//  DiscoverManager.cpp
//  Moonlight
//
// Created by XITRIX on 31.03.2025.
//

#include "AVFrameHolder.hpp"

#include <algorithm>
#include <tracy/Tracy.hpp>

namespace {

constexpr size_t kBurstHeadroomFrames = 5;
constexpr auto kArrivalRateWindow = std::chrono::milliseconds(250);
constexpr auto kArrivalRateResetGap = std::chrono::milliseconds(500);
constexpr double kArrivalRateSmoothing = 0.35;
constexpr double kOccupancyCorrectionPerFrame = 0.01;
constexpr double kMaximumOccupancyCorrection = 0.08;

void freeFrameQueue(std::queue<AVFrame*>& frames) {
    for (; !frames.empty(); frames.pop()) {
        AVFrame* frame = frames.front();
        av_frame_free(&frame);
    }
}

void freeTimedFrames(std::deque<TimedFrame>& frames) {
    for (; !frames.empty(); frames.pop_front()) {
        AVFrame* frame = frames.front().frame;
        av_frame_free(&frame);
    }
}

void recycleFrame(std::queue<AVFrame*>& freeQueue, AVFrame*& frame) {
    if (!frame) {
        return;
    }

    av_frame_unref(frame);
    freeQueue.push(frame);
    frame = nullptr;
}

} // namespace

AVFrameQueue::AVFrameQueue() {}

AVFrameQueue::~AVFrameQueue() {
    cleanup();
    freeFrameQueue(freeQueue);
}

size_t AVFrameQueue::capacityFor(size_t configuredQueueSize) {
    return std::max<size_t>(configuredQueueSize, 1) + kBurstHeadroomFrames;
}

bool AVFrameQueue::push(AVFrame* item) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!item) {
        return false;
    }

    if (transferOwnership) {
        return pushTransferredLocked(item);
    }

    AVFrame* queuedFrame = acquireFrameLocked();
    if (!queuedFrame) {
        return false;
    }

    if (av_frame_ref(queuedFrame, item) < 0) {
        av_frame_free(&queuedFrame);
        return false;
    }

    const Timestamp timeEstimate = recordArrivalLocked(std::chrono::steady_clock::now());
    queue.push_back(TimedFrame{timeEstimate, queuedFrame});
    pushesSincePop++;
    maxPushBurstStat = std::max(maxPushBurstStat, pushesSincePop);

    if (queue.size() > limit) {
        const size_t keepFrames = targetBufferedFrames + 1;
        while (queue.size() > keepFrames) {
            AVFrame* droppedFrame = queue.front().frame;
            queue.pop_front();
            recycleFrame(freeQueue, droppedFrame);
            framesDroppedStat++;
            overflowDropStat++;
        }
        resetArrivalRateEstimatorLocked();
        draw.resyncNeeded = true;
    }

    return true;
}

bool AVFrameQueue::pushTransferred(AVFrame* item) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return pushTransferredLocked(item);
}

AVFrame* AVFrameQueue::pop(bool* consumed) {
    std::lock_guard<std::mutex> lock(m_mutex);

    pushesSincePop = 0;

    if (consumed) {
        *consumed = false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!draw.clockStarted) {
        draw.lastDraw = now;
        draw.clockStarted = true;
    } else {
        const auto drawInterval =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - draw.lastDraw);
        draw.lastDraw = now;

        // App suspension and debugger pauses must not turn into a large burst
        // of overdue video frames when drawing resumes.
        if (drawInterval <= std::chrono::nanoseconds::zero() ||
            drawInterval > std::chrono::milliseconds(250)) {
            brls::Logger::info("invalid drawInterval, resyncing");
            draw.averageInterval = std::chrono::nanoseconds::zero();
            draw.frameCredit = 0.0;
            draw.resyncNeeded = true;
        } else if (draw.averageInterval == std::chrono::nanoseconds::zero()) {
            draw.averageInterval = drawInterval;
        } else {
            draw.averageInterval =
                (draw.averageInterval * 15 + drawInterval) / 16;
        }
    }

    if (draw.startupBuffering && queue.size() <= targetBufferedFrames) {
        if (bufferFrame) {
            fakeFrameUsedStat++;
            rebufferHoldStat++;
        }
        return bufferFrame;
    }

    if (draw.startupBuffering) {
        // Establish a small jitter reserve once at startup. Rebuilding the
        // whole reserve after every ordinary miss batches a variable-rate
        // source into visible freeze-and-catch-up cycles.
        draw.startupBuffering = false;
        draw.frameCredit = 0.0;
        draw.resyncNeeded = true;
    }

    size_t dueFrames = 0;
    const bool backlogResync = limit > 0 && queue.size() >= limit;
    if ((draw.resyncNeeded || backlogResync) && !queue.empty()) {
        brls::Logger::info("Processing draw.resyncNeeded");
        // Resume immediately after a real miss. If latency has reached the
        // hard limit, discard the stale backlog once instead of repeatedly
        // overflowing the oldest frame while playback remains frozen.
        trimToPlayoutWindowLocked();
        if (backlogResync) {
            brls::Logger::info("overfull (backlogResync)");
            resetArrivalRateEstimatorLocked();
        }
        draw.resyncNeeded = false;
        draw.frameCredit = 0.0;
        playoutResyncStat++;
        dueFrames = 1;
    } else if (!arrival.rateComputed ||
               arrival.frameInterval <= std::chrono::nanoseconds::zero() ||
               draw.averageInterval <= std::chrono::nanoseconds::zero()) {
        brls::Logger::info("!arrival.rateComputed");
        // During the short measurement warm-up, consume only above the jitter
        // reserve. This follows arrivals without assuming configured FPS is
        // the FPS the host is actually producing.
        dueFrames = queue.size() > targetBufferedFrames ? 1 : 0;
    } else if (arrival.frameInterval > std::chrono::nanoseconds::zero() &&
               draw.averageInterval > std::chrono::nanoseconds::zero()) {
        // input / output
        const double baseFramesPerDraw =
            static_cast<double>(draw.averageInterval.count()) /
            static_cast<double>(arrival.frameInterval.count());

        for (; dueFrames < queue.size(); dueFrames++) {
            // Always show at least 1 frame if the server isn't slow.
            if (baseFramesPerDraw >= 0.98 && dueFrames <= 0) {
                continue;
            }

            // We shouldn't skip frame 0 to a just-received frame 1, since the next
            // present may not have frame 2 ready, resulting in a duplicated frame.
            const Timestamp safeArrivalTime = now - 2 * arrival.lastJitter;
            if (queue[dueFrames].timeEstimate > safeArrivalTime) {
                break;
            }
        }
    }

    TracyPlot("pop", (int64_t)dueFrames);
    TracyPlotConfig("pop", tracy::PlotFormatType::Number, true, true, 0);
    if (dueFrames == 0) {
        scheduledHoldStat++;
        return bufferFrame;
    }

    if (queue.empty()) {
        if (bufferFrame) {
            fakeFrameUsedStat++;
            emptyQueueStat++;
        }
        draw.frameCredit = 0.0;
        draw.resyncNeeded = true;
        brls::Logger::info("buffer underflow");
        // The measured cadence may now be too high because the host FPS fell.
        // Relearn it from fresh arrivals while occupancy pacing protects the
        // jitter reserve, rather than causing repeated underflow/resume cycles.
        resetArrivalRateEstimatorLocked();
        return bufferFrame;
    }

    const size_t consumeCount = std::min(queue.size(), dueFrames);
    TracyPlot("pop", (int64_t)consumeCount);

    recycleFrame(freeQueue, bufferFrame);
    for (size_t i = 0; i < consumeCount; i++) {
        TimedFrame item = queue.front();
        queue.pop_front();

        if (i + 1 < consumeCount) {
            recycleFrame(freeQueue, item.frame);
            framesDroppedStat++;
            pacingSkipStat++;
        } else {
            bufferFrame = item.frame;
        }
    }

    if (consumed) {
        *consumed = true;
    }
    localClockPacedFrameStat++;

    return bufferFrame;
}

AVFrame* AVFrameQueue::acquireWriteFrame() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return acquireFrameLocked();
}

void AVFrameQueue::recycleWriteFrame(AVFrame*& frame) {
    std::lock_guard<std::mutex> lock(m_mutex);
    recycleFrame(freeQueue, frame);
}

void AVFrameQueue::configure(size_t queueLimit, int configuredStreamFps,
                             bool transferOwnershipEnabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const size_t configuredDepth = std::max<size_t>(queueLimit, 1);
    limit = capacityFor(configuredDepth);
    targetBufferedFrames =
        configuredDepth > 1 ? std::min<size_t>(configuredDepth - 1, 2) : 0;
    transferOwnership = transferOwnershipEnabled;
    streamFps = configuredStreamFps;
    arrival = Arrival{};
    draw = Draw{};
}

size_t AVFrameQueue::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return queue.size();
}

size_t AVFrameQueue::targetDepth() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return targetBufferedFrames;
}

size_t AVFrameQueue::capacity() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return limit;
}

size_t AVFrameQueue::getFakeFrameUsage() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return fakeFrameUsedStat;
}

size_t AVFrameQueue::getFramesDropStat() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return framesDroppedStat;
}

size_t AVFrameQueue::getEmptyQueueStat() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return emptyQueueStat;
}

size_t AVFrameQueue::getRebufferHoldStat() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return rebufferHoldStat;
}

size_t AVFrameQueue::getOverflowDropStat() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return overflowDropStat;
}

size_t AVFrameQueue::getPacingSkipStat() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return pacingSkipStat;
}

size_t AVFrameQueue::getScheduledHoldStat() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return scheduledHoldStat;
}

size_t AVFrameQueue::getMaxPushBurstStat() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return maxPushBurstStat;
}

size_t AVFrameQueue::getLocalClockPacedFrameStat() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return localClockPacedFrameStat;
}

size_t AVFrameQueue::getPlayoutResyncStat() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return playoutResyncStat;
}

double AVFrameQueue::getEstimatedSourceFps() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return arrival.estimatedSourceFps;
}

double AVFrameQueue::getJitterMs() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto jitterNs = arrival.lastJitter.count();
    return jitterNs / 1'000'000.;
}

AVFrame* AVFrameQueue::acquireFrameLocked() {
    AVFrame* frame = nullptr;
    if (!freeQueue.empty()) {
        frame = freeQueue.front();
        freeQueue.pop();
        av_frame_unref(frame);
        return frame;
    }

    frame = av_frame_alloc();
    return frame;
}

bool AVFrameQueue::pushTransferredLocked(AVFrame* item) {
    ZoneScoped;
    if (!item) {
        return false;
    }

    const Timestamp timeEstimate = recordArrivalLocked(std::chrono::steady_clock::now());
    queue.push_back(TimedFrame{timeEstimate, item});
    pushesSincePop++;
    maxPushBurstStat = std::max(maxPushBurstStat, pushesSincePop);

    if (queue.size() > limit) {
        const size_t keepFrames = targetBufferedFrames + 1;
        while (queue.size() > keepFrames) {
            AVFrame* droppedFrame = queue.front().frame;
            queue.pop_front();
            recycleFrame(freeQueue, droppedFrame);
            framesDroppedStat++;
            overflowDropStat++;
        }
        resetArrivalRateEstimatorLocked();
        draw.resyncNeeded = true;
    }

    return true;
}

// https://en.wikipedia.org/wiki/Alpha_beta_filter
// source: i made it up
constexpr double ALPHA = 1. / 3.;
// https://www.oedigital.com/news/457127-applying-real-time-magnetic-declination-in-arctic-marine-seismic-acquisition
constexpr double BETA = ALPHA * ALPHA / (2. - ALPHA);

Timestamp AVFrameQueue::recordArrivalLocked(const Timestamp now) {
    ZoneScoped;
    auto initArrival = [&]() {
        ZoneScopedN("initArrival");
        arrival.windowStart = now;
        arrival.lastArrival = now;
        arrival.windowFrames = 1;
        arrival.jitterSoFar = std::chrono::nanoseconds::zero();
        return now;
    };

    if (!arrival.clockStarted) {
        arrival.clockStarted = true;
        return initArrival();
    }

    if (now - arrival.lastArrival > kArrivalRateResetGap) {
        // Do not interpret a network pause or app suspension as a permanent
        // low source rate. Start a fresh window when frames resume.
        return initArrival();
    }

    arrival.jitterSoFar += abs(now - arrival.lastArrival - arrival.frameInterval);
    arrival.windowFrames++;

    // Estimate smoothed time for frame pacing.
    Timestamp output;
    if (arrival.rateComputed) {
        // https://en.wikipedia.org/wiki/Alpha_beta_filter
        // (1)
        const Timestamp timePredicted = arrival.lastArrival + arrival.frameInterval;
        TracyPlot("timePredicted", timePredicted.time_since_epoch().count() / 1'000'000.);
        // (3)
        const Duration residual = now - timePredicted;
        TracyPlot("residual", residual.count() / 1'000'000.);
        // (4)
        const Timestamp smoothedNow = timePredicted + Duration((int64_t) (ALPHA * residual.count()));
        // (5)
        arrival.frameInterval += Duration((int64_t) (BETA * residual.count()));

        // The next iteration's step (1) takes *filter output*, not unfiltered now!
        arrival.lastArrival = smoothedNow;

        // TODO pick between stability and responsiveness
        output = timePredicted;
    } else {
        output = now;
    }

    TracyPlot("arrivalInterval", arrival.frameInterval.count() / 1'000'000.);
    TracyPlotConfig("arrivalInterval", tracy::PlotFormatType::Number, true, true, 0);
    TracyPlot("lastArrival", arrival.lastArrival.time_since_epoch().count() / 1'000'000.);
    TracyPlotConfig("lastArrival", tracy::PlotFormatType::Number, true, true, 0);

    static const char * const RATE_COMPUTED = "rateComputed";
    TracyPlot(RATE_COMPUTED, (long)arrival.rateComputed);
    TracyPlotConfig(RATE_COMPUTED, tracy::PlotFormatType::Number, true, true, 0);

    const Duration elapsed = now - arrival.windowStart;

    // Periodically recompute stats.
    if (elapsed < kArrivalRateWindow) {
        return output;
    }

    if (arrival.windowFrames > 1) {
        brls::Logger::info("Computing stats...");
        // duration<double>() is measured in seconds: https://en.cppreference.com/cpp/chrono/duration
        const double elapsedSeconds =
            std::chrono::duration<double>(elapsed).count();

        const double maximumFps =
            streamFps > 0 ? static_cast<double>(streamFps) : 240.0;

        if (!arrival.rateComputed) {
            arrival.rateComputed = true;
            TracyPlot(RATE_COMPUTED, (long)arrival.rateComputed);

            double sampleFps = static_cast<double>(arrival.windowFrames - 1) / elapsedSeconds;
            sampleFps = std::clamp(sampleFps, 1.0, maximumFps);
            arrival.estimatedSourceFps = sampleFps;

            // Initialize arrival.frameInterval with estimated interval.
            // (no left/both taper, but it's good enough for an initial guess.)
            arrival.frameInterval = std::chrono::nanoseconds(
                static_cast<int64_t>(1000000000.0 / arrival.estimatedSourceFps));
            brls::Logger::info("arrival.frameInterval := {}", arrival.frameInterval);
        } else {
            const double dSecPerFrame = arrival.frameInterval.count() / 1'000'000'000.;

            // Used to be `static_cast<double>(arrival.windowFrames - 1) / elapsedSeconds`
            // but I feel like using the filtered frameInterval today.
            double sampleFps = 1. / dSecPerFrame;
            sampleFps = std::clamp(sampleFps, 1.0, maximumFps);

            // The quarter-second sample ignores short decoder bursts. The EMA
            // follows sustained FPS changes without making network jitter a
            // new presentation cadence every window.
            arrival.estimatedSourceFps =
                arrival.estimatedSourceFps * (1.0 - kArrivalRateSmoothing) +
                sampleFps * kArrivalRateSmoothing;
        }

        Duration newJitter = arrival.jitterSoFar / (arrival.windowFrames - 1);
        // Raise jitter to match spikes immediately, lower gradually.
        if (arrival.lastJitter == Duration(0) || newJitter > arrival.lastJitter) {
            arrival.lastJitter = newJitter;
        } else {
            arrival.lastJitter += Duration((int64_t) (
                kArrivalRateSmoothing * (newJitter - arrival.lastJitter).count()));
        }
    }

    brls::Logger::info("Next round of stats...");
    arrival.windowStart = now;
    arrival.windowFrames = 1;
    arrival.jitterSoFar = Duration::zero();
    return output;
}

void AVFrameQueue::resetArrivalRateEstimatorLocked() {
    ZoneScoped;
    arrival = Arrival{};
}

void AVFrameQueue::trimToPlayoutWindowLocked() {
    const size_t keepFrames = targetBufferedFrames + 1;
    while (queue.size() > keepFrames) {
        TimedFrame droppedFrame = queue.front();
        queue.pop_front();
        recycleFrame(freeQueue, droppedFrame.frame);
        framesDroppedStat++;
        pacingSkipStat++;
    }
}

void AVFrameQueue::cleanup() {
    std::lock_guard<std::mutex> lock(m_mutex);
    fakeFrameUsedStat = 0;
    framesDroppedStat = 0;
    emptyQueueStat = 0;
    rebufferHoldStat = 0;
    overflowDropStat = 0;
    pacingSkipStat = 0;
    scheduledHoldStat = 0;
    pushesSincePop = 0;
    maxPushBurstStat = 0;
    localClockPacedFrameStat = 0;
    playoutResyncStat = 0;
    arrival = Arrival{};
    draw = Draw{};

    if (bufferFrame) {
        av_frame_free(&bufferFrame);
    }

    freeTimedFrames(queue);
    freeFrameQueue(freeQueue);
    queue = {};
    freeQueue = {};
}
