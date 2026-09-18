#include <bloom/document/data_block.hpp>
#include <bloom/runtime/value_utility_kernels.hpp>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {
using namespace bloom;

int run() {
    document::DataBlockRecord curve;
    curve.id = document::DataBlockRecordId::fromRaw(1);
    curve.kind = document::DataBlockKind::Curve;
    curve.payload = document::DataBlockCurve{0.0, 1.0, {0.0, 4.0, 8.0}};

    runtime::CompiledValue time = 0.25;
    std::vector<const runtime::CompiledValue*> operands{&time};
    const auto sample = runtime::evaluateValueUtility({document::ValueUtilityKernel::DataSample,
                                                       operands,
                                                       {},
                                                       core::RationalTime::fromInteger(0),
                                                       document::FrameRate::framesPerSecond24(),
                                                       &curve});
    const auto* sampled = std::get_if<double>(&sample.outputs[0]);
    if (sample.failed || sampled == nullptr || std::abs(*sampled - 2.0) > 1e-12)
        return 1;

    document::DataBlockRecord table;
    table.id = document::DataBlockRecordId::fromRaw(2);
    table.kind = document::DataBlockKind::Table;
    table.payload =
        document::DataBlockTable{{{"value", document::DataBlockValueKind::Float64, {3.0}}}};
    runtime::CompiledValue row = std::int64_t{3};
    runtime::CompiledValue column = std::int64_t{0};
    std::vector<const runtime::CompiledValue*> lookupOperands{&row, &column};
    const auto lookup = runtime::evaluateValueUtility({document::ValueUtilityKernel::TableLookup,
                                                       lookupOperands,
                                                       {},
                                                       core::RationalTime::fromInteger(0),
                                                       document::FrameRate::framesPerSecond24(),
                                                       &table});
    return lookup.failed ? 0 : 1;
}
} // namespace

int main() {
    const auto result = run();
    if (result != 0)
        std::cerr << "data block reader test failed\n";
    return result;
}
