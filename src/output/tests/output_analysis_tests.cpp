#include <bloom/output/output_analysis.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace output = bloom::output;

using Code = output::OutputFacetStableCodeV1;
using Error = output::OutputAnalysisReportErrorCodeV1;
using Facet = output::OutputFacetIdV1;
using Preset = output::OutputPresetV1;
using State = output::OutputPreservationStateV1;

constexpr std::string_view kChannels =
    "count=u:4;name-0=utf8:52;name-1=utf8:47;name-2=utf8:42;name-3=utf8:41;"
    "role-0=id:red;role-1=id:green;role-2=id:blue;role-3=id:alpha";
constexpr std::string_view kSourcePixels =
    "height=u:1;packing=id:rgba;sample-type=id:binary32;width=u:1";
constexpr std::string_view kPngPixels = "height=u:1;packing=id:rgba;sample-type=id:uint8;width=u:1";
constexpr std::string_view kWindow = "height=u:1;origin-x=i:0;origin-y=i:0;width=u:1";
constexpr std::string_view kNoDependencies = "kind=id:none;revision=id:none";
constexpr std::string_view kOcioDependency =
    "kind=id:ocio;revision=id:0000000000000000000000000000000000000000000000000000000000000000";

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

template <typename Enum> [[nodiscard]] Enum enumWithBits(const std::uint8_t bits) noexcept {
    static_assert(sizeof(Enum) == sizeof(bits));
    Enum value = static_cast<Enum>(1U);
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

[[nodiscard]] std::array<output::OutputFacetAssessmentV1View, output::kOutputAnalysisFacetCountV1>
makeExrReport() noexcept {
    return {{{Facet::Pixels, State::Exact, Code::None, kSourcePixels, kSourcePixels},
             {Facet::Precision, State::Exact, Code::None, "component-type=id:binary32",
              "component-type=id:binary32"},
             {Facet::Color, State::Exact, Code::None, "color-id=id:lin_rec709_scene",
              "color-id=id:lin_rec709_scene"},
             {Facet::AlphaAssociation, State::Exact, Code::None,
              "association=id:premultiplied;zero-alpha=id:canonical-zero",
              "association=id:premultiplied;zero-alpha=id:canonical-zero"},
             {Facet::Channels, State::Exact, Code::None, kChannels, kChannels},
             {Facet::DataWindow, State::Exact, Code::None, kWindow, kWindow},
             {Facet::DisplayWindow, State::Exact, Code::None, kWindow, kWindow},
             {Facet::PixelAspect, State::Exact, Code::None, "denominator=u:1;numerator=u:1",
              "value=f32:3f800000"},
             {Facet::Compression, State::Exact, Code::None, "", "method=id:zip"},
             {Facet::Metadata, State::Exact, Code::None, "profile=id:none", "profile=id:none"},
             {Facet::ExternalDependencies, State::Exact, Code::None, kNoDependencies,
              kNoDependencies}}};
}

[[nodiscard]] std::array<output::OutputFacetAssessmentV1View, output::kOutputAnalysisFacetCountV1>
makePngReport() noexcept {
    return {{{Facet::Pixels, State::Approximated, Code::PngDisplayTransformClampQuantize,
              kSourcePixels, kPngPixels},
             {Facet::Precision, State::Approximated, Code::PngFloat32ToUint8,
              "component-type=id:binary32", "component-type=id:uint8"},
             {Facet::Color, State::Approximated, Code::PngLinRec709SceneToSrgb,
              "color-id=id:lin_rec709_scene", "color-id=id:srgb_rec709_display"},
             {Facet::AlphaAssociation, State::Approximated, Code::PngPremultipliedToStraight,
              "association=id:premultiplied;zero-alpha=id:canonical-zero",
              "association=id:straight;zero-alpha=id:canonical-zero"},
             {Facet::Channels, State::Exact, Code::None, kChannels, kChannels},
             {Facet::DataWindow, State::Exact, Code::None, kWindow, kWindow},
             {Facet::DisplayWindow, State::Exact, Code::None, kWindow, kWindow},
             {Facet::PixelAspect, State::Exact, Code::None, "denominator=u:1;numerator=u:1",
              "denominator=u:1;numerator=u:1"},
             {Facet::Compression, State::Exact, Code::None, "",
              "method=id:deflate-level-6-filter-none"},
             {Facet::Metadata, State::Exact, Code::None, "profile=id:none", "profile=id:none"},
             {Facet::ExternalDependencies, State::ExternalReference, Code::PngOcioExternalReference,
              kNoDependencies, kOcioDependency}}};
}

[[nodiscard]] output::OutputAnalysisReportValidationV1
validate(const Preset preset,
         const std::span<const output::OutputFacetAssessmentV1View> facets) noexcept {
    return output::validateOutputAnalysisReportV1({preset, facets});
}

void testPresetIdentityAndSchemaMatrix(Expectations& expectations) {
    const auto png = output::outputPresetIdentityV1(Preset::PngRgba8SrgbV1);
    expectations.expect(png && png->serializedId == "PngRgba8SrgbV1" && png->version == 1 &&
                            png->outputPixelSemanticsProfileId ==
                                "bloom.output.png-rgba8-srgb.semantic.v1",
                        "the PNG typed preset derives its exact portable identity tuple");
    const auto exr = output::outputPresetIdentityV1(Preset::FlatExrRgba32fLinRec709SceneV1);
    expectations.expect(exr && exr->serializedId == "FlatExrRgba32fLinRec709SceneV1" &&
                            exr->version == 1 &&
                            exr->outputPixelSemanticsProfileId ==
                                "bloom.output.exr-rgba32f-lin-rec709-scene.semantic.v1",
                        "the EXR typed preset derives its exact portable identity tuple");
    expectations.expect(!output::outputPresetIdentityV1(enumWithBits<Preset>(0xFFU)),
                        "an unknown preset fails closed");

    using Schema = output::OutputFacetDescriptorSchemaV1;
    constexpr std::array sourceSchemas{Schema::Pixels,
                                       Schema::Precision,
                                       Schema::Color,
                                       Schema::AlphaAssociation,
                                       Schema::Channels,
                                       Schema::Window,
                                       Schema::Window,
                                       Schema::PixelAspectRational,
                                       Schema::Absent,
                                       Schema::Metadata,
                                       Schema::ExternalDependencies};
    constexpr std::array pngTargets{Schema::Pixels,
                                    Schema::Precision,
                                    Schema::Color,
                                    Schema::AlphaAssociation,
                                    Schema::Channels,
                                    Schema::Window,
                                    Schema::Window,
                                    Schema::PixelAspectRational,
                                    Schema::Compression,
                                    Schema::Metadata,
                                    Schema::ExternalDependencies};
    constexpr std::array exrTargets{Schema::Pixels,
                                    Schema::Precision,
                                    Schema::Color,
                                    Schema::AlphaAssociation,
                                    Schema::Channels,
                                    Schema::Window,
                                    Schema::Window,
                                    Schema::PixelAspectBinary32,
                                    Schema::Compression,
                                    Schema::Metadata,
                                    Schema::ExternalDependencies};

    bool matrixMatches = true;
    for (std::size_t index = 0; index < sourceSchemas.size(); ++index) {
        const auto facet = static_cast<Facet>(index + 1U);
        const auto pngSchemas =
            output::outputFacetDescriptorSchemasV1(Preset::PngRgba8SrgbV1, facet);
        const auto exrSchemas =
            output::outputFacetDescriptorSchemasV1(Preset::FlatExrRgba32fLinRec709SceneV1, facet);
        matrixMatches = matrixMatches && pngSchemas && exrSchemas &&
                        pngSchemas->source == sourceSchemas[index] &&
                        exrSchemas->source == sourceSchemas[index] &&
                        pngSchemas->target == pngTargets[index] &&
                        exrSchemas->target == exrTargets[index];
    }
    expectations.expect(matrixMatches, "all eleven source/PNG/EXR descriptor schemas are closed");
    expectations.expect(
        !output::outputFacetDescriptorSchemasV1(Preset::PngRgba8SrgbV1, enumWithBits<Facet>(0xFFU)),
        "an unknown facet has no schema mapping");
}

void testStableCodeMapping(Expectations& expectations) {
    struct Case final {
        Code code;
        std::string_view text;
        State state;
        std::uint16_t facetMask;
        bool png;
        bool exr;
        bool permits;
    };
    constexpr auto bit = [](const Facet facet) {
        return static_cast<std::uint16_t>(1U << (static_cast<std::uint8_t>(facet) - 1U));
    };
    constexpr std::array cases{
        Case{Code::None, "", State::Exact, output::kOutputAnalysisAllFacetsPermittedV1, true, true,
             true},
        Case{Code::PngDisplayTransformClampQuantize, "png.display-transform-clamp-quantize",
             State::Approximated, bit(Facet::Pixels), true, false, true},
        Case{Code::ProcessFrameMissing, "process-frame.missing", State::Missing, bit(Facet::Pixels),
             true, true, false},
        Case{Code::PixelsUnsupported, "pixels.unsupported", State::Unsupported, bit(Facet::Pixels),
             true, true, false},
        Case{Code::PngFloat32ToUint8, "png.float32-to-uint8", State::Approximated,
             bit(Facet::Precision), true, false, true},
        Case{Code::PrecisionUnsupported, "precision.unsupported", State::Unsupported,
             bit(Facet::Precision), true, true, false},
        Case{Code::PngLinRec709SceneToSrgb, "png.lin-rec709-scene-to-srgb", State::Approximated,
             bit(Facet::Color), true, false, true},
        Case{Code::OcioMissing, "ocio.missing", State::Missing, bit(Facet::Color), true, false,
             false},
        Case{Code::OcioChanged, "ocio.changed", State::Missing, bit(Facet::Color), true, false,
             false},
        Case{Code::OcioInvalid, "ocio.invalid", State::Missing, bit(Facet::Color), true, false,
             false},
        Case{Code::OcioResourceMissing, "ocio.resource-missing", State::Missing, bit(Facet::Color),
             true, false, false},
        Case{Code::OcioVersionUnsupported, "ocio.version-unsupported", State::Missing,
             bit(Facet::Color), true, false, false},
        Case{Code::ColorUnsupported, "color.unsupported", State::Unsupported, bit(Facet::Color),
             true, true, false},
        Case{Code::PngPremultipliedToStraight, "png.premultiplied-to-straight", State::Approximated,
             bit(Facet::AlphaAssociation), true, false, true},
        Case{Code::AlphaUnsupported, "alpha.unsupported", State::Unsupported,
             bit(Facet::AlphaAssociation), true, true, false},
        Case{Code::ChannelsUnsupported, "channels.unsupported", State::Unsupported,
             bit(Facet::Channels), true, true, false},
        Case{Code::PngOriginWindowRequired, "png.origin-window-required", State::Unsupported,
             bit(Facet::DataWindow), true, false, false},
        Case{Code::WindowOutOfRange, "window.out-of-range", State::Unsupported,
             static_cast<std::uint16_t>(bit(Facet::DataWindow) | bit(Facet::DisplayWindow)), true,
             true, false},
        Case{Code::PngEqualWindowRequired, "png.equal-window-required", State::Unsupported,
             bit(Facet::DisplayWindow), true, false, false},
        Case{Code::PngSquarePixelRequired, "png.square-pixel-required", State::Unsupported,
             bit(Facet::PixelAspect), true, false, false},
        Case{Code::ExrParRoundedBinary32, "exr.par-rounded-binary32", State::Approximated,
             bit(Facet::PixelAspect), false, true, true},
        Case{Code::PixelAspectUnsupported, "pixel-aspect.unsupported", State::Unsupported,
             bit(Facet::PixelAspect), true, true, false},
        Case{Code::CompressionUnavailable, "compression.unavailable", State::Missing,
             bit(Facet::Compression), true, true, false},
        Case{Code::CompressionUnsupported, "compression.unsupported", State::Unsupported,
             bit(Facet::Compression), true, true, false},
        Case{Code::MetadataUnsupported, "metadata.unsupported", State::Unsupported,
             bit(Facet::Metadata), true, true, false},
        Case{Code::PngOcioExternalReference, "png.ocio-external-reference",
             State::ExternalReference, bit(Facet::ExternalDependencies), true, false, true},
        Case{Code::DependencyMissing, "dependency.missing", State::Missing,
             bit(Facet::ExternalDependencies), true, true, false},
        Case{Code::AdapterUnavailable, "adapter.unavailable", State::Missing,
             bit(Facet::ExternalDependencies), true, true, false},
        Case{Code::ResourceLimitExceeded, "resource.limit-exceeded", State::Missing,
             bit(Facet::ExternalDependencies), true, true, false},
    };

    bool allMappingsMatch = true;
    for (const auto& expected : cases) {
        const auto rule = output::outputFacetStableCodeRuleV1(expected.code);
        const auto text = output::outputFacetStableCodeTextV1(expected.code);
        allMappingsMatch =
            allMappingsMatch && rule && rule->serializedCode == expected.text &&
            rule->requiredState == expected.state && rule->facetMask == expected.facetMask &&
            rule->validForPng == expected.png && rule->validForFlatExr == expected.exr &&
            rule->presetPermits == expected.permits && text && *text == expected.text;
    }
    expectations.expect(allMappingsMatch,
                        "every version-one stable code has its exact closed derived rule");
    expectations.expect(cases.size() == static_cast<std::size_t>(Code::ResourceLimitExceeded) + 1U,
                        "the mapping test enumerates every closed stable-code value");
    expectations.expect(!output::outputFacetStableCodeRuleV1(enumWithBits<Code>(0xFFU)),
                        "an unknown stable code fails closed");
    expectations.expect(!output::outputFacetStableCodeTextV1(enumWithBits<Code>(0xFFU)),
                        "an unknown code cannot masquerade as the valid empty Exact code");
}

void testValidReportsAndDerivedPermissions(Expectations& expectations) {
    auto exr = makeExrReport();
    const auto validExr = validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr);
    expectations.expect(validExr && validExr.value() != nullptr && validExr.value()->approvable() &&
                            validExr.value()->permissionMask().bits() ==
                                output::kOutputAnalysisAllFacetsPermittedV1,
                        "an all-exact EXR report validates with all derived permission bits");

    auto png = makePngReport();
    const auto validPng = validate(Preset::PngRgba8SrgbV1, png);
    expectations.expect(validPng && validPng.value() != nullptr && validPng.value()->approvable() &&
                            validPng.value()->permissionMask().bits() ==
                                output::kOutputAnalysisAllFacetsPermittedV1,
                        "the nominal PNG report derives all permitted conversion bits");

    exr[8].state = State::Missing;
    exr[8].stableCode = Code::CompressionUnavailable;
    const auto nonApprovable = validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr);
    expectations.expect(
        nonApprovable && nonApprovable.value() != nullptr && !nonApprovable.value()->approvable() &&
            !nonApprovable.value()->permissionMask().permits(Facet::Compression) &&
            nonApprovable.value()->permissionMask().permits(Facet::Metadata) &&
            !nonApprovable.value()->permissionMask().permits(enumWithBits<Facet>(0xFFU)),
        "a valid Missing report remains inspectable but is derived as non-approvable");
}

void testClosedReportStructure(Expectations& expectations) {
    auto exr = makeExrReport();
    expectations.expect(validate(enumWithBits<Preset>(0xFFU), exr).issue().code ==
                            Error::InvalidPreset,
                        "unknown report presets fail closed");
    expectations.expect(
        validate(Preset::FlatExrRgba32fLinRec709SceneV1, std::span(exr).first<10>()).issue().code ==
            Error::IncorrectFacetCount,
        "a report must contain exactly eleven facets");

    exr[0].facet = enumWithBits<Facet>(0xFFU);
    expectations.expect(validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr).issue().code ==
                            Error::InvalidFacet,
                        "unknown facet IDs fail closed");
    exr = makeExrReport();
    std::swap(exr[0], exr[1]);
    expectations.expect(validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr).issue().code ==
                            Error::FacetOutOfOrder,
                        "facet records must use the fixed serialization order");

    exr = makeExrReport();
    exr[0].state = enumWithBits<State>(0xFFU);
    expectations.expect(validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr).issue().code ==
                            Error::InvalidState,
                        "unknown preservation states fail closed");
    exr = makeExrReport();
    exr[0].stableCode = enumWithBits<Code>(0xFFU);
    expectations.expect(validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr).issue().code ==
                            Error::InvalidStableCode,
                        "unknown stable codes fail closed");

    exr = makeExrReport();
    exr[0].state = State::Unsupported;
    exr[0].stableCode = Code::PrecisionUnsupported;
    expectations.expect(validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr).issue().code ==
                            Error::StableCodeFacetMismatch,
                        "stable codes are valid only for their closed facet set");
    exr = makeExrReport();
    exr[2].state = State::Missing;
    exr[2].stableCode = Code::OcioMissing;
    expectations.expect(validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr).issue().code ==
                            Error::StableCodePresetMismatch,
                        "PNG-only failures cannot be smuggled into EXR analysis");
    exr = makeExrReport();
    exr[0].state = State::Approximated;
    expectations.expect(validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr).issue().code ==
                            Error::StateMismatch,
                        "callers cannot choose a state independently of the stable code");

    exr = makeExrReport();
    exr[9].state = State::Equivalent;
    expectations.expect(validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr).issue().code ==
                            Error::StateMismatch,
                        "Equivalent has no valid tuple in report version one");
    exr[9].state = State::Omitted;
    expectations.expect(validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr).issue().code ==
                            Error::StateMismatch,
                        "Omitted has no valid tuple in report version one");

    auto png = makePngReport();
    png[0].state = State::Exact;
    png[0].stableCode = Code::None;
    expectations.expect(validate(Preset::PngRgba8SrgbV1, png).issue().code ==
                            Error::NominalTupleMismatch,
                        "an impossible all-Exact PNG pixel tuple cannot forge approval");
}

void testDescriptorValidationAndBounds(Expectations& expectations) {
    auto exr = makeExrReport();
    exr[8].sourceDescriptor = "method=id:none";
    const auto absentSource = validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr);
    expectations.expect(absentSource.issue().code == Error::SourceDescriptorInvalid &&
                            absentSource.issue().descriptorSide ==
                                output::OutputFacetDescriptorSideV1::Source &&
                            absentSource.issue().descriptorError ==
                                output::OutputFacetDescriptorErrorCode::ExpectedAbsent,
                        "the per-side schema preserves the nested descriptor diagnostic");

    exr = makeExrReport();
    exr[9].targetDescriptor = "profile=id:none;unknown=id:no";
    const auto closedTarget = validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr);
    expectations.expect(closedTarget.issue().code == Error::TargetDescriptorInvalid &&
                            closedTarget.issue().descriptorError ==
                                output::OutputFacetDescriptorErrorCode::UnknownKey,
                        "report validation delegates exact target grammar and schema checks");

    std::string oversized = "profile=id:";
    oversized.append(output::kOutputFacetDescriptorV1MaximumBytes - oversized.size() + 1U, 'a');
    exr = makeExrReport();
    exr[9].sourceDescriptor = oversized;
    const auto tooLong = validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr);
    expectations.expect(
        tooLong.issue().code == Error::SourceDescriptorInvalid &&
            tooLong.issue().descriptorError ==
                output::OutputFacetDescriptorErrorCode::DescriptorTooLong &&
            tooLong.issue().descriptorErrorOffset == output::kOutputFacetDescriptorV1MaximumBytes,
        "the 1024-byte descriptor cap fails before grammar traversal with a distinct result");
}

void testPresetVocabularyAndRelationships(Expectations& expectations) {
    auto exr = makeExrReport();
    exr[2].sourceDescriptor = "color-id=id:invented";
    exr[2].targetDescriptor = "color-id=id:invented";
    expectations.expect(validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr).issue().code ==
                            Error::DescriptorVocabularyMismatch,
                        "canonical grammar alone cannot replace the preset's fixed color ID");

    exr = makeExrReport();
    exr[0].targetDescriptor = "height=u:1;packing=id:rgba;sample-type=id:binary32;width=u:2";
    expectations.expect(validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr).issue().code ==
                            Error::DescriptorRelationshipMismatch,
                        "an Exact facet requires equal source and target meaning");

    auto png = makePngReport();
    png[5].targetDescriptor = "height=u:1;origin-x=i:1;origin-y=i:0;width=u:1";
    png[5].state = State::Unsupported;
    png[5].stableCode = Code::PngOriginWindowRequired;
    expectations.expect(validate(Preset::PngRgba8SrgbV1, png).issue().code ==
                            Error::DescriptorRelationshipMismatch,
                        "the PNG target window is always its zero-origin implicit canvas");

    png = makePngReport();
    png[10].targetDescriptor = "kind=id:ocio;revision=id:none";
    expectations.expect(validate(Preset::PngRgba8SrgbV1, png).issue().code ==
                            Error::DescriptorVocabularyMismatch,
                        "the PNG dependency descriptor requires an exact lowercase revision");
}

void testExactPixelAspectRounding(Expectations& expectations) {
    auto exr = makeExrReport();
    exr[7].state = State::Approximated;
    exr[7].stableCode = Code::ExrParRoundedBinary32;
    exr[7].sourceDescriptor = "denominator=u:3;numerator=u:1";
    exr[7].targetDescriptor = "value=f32:3eaaaaab";
    const auto rounded = validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr);
    expectations.expect(rounded && rounded.value() != nullptr && rounded.value()->approvable(),
                        "EXR pixel aspect uses deterministic round-to-nearest-even binary32 bits");

    exr[7].targetDescriptor = "value=f32:3eaaaaaa";
    expectations.expect(validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr).issue().code ==
                            Error::DescriptorRelationshipMismatch,
                        "a caller cannot approve arbitrary approximate pixel-aspect bits");

    exr = makeExrReport();
    exr[7].state = State::Approximated;
    exr[7].stableCode = Code::ExrParRoundedBinary32;
    expectations.expect(validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr).issue().code ==
                            Error::DescriptorRelationshipMismatch,
                        "an exactly representable pixel aspect cannot be labeled approximated");
}

void testWindowPermissionTruth(Expectations& expectations) {
    auto exr = makeExrReport();
    constexpr std::string_view widePixels =
        "height=u:1;packing=id:rgba;sample-type=id:binary32;width=u:2";
    constexpr std::string_view outOfRangeWindow =
        "height=u:1;origin-x=i:2147483647;origin-y=i:0;width=u:2";
    exr[0].sourceDescriptor = widePixels;
    exr[0].targetDescriptor = widePixels;
    exr[5].sourceDescriptor = outOfRangeWindow;
    exr[5].targetDescriptor = outOfRangeWindow;
    expectations.expect(validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr).issue().code ==
                            Error::DescriptorRelationshipMismatch,
                        "EXR Exact cannot hide an inclusive bound beyond signed 32-bit");
    exr[5].state = State::Unsupported;
    exr[5].stableCode = Code::WindowOutOfRange;
    const auto rejectedWindow = validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr);
    expectations.expect(rejectedWindow && rejectedWindow.value() != nullptr &&
                            !rejectedWindow.value()->approvable(),
                        "EXR signed-window overflow derives the non-permitted range code");

    constexpr std::string_view maximumWidthPixels =
        "height=u:1;packing=id:rgba;sample-type=id:binary32;width=u:4294967295";
    constexpr std::string_view signedBoundaryWindow =
        "height=u:1;origin-x=i:-2147483648;origin-y=i:0;width=u:4294967295";
    exr = makeExrReport();
    exr[0].sourceDescriptor = maximumWidthPixels;
    exr[0].targetDescriptor = maximumWidthPixels;
    exr[5].sourceDescriptor = signedBoundaryWindow;
    exr[5].targetDescriptor = signedBoundaryWindow;
    expectations.expect(validate(Preset::FlatExrRgba32fLinRec709SceneV1, exr).hasValue(),
                        "the full inclusive signed-32 EXR boundary remains exactly representable");

    auto png = makePngReport();
    constexpr std::string_view offsetDataWindow = "height=u:1;origin-x=i:1;origin-y=i:0;width=u:1";
    png[5].sourceDescriptor = offsetDataWindow;
    expectations.expect(
        validate(Preset::PngRgba8SrgbV1, png).issue().code == Error::DescriptorRelationshipMismatch,
        "PNG data-window Exact is valid only when source equals the implicit target");
    png[5].state = State::Unsupported;
    png[5].stableCode = Code::PngOriginWindowRequired;
    expectations.expect(validate(Preset::PngRgba8SrgbV1, png).hasValue(),
                        "a differing PNG source data window requires the origin code");

    png = makePngReport();
    png[5].state = State::Unsupported;
    png[5].stableCode = Code::PngOriginWindowRequired;
    expectations.expect(validate(Preset::PngRgba8SrgbV1, png).issue().code ==
                            Error::DescriptorRelationshipMismatch,
                        "the PNG origin code cannot be attached to an equal window");

    constexpr std::string_view pngOversizeSource =
        "height=u:1;packing=id:rgba;sample-type=id:binary32;width=u:2147483648";
    constexpr std::string_view pngOversizeTarget =
        "height=u:1;packing=id:rgba;sample-type=id:uint8;width=u:2147483648";
    constexpr std::string_view pngOversizeWindow =
        "height=u:1;origin-x=i:0;origin-y=i:0;width=u:2147483648";
    png = makePngReport();
    png[0].sourceDescriptor = pngOversizeSource;
    png[0].targetDescriptor = pngOversizeTarget;
    png[5].sourceDescriptor = pngOversizeWindow;
    png[5].targetDescriptor = pngOversizeWindow;
    png[5].state = State::Unsupported;
    png[5].stableCode = Code::WindowOutOfRange;
    png[6].sourceDescriptor = pngOversizeWindow;
    png[6].targetDescriptor = pngOversizeWindow;
    png[6].state = State::Unsupported;
    png[6].stableCode = Code::WindowOutOfRange;
    const auto oversizedPng = validate(Preset::PngRgba8SrgbV1, png);
    expectations.expect(
        oversizedPng && oversizedPng.value() != nullptr && !oversizedPng.value()->approvable(),
        "PNG dimensions beyond its signed maximum require range failures on both windows");

    png[5].sourceDescriptor = "height=u:1;origin-x=i:1;origin-y=i:0;width=u:2147483648";
    png[5].stableCode = Code::PngOriginWindowRequired;
    expectations.expect(validate(Preset::PngRgba8SrgbV1, png).issue().code ==
                            Error::DescriptorRelationshipMismatch,
                        "PNG dimension overflow takes precedence over the origin mismatch code");
}

} // namespace

int main() {
    Expectations expectations;
    testPresetIdentityAndSchemaMatrix(expectations);
    testStableCodeMapping(expectations);
    testValidReportsAndDerivedPermissions(expectations);
    testClosedReportStructure(expectations);
    testDescriptorValidationAndBounds(expectations);
    testPresetVocabularyAndRelationships(expectations);
    testExactPixelAspectRounding(expectations);
    testWindowPermissionTruth(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
