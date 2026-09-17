#include <algorithm>
#include <array>
#include <bloom/document/graph.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/runtime/compiled_plan_cache.hpp>
#include <bloom/runtime/composition_time.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace bloom;
using namespace document;
void require(const bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <typename T> T checked(std::optional<T> value) {
    if (!value)
        throw std::runtime_error("invalid fixture value");
    return *value;
}
core::RationalTime time(const std::int64_t numerator, const std::int64_t denominator = 1) {
    return checked(core::RationalTime::create(numerator, denominator));
}
ParameterId parameter(const Composition& composition, const NodeId node,
                      const std::string_view role) {
    for (const auto& binding : composition.graph().findNode(node)->parameters)
        if (binding.role == role)
            return binding.parameterId;
    throw std::runtime_error("missing fixture parameter");
}
void set(Composition& composition, const NodeId node, const std::string_view role,
         ParameterValue value) {
    require(composition.parameters().setSource(parameter(composition, node, role),
                                               ConstantValueSource{std::move(value)}),
            "set fixture constant");
}
NodeId add(Composition& composition, const std::string_view type, const std::uint64_t local) {
    const auto raw = composition.id().value() * 100 + local;
    const auto node = NodeId::fromRaw(raw);
    const auto& registry = builtInNodeDefinitions();
    const auto found = std::ranges::find(
        registry.definitions(), type, [](const auto& definition) { return definition.key.typeId; });
    require(found != registry.definitions().end(), "fixture definition exists");
    NodeRecord record{node, std::string(type), {}, found->key.schemaVersion};
    std::uint64_t index = 0;
    for (const auto& schema : found->parameters) {
        const auto id = ParameterId::fromRaw(raw * 100 + index++);
        require(composition.parameters().insert(
                    {id, schema.schemaKey, ConstantValueSource{schema.defaultValue}}),
                "fixture parameter");
        record.parameters.push_back({schema.role, id});
    }
    require(composition.graph().addNode(std::move(record)), "fixture node");
    return node;
}
Composition makeComposition(const std::uint64_t id, const bool source) {
    Composition composition(
        CompositionId::fromRaw(id), "Composition", time(2), CanonicalGraph(NodeId{}),
        checked(CompositionFormat::create(8, 8, core::PixelAspectRatio::square(),
                                          checked(FrameRate::create(2, 1)))));
    const auto output = add(composition, kCompositionOutputNodeType, 1);
    const auto node =
        add(composition, source ? kCompositionSourceNodeType : kSolidSourceNodeType, 2);
    if (source)
        set(composition, node, "composition", std::int64_t{1});
    else {
        set(composition, node, "width", 2.0);
        set(composition, node, "height", 4.0);
        const auto curve = AnimationCurveId::fromRaw(1);
        require(
            composition.animationCurves().insert(ScalarAnimationCurve{
                curve,
                {{KeyframeId::fromRaw(1), time(0), 1.0}, {KeyframeId::fromRaw(2), time(2), 7.0}}}),
            "fixture curve");
        require(composition.parameters().setSource(parameter(composition, node, "width"),
                                                   AnimationCurveSource{curve}),
                "set fixture animation");
    }
    require(composition.graph().addEdge(
                {EdgeId::fromRaw(id * 100 + 1), {node, "image"}, NodeInputRef{output, "image"}}),
            "fixture image edge");
    composition.graph().setCompositionOutput({output, "image"});
    return composition;
}
runtime::EvaluationResult
evaluate(runtime::CpuCompositionEvaluator& evaluator,
         const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
         const core::RationalTime at) {
    return evaluator.evaluate(plan,
                              {at, plan->output(), runtime::CompositionFormatResolution{},
                               runtime::EvaluationQuality::Reference,
                               runtime::EvaluationColorIntent::LinearRec709Scene,
                               std::size_t{1024} * 1024},
                              {});
}
void connectAudio(Composition& composition, const NodeId source) {
    const auto base = composition.id().value() * 100;
    require(composition.graph().addEdge({EdgeId::fromRaw(base + 2),
                                         {source, "audio"},
                                         NodeInputRef{NodeId::fromRaw(base + 1), "audio"}}),
            "fixture audio output");
}
void addAudio(Composition& composition) {
    const auto audio = add(composition, kAudioSourceNodeType, 3);
    set(composition, audio, "asset", std::string("1"));
    set(composition, audio, "level", 0.5);
    connectAudio(composition, audio);
}
Project audioProject(const bool audible = true) {
    auto inner = makeComposition(1, false);
    if (audible)
        addAudio(inner);
    auto outer = makeComposition(2, true);
    require(outer.setDuration(time(8)), "extend outer audio duration");
    connectAudio(outer, NodeId::fromRaw(202));
    Project project(ProjectId::fromRaw(1), "Audio playback nesting");
    AssetRecord asset;
    asset.id = AssetId::fromRaw(1);
    asset.kind = AssetKind::Audio;
    asset.locator = {"file", "project-relative", "tone.wav", "file:tone.wav"};
    asset.rate = 48000;
    asset.channels = 1;
    asset.frames = 96000;
    asset.duration = time(2);
    require(project.addAsset(asset) && project.addComposition(std::move(inner)) &&
                project.addComposition(std::move(outer)),
            "audio playback project");
    return project;
}
std::optional<runtime::AudioMixDescription>
audioMix(const Snapshot& snapshot, const std::uint64_t composition, const core::RationalTime at) {
    const auto compiled = runtime::SnapshotCompiler(builtInNodeDefinitions())
                              .compile({snapshot, CompositionId::fromRaw(composition)}, {});
    require(compiled.plan != nullptr, "audio playback composition compiles");
    return runtime::CpuCompositionEvaluator{}.evaluateAudioMix(compiled.plan, at);
}
std::shared_ptr<const media::audio::AudioBuffer> audioBuffer() {
    // Distinct values at each 8 Hz output sample, with a valid 48 kHz source descriptor.
    std::vector<float> samples(96000);
    for (std::size_t index = 0; index < 16; ++index)
        std::ranges::fill(std::span(samples).subspan(index * 6000, 6000),
                          static_cast<float>(index + 1));
    return std::make_shared<const media::audio::AudioBuffer>(
        media::audio::AudioBuffer{48000, 1, samples.size(), {std::move(samples)}});
}
class AudioRenderer final {
  public:
    AudioRenderer() : AudioRenderer(std::make_unique<media::audio::playback::NullBackend>()) {}

    std::vector<float> render(const std::optional<runtime::AudioMixDescription>& mix,
                              const core::RationalTime at, const std::size_t count) {
        std::vector<media::audio::playback::AudioClip> clips;
        if (mix) {
            for (const auto& description : mix->clips) {
                clips.push_back({buffer_, description.startTime,
                                 static_cast<float>(description.level), description.muted,
                                 description.solo, description.endTime});
                if (!description.timeMappings.empty())
                    clips.back().mapTime = [description](const core::RationalTime time) {
                        return runtime::mapAudioClipTime(description, time);
                    };
            }
        }
        engine_.replaceClips(std::move(clips));
        backend_->clearCaptured();
        require(!engine_.play(at), "start sample capture");
        require(!engine_.renderForTesting(count), "render audio samples");
        return {backend_->captured().begin(), backend_->captured().end()};
    }

  private:
    explicit AudioRenderer(std::unique_ptr<media::audio::playback::NullBackend> backend)
        : backend_(backend.get()), engine_(std::move(backend), {8, 1, 1024}),
          buffer_(audioBuffer()) {}

    media::audio::playback::NullBackend* backend_;
    media::audio::playback::AudioEngine engine_;
    std::shared_ptr<const media::audio::AudioBuffer> buffer_;
};
void mappedPixels() {
    for (const auto loop : {0, 1, 2})
        require(runtime::mapCompositionTime(time(1, 4), 0, 1, time(1, 2),
                                            checked(FrameRate::create(2, 1)), loop) == time(0),
                "a one-frame source holds its only frame");
    struct Case {
        double offset, scale;
        std::int64_t loop;
        core::RationalTime parent, nested;
    };
    const std::array cases{Case{0, 1, 0, time(1, 2), time(1, 2)},
                           Case{0.25, 2, 0, time(3, 4), time(1)},
                           Case{0, 1, 0, time(7), time(3, 2)},
                           Case{0, 1, 1, time(9, 4), time(1, 4)},
                           Case{0, 1, 2, time(2), time(1)},
                           Case{1, -1, 0, time(1, 4), time(3, 4)},
                           Case{0, 0, 1, time(8), time(0)},
                           Case{1, 1, 1, time(0), time(0)},
                           Case{0.1, 1.1, 0, time(3, 10), time(11, 50)}};
    for (const auto& test : cases) {
        auto outer = makeComposition(2, true);
        const auto node = NodeId::fromRaw(202);
        set(outer, node, "timeOffset", test.offset);
        set(outer, node, "timeScale", test.scale);
        set(outer, node, "loopMode", test.loop);
        Project project(ProjectId::fromRaw(1), "Nesting");
        require(project.addComposition(makeComposition(1, false)) &&
                    project.addComposition(std::move(outer)),
                "fixture compositions");
        const auto validation = project.validate();
        for (const auto& issue : validation.issues())
            std::cerr << issue.path << ": " << issue.message << '\n';
        Document document(std::move(project));
        runtime::SnapshotCompiler compiler(builtInNodeDefinitions());
        runtime::CompiledPlanCache cache;
        const auto nested =
            cache.compile(compiler, {document.snapshot(), CompositionId::fromRaw(1)}, {});
        const auto parent =
            cache.compile(compiler, {document.snapshot(), CompositionId::fromRaw(2)}, {});
        for (const auto& diagnostic : parent.diagnostics)
            std::cerr << diagnostic.summary << ": " << diagnostic.detail << '\n';
        require(nested.plan && parent.plan, "both compositions compile");
        require(parent.plan->nestedPlans().size() == 1 &&
                    parent.plan->nestedPlans().front() == nested.plan,
                "nested plan is shared through the revision cache");
        require(cache.statistics().compiles == 2 && cache.statistics().hits == 1,
                "nested compilation hits the same cache");
        runtime::CpuCompositionEvaluator evaluator;
        const auto direct = evaluate(evaluator, nested.plan, test.nested);
        const auto composed = evaluate(evaluator, parent.plan, test.parent);
        for (const auto& diagnostic : composed.diagnostics())
            std::cerr << diagnostic.summary << ": " << diagnostic.detail << '\n';
        require(direct.frame() && composed.frame(), "both frames evaluate");
        const auto a = direct.frame()->processImage().pixels();
        const auto b = composed.frame()->processImage().pixels();
        require(a.size_bytes() == b.size_bytes() &&
                    std::memcmp(a.data(), b.data(), a.size_bytes()) == 0,
                "nested pixels equal direct pixels at independently expected mapped time");
        require(direct.frame()->evaluatedBounds()[nested.plan->output().value()].output ==
                    composed.frame()->evaluatedBounds()[parent.plan->output().value()].output,
                "nested evaluated bounds equal direct bounds");
        const auto cached = evaluate(evaluator, parent.plan, test.parent);
        require(cached.frame() && cached.frame()->operationCacheStatistics().misses == 0,
                "nested frames use operation cache");
    }
}
void multipleLevelsAndRevision() {
    auto middle = makeComposition(2, true);
    set(middle, NodeId::fromRaw(202), "timeOffset", 0.25);
    set(middle, NodeId::fromRaw(202), "timeScale", 0.5);
    auto outer = makeComposition(3, true);
    set(outer, NodeId::fromRaw(302), "composition", std::int64_t{2});
    set(outer, NodeId::fromRaw(302), "timeScale", 2.0);
    Project project(ProjectId::fromRaw(1), "Multiple levels");
    require(project.addComposition(makeComposition(1, false)) &&
                project.addComposition(std::move(middle)) &&
                project.addComposition(std::move(outer)),
            "three compositions");
    Document document(std::move(project));
    runtime::SnapshotCompiler compiler(builtInNodeDefinitions());
    runtime::CompiledPlanCache cache;
    const auto before =
        cache.compile(compiler, {document.snapshot(), CompositionId::fromRaw(3)}, {});
    require(before.plan && cache.statistics().compiles == 3, "each nested level compiles once");
    runtime::CpuCompositionEvaluator evaluator;
    const auto first = evaluate(evaluator, before.plan, time(1, 2));
    const auto directPlan =
        cache.compile(compiler, {document.snapshot(), CompositionId::fromRaw(1)}, {});
    require(directPlan.plan != nullptr, "direct inner plan");
    const auto direct = evaluate(evaluator, directPlan.plan, time(3, 8));
    require(first.frame() && direct.frame() &&
                std::ranges::equal(first.frame()->processImage().pixels(),
                                   direct.frame()->processImage().pixels()),
            "two mappings compose to the directly evaluated source time");
    const auto snapshot = document.snapshot();
    auto draft = document.draft(snapshot);
    auto* inner = draft.project().findComposition(CompositionId::fromRaw(1));
    require(inner != nullptr, "inner edit target");
    set(*inner, NodeId::fromRaw(102), "width", 6.0);
    require(inner->animationCurves().erase(AnimationCurveId::fromRaw(1)), "remove replaced curve");
    require(document.commit(snapshot.revision(), std::move(draft)).committed(),
            "edit child at a new revision");
    const auto after =
        cache.compile(compiler, {document.snapshot(), CompositionId::fromRaw(3)}, {});
    require(after.plan && after.plan != before.plan && cache.statistics().compiles == 6,
            "a child edit recompiles all levels at the new revision");
    const auto changed = evaluate(evaluator, after.plan, time(1, 2));
    require(changed.frame() && !std::ranges::equal(first.frame()->processImage().pixels(),
                                                   changed.frame()->processImage().pixels()),
            "a child edit invalidates the parent operation cache");
}
void nestedAudio() {
    auto inner = makeComposition(1, false);
    const auto audio = add(inner, kAudioSourceNodeType, 3);
    set(inner, audio, "asset", std::string("1"));
    set(inner, audio, "level", 0.5);
    require(
        inner.graph().addEdge(
            {EdgeId::fromRaw(102), {audio, "audio"}, NodeInputRef{NodeId::fromRaw(101), "audio"}}),
        "inner audio edge");
    auto outer = makeComposition(2, true);
    const auto source = NodeId::fromRaw(202);
    set(outer, source, "timeOffset", 0.25);
    set(outer, source, "timeScale", 2.0);
    set(outer, source, "loopMode", std::int64_t{1});
    const auto merge = add(outer, kLayerStackNodeType, 3);
    for (std::uint64_t i = 0; i < 2; ++i) {
        const auto node = add(outer, kLayerOutputNodeType, 4 + i);
        const auto layer = LayerId::fromRaw(20 + i);
        const auto slot = LayerSlotId::fromRaw(20 + i);
        require(outer.graph().addLayerOutput({node, layer, "Nested audio", "image"}),
                "outer audio layer");
        require(outer.graph().merge(merge)->append({slot, layer}), "outer audio slot");
        require(outer.graph().addEdge(
                    {EdgeId::fromRaw(220 + i * 2), {source, "image"}, NodeInputRef{node, "image"}}),
                "nested image to layer");
        require(outer.graph().addEdge({EdgeId::fromRaw(221 + i * 2),
                                       {node, "image"},
                                       LayerStackInputRef{merge, slot, "content"}}),
                "nested image to merge");
        require(outer.graph().addEdge(
                    {EdgeId::fromRaw(202 + i * 2), {source, "audio"}, NodeInputRef{node, "audio"}}),
                "nested audio to layer");
        require(outer.graph().addEdge({EdgeId::fromRaw(203 + i * 2),
                                       {node, "audio"},
                                       LayerStackInputRef{merge, slot, "audio"}}),
                "nested audio to merge");
    }
    require(outer.graph().eraseEdge(EdgeId::fromRaw(201)), "replace outer image output");
    require(
        outer.graph().addEdge(
            {EdgeId::fromRaw(201), {merge, "image"}, NodeInputRef{NodeId::fromRaw(201), "image"}}),
        "outer merged image");
    require(
        outer.graph().addEdge(
            {EdgeId::fromRaw(210), {merge, "audio"}, NodeInputRef{NodeId::fromRaw(201), "audio"}}),
        "outer audio output");
    Project project(ProjectId::fromRaw(1), "Audio nesting");
    AssetRecord asset;
    asset.id = AssetId::fromRaw(1);
    asset.kind = AssetKind::Audio;
    asset.locator = {"file", "project-relative", "tone.wav", "file:tone.wav"};
    asset.rate = 48000;
    asset.channels = 1;
    asset.frames = 96000;
    asset.duration = time(2);
    require(project.addAsset(asset) && project.addComposition(std::move(inner)) &&
                project.addComposition(std::move(outer)),
            "audio project");
    const auto validation = project.validate();
    for (const auto& issue : validation.issues())
        std::cerr << issue.path << ": " << issue.message << '\n';
    Document document(std::move(project));
    const auto compiled = runtime::SnapshotCompiler(builtInNodeDefinitions())
                              .compile({document.snapshot(), CompositionId::fromRaw(2)}, {});
    for (const auto& diagnostic : compiled.diagnostics)
        std::cerr << diagnostic.summary << ": " << diagnostic.detail << '\n';
    require(compiled.plan != nullptr, "nested audio compiles");
    const auto mix = runtime::CpuCompositionEvaluator{}.evaluateAudioMix(compiled.plan, time(3, 4));
    require(mix && mix->clips.size() == 2, "two nested layers contribute two summable clips");
    if (!mix)
        throw std::runtime_error("missing audio mix");
    double sum = 0;
    for (const auto& clip : mix->clips) {
        sum += clip.level;
        require(!clip.muted && clip.timeMappings.size() == 1, "nested audio mapping retained");
        require(runtime::mapAudioClipTime(clip, time(3, 4)) == time(1) &&
                    !runtime::mapAudioClipTime(clip, time(2)),
                "nested audio time respects the enclosing layer range");
        const auto& mapping = clip.timeMappings.front();
        require(runtime::mapCompositionTime(time(3, 4), mapping.offset, mapping.scale,
                                            mapping.duration, mapping.frameRate,
                                            mapping.loopMode) == time(1),
                "audio and image map to the same nested time");
    }
    require(sum == 1.0, "nested audio levels sum");
    AudioRenderer renderer;
    require(renderer.render(mix, time(3, 4), 8) == std::vector<float>{9, 11, 13, 15, 1, 3, 5, 7},
            "two nested layers sum actual samples across a scaled loop boundary");
    require(renderer.render(mix, time(15, 8), 3) == std::vector<float>{11, 0, 0},
            "enclosing layer range silences samples at and after its out point");
    auto disabledSolo = compiled.plan->copyDefinition();
    disabledSolo.audioMix.nestedLayers.back().solo = true;
    disabledSolo.audioMix.nestedLayers.back().enabled = false;
    const auto filtered = runtime::CpuCompositionEvaluator{}.evaluateAudioMix(
        std::make_shared<const runtime::CompiledCompositionPlan>(std::move(disabledSolo)),
        time(3, 4));
    require(filtered && filtered->clips.size() == 2 && !filtered->clips.front().muted &&
                filtered->clips.back().muted,
            "a disabled nested solo layer does not silence another layer");
    require(renderer.render(filtered, time(3, 4), 4) == std::vector<float>{4.5F, 5.5F, 6.5F, 7.5F},
            "disabled solo leaves only the other nested layer audible");
    auto trimmed = *mix;
    for (auto& clip : trimmed.clips)
        clip.timeMappings.front().inPoint = time(1);
    require(renderer.render(trimmed, time(7, 8), 3) == std::vector<float>{0, 13, 15},
            "enclosing layer in point gates samples before mapping");
}
void mappedAudioSamples() {
    // At 8 Hz the two-second source has samples 0..15; its final frame starts at sample 12.
    // These indices are independent expected results, not calculated by the production mapper.
    struct Case {
        double offset, scale;
        std::int64_t loop;
        core::RationalTime parent;
        std::vector<std::int64_t> sourceFrames;
    };
    const std::array cases{Case{0, 1, 0, time(1, 2), {4, 5, 6, 7}},
                           Case{0.25, 2, 0, time(3, 4), {8, 10, 12, 12, 12, 12}},
                           Case{0, 1, 0, time(7), {12, 12, 12, 12}},
                           Case{0, 1, 1, time(7, 4), {14, 15, 0, 1, 2, 3}},
                           Case{0.25, 2, 1, time(3, 4), {8, 10, 12, 14, 0, 2, 4, 6}},
                           Case{0, 0.5, 1, time(1, 2), {2, 2, 3, 3, 4, 4}},
                           Case{0, 1, 2, time(5, 4), {10, 11, 12, 11, 10, 9}},
                           Case{0, 2, 2, time(5, 4), {4, 2, 0, 2, 4, 6}},
                           Case{1, -1, 0, time(1, 4), {6, 5, 4, 3}},
                           Case{0, 0, 1, time(4), {0, 0, 0, 0}},
                           Case{1, 1, 1, time(3, 4), {0, 0, 0, 1}}};
    for (const auto& test : cases) {
        auto project = audioProject();
        auto* outer = project.findComposition(CompositionId::fromRaw(2));
        require(outer != nullptr, "audio mapping target");
        set(*outer, NodeId::fromRaw(202), "timeOffset", test.offset);
        set(*outer, NodeId::fromRaw(202), "timeScale", test.scale);
        set(*outer, NodeId::fromRaw(202), "loopMode", test.loop);
        Document document(std::move(project));
        const auto nested = audioMix(document.snapshot(), 2, test.parent);
        require(nested && nested->clips.size() == 1, "one mapped audio clip");
        AudioRenderer directRenderer;
        std::vector<float> expected;
        for (const auto frame : test.sourceFrames) {
            const auto sourceTime = time(frame, 8);
            const auto direct = audioMix(document.snapshot(), 1, sourceTime);
            const auto sample = directRenderer.render(direct, sourceTime, 1).front();
            require(sample == static_cast<float>(frame + 1) * 0.5F,
                    "direct source sample is audible and distinct");
            expected.push_back(sample);
        }
        require(AudioRenderer{}.render(nested, test.parent, expected.size()) == expected,
                "nested playback equals direct composition samples at expected mapped times");
    }
}
void multipleAudioLevels() {
    auto project = audioProject();
    auto* middle = project.findComposition(CompositionId::fromRaw(2));
    require(middle != nullptr, "middle audio composition");
    set(*middle, NodeId::fromRaw(202), "timeOffset", 0.25);
    set(*middle, NodeId::fromRaw(202), "timeScale", 0.5);
    auto outer = makeComposition(3, true);
    connectAudio(outer, NodeId::fromRaw(302));
    set(outer, NodeId::fromRaw(302), "composition", std::int64_t{2});
    set(outer, NodeId::fromRaw(302), "timeScale", 2.0);
    require(project.addComposition(std::move(outer)), "third audio composition");
    Document document(std::move(project));
    const auto mix = audioMix(document.snapshot(), 3, time(1, 2));
    require(mix && mix->clips.size() == 1 && mix->clips.front().timeMappings.size() == 2,
            "audio retains both enclosing mappings");
    AudioRenderer renderer;
    require(renderer.render(mix, time(1, 2), 4) == std::vector<float>{2, 2.5F, 3, 3.5F},
            "audio applies outer mapping before inner mapping");
}
void silentAndRejectedAudio() {
    Document audible(audioProject());
    AudioRenderer renderer;
    require(renderer.render(audioMix(audible.snapshot(), 2, time(0)), time(0), 4) ==
                std::vector<float>{0.5F, 1, 1.5F, 2},
            "audible mix primes playback");
    Document silent(audioProject(false));
    const auto snapshot = silent.snapshot();
    const auto mix = audioMix(snapshot, 2, time(0));
    require(mix && mix->clips.empty(), "nested composition without audio contributes no clips");
    require(renderer.render(mix, time(0), 4) == std::vector<float>(4, 0),
            "replacing audible playback with a silent nested composition clears old samples");

    // A cycle cannot enter an immutable snapshot. Adding sound together with the reverse
    // reference must fail publication, leaving the playable snapshot silent.
    auto draft = silent.draft(snapshot);
    auto* inner = draft.project().findComposition(CompositionId::fromRaw(1));
    require(inner != nullptr, "cycle audio target");
    addAudio(*inner);
    const auto reverse = add(*inner, kCompositionSourceNodeType, 4);
    set(*inner, reverse, "composition", std::int64_t{2});
    const auto rejected = silent.commit(snapshot.revision(), std::move(draft));
    require(rejected.status == CommitStatus::InvalidDraft &&
                std::ranges::any_of(rejected.validation.issues(),
                                    [](const auto& issue) {
                                        return issue.code ==
                                               ValidationCode::CompositionNestingCycle;
                                    }),
            "A to B to A audio draft is refused with the typed cycle reason");
    require(silent.snapshot().revision() == snapshot.revision(),
            "cycle rejection preserves the published audio revision");
    require(renderer.render(audioMix(silent.snapshot(), 2, time(0)), time(0), 4) ==
                std::vector<float>(4, 0),
            "cycle-refused composition contributes no audio");
    require(renderer.render(std::nullopt, time(0), 4) == std::vector<float>(4, 0),
            "an unavailable mix is silent");
}
void missingSource() {
    Project project(ProjectId::fromRaw(1), "Missing");
    require(project.addComposition(makeComposition(2, true)), "missing fixture");
    require(project.validate().ok(), "missing reference remains a valid file");
    const auto validation = project.validate();
    for (const auto& issue : validation.issues())
        std::cerr << issue.path << ": " << issue.message << '\n';
    Document document(std::move(project));
    const auto result = runtime::SnapshotCompiler(builtInNodeDefinitions())
                            .compile({document.snapshot(), CompositionId::fromRaw(2)}, {});
    require(!result.plan &&
                std::ranges::any_of(
                    result.diagnostics,
                    [](const auto& issue) {
                        return issue.code == runtime::CompileDiagnosticCode::CompositionNotFound &&
                               issue.subject.nodeId == NodeId::fromRaw(202);
                    }),
            "missing source produces a typed, node-addressed compile diagnostic");
}
} // namespace
int main() {
    try {
        mappedPixels();
        multipleLevelsAndRevision();
        missingSource();
        nestedAudio();
        mappedAudioSamples();
        multipleAudioLevels();
        silentAndRejectedAudio();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
