// ADR 0024 layer-owned time: the mapping core, the compiler's clock placement, and the CPU
// evaluator's per-clock operands, on a real compiled document. Content is a Solid whose width is
// animated 1 -> 7 over [0, 2) s, so the covered width of a row reads back the content time.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/composition_time.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace bloom;
namespace {
int failures = 0;
void expect(const bool value, const char* message) {
    if (!value) {
        ++failures;
        std::cerr << message << '\n';
    }
}
core::RationalTime at(const std::int64_t numerator, const std::int64_t denominator = 1) {
    const auto value = core::RationalTime::create(numerator, denominator);
    if (!value)
        throw std::logic_error("invalid fixture time");
    return *value;
}
document::FrameRate rate(const std::uint32_t fps) { return *document::FrameRate::create(fps, 1); }

void mappingCore() {
    using document::LayerTimeMode;
    const runtime::TimeExtent extent{at(1), rate(4)}; // frames at 0, 1/4, 1/2, 3/4
    const auto wrap = [&](const core::RationalTime time, const LayerTimeMode before,
                          const LayerTimeMode after) {
        return runtime::wrapToExtent(time, extent, before, after);
    };
    expect(!wrap(at(-1, 4), LayerTimeMode::None, LayerTimeMode::None)->present,
           "None before the extent is absent");
    expect(!wrap(at(1), LayerTimeMode::None, LayerTimeMode::None)->present,
           "None at the extent end is absent");
    expect(wrap(at(1, 2), LayerTimeMode::None, LayerTimeMode::None)->time == at(1, 2),
           "times inside the extent pass through");
    expect(wrap(at(-1, 4), LayerTimeMode::Hold, LayerTimeMode::None)->time == at(0),
           "Hold before the extent holds the first frame");
    expect(wrap(at(3), LayerTimeMode::None, LayerTimeMode::Hold)->time == at(3, 4),
           "Hold after the extent holds the last frame start");
    expect(wrap(at(5, 4), LayerTimeMode::None, LayerTimeMode::Loop)->time == at(1, 4),
           "Loop wraps at the extent duration");
    expect(wrap(at(-1, 4), LayerTimeMode::Loop, LayerTimeMode::None)->time == at(3, 4),
           "Loop before the extent wraps to a non-negative time");
    expect(wrap(at(1), LayerTimeMode::None, LayerTimeMode::PingPong)->time == at(1, 2),
           "PingPong reflects around the last frame start");
    expect(wrap(at(-1, 4), LayerTimeMode::PingPong, LayerTimeMode::None)->time == at(1, 4),
           "PingPong reflects before the extent too");
    expect(runtime::mapLayerTime(at(3), at(1), *document::LayerSpeed::create(1, 2)) == at(1),
           "layer time is (t - start) * speed exactly");
    expect(runtime::secondsToTime(0.25) == at(1, 4), "binary seconds convert exactly");
    // mapCompositionTime keeps its historical rule through the shared core.
    for (const std::int64_t loop : {0, 1, 2})
        expect(runtime::mapCompositionTime(at(-1), 0, 1, at(1), rate(4), loop) == at(0),
               "composition sources hold their first frame before zero");
    expect(runtime::mapCompositionTime(at(5, 4), 0, 1, at(1), rate(4), 1) == at(1, 4),
           "composition source Loop is unchanged");
    expect(runtime::mapCompositionTime(at(7, 8), 0, 1, at(1), rate(4), 0) == at(3, 4),
           "composition source Hold clamps inside the last frame as before");
}

struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Layer time", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(16, 4, core::PixelAspectRatio::square(), rate(24)));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::LayerId layer;
    document::NodeId solid;
    runtime::CpuCompositionEvaluator evaluator;

    Fixture() {
        commands::Transaction add("Add solid", document.snapshot().revision());
        add.emplace<commands::AddSolidLayer>(initial.initialCompositionId, "Solid",
                                             core::Color4d{1, 1, 1, 1},
                                             document::Vec2d{8, 2});
        const auto result = commands.execute(std::move(add));
        if (!result.changed())
            throw std::runtime_error("solid creation failed");
        layer = *result.outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
        solid = *result.outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput);
        const auto width = parameter("width");
        commands::Transaction animate("Animate width", document.snapshot().revision());
        animate.emplace<commands::SetParameterSource>(initial.initialCompositionId, width,
                                                      document::ConstantValueSource{1.0});
        animate.emplace<commands::CreateAnimationForParameter>(initial.initialCompositionId,
                                                               width, at(0));
        animate.emplace<commands::SetKeyframeAtTimeForParameter>(initial.initialCompositionId,
                                                                 width, at(2), 13.0);
        if (!commands.execute(std::move(animate)).succeeded())
            throw std::runtime_error("width animation failed");
    }
    document::ParameterId parameter(const std::string_view role) const {
        const auto snapshot = document.snapshot();
        const auto* node = snapshot.project()
                               .findComposition(initial.initialCompositionId)
                               ->graph()
                               .findNode(solid);
        for (const auto& binding : node->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("missing solid parameter");
    }
    void setTiming(const document::LayerTiming& timing, const core::RationalTime in = {},
                   const core::RationalTime out = {}) {
        const auto base = document.snapshot();
        auto draft = document.draft(base);
        auto* boundary =
            draft.project().findComposition(initial.initialCompositionId)->graph().findLayer(layer);
        boundary->timing = timing;
        boundary->inPoint = in;
        boundary->outPoint = out;
        if (!document.commit(base.revision(), std::move(draft)).committed())
            throw std::runtime_error("timing commit refused");
    }
    std::shared_ptr<const runtime::CompiledCompositionPlan> compile() const {
        runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled =
            compiler.compile({document.snapshot(), initial.initialCompositionId}, {});
        if (!compiled.plan) {
            for (const auto& diagnostic : compiled.diagnostics)
                std::cerr << diagnostic.summary << '\n';
            throw std::runtime_error("layer time compilation failed");
        }
        return compiled.plan;
    }
    // Opaque pixel count of row 2: the solid's covered width at the content time.
    int covered(const core::RationalTime time) {
        const auto plan = compile();
        const auto result =
            evaluator.evaluate(plan,
                               {time, plan->output(), runtime::CompositionFormatResolution{},
                                runtime::EvaluationQuality::Reference,
                                runtime::EvaluationColorIntent::LinearRec709Scene, 1U << 20U},
                               {});
        if (!result.frame()) {
            for (const auto& diagnostic : result.diagnostics())
                std::cerr << diagnostic.summary << '\n';
            ++failures;
            return -1;
        }
        const auto pixels = result.frame()->processImage().pixels();
        int count = 0;
        for (std::size_t x = 0; x < 16; ++x)
            if (pixels[2 * 16 + x].alpha() > 0.5F)
                ++count;
        return count;
    }
};

void evaluation() {
    Fixture fixture;
    expect(fixture.compile()->clocks().empty(), "identity timing compiles without clocks");
    const auto identityHalf = fixture.covered(at(1, 2));
    const auto identityOne = fixture.covered(at(1));
    expect(identityHalf > 0 && identityHalf < identityOne, "fixture animation is visible");

    document::LayerTiming moved;
    moved.startTime = at(1, 2);
    fixture.setTiming(moved);
    expect(!fixture.compile()->clocks().empty(), "a moved layer compiles onto layer clocks");
    expect(fixture.covered(at(1)) == identityHalf,
           "moving the start time moves the content and its keys");

    document::LayerTiming fast;
    fast.speed = *document::LayerSpeed::create(2, 1);
    fixture.setTiming(fast);
    expect(fixture.covered(at(1, 2)) == identityOne, "speed 2 plays the content twice as fast");

    document::LayerTiming bounded;
    bounded.contentExtent = document::FixedLayerExtent{at(1, 2), rate(24)};
    bounded.afterMode = document::LayerTimeMode::None;
    fixture.setTiming(bounded);
    expect(fixture.covered(at(1)) == 0, "None after the content extent is transparent");
    bounded.afterMode = document::LayerTimeMode::Loop;
    fixture.setTiming(bounded);
    const auto quarter = fixture.covered(at(1, 4));
    expect(fixture.covered(at(3, 4)) == quarter, "Loop repeats the content extent");
    bounded.afterMode = document::LayerTimeMode::Hold;
    fixture.setTiming(bounded);
    expect(fixture.covered(at(2)) == fixture.covered(at(11, 24)),
           "Hold shows the last content frame");

    // A layer that starts before composition time zero is valid.
    document::LayerTiming early;
    early.startTime = at(-1);
    fixture.setTiming(early, at(-1), at(2));
    expect(fixture.covered(at(0)) == identityOne, "a range may start before composition zero");
}
// Audio follows exactly the picture mapping: a 1 s tone asset placed at start 2 s plays content
// time t - 2 inside its range and is silent after its content extent (after mode None).
void audio() {
    auto initial = document::makeNewProject(
        "Audio time", "Main", core::RationalTime::fromInteger(8),
        *document::CompositionFormat::create(4, 4, core::PixelAspectRatio::square(), rate(24)));
    {
        document::AssetRecord tone;
        tone.id = document::AssetId::fromRaw(9000);
        tone.kind = document::AssetKind::Audio;
        tone.locator = {"file", "project-relative", "tone.wav", "file:tone.wav"};
        tone.rate = 48000;
        tone.channels = 1;
        tone.frames = 48000;
        tone.duration = core::RationalTime::fromInteger(1);
        if (!initial.project.addAsset(tone))
            throw std::runtime_error("audio asset");
    }
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    commands::Transaction add("Add audio", document.snapshot().revision());
    add.emplace<commands::AddAudioLayer>(initial.initialCompositionId,
                                         document::AssetId::fromRaw(9000));
    if (!commands.execute(std::move(add)).changed())
        throw std::runtime_error("audio layer");
    const auto layerId =
        document.snapshot().project().findComposition(initial.initialCompositionId)
            ->graph()
            .layerOutputs()
            .back()
            .layerId;
    {
        const auto* layer = document.snapshot()
                                .project()
                                .findComposition(initial.initialCompositionId)
                                ->graph()
                                .findLayer(layerId);
        expect(layer->endPoint(core::RationalTime::fromInteger(8)) == at(1),
               "a new audio layer ends at its asset length");
    }
    commands::Transaction move("Move", document.snapshot().revision());
    move.emplace<commands::SetLayerTiming>(initial.initialCompositionId, layerId, at(2), at(2),
                                           at(5));
    if (!commands.execute(std::move(move)).changed())
        throw std::runtime_error("audio move");
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({document.snapshot(), initial.initialCompositionId}, {});
    if (!compiled.plan)
        throw std::runtime_error("audio compile");
    const auto mix = runtime::CpuCompositionEvaluator{}.evaluateAudioMix(compiled.plan, at(0));
    expect(mix && mix->clips.size() == 1 && !mix->clips.front().timeMappings.empty(),
           "a retimed audio layer carries its layer mapping");
    if (!mix || mix->clips.empty())
        return;
    const auto& clip = mix->clips.front();
    expect(!runtime::mapAudioClipTime(clip, at(19, 10)), "silent before the layer range");
    expect(runtime::mapAudioClipTime(clip, at(5, 2)) == at(1, 2),
           "inside the range the content time is t - start");
    expect(!runtime::mapAudioClipTime(clip, at(7, 2)),
           "silent after the content extent (after mode None), though the range continues");
}
// An identity Time Remap (two Linear keys over the range) shows exactly the same frames: its
// binary64 seconds are snapped onto frame starts within a nanosecond.
void identityRemap() {
    Fixture fixture;
    std::vector<int> plain;
    for (std::int64_t frame = 0; frame < 48; ++frame)
        plain.push_back(fixture.covered(at(frame, 24)));
    commands::Transaction enable("Remap", fixture.document.snapshot().revision());
    enable.emplace<commands::EnableLayerTimeRemap>(fixture.initial.initialCompositionId,
                                                   fixture.layer);
    expect(fixture.commands.execute(std::move(enable)).changed(), "Time Remap enables");
    bool same = true;
    for (std::int64_t frame = 0; frame < 48; ++frame)
        same = same && fixture.covered(at(frame, 24)) == plain[static_cast<std::size_t>(frame)];
    expect(same, "an identity Time Remap selects exactly the same frames");
    commands::Transaction disable("Remap", fixture.document.snapshot().revision());
    disable.emplace<commands::DisableLayerTimeRemap>(fixture.initial.initialCompositionId,
                                                     fixture.layer);
    expect(fixture.commands.execute(std::move(disable)).changed(), "Time Remap disables");
    expect(fixture.covered(at(30, 24)) == plain[30], "disabling restores plain playback");
}
} // namespace

int main() {
    try {
        mappingCore();
        evaluation();
        audio();
        identityRemap();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
