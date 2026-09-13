#include <bloom/runtime/row_band_execution.hpp>

#include <algorithm>
#include <utility>

namespace bloom::runtime {
namespace {

// The same bound TaskSchedulerConfig::defaults() puts on CPU workers: one core left for the
// interface, and never more than sixteen threads however wide the machine is.
constexpr std::size_t kMaximumBandWorkers = 16;

[[nodiscard]] std::size_t derivedWorkerCount() noexcept {
    const unsigned int available = std::thread::hardware_concurrency();
    // The caller's own thread runs bands too, so the pool needs one fewer thread than the
    // parallelism it provides: `available` usable cores means `available - 1` workers.
    const auto useful = available > 1 ? static_cast<std::size_t>(available - 1U) : std::size_t{0};
    return std::min(useful, kMaximumBandWorkers);
}

} // namespace

std::vector<RowBand> planRowBands(const std::uint32_t rowCount, const std::size_t maximumBands) {
    std::vector<RowBand> bands;
    if (rowCount == 0 || maximumBands == 0) {
        return bands;
    }
    const auto wholeBands = static_cast<std::size_t>(rowCount / kMinimumRowsPerBand);
    const auto bandCount = std::max<std::size_t>(1, std::min(maximumBands, wholeBands));
    const auto base = static_cast<std::uint32_t>(rowCount / bandCount);
    const auto remainder = static_cast<std::uint32_t>(rowCount % bandCount);
    bands.reserve(bandCount);
    std::uint32_t beginRow = 0;
    for (std::size_t index = 0; index < bandCount; ++index) {
        const std::uint32_t rows = base + (static_cast<std::uint32_t>(index) < remainder ? 1U : 0U);
        bands.push_back({.beginRow = beginRow, .rowCount = rows});
        beginRow += rows;
    }
    return bands;
}

CpuRowBandExecutor::CpuRowBandExecutor(const std::size_t workerCount) {
    const auto count = workerCount == 0 ? derivedWorkerCount() : workerCount;
    workers_.reserve(count);
    try {
        for (std::size_t index = 0; index < count; ++index) {
            workers_.emplace_back([this] { workerLoop(); });
        }
    } catch (...) {
        // A pool that could not start every thread it wanted still works, because the thread
        // calling run() always runs bands itself. Keeping the partially started pool is therefore
        // better than failing construction and taking the whole scheduler down with it -- the only
        // consequence is a narrower bandLimit(), which changes how long a frame takes and nothing
        // about what it contains.
        workers_.shrink_to_fit();
    }
}

CpuRowBandExecutor::~CpuRowBandExecutor() {
    // Destruction is the owner's promise that no run() is in flight; the pool has no way to finish
    // a parallel region whose band closure is about to be destroyed with its caller's stack frame.
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    available_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

std::size_t CpuRowBandExecutor::bandLimit() const noexcept { return workers_.size() + 1; }

std::size_t CpuRowBandExecutor::workerCount() const noexcept { return workers_.size(); }

bool CpuRowBandExecutor::run(const std::size_t bandCount,
                             const std::function<void(std::size_t)>& band) {
    if (bandCount == 0) {
        return true;
    }
    if (bandCount == 1 || workers_.empty()) {
        // One band, or nobody to share with: run right here and never touch the pool at all. Every
        // image shorter than two bands takes this path, and it must cost nothing.
        bool threw = false;
        for (std::size_t index = 0; index < bandCount; ++index) {
            try {
                band(index);
            } catch (...) {
                threw = true;
            }
        }
        return !threw;
    }

    Job job{.band = &band, .bandCount = bandCount, .claimed = 0, .completed = 0, .threw = false};
    {
        const std::lock_guard lock(mutex_);
        jobs_.push_back(&job);
    }
    available_.notify_all();

    // The calling thread runs bands of its OWN job until none is left, then waits for the bands
    // other threads claimed. It never claims another job's band, so it can never block behind work
    // it is itself responsible for finishing.
    while (true) {
        std::size_t bandIndex = 0;
        {
            const std::lock_guard lock(mutex_);
            if (job.claimed >= job.bandCount) {
                break;
            }
            bandIndex = job.claimed++;
        }
        bool threw = false;
        try {
            band(bandIndex);
        } catch (...) {
            threw = true;
        }
        completeBand(job, threw);
    }

    std::unique_lock lock(mutex_);
    completed_.wait(lock, [&job] { return job.completed == job.bandCount; });
    const auto position = std::ranges::find(jobs_, &job);
    if (position != jobs_.end()) {
        jobs_.erase(position);
    }
    return !job.threw;
}

void CpuRowBandExecutor::completeBand(Job& job, const bool threw) {
    {
        const std::lock_guard lock(mutex_);
        job.threw = job.threw || threw;
        ++job.completed;
    }
    completed_.notify_all();
}

void CpuRowBandExecutor::workerLoop() {
    const auto hasClaimableBand = [](const Job* pending) {
        return pending->claimed < pending->bandCount;
    };
    while (true) {
        Job* job = nullptr;
        std::size_t bandIndex = 0;
        const std::function<void(std::size_t)>* band = nullptr;
        {
            std::unique_lock lock(mutex_);
            available_.wait(lock, [this, &hasClaimableBand] {
                return stopping_ || std::ranges::any_of(jobs_, hasClaimableBand);
            });
            if (stopping_) {
                return;
            }
            // Oldest job first, so a parallel region that is already half finished is completed
            // before a newer one is started: that is what keeps the frame a caller is waiting on
            // from being starved by the next frame's bands.
            const auto position = std::ranges::find_if(jobs_, hasClaimableBand);
            if (position == jobs_.end()) {
                continue;
            }
            job = *position;
            band = job->band;
            bandIndex = job->claimed++;
        }
        bool threw = false;
        try {
            (*band)(bandIndex);
        } catch (...) {
            threw = true;
        }
        completeBand(*job, threw);
    }
}

} // namespace bloom::runtime
