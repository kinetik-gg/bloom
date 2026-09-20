// Mutation-proof probe (expected to FAIL to compile under -Werror=switch). The contract classifies
// feature enumerators (document::ShapeKind) with no-default switches, so adding an enumerator
// without classifying it is a compile failure. This probe reproduces that exact guard with its own
// synthetic feature enum and one unhandled case, proving the build actually enforces it; the
// positive probe compiles the same pattern exhaustively.

#include <bloom/runtime/gpu_coverage_contract.hpp>

#include <string_view>

namespace {

enum class SyntheticFeatureEnum {
    SyntheticFeatureEnumFirst,
    SyntheticFeatureEnumSecond,
    SyntheticFeatureEnumThird,
};

// Deliberately missing SyntheticFeatureEnumThird, mirroring a new unclassified ShapeKind. The
// enumerator carries the type name so the -Wswitch diagnostic names it.
[[maybe_unused, nodiscard]] std::string_view
syntheticFeatureLabel(const SyntheticFeatureEnum value) {
    switch (value) {
    case SyntheticFeatureEnum::SyntheticFeatureEnumFirst:
        return "first";
    case SyntheticFeatureEnum::SyntheticFeatureEnumSecond:
        return "second";
    }
    return {};
}

} // namespace
