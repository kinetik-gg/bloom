#include "packed_host_retirement_private.hpp"
#include <array>
#include <atomic>
#include <bloom/media/video/audio.hpp>
#include <bloom/media/video/session.hpp>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <new>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#if defined(__linux__)
#include <sys/wait.h>
#endif

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
        // The first cold gesture hashes the original. The second has a verified identity and
        // adopts the oracle's live capture of the same digest. Neither inserts anything.
        check(i == 0 ? !retained && work.bytesHashed == size
                     : retained && work.bytesRead == 0 && work.bytesHashed == 0,
              "cold read-only gesture hashes once, then adopts the verified live capture");
        check(gestureQuota->residentBytes() == 0 && gestureCache.residentBytes() == 0,
              "cold read-only gesture inserts neither source nor decoded frame");
    }
}
#if defined(__linux__)
void audioOnePass(const std::filesystem::path& fixtures, const std::filesystem::path& scratch) {
    const auto source = fixtures / "numbered-prores.mov";
    std::vector<std::int64_t> launches;
    video::VideoDecodeSession session(
        source, video::VideoDecodeSession::defaultWorker(), {}, scratch / "audio-spool", {},
        [&](const std::int64_t process) { launches.push_back(process); });
    const auto probe = product(session.probe());
    const auto stream = std::ranges::find(probe.streams, provider::MediaKind::Audio,
                                          &provider::StreamDescriptor::kind);
    check(stream != probe.streams.end(), "the ProRes fixture carries audio");
    const auto frames = video::audioClipFrameCount(*stream);
    check(frames && *frames > provider::Limits::audioSamples,
          "the fixture spans more than one audio block");
    const auto clip =
        product(video::decodeAudioClip(session, probe, *frames * stream->channelLayout.size()));
    check(launches.size() == 1 && clip.frames == *frames,
          "every audio block of a clip reuses the probe's retained worker in one pass");
    // Oracle: the same blocks in reverse order cannot continue a cursor, so each restarts.
    for (std::uint64_t start =
             (*frames - 1) / provider::Limits::audioSamples * provider::Limits::audioSamples;
         ; start -= provider::Limits::audioSamples) {
        const auto count = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(provider::Limits::audioSamples, *frames - start));
        const auto block = product(session.audio(probe, stream->id, start, count));
        for (std::size_t channel = 0; channel < block.channels.size(); ++channel)
            check(std::equal(block.channels[channel].begin(), block.channels[channel].end(),
                             clip.planes[channel].begin() + static_cast<std::ptrdiff_t>(start)),
                  "sequential continuation equals independently positioned blocks");
        if (start == 0)
            break;
    }
}
void identityVerifiedRevalidation(const std::filesystem::path& source,
                                  const std::filesystem::path& scratch) {
    const auto copy = scratch / "identity.mp4";
    std::filesystem::copy_file(source, copy, std::filesystem::copy_options::overwrite_existing);
    const auto size = std::filesystem::file_size(copy);
    const auto reads = [] { return bloom::platform::sourceReadWork().fullReads; };
    video::VideoDecodeSession first(copy, video::VideoDecodeSession::defaultWorker(), {},
                                    scratch / "identity-spool");
    const auto probe = product(first.probe());
    const auto verified = reads();
    // Playback, edits and per-frame export all revalidate by default: identity only, no reads.
    bloom::platform::FileSnapshotStatistics work;
    bool retained = false;
    const video::SourceRevisionAccess revalidate{
        video::SourceRevisionPolicy::Revalidate, true, &work, &retained, {}};
    for (std::uint64_t i = 0; i < 6; ++i) {
        (void)product(first.frame(probe, 0, i, 0, nullptr, {}, {}, {}, {}, revalidate));
        check(retained && work.bytesRead == 0 && work.bytesHashed == 0,
              "revalidation of an unchanged source reuses the verified snapshot");
        check(!first.verifySource(probe.sourceDigest), "unchanged source verifies");
    }
    check(reads() == verified, "repeated revalidation performs zero full-file reads");
    // A second consumer (thumbnails, export, audio) adopts the live capture by digest.
    video::VideoDecodeSession second(copy, video::VideoDecodeSession::defaultWorker(), {},
                                     scratch / "identity-spool-second");
    (void)product(second.frame(probe, 0, 2, 0, nullptr, {}, {}, {}, {}, revalidate));
    check(retained && work.bytesHashed == 0 && reads() == verified &&
              video::sharedSourceSnapshotCount() >= 1,
          "another session adopts the verified snapshot without reading the source");
    // A timestamp-only touch rehashes exactly once and stays available.
    std::filesystem::last_write_time(copy, std::filesystem::last_write_time(copy) +
                                               std::chrono::seconds(3));
    (void)product(first.frame(probe, 0, 3, 0, nullptr, {}, {}, {}, {}, revalidate));
    check(!retained && work.bytesHashed == size && reads() == verified + 1,
          "a changed identity is recaptured and rehashed once");
    (void)product(second.frame(probe, 0, 3, 0, nullptr, {}, {}, {}, {}, revalidate));
    check(retained && reads() == verified + 1, "the other session stays verified without reading");
    // A same-size content change is still detected and reported with the relink diagnostic.
    const auto modified = std::filesystem::last_write_time(copy);
    {
        std::fstream file(copy, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(static_cast<std::streamoff>(size / 2));
        file.put('x');
    }
    std::filesystem::last_write_time(copy, modified);
    const auto changed = second.frame(probe, 0, 3, 0, nullptr);
    const auto* failure = std::get_if<provider::Unavailable>(&changed);
    check(failure != nullptr && failure->reason == provider::Error::SourceChanged &&
              failure->detail.find("relink") != std::string::npos,
          "a changed source reports the relink diagnostic");
    const auto verify = first.verifySource(probe.sourceDigest);
    check(verify && verify->reason == provider::Error::SourceChanged,
          "verification of a changed source reports SourceChanged without another read");
}
// #401: a NEW session on an unchanged file (a thumbnail or probe after the previous session ended)
// adopts the retained verified snapshot: no copy, no hash, no full read. A caller-supplied quota is
// never held past its sessions, and an explicit release makes the next session capture again.
void freshSessionAdoptsRetainedSnapshot(const std::filesystem::path& source,
                                        const std::filesystem::path& scratch) {
    const auto copy = scratch / "retained.mp4";
    std::filesystem::copy_file(source, copy, std::filesystem::copy_options::overwrite_existing);
    const auto reads = [] { return bloom::platform::sourceReadWork().fullReads; };
    video::releaseRetainedSourceSnapshots();
    provider::ProbeResult probe;
    {
        video::VideoDecodeSession first(copy, video::VideoDecodeSession::defaultWorker(), {},
                                        scratch / "retained-spool");
        probe = product(first.probe());
    }
    check(video::retainedSourceSnapshotCount() == 1,
          "an unrooted private-quota snapshot outlives its session");
    const auto captured = reads();
    bloom::platform::FileSnapshotStatistics work;
    bool retained = false;
    const video::SourceRevisionAccess revalidate{
        video::SourceRevisionPolicy::Revalidate, true, &work, &retained, {}};
    {
        video::VideoDecodeSession second(copy, video::VideoDecodeSession::defaultWorker(), {},
                                         scratch / "retained-spool-second");
        (void)product(second.frame(probe, 0, 1, 0, nullptr, {}, {}, {}, {}, revalidate));
        check(retained && work.bytesRead == 0 && work.bytesHashed == 0 && reads() == captured,
              "a second session on an unchanged file does no full read");
    }
    video::releaseRetainedSourceSnapshots();
    check(video::retainedSourceSnapshotCount() == 0, "release drops every retained snapshot");
    {
        video::VideoDecodeSession third(copy, video::VideoDecodeSession::defaultWorker(), {},
                                        scratch / "retained-spool-third");
        (void)product(third.frame(probe, 0, 1, 0, nullptr, {}, {}, {}, {}, revalidate));
        check(!retained && reads() == captured + 1, "after a release the next session captures");
    }
    const auto size = std::filesystem::file_size(copy);
    auto quota = std::make_shared<Quota>(size, 1);
    {
        video::VideoDecodeSession explicitQuota(copy, video::VideoDecodeSession::defaultWorker(),
                                                quota, scratch / "retained-spool-explicit");
        (void)product(explicitQuota.frame(probe, 0, 1, 0, nullptr, {}, {}, {}, {}, revalidate));
    }
    check(quota->residentFiles() == 0 && quota->residentBytes() == 0,
          "a caller-supplied quota is returned when its session ends");
    video::releaseRetainedSourceSnapshots();
}
void reaped(const std::int64_t process) {
    check(process > 0, "retained worker launched");
    int status = 0;
    errno = 0;
    check(::waitpid(static_cast<pid_t>(process), &status, WNOHANG) == -1 && errno == ECHILD,
          "retained worker was reaped");
}
void retainedWorker(const std::filesystem::path& source, const std::filesystem::path& scratch) {
    const auto size = std::filesystem::file_size(source);
    auto quota = std::make_shared<Quota>(size, 1);
    std::vector<std::int64_t> launches;
    {
        video::VideoDecodeSession session(
            source, video::VideoDecodeSession::defaultWorker(), quota, scratch / "reuse-spool", {},
            [&](const std::int64_t process) { launches.push_back(process); });
        const auto probe = product(session.probe());
        const video::SourceRevisionAccess retained{video::SourceRevisionPolicy::Retained};
        const auto index = product(session.index(probe, {}, retained));
        check(launches.size() == 1,
              "probe and index reopen the same anonymous input in one worker");
        // The MOV fixture also carries one keyed timecode/data packet. The demux index covers
        // every stream, while the frame oracle below reads the four video keyframes.
        check(probe.streams.size() == 2 && probe.streams[0].kind == provider::MediaKind::Video &&
                  probe.streams[1].kind == provider::MediaKind::Data &&
                  index.keyframes.size() == 5,
              "retained demux index keeps video and timecode stream key packets");
        constexpr std::array<provider::Rational, 4> videoKeyTimes{
            {{0, 1}, {1, 2}, {1, 1}, {3, 2}}};
        std::size_t videoKeys = 0, dataKeys = 0;
        for (const auto& key : index.keyframes) {
            if (key.stream == 0) {
                check(videoKeys < videoKeyTimes.size() &&
                          key.pts == videoKeyTimes[videoKeys] && key.dts == key.pts,
                      "retained index preserves every exact video keyframe time");
                ++videoKeys;
            } else {
                check(key.stream == 1 && key.pts == provider::Rational{0, 1} &&
                          key.dts == key.pts,
                      "retained index preserves the timecode packet");
                ++dataKeys;
            }
        }
        check(videoKeys == 4 && dataKeys == 1, "retained index covers both fixture streams");
        for (unsigned request = 0; request < 100; ++request) {
            const auto ordinal = request % 48U;
            const auto divisor = std::gcd(ordinal, 24U);
            const auto frame =
                product(session.frame(probe, 0, ordinal, 0, nullptr, {}, {}, {}, {}, retained));
            check(frame->pts == provider::Rational{ordinal / divisor, 24U / divisor} &&
                      std::to_integer<unsigned>(frame->planes.front().bytes.front()) ==
                          16U + ordinal * 3U,
                  "retained sequential decode preserves the exact frame oracle");
        }
        for (const auto ordinal : {47U, 0U, 36U, 1U, 35U, 12U}) {
            const auto frame =
                product(session.frame(probe, 0, ordinal, 0, nullptr, {}, {}, {}, {}, retained));
            const auto divisor = std::gcd(ordinal, 24U);
            check(frame->pts == provider::Rational{ordinal / divisor, 24U / divisor} &&
                      std::to_integer<unsigned>(frame->planes.front().bytes.front()) ==
                          16U + ordinal * 3U,
                  "retained random seeks return the requested frame and exact time");
        }
        check(launches.size() == 1 && quota->residentBytes() == size && quota->residentFiles() == 1,
              "100 frame requests amortize one worker and one indexed source within quota");
        for (unsigned ordinal = 0; ordinal < 20; ++ordinal)
            (void)product(session.frame(probe, 0, ordinal, 0, nullptr, {}, {}, {}, {}, retained));
        (void)product(session.frame(probe, 0, 20, 0, nullptr, {}, {}, {}, {}, retained));
        // Issue #366: the call bound is sized for continuous playback; relaunching every 128
        // calls discarded the decoder position and packet index every few seconds.
        check(launches.size() == 1, "retained worker stays live past 128 calls");

        int cancelChecks = 0;
        const auto cancelled = session.frame(
            probe, 0, 7, 0, nullptr, {}, {}, {}, [&] { return ++cancelChecks >= 4; }, retained);
        check(std::get<provider::Unavailable>(cancelled).reason == provider::Error::Cancelled,
              "in-flight retained call observes cancellation");
        reaped(launches[0]);
        const auto restarted =
            product(session.frame(probe, 0, 7, 0, nullptr, {}, {}, {}, {}, retained));
        check(launches.size() == 2 &&
                  std::to_integer<unsigned>(restarted->planes.front().bytes.front()) == 37U,
              "cancelled retained worker is replaced without losing immutable source authority");
    }
    reaped(launches.back());
    check(quota->residentBytes() == 0 && quota->residentFiles() == 0,
          "session teardown releases retained worker and source charge");
}
void failedLeaderCompletesFollowers(const std::filesystem::path& source,
                                    const std::filesystem::path& scratch) {
    video::VideoDecodeSession session(source, video::VideoDecodeSession::defaultWorker(), {},
                                      scratch / "failed-flight-spool");
    const auto probe = product(session.probe());
    const video::SourceRevisionAccess retained{video::SourceRevisionPolicy::Retained};
    for (unsigned failure = 0; failure < 2; ++failure) {
        const auto ordinal = 9U + failure;
        std::atomic<bool> leaderAdmitted{false}, followerWaiting{false};
        std::atomic<unsigned> leaderChecks{0}, followerChecks{0};
        auto leader = std::async(std::launch::async, [&] {
            return session.frame(
                probe, 0, ordinal, 0, nullptr, {}, {}, {},
                [&] {
                    if (++leaderChecks == 2) {
                        leaderAdmitted = true;
                        const auto deadline =
                            std::chrono::steady_clock::now() + std::chrono::seconds(3);
                        while (!followerWaiting && std::chrono::steady_clock::now() < deadline)
                            std::this_thread::yield();
                        if (failure == 0)
                            throw std::bad_alloc{};
                        throw 316; // An uncaught non-standard exception must still finish flight.
                    }
                    return false;
                },
                retained);
        });
        const auto admissionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!leaderAdmitted && std::chrono::steady_clock::now() < admissionDeadline)
            std::this_thread::yield();
        check(leaderAdmitted, "leader reached the admitted frame flight");
        auto follower = std::async(std::launch::async, [&] {
            return session.frame(
                probe, 0, ordinal, 0, nullptr, {}, {}, {},
                [&] {
                    if (++followerChecks == 2)
                        followerWaiting = true;
                    return false;
                },
                retained);
        });
        check(leader.wait_for(std::chrono::seconds(3)) == std::future_status::ready &&
                  follower.wait_for(std::chrono::seconds(3)) == std::future_status::ready &&
                  followerWaiting,
              "failure after admission terminates leader and attached follower");
        const auto followerResult = follower.get();
        if (failure == 0) {
            const auto leaderResult = leader.get();
            check(std::get<provider::Unavailable>(leaderResult).reason ==
                          provider::Error::Oversized &&
                      std::get<provider::Unavailable>(followerResult).reason ==
                          provider::Error::Oversized,
                  "allocation failure publishes one terminal outcome to both callers");
        } else {
            bool markerCaught = false;
            try {
                (void)leader.get();
            } catch (const int marker) {
                markerCaught = marker == 316;
            }
            check(markerCaught &&
                      std::get<provider::Unavailable>(followerResult).reason == provider::Error::Io,
                  "uncaught leader unwind publishes prebuilt fallback to follower");
        }
        (void)product(session.frame(probe, 0, ordinal, 0, nullptr, {}, {}, {}, {}, retained));
    }
}
void retainedGenerationReplacement(const std::filesystem::path& source,
                                   const std::filesystem::path& scratch) {
    const auto copy = scratch / "retained-generation.mp4";
    std::filesystem::copy_file(source, copy);
    const auto size = std::filesystem::file_size(copy);
    auto quota = std::make_shared<Quota>(size + 16, 1);
    std::vector<std::int64_t> launches;
    {
        video::VideoDecodeSession session(
            copy, video::VideoDecodeSession::defaultWorker(), quota, scratch / "generation-spool",
            {}, [&](const std::int64_t process) { launches.push_back(process); });
        const auto old = product(session.probe());
        const video::SourceRevisionAccess retained{video::SourceRevisionPolicy::Retained};
        (void)product(session.frame(old, 0, 3, 0, nullptr, {}, {}, {}, {}, retained));
        {
            std::ofstream changed(copy, std::ios::binary | std::ios::app);
            changed << "changed";
        }
        (void)product(session.frame(old, 0, 4, 0, nullptr, {}, {}, {}, {}, retained));
        check(launches.size() == 1, "retained generation is immutable across path replacement");
        const auto stale = session.frame(old, 0, 4, 0, nullptr);
        check(std::get<provider::Unavailable>(stale).reason == provider::Error::SourceChanged,
              "default revalidation rejects the changed source before publication");
        reaped(launches.front());
        const auto fresh = product(session.probe());
        check(fresh.sourceDigest != old.sourceDigest && launches.size() == 2 &&
                  quota->residentFiles() == 1,
              "fresh generation rotates worker under a one-file quota");
    }
    reaped(launches.back());
    check(quota->residentFiles() == 0 && quota->residentBytes() == 0,
          "generation replacement releases final source authority");
}
#endif
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
#if defined(__linux__)
        retainedWorker(source, scratch.path);
        failedLeaderCompletesFollowers(source, scratch.path);
        retainedGenerationReplacement(source, scratch.path);
        identityVerifiedRevalidation(source, scratch.path);
        freshSessionAdoptsRetainedSnapshot(source, scratch.path);
        audioOnePass(fixtures, scratch.path);
#endif
        retirement(source, scratch.path / "retirement-spool");
        std::cout << "PASS: immutable disk-backed video source revisions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
