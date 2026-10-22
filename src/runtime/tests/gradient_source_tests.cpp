#include "gradient_source.hpp"

#include <bloom/document/graph.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <cmath>
#include <iostream>

namespace {
bool near(const float actual, const float expected) {
    return std::fabs(actual - expected) < 0.00001F;
}
}

int main() {
    using bloom::runtime::detail::ResolvedGradient;
    using bloom::runtime::detail::gradientPixel;
    using namespace bloom::document;
    const auto& definitions = builtInNodeDefinitions();
    const auto* linear = definitions.find(kLinearGradientNodeType, kGradientNodeSchemaVersion);
    const auto* radial = definitions.find(kRadialGradientNodeType, kGradientNodeSchemaVersion);
    if (!linear || !radial || linear->lowering != NodeLoweringKind::Gradient ||
        radial->lowering != NodeLoweringKind::Gradient || linear->parameters.size() != 7 ||
        radial->parameters.size() != 7) {
        std::cerr << "gradient node schemas are not registered\n";
        return 1;
    }
    ResolvedGradient gradient{{-1, 0, 2, 1}, {3, 0, -2, 0}, {0, 0}, {4, 0}, 0, false, 1};
    const auto first = gradientPixel(gradient, 0.5, 0.5);
    const auto last = gradientPixel(gradient, 3.5, 0.5);
    if (!first || !last || !near(first.value()->red(), -0.4375F) ||
        !near(first.value()->alpha(), 0.875F) || !near(last.value()->red(), 0.3125F) ||
        !near(last.value()->alpha(), 0.125F)) {
        std::cerr << "scene-linear RGB and alpha interpolation changed\n";
        return 1;
    }
    gradient.first = {0, 0, 0, 1};
    gradient.second = {1, 1, 1, 1};
    gradient.repeat = 1;
    const auto repeated = gradientPixel(gradient, 5.0, 0);
    gradient.repeat = 2;
    const auto mirrored = gradientPixel(gradient, 5.0, 0);
    if (!repeated || !mirrored || !near(repeated.value()->red(), 0.25F) ||
        !near(mirrored.value()->red(), 0.75F)) {
        std::cerr << "gradient repeat modes changed\n";
        return 1;
    }
    gradient.repeat = 0;
    gradient.radial = true;
    gradient.aspect = 2;
    const auto aspect = gradientPixel(gradient, 0, 4);
    if (!aspect || !near(aspect.value()->red(), 0.5F)) {
        std::cerr << "radial pixel-aspect geometry changed\n";
        return 1;
    }
    gradient.end = gradient.start;
    const auto degenerate = gradientPixel(gradient, 100, 100);
    if (!degenerate || !near(degenerate.value()->red(), 0)) {
        std::cerr << "degenerate gradient must choose first colour\n";
        return 1;
    }
    gradient.aspect = 1;
    for (const bool radialMode : {false, true}) {
        gradient.radial = radialMode;
        gradient.start = {-1e200, 0};
        gradient.end = {1e200, 0};
        const auto large = gradientPixel(gradient, 0.5, 0.5);
        gradient.start = {0, 0};
        gradient.end = {1e-200, 0};
        const auto tiny = gradientPixel(gradient, 0.5, 0.5);
        if (!large || !tiny || !near(large.value()->red(), 0.5F) ||
            !near(tiny.value()->red(), 1.0F)) {
            std::cerr << "nonzero extreme gradient spans must not become degenerate\n";
            return 1;
        }
    }
    return 0;
}
