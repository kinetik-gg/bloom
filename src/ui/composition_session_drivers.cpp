#include "driven_value_resolver.hpp"
#include "node_editor_items.hpp"
#include <algorithm>
#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <deque>
#include <set>
#include <variant>

// Task DRIVE-1. The session's driver-chain readers: what drives a parameter, what the chain behind
// a node contains, and what one driven parameter currently resolves to. Properties grew the first
// two inline before the timeline needed them; they live here now so the two surfaces cannot
// disagree about which node drives a parameter or what its value is this frame.
namespace bloom::ui {
namespace {
template <typename Value>
[[nodiscard]] std::optional<Value> sampledAs(const std::optional<ParameterSample>& sample) {
    if (!sample.has_value()) {
        return std::nullopt;
    }
    const auto* value = std::get_if<Value>(&*sample);
    return value == nullptr ? std::nullopt : std::optional<Value>(*value);
}
} // namespace

std::optional<document::Vec3d>
CompositionSession::effectiveVec3Value(const document::ParameterId parameterId) const {
    const auto* current = composition();
    return sampledAs<document::Vec3d>(effectiveParameterValue(
        current == nullptr ? nullptr : current->parameters().find(parameterId)));
}

std::optional<QString>
CompositionSession::effectiveStringValue(const document::ParameterId parameterId) const {
    const auto* current = composition();
    const auto text = sampledAs<std::string>(effectiveParameterValue(
        current == nullptr ? nullptr : current->parameters().find(parameterId)));
    return text.has_value() ? std::optional(QString::fromStdString(*text)) : std::nullopt;
}

std::optional<std::int64_t>
CompositionSession::effectiveIntegerValue(const document::ParameterId parameterId) const {
    const auto* current = composition();
    return sampledAs<std::int64_t>(effectiveParameterValue(
        current == nullptr ? nullptr : current->parameters().find(parameterId)));
}

std::optional<bool>
CompositionSession::effectiveBooleanValue(const document::ParameterId parameterId) const {
    const auto* current = composition();
    return sampledAs<bool>(effectiveParameterValue(
        current == nullptr ? nullptr : current->parameters().find(parameterId)));
}

const document::DriverBindingSource*
CompositionSession::driverBindingFor(const document::ParameterId parameterId) const noexcept {
    const auto* current = composition();
    const auto* parameter = current == nullptr ? nullptr : current->parameters().find(parameterId);
    return parameter == nullptr ? nullptr
                                : std::get_if<document::DriverBindingSource>(&parameter->source);
}

QString CompositionSession::driverDisplayName(const document::ParameterId parameterId) const {
    const auto* driver = driverBindingFor(parameterId);
    if (driver == nullptr) {
        return {};
    }
    const auto* current = composition();
    const auto* node = current->graph().findNode(driver->sourceNodeId);
    return node == nullptr ? tr("Missing driver") : node_editor::nodeDisplayName(*current, *node);
}

std::vector<UpstreamNode>
CompositionSession::upstreamNodes(const std::span<const document::NodeId> seeds,
                                  const UpstreamTraversal traversal) const {
    std::vector<UpstreamNode> reached;
    const auto* current = composition();
    if (current == nullptr) {
        return reached;
    }
    std::set<document::NodeId> visited;
    std::deque<UpstreamNode> queue;
    for (const auto seed : seeds) {
        if (visited.insert(seed).second) {
            queue.push_back({seed, 0});
        }
    }
    while (!queue.empty()) {
        const auto [id, depth] = queue.front();
        queue.pop_front();
        const auto* node = current->graph().findNode(id);
        if (node == nullptr) {
            continue;
        }
        if (depth > 0) {
            reached.push_back({id, depth});
        }
        const auto* definition =
            document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion);
        if (definition != nullptr &&
            (definition->lowering == document::NodeLoweringKind::LayerStack ||
             definition->lowering == document::NodeLoweringKind::CompositionOutput)) {
            continue;
        }
        const auto enqueue = [&](const document::NodeId upstream) {
            if (visited.insert(upstream).second) {
                queue.push_back({upstream, depth + 1});
            }
        };
        // A node with no parameters at all is a Reroute's pass-through, and only that: it carries
        // someone else's value, so its input is an ordinary edge even on the driver-only walk.
        // Without this a reroute would hide the node behind it from the surface asking.
        if (traversal == UpstreamTraversal::DriverLinksAndInputEdges || node->parameters.empty()) {
            for (const auto& edge : current->graph().edges()) {
                const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
                if (input != nullptr && input->nodeId == id) {
                    enqueue(edge.source.nodeId);
                }
            }
        }
        // A parameter socket is LINKED by its parameter's own driver binding rather than by an
        // edge -- CanonicalGraph::validate() refuses an edge into one -- so every walk follows
        // these, whichever traversal it is.
        for (const auto& binding : node->parameters) {
            if (const auto* driver = driverBindingFor(binding.parameterId)) {
                enqueue(driver->sourceNodeId);
            }
        }
    }
    return reached;
}

QString CompositionSession::drivenValueText(const document::ParameterId parameterId) const {
    const auto found = drivenText_.find(parameterId);
    return found == drivenText_.end() ? QString{} : found->second;
}

void CompositionSession::refreshDrivenValues() {
    const auto* current = composition();
    std::vector<document::ParameterId> driven;
    if (current != nullptr) {
        for (const auto& parameter : current->parameters().records()) {
            if (std::holds_alternative<document::DriverBindingSource>(parameter.source)) {
                driven.push_back(parameter.id);
            }
        }
    }
    // The signature is what the answer would depend on: the document it is read from, the instant
    // it is read at, and the parameters asked about. An identical request starts nothing, which is
    // what lets a row call this from its own refresh without the answer's signal looping back.
    auto signature = QString("%1/%2/%3")
                         .arg(snapshot().revision().value())
                         .arg(currentTime().numerator())
                         .arg(currentTime().denominator());
    for (const auto id : driven) {
        signature += QString("/%1").arg(id.value());
    }
    if (signature == drivenRequest_) {
        return;
    }
    drivenRequest_ = signature;
    if (driven.empty()) {
        if (!drivenText_.empty()) {
            drivenText_.clear();
            Q_EMIT drivenValuesChanged();
        }
        return;
    }
    if (drivenValues_ == nullptr) {
        drivenValues_ = new DrivenValueResolver(*this, this);
        drivenValues_->ready = [this](const DrivenValueResolver::Values& values) {
            drivenText_ = values;
            Q_EMIT drivenValuesChanged();
        };
    }
    drivenValues_->request(std::move(driven));
}
} // namespace bloom::ui
