#pragma once

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/scripting/session.hpp>

#include <optional>
#include <span>

namespace bloom::scripting {

class Query final {
  public:
    explicit Query(const Session& session) : session_(session), snapshot_(session.snapshot()) {}

    [[nodiscard]] document::Snapshot snapshot() const {
        snapshot_ = session_.snapshot();
        return snapshot_;
    }
    [[nodiscard]] std::span<const document::Composition> compositions() const {
        snapshot_ = session_.snapshot();
        return snapshot_.project().compositions();
    }
    [[nodiscard]] const document::Composition*
    composition(document::CompositionId id) const noexcept {
        snapshot_ = session_.snapshot();
        return snapshot_.project().findComposition(id);
    }
    [[nodiscard]] const document::CanonicalGraph* graph(document::CompositionId id) const noexcept {
        const auto* value = composition(id);
        return value == nullptr ? nullptr : &value->graph();
    }
    [[nodiscard]] const document::ParameterStore*
    parameters(document::CompositionId id) const noexcept {
        const auto* value = composition(id);
        return value == nullptr ? nullptr : &value->parameters();
    }
    [[nodiscard]] const document::AnimationCurveStore*
    curves(document::CompositionId id) const noexcept {
        const auto* value = composition(id);
        return value == nullptr ? nullptr : &value->animationCurves();
    }
    [[nodiscard]] std::span<const document::AssetRecord> assets() const {
        snapshot_ = session_.snapshot();
        return snapshot_.project().assets();
    }
    [[nodiscard]] std::span<const document::NodeDefinition> nodeDefinitions() const noexcept {
        return document::builtInNodeDefinitions().definitions();
    }

  private:
    const Session& session_;
    mutable document::Snapshot snapshot_;
};

} // namespace bloom::scripting
