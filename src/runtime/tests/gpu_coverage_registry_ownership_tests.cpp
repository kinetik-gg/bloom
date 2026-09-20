// Ownership regression for the GPU coverage contract registries. Generated names must be owned by
// the returned entry: a std::string_view into a temporary std::string dangles as soon as the
// temporary dies. The registries are retained, the allocator is churned with same-size poison
// blocks, and every name is then read back. Under ASAN a stale read is a hard heap-use-after-free;
// without ASAN the reused bytes make a stale label fail its exact comparison.
//
// This TU depends only on the header-only registries, so the ASAN variant links no already-built
// library and rebuilds no dependency.

#include <bloom/runtime/gpu_coverage_contract.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

template <typename Entry>
[[nodiscard]] const Entry* findById(const std::vector<Entry>& entries, const std::string_view id) {
    for (const auto& entry : entries) {
        if (std::string_view{entry.id} == id) {
            return &entry;
        }
    }
    return nullptr;
}

// Allocate and retain blocks in the same size classes as the generated labels, filled with a poison
// byte. A freed label block reused by one of these stays overwritten while the retained registry is
// read, so a dangling view can no longer read its original text.
[[nodiscard]] std::vector<std::string> churnAllocator() {
    std::vector<std::string> blocks;
    blocks.reserve(512);
    for (std::size_t size = 4; size <= 64; ++size) {
        for (int repeat = 0; repeat < 8; ++repeat) {
            blocks.emplace_back(size, static_cast<char>(0xAB));
        }
    }
    return blocks;
}

[[nodiscard]] bool isPoisoned(const std::string_view text) {
    return text.find(static_cast<char>(0xAB)) != std::string_view::npos;
}

} // namespace

int main() {
    std::size_t failures = 0;
    const auto expect = [&failures](const bool condition, const std::string_view what) {
        if (!condition) {
            ++failures;
            std::cerr << "FAIL: " << what << '\n';
        }
    };

    const auto features = bloom::runtime::gpuFeatureCoverage();
    const auto operations = bloom::runtime::gpuOperationCoverage();
    const auto effects = bloom::runtime::gpuImageEffectCoverage();
    const auto routes = bloom::runtime::gpuRenderRouteCoverage();

    // Retain every registry first, then churn the allocator before reading any name back.
    const auto poison = churnAllocator();

    for (const auto& entry : features) {
        expect(!entry.id.empty(), "feature id is nonempty");
        expect(!entry.label.empty(), "feature label is nonempty");
        expect(!isPoisoned(entry.label), "feature label is not allocator poison");
    }
    for (const auto& entry : operations) {
        expect(!entry.id.empty(), "operation id is nonempty");
        expect(!entry.label.empty(), "operation label is nonempty");
        expect(!isPoisoned(entry.label), "operation label is not allocator poison");
    }
    for (const auto& entry : effects) {
        expect(!entry.id.empty(), "effect id is nonempty");
        expect(!entry.label.empty(), "effect label is nonempty");
    }
    for (const auto& entry : routes) {
        expect(!entry.id.empty(), "route id is nonempty");
        expect(!entry.label.empty(), "route label is nonempty");
        expect(!entry.owner.empty(), "route owner is nonempty");
    }

    // The shape labels are the generated text that previously dangled. Compare each one exactly
    // after the churn.
    constexpr std::array<bloom::document::ShapeKind, 7> kShapeKinds{
        bloom::document::ShapeKind::Rectangle, bloom::document::ShapeKind::Ellipse,
        bloom::document::ShapeKind::Triangle,  bloom::document::ShapeKind::Polygon,
        bloom::document::ShapeKind::Star,      bloom::document::ShapeKind::Line,
        bloom::document::ShapeKind::Path};
    for (const auto kind : kShapeKinds) {
        const auto id =
            std::string{"feature.shape."} + std::to_string(static_cast<std::int64_t>(kind));
        const auto* entry = findById(features, id);
        expect(entry != nullptr, "shape feature entry exists: " + id);
        if (entry != nullptr) {
            const auto expected =
                "shape kind axis: " + std::string{bloom::runtime::gpuShapeKindFeatureLabel(kind)};
            expect(entry->label == expected, "shape label survives: " + expected);
        }
    }

    // Blend labels are literals but must survive the same churn.
    for (const auto mode : bloom::core::kBlendModes) {
        const auto id =
            std::string{"feature.blend."} + std::to_string(bloom::core::blendModeStoredValue(mode));
        const auto* entry = findById(features, id);
        expect(entry != nullptr && entry->label == "blend mode axis",
               "blend label survives: " + id);
    }

    if (failures != 0) {
        std::cerr << failures << " registry ownership expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: coverage contract registry names are owned and survive allocator churn\n";
    return 0;
}
