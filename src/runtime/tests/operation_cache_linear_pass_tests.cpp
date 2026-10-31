#include <bloom/runtime/operation_cache.hpp>

#include <iostream>
#include <string>
#include <string_view>
#include <thread>

// A linear pass (an export) retains only entries that recur within the pass: the first offer of a
// key is not stored, the second is. Per-frame footage decodes and results therefore stream through
// while static layers, held frames and looped sources stay cached. The pass is an object owned by
// the export run and handed explicitly to each store; stores without it (viewer, RAM Preview) are
// unaffected.
namespace {
using namespace bloom;
int failures = 0;
void expect(const bool value, const std::string_view message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

runtime::OperationCacheValue value() {
    return {.image = {}, .values = {runtime::CompiledValue{0.5}}};
}

bool retained(runtime::OperationCache& cache, const std::string& key) {
    return cache.find(key, document::Revision::fromRaw(1)).has_value();
}

void store(
    runtime::OperationCache& cache, const std::string& key, runtime::LinearPassRetention* pass,
    const runtime::OperationCacheEntryKind kind = runtime::OperationCacheEntryKind::Operation) {
    cache.store(key, document::Revision::fromRaw(1), value(), kind, {}, pass);
}

} // namespace

int main() {
    runtime::OperationCache cache;
    store(cache, "ordinary", nullptr);
    expect(retained(cache, "ordinary"), "outside a linear pass every store is retained");

    runtime::LinearPassRetention pass;
    // A static layer: offered on every frame of the pass.
    store(cache, "static-layer", &pass);
    expect(!retained(cache, "static-layer"), "a key offered once is not retained");
    store(cache, "static-layer", &pass);
    expect(retained(cache, "static-layer"), "a key offered again is retained");

    // Footage: a new decoded frame and a new result on every frame, never retained.
    for (int frame = 0; frame < 50; ++frame) {
        store(cache, "footage-" + std::to_string(frame), &pass,
              runtime::OperationCacheEntryKind::DecodedMedia);
        store(cache, "result-" + std::to_string(frame), &pass);
    }
    bool anyFrameUnique = false;
    for (int frame = 0; frame < 50; ++frame)
        anyFrameUnique = anyFrameUnique || retained(cache, "footage-" + std::to_string(frame)) ||
                         retained(cache, "result-" + std::to_string(frame));
    expect(!anyFrameUnique, "frame-unique entries stream through a linear pass");
    expect(cache.retainedBytes(runtime::OperationCacheEntryKind::DecodedMedia) == 0,
           "no decoded media is retained for frame-unique footage");
    expect(cache.statistics().linearPassDeferrals == 101,
           "every first offer is counted as deferred");

    // A viewer or RAM Preview store beside the export carries no pass and is retained.
    std::thread interactive([&cache] { store(cache, "interactive", nullptr); });
    interactive.join();
    expect(retained(cache, "interactive"), "an interactive store beside an export is retained");

    // The pass is shared by the frames it evaluates at once: offers from any thread count.
    std::thread frameA([&cache, &pass] { store(cache, "held-frame", &pass); });
    frameA.join();
    std::thread frameB([&cache, &pass] { store(cache, "held-frame", &pass); });
    frameB.join();
    expect(retained(cache, "held-frame"), "a key recurring across concurrent frames is retained");

    // Each export run owns its own pass: a key seen by an earlier run starts over.
    runtime::LinearPassRetention nextRun;
    store(cache, "next-run-static", &pass);
    store(cache, "next-run-static", &nextRun);
    expect(!retained(cache, "next-run-static"), "a pass does not inherit another pass's offers");

    // The remembered keys are bounded: the oldest offer is forgotten first.
    runtime::LinearPassRetention bounded;
    expect(!bounded.offer("oldest", false), "a first offer is not admitted");
    for (std::size_t index = 0; index < runtime::LinearPassRetention::kRememberedKeys; ++index)
        static_cast<void>(bounded.offer("key-" + std::to_string(index), false));
    expect(!bounded.offer("oldest", false), "the oldest remembered key is forgotten");
    expect(bounded.offer("key-" + std::to_string(runtime::LinearPassRetention::kRememberedKeys - 1),
                         false),
           "a recent key is still remembered");
    expect(!bounded.offer("prepared", false) && !bounded.offer("prepared", true),
           "prepared and ordinary namespaces are offered separately");

    store(cache, "after", nullptr);
    expect(retained(cache, "after"), "stores without a pass are retained");
    if (failures == 0)
        std::cout << "operation cache linear pass: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
