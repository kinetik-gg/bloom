#pragma once

#include <bloom/runtime/cancellation.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace bloom::runtime {

// The smallest number of image rows one parallel band may own. A band is a cache-coherent run of
// whole rows, and a row of a composition-sized Float32 image is already tens of kilobytes, so
// bands smaller than this spend more on hand-off than on pixels. It is a FLOOR, not a target: a
// short image simply produces fewer bands, and an image shorter than this produces exactly one.
inline constexpr std::uint32_t kMinimumRowsPerBand = 8;

// One contiguous run of rows, expressed as an offset from the first row of the work, never as an
// absolute image coordinate -- the caller adds its own data window origin.
struct RowBand final {
    std::uint32_t beginRow = 0;
    std::uint32_t rowCount = 0;

    friend constexpr bool operator==(const RowBand&, const RowBand&) noexcept = default;
};

// The one deterministic split of `rowCount` rows into at most `maximumBands` bands of at least
// kMinimumRowsPerBand rows each. The split depends ONLY on these two numbers -- never on timing,
// thread count at run time, or the order bands happen to finish -- so the same image always divides
// the same way, which is what makes a parallel evaluation reproducible rather than merely correct.
// The remainder is spread one row at a time across the leading bands, so band sizes differ by at
// most one.
[[nodiscard]] std::vector<RowBand> planRowBands(std::uint32_t rowCount, std::size_t maximumBands);

// A bounded pool of worker threads that runs row bands, owned by the TaskScheduler so that ONE
// object bounds how much CPU Bloom spends on pixels at a time (see TaskScheduler::rowBandExecutor()
// and docs/architecture/task-system.md).
//
// Why this is not "submit more tasks to the CPU executor and wait": a task body that blocks on
// tasks of its own deadlocks the moment every CPU worker is itself such a body, and Bloom's default
// CPU worker count can legitimately be one. Band workers are therefore their own threads, and the
// thread calling run() PARTICIPATES in its own job rather than sleeping -- so a parallel region
// always makes progress with the caller alone, whatever else the pool is doing.
//
// Concurrent run() calls from different threads are supported: each call owns its own job, workers
// take bands from the oldest job that still has one, and a caller only ever runs bands of its own
// job. That rules out a caller waiting on work it could not reach.
class CpuRowBandExecutor final {
  public:
    // `workerCount` of zero derives a bounded default from std::thread::hardware_concurrency().
    // Worker threads are created eagerly; they are idle until the first run().
    explicit CpuRowBandExecutor(std::size_t workerCount = 0);
    CpuRowBandExecutor(const CpuRowBandExecutor&) = delete;
    CpuRowBandExecutor& operator=(const CpuRowBandExecutor&) = delete;
    CpuRowBandExecutor(CpuRowBandExecutor&&) = delete;
    CpuRowBandExecutor& operator=(CpuRowBandExecutor&&) = delete;
    ~CpuRowBandExecutor();

    // How many bands it is worth planning: the worker threads plus the calling thread, which runs
    // bands too. Never zero.
    [[nodiscard]] std::size_t bandLimit() const noexcept;
    [[nodiscard]] std::size_t workerCount() const noexcept;

    // Runs band indices [0, bandCount) exactly once each, in an unspecified order, and returns once
    // every one of them has completed. False means at least one band exited by throwing: the caller
    // must treat its output as incomplete. `band` is expected not to throw -- every row kernel
    // Bloom runs through here is noexcept -- so this is a safety net, not a control-flow channel.
    [[nodiscard]] bool run(std::size_t bandCount, const std::function<void(std::size_t)>& band);

  private:
    struct Job final {
        const std::function<void(std::size_t)>* band = nullptr;
        std::size_t bandCount = 0;
        std::size_t claimed = 0;
        std::size_t completed = 0;
        bool threw = false;
    };

    void workerLoop();
    void completeBand(Job& job, bool threw);

    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::condition_variable completed_;
    std::deque<Job*> jobs_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

// What a banded row pass ended as: the ordinary success, cancellation observed at a row boundary,
// one row's structured failure, or -- never expected, because every row kernel Bloom has is
// noexcept -- a band that escaped by throwing, which the caller reports in its own diagnostic
// vocabulary.
template <typename Failure> struct RowBandPassOutcome final {
    bool cancelled = false;
    bool incomplete = false;
    Failure failure{};
};

// Runs `rowFunction` once for every row in [originY, originY + rowCount), in row BANDS across
// `executor` -- or inline in band order when `executor` is null, which is what a serial
// configuration and any single-band image both get. `rowFunction` returns an engaged optional to
// fail the pass.
//
// Why the pixels cannot depend on how the rows were divided: a row kernel writes only its own
// output row span and reads only immutable inputs, bands partition the rows, so no two bands touch
// the same destination pixel and no band observes another's output. The split itself is
// planRowBands(), a pure function of the row count and the band limit. The one thing banding does
// change is WHICH failing row is reported when several fail at once: the lowest band index wins,
// which is the row a serial pass would have stopped at whenever only one row can fail.
//
// Progress is deliberately NOT reported from inside a band: a progress callback belongs to the task
// that owns the work and is not thread-safe. Callers report the start and the end of a row pass
// from their own thread instead.
//
// Addressing the destination: a band reaches its rows through the builder's own row() accessor,
// which is non-const only because it hands back a mutable span -- it reads the builder's descriptor
// and storage pointer and mutates nothing, so concurrent calls from band threads are reads of
// shared state and the spans they return are disjoint by construction.
template <typename RowFunction>
[[nodiscard]] auto
runRowBandPass(CpuRowBandExecutor* const executor, const CancellationToken& cancellation,
               const std::uint32_t rowCount, const std::int64_t originY, RowFunction&& rowFunction)
    -> RowBandPassOutcome<std::invoke_result_t<RowFunction&, std::int64_t>> {
    using Failure = std::invoke_result_t<RowFunction&, std::int64_t>;
    RowBandPassOutcome<Failure> outcome;
    const auto bands =
        planRowBands(rowCount, executor == nullptr ? std::size_t{1} : executor->bandLimit());
    if (bands.empty()) {
        return outcome;
    }
    std::vector<Failure> failures(bands.size());
    std::vector<std::uint8_t> cancellations(bands.size(), 0);
    const auto runBand = [&](const std::size_t index) {
        const auto band = bands[index];
        for (std::uint32_t row = 0; row < band.rowCount; ++row) {
            if (cancellation.isCancellationRequested()) {
                cancellations[index] = 1;
                return;
            }
            auto failure = rowFunction(originY + static_cast<std::int64_t>(band.beginRow) +
                                       static_cast<std::int64_t>(row));
            if (failure.has_value()) {
                failures[index] = std::move(failure);
                return;
            }
        }
    };

    if (executor == nullptr || bands.size() == 1) {
        // Band order, stopping at the first band that fails or is cancelled -- the control flow the
        // per-row loops had before they were banded.
        for (std::size_t index = 0; index < bands.size(); ++index) {
            runBand(index);
            if (failures[index].has_value() || cancellations[index] != 0) {
                break;
            }
        }
    } else {
        const std::function<void(std::size_t)> band = runBand;
        if (!executor->run(bands.size(), band)) {
            outcome.incomplete = true;
            return outcome;
        }
    }
    for (auto& failure : failures) {
        if (failure.has_value()) {
            outcome.failure = std::move(failure);
            break;
        }
    }
    outcome.cancelled =
        std::ranges::any_of(cancellations, [](const std::uint8_t flag) { return flag != 0; });
    return outcome;
}

} // namespace bloom::runtime
