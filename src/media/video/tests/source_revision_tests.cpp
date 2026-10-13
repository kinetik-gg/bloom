#include "packed_host_retirement_private.hpp"
#include <array>
#include <bloom/media/video/session.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace bloom::media;
using Snapshot = bloom::platform::ReadOnlyFileSnapshot;
using Quota = bloom::platform::FileSnapshotQuota;
void check(const bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <typename T> T product(provider::Result<T> result) {
    if (const auto* error = std::get_if<provider::Unavailable>(&result))
        throw std::runtime_error(error->detail);
    return std::get<T>(std::move(result));
}
bool equal(const provider::FrameProduct& a, const provider::FrameProduct& b) {
    if (a.pts != b.pts || a.format != b.format || a.planes.size() != b.planes.size())
        return false;
    for (std::size_t i = 0; i < a.planes.size(); ++i)
        if (a.planes[i].bytes != b.planes[i].bytes || a.planes[i].digest != b.planes[i].digest)
            return false;
    return true;
}
struct Scratch final {
    std::filesystem::path path;
    explicit Scratch(const std::filesystem::path& fixtures)
        : path(fixtures / "source-revision-test") {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
        std::filesystem::create_directories(path);
    }
    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
void captureLimits(const std::filesystem::path& source, const std::filesystem::path& spool) {
    const auto size = std::filesystem::file_size(source);
    auto quota = std::make_shared<Quota>(size, 1);
    auto capture = Snapshot::capture(source, spool, quota, provider::Limits::sourceBytes);
    check(std::holds_alternative<std::unique_ptr<Snapshot>>(capture), "disk capture succeeds");
    auto snapshot = std::get<std::unique_ptr<Snapshot>>(std::move(capture));
    check(snapshot->size() == size && snapshot->statistics().bytesRead == size &&
              snapshot->statistics().bytesHashed == size && quota->residentBytes() == size &&
              quota->residentFiles() == 1,
          "exact captured bytes and live disk charge");
    check(std::filesystem::is_empty(spool), "published source has no writable pathname");
    const auto second = Snapshot::capture(source, spool, quota, provider::Limits::sourceBytes);
    check(std::get<bloom::platform::ProcessFailure>(second).code ==
              bloom::platform::ProcessError::ResourceLimit,
          "pending/live source cannot donate quota");
    snapshot.reset();
    check(quota->residentBytes() == 0 && quota->residentFiles() == 0,
          "actual handle close releases quota");
    std::uint64_t completed = 0;
    const auto cancelled = Snapshot::capture(
        source, spool, quota, provider::Limits::sourceBytes, [&] { return completed >= 65536; },
        [&](const auto bytes, const auto total) {
            check(total == size, "capture progress has bounded total");
            completed = bytes;
        });
    check(std::get<bloom::platform::ProcessFailure>(cancelled).code ==
                  bloom::platform::ProcessError::Cancelled &&
              completed == 65536 && quota->residentBytes() == 0 && std::filesystem::is_empty(spool),
          "chunk cancellation removes partial spool and returns quota");
    const auto hostRefused =
        Snapshot::capture(source, spool, quota, provider::Limits::sourceBytes, {}, {}, {}, 1);
    check(std::get<bloom::platform::ProcessFailure>(hostRefused).code ==
                  bloom::platform::ProcessError::ResourceLimit &&
              quota->residentBytes() == 0,
          "host capture allowance refuses before disk admission");
    std::uint64_t last = 0;
    auto smallChunks = Snapshot::capture(
        source, spool, quota, provider::Limits::sourceBytes, {},
        [&](const auto bytes, const auto) {
            check(bytes - last <= 8192, "capture chunks obey remaining host allowance");
            last = bytes;
        },
        {}, 8192);
    check(std::holds_alternative<std::unique_ptr<Snapshot>>(smallChunks) && last == size,
          "small live host allowance streams identical movie without a fixed buffer floor");
    std::get<std::unique_ptr<Snapshot>>(smallChunks).reset();
    auto tooSmall = std::make_shared<Quota>(size - 1, 1);
    const auto refused = Snapshot::capture(source, spool, tooSmall, provider::Limits::sourceBytes);
    check(std::get<bloom::platform::ProcessFailure>(refused).code ==
              bloom::platform::ProcessError::ResourceLimit,
          "encoded source admission uses byte quota before copy");
}
void capturePathReplacement(const std::filesystem::path& source,
                            const std::filesystem::path& spool) {
    const auto size = std::filesystem::file_size(source);
    const auto alias = spool.parent_path() / "capture-alias";
    auto quota = std::make_shared<Quota>(size, 1);
    for (int mode = 0; mode < 3; ++mode) {
        bool replaced = false;
        const auto captured = Snapshot::capture(
            source, spool, quota, provider::Limits::sourceBytes, {},
            [&](const auto bytes, const auto total) {
                if (bytes != total)
                    return;
                auto iterator = std::filesystem::directory_iterator(spool);
                check(iterator != std::filesystem::directory_iterator{},
                      "capture private directory exists at completion progress");
                const auto temporary = iterator->path() / "source";
                if (mode == 2) {
                    std::filesystem::create_hard_link(temporary, alias);
                } else {
                    std::filesystem::remove(temporary);
                    if (mode == 1) {
                        std::filesystem::create_symlink(source, temporary);
                    } else {
                        std::filesystem::copy_file(source, temporary);
                        std::fstream replacement(temporary,
                                                 std::ios::binary | std::ios::in | std::ios::out);
                        replacement.put('x');
                    }
                }
                replaced = true;
            });
        const auto* failure = std::get_if<bloom::platform::ProcessFailure>(&captured);
        check(replaced && failure != nullptr &&
                  failure->code == bloom::platform::ProcessError::Io &&
                  quota->residentBytes() == 0 && quota->residentFiles() == 0 &&
                  std::filesystem::is_empty(spool),
              "same-size pathname replacement, symlink and retained hard link cannot publish "
              "hashed authority");
        std::filesystem::remove(alias);
        auto retry = Snapshot::capture(source, spool, quota, provider::Limits::sourceBytes);
        check(std::holds_alternative<std::unique_ptr<Snapshot>>(retry),
              "capture recovers after replacement refusal with the same exact quota");
    }
}
void sessionRevisions(const std::filesystem::path& source, const std::filesystem::path& scratch) {
    const auto copy = scratch / "mutable.mp4";
    std::filesystem::copy_file(source, copy, std::filesystem::copy_options::overwrite_existing);
    const auto size = std::filesystem::file_size(copy);
    const auto modified = std::filesystem::last_write_time(copy);
    auto quota = std::make_shared<Quota>(2 * size, 2);
    video::VideoDecodeSession session(copy, video::VideoDecodeSession::defaultWorker(), quota,
                                      scratch / "spool");
    const auto probe = product(session.probe());
    video::DecodedVideoCache cache(std::size_t{4608} * 2);
    bloom::platform::FileSnapshotStatistics work;
    bool retained = false;
    const video::SourceRevisionAccess interactive{
        video::SourceRevisionPolicy::Retained, true, &work, &retained, {}};
    const auto first = product(session.frame(probe, 0, 12, 0, &cache, {}, {}, {}, {}, interactive));
    check(retained && work.bytesRead == 0 && work.bytesHashed == 0,
          "probe hands exact retained source to decoder without rehash");
    {
        std::fstream file(copy, std::ios::in | std::ios::out | std::ios::binary);
        char byte = 0;
        file.read(&byte, 1);
        byte ^= 1;
        file.seekp(0);
        file.write(&byte, 1);
    }
    std::filesystem::last_write_time(copy, modified);
    check(std::filesystem::file_size(copy) == size, "same size replacement fixture");
    const auto cached =
        product(session.frame(probe, 0, 12, 0, &cache, {}, {}, {}, {}, interactive));
    check(first == cached && retained && work.bytesRead == 0 && work.bytesHashed == 0,
          "warm source ticket survives same-size same-mtime replacement with zero source work");
    std::filesystem::remove(copy);
    const auto thirteen =
        product(session.frame(probe, 0, 13, 0, &cache, {}, {}, {}, {}, interactive));
    video::VideoDecodeSession oracle(source);
    const auto oracleFrame = product(oracle.frame(probe, 0, 13, 0, nullptr));
    check(equal(*thirteen, *oracleFrame) && retained && work.bytesHashed == 0,
          "a different uncached frame decodes the same anonymous bytes after original disappears");
    const auto missing = session.frame(probe, 0, 12, 0, &cache);
    check(std::get<provider::Unavailable>(missing).reason == provider::Error::Io,
          "default final source policy rejects missing original before decoded hit");
    std::filesystem::copy_file(source, copy);
    const auto reconnected = product(
        session.frame(probe, 0, 12, 0, &cache, {}, {}, {}, {},
                      {video::SourceRevisionPolicy::Revalidate, true, &work, &retained, {}}));
    check(reconnected == first && !retained && work.bytesRead == size && work.bytesHashed == size,
          "reconnect freshly captures original before cached pixels can hit");
    {
        std::fstream file(copy, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(0);
        file.put('x');
    }
    std::filesystem::last_write_time(copy, modified);
    const auto changed = session.frame(probe, 0, 12, 0, &cache);
    check(std::get<provider::Unavailable>(changed).reason == provider::Error::SourceChanged,
          "default policy rejects changed same-mtime bytes before cached frame");
    std::filesystem::copy_file(source, copy, std::filesystem::copy_options::overwrite_existing);
    auto gestureQuota = std::make_shared<Quota>(size, 1);
    video::VideoDecodeSession gesture(copy, video::VideoDecodeSession::defaultWorker(),
                                      gestureQuota, scratch / "gesture-spool");
    video::DecodedVideoCache gestureCache(4608);
    for (int i = 0; i < 2; ++i) {
        (void)product(
            gesture.frame(probe, 0, 12, 0, &gestureCache, {}, {}, {}, {},
                          {video::SourceRevisionPolicy::Retained, false, &work, &retained, {}}));
        check(!retained && work.bytesHashed == size && gestureQuota->residentBytes() == 0 &&
                  gestureCache.residentBytes() == 0,
              "cold read-only gesture inserts neither source nor decoded frame");
    }
}
struct RetirementScope final {
    std::shared_ptr<bloom::render::PackedHostRetirementOwner> owner =
        bloom::render::PackedHostRetirementOwner::create();
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false, released = false;
    std::thread::id worker;
    RetirementScope() {
        auto state = owner->producerState();
        state->testContext = this;
        state->beforeFree = [](void* context) noexcept {
            auto& scope = *static_cast<RetirementScope*>(context);
            std::unique_lock lock(scope.mutex);
            scope.worker = std::this_thread::get_id();
            scope.entered = true;
            scope.condition.notify_all();
            scope.condition.wait(lock, [&] { return scope.released; });
        };
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }
    ~RetirementScope() {
        release();
        owner->beginShutdown();
        while (!owner->exitAcknowledged())
            std::this_thread::yield();
        (void)owner->collect();
    }
};
void retirement(const std::filesystem::path& source, const std::filesystem::path& spool) {
    RetirementScope scope;
    const auto size = std::filesystem::file_size(source);
    auto quota = std::make_shared<Quota>(size, 1);
    auto session = std::make_unique<video::VideoDecodeSession>(
        source, video::VideoDecodeSession::defaultWorker(), quota, spool,
        scope.owner->producerState());
    (void)product(session->probe());
    check(quota->residentBytes() == size, "retained snapshot owns exact disk bytes");
    const auto releasingThread = std::this_thread::get_id();
    session.reset();
    {
        std::unique_lock lock(scope.mutex);
        check(
            scope.condition.wait_for(lock, std::chrono::seconds(3), [&] { return scope.entered; }),
            "snapshot final reference queues on existing owner");
        check(scope.worker != releasingThread && quota->residentBytes() == size &&
                  quota->residentFiles() == 1,
              "foreign/UI final release preserves charges until worker actually closes backing");
    }
    const auto denied = Snapshot::capture(source, spool, quota, provider::Limits::sourceBytes);
    check(std::get<bloom::platform::ProcessFailure>(denied).code ==
              bloom::platform::ProcessError::ResourceLimit,
          "retirement-pending source cannot donate capacity");
    scope.release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (quota->residentFiles() != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    check(quota->residentBytes() == 0 && quota->residentFiles() == 0,
          "actual worker release returns disk charge");
}
} // namespace
int main(int argc, char** argv) {
    try {
        check(argc == 2, "fixture directory");
        const auto fixtures = std::filesystem::absolute(argv[1]);
        Scratch scratch(fixtures);
        const auto source = fixtures / "numbered-h264.mp4";
        captureLimits(source, scratch.path / "limits-spool");
        capturePathReplacement(source, scratch.path / "replacement-spool");
        sessionRevisions(source, scratch.path);
        retirement(source, scratch.path / "retirement-spool");
        std::cout << "PASS: immutable disk-backed video source revisions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
