#pragma once

// Runtime mutation proof for the built-in node-type coverage. Registering a new node type against
// an existing pixel lowering must gain its own required `node.<typeId>` id, with no fixture, so it
// cannot silently reuse another node's fixture. This is the runtime half of the mutation gate; the
// compile-time operation/effect/feature probes are separate translation units.

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/runtime/gpu_coverage_contract.hpp>

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace bloom::gpu_coverage_mutation {

inline void nodeTypeMutationProof(const std::vector<std::string>& fixtureIds,
                                  std::vector<std::string>& failures) {
    using bloom::document::NodeDefinition;
    using bloom::document::NodeDefinitionRegistry;
    using bloom::document::NodeRegistrationStatus;
    const auto* solid = bloom::document::builtInNodeDefinitions().find(
        bloom::document::kSolidSourceNodeType, bloom::document::kSolidSourceNodeSchemaVersion);
    if (solid == nullptr) {
        failures.emplace_back("node-type mutation proof: built-in solid definition missing");
        return;
    }
    NodeDefinition synthetic = *solid;
    synthetic.key.typeId = "test.synthetic-image-node";
    NodeDefinitionRegistry registry;
    if (registry.registerDefinition(std::move(synthetic)) != NodeRegistrationStatus::Registered) {
        failures.emplace_back("node-type mutation proof: synthetic definition was rejected");
        return;
    }
    registry.freeze();
    bool found = false;
    for (const auto& entry : bloom::runtime::gpuNodeTypeCoverage(registry)) {
        found = found || entry.id == "node.test.synthetic-image-node";
    }
    if (!found) {
        failures.emplace_back(
            "node-type mutation proof: a new node type escaped node-type coverage");
        return;
    }
    for (const auto& id : fixtureIds) {
        if (id == "node.test.synthetic-image-node") {
            failures.emplace_back("node-type mutation proof: synthetic node unexpectedly fixtured");
            return;
        }
    }
    std::cout << "MUTATION node-type: a new node type sharing the Solid lowering gains a required "
                 "id with no fixture (reported RED)\n";
}

} // namespace bloom::gpu_coverage_mutation
