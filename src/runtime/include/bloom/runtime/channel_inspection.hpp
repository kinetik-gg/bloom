#pragma once

// ADR 0026 Viewer channel inspection: a session display choice that shows channels of a ChannelSet
// output directly -- one channel as raw greyscale, three as RGB -- without wiring a Shuffle. It
// derives a viewer-only plan (a Raw Shuffle as the output) and never enters the project, the
// project's compiled plan, or any render or export.

#include <bloom/document/ids.hpp>

#include <string>
#include <vector>

namespace bloom::runtime {

struct ChannelInspection final {
    document::NodeId node;
    std::string port;
    std::vector<std::string> channels;
    friend bool operator==(const ChannelInspection&, const ChannelInspection&) = default;
};

} // namespace bloom::runtime
