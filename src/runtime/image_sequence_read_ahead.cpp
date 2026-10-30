#include <bloom/runtime/image_sequence_read_ahead.hpp>

#include <bloom/media/image.hpp>
#include <bloom/runtime/operation_cache.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <utility>

namespace bloom::runtime {

struct ImageSequenceReadAhead::State final {
    struct Job final {
        Request request;
        std::weak_ptr<OperationCache> cache;
    };
    std::size_t workerLimit = 1;
    std::size_t queueLimit = 1;
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::deque<Job> queue;
    // Revision keys queued or being captured.
    std::set<std::string> pending;
    std::vector<std::thread> workers;
    std::atomic<bool> stopping{false};
    Statistics statistics;

    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { return stopping.load() || !queue.empty(); });
                if (stopping.load())
                    return;
                job = std::move(queue.front());
                queue.pop_front();
            }
            const bool captured = capture(job);
            const std::lock_guard lock(mutex);
            pending.erase(job.request.revisionKey);
            ++(captured ? statistics.captured : statistics.failed);
        }
    }

    [[nodiscard]] bool capture(const Job& job) const {
        const auto cache = job.cache.lock();
        if (!cache || stopping.load())
            return false;
        if (cache->find(job.request.revisionKey, document::Revision{}))
            return true;
        // Never evict for a guess: the file must fit the cache's free budget as it stands.
        std::error_code error;
        const auto size = std::filesystem::file_size(job.request.path, error);
        if (error || size > job.request.byteBudget ||
            cache->retainedBytes() + size > cache->byteBudget())
            return false;
        const auto generation = cache->decodedMediaGeneration();
        auto revision = media::captureImageSourceRevision(
            job.request.path, job.request.byteBudget, [this] { return stopping.load(); },
            job.request.digest);
        if (!revision.value || !*revision.value ||
            (*revision.value)->probe().contentDigest != job.request.digest)
            return false;
        cache->store(job.request.revisionKey, document::Revision{},
                     {.image = {}, .values = {}, .bounds = {}, .sourceRevision = *revision.value},
                     OperationCacheEntryKind::DecodedMedia, generation);
        return true;
    }
};

ImageSequenceReadAhead::ImageSequenceReadAhead(const std::size_t workers,
                                               const std::size_t queueLimit)
    : state_(std::make_shared<State>()) {
    state_->workerLimit = std::max<std::size_t>(1, workers);
    state_->queueLimit = std::max<std::size_t>(1, queueLimit);
}

ImageSequenceReadAhead::~ImageSequenceReadAhead() {
    {
        const std::lock_guard lock(state_->mutex);
        state_->stopping.store(true);
        state_->queue.clear();
    }
    state_->ready.notify_all();
    for (auto& worker : state_->workers)
        if (worker.joinable())
            worker.join();
}

void ImageSequenceReadAhead::request(const std::shared_ptr<OperationCache>& cache,
                                     std::vector<Request> requests) {
    if (!cache || requests.empty())
        return;
    std::size_t added = 0;
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->stopping.load())
            return;
        for (auto& request : requests) {
            if (state_->queue.size() >= state_->queueLimit ||
                state_->pending.contains(request.revisionKey) ||
                cache->find(request.revisionKey, document::Revision{})) {
                ++state_->statistics.skipped;
                continue;
            }
            state_->pending.insert(request.revisionKey);
            state_->queue.push_back({.request = std::move(request), .cache = cache});
            ++state_->statistics.queued;
            ++added;
        }
        // Workers start on first use and stay bounded; each one keeps a read outstanding.
        while (state_->workers.size() < std::min(state_->workerLimit, state_->queue.size()))
            state_->workers.emplace_back([state = state_.get()] { state->run(); });
    }
    if (added != 0)
        state_->ready.notify_all();
}

ImageSequenceReadAhead::Statistics ImageSequenceReadAhead::statistics() const {
    const std::lock_guard lock(state_->mutex);
    return state_->statistics;
}

} // namespace bloom::runtime
