#pragma once

#include <bloom/core/sha256.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bloom::runtime {

class OperationCache;

// Captures upcoming image-sequence files into an evaluator's operation cache on its own I/O
// workers. A frame's source read and hash then overlap earlier frames instead of running inside
// that frame's scene preparation, and several reads stay outstanding so a network share overlaps
// its latency. Captures are the same verified source revisions selection makes (exact bytes,
// content digest checked), stored under the same keys, so a later selection finds them like any
// other cached revision. Read-ahead never evicts: a file that would not fit the cache's free
// budget is skipped. Thread-safe; request() never blocks.
class ImageSequenceReadAhead final {
  public:
    struct Request final {
        std::filesystem::path path;
        core::Sha256Digest digest;
        std::string revisionKey;
        std::size_t byteBudget = 0;
    };
    struct Statistics final {
        std::uint64_t queued = 0;
        std::uint64_t captured = 0;
        std::uint64_t skipped = 0;
        std::uint64_t failed = 0;
    };

    explicit ImageSequenceReadAhead(std::size_t workers = 4, std::size_t queueLimit = 32);
    // Cancels outstanding captures and joins the workers.
    ~ImageSequenceReadAhead();
    ImageSequenceReadAhead(const ImageSequenceReadAhead&) = delete;
    ImageSequenceReadAhead& operator=(const ImageSequenceReadAhead&) = delete;

    // Queues captures into `cache`, skipping files already cached, queued or in flight, and any
    // beyond the queue bound. The cache is held weakly; a cache that is gone skips its captures.
    void request(const std::shared_ptr<OperationCache>& cache, std::vector<Request> requests);
    [[nodiscard]] Statistics statistics() const;

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace bloom::runtime
