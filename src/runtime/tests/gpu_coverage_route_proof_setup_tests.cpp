// CTest fixture setup for the GPU route-proof handoff. It creates the build-owned proof directory,
// removes ONLY the six known proof files, and writes a fresh run nonce. It never writes a proof: a
// genuine harness must publish its own, so an absent harness leaves every route MISSING.

#include "gpu_route_proof_io.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: gpu_coverage_route_proof_setup <proof-directory>\n";
        return 2;
    }
    const std::filesystem::path directory = argv[1];
    if (bloom::gpu_route_proof_io::clearKnownProofFiles(directory) !=
        bloom::gpu_route_proof_io::RouteProofIoStatus::Ok) {
        std::cerr << "FAIL: could not clear the known proof files\n";
        return 1;
    }
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    std::random_device entropy;
    const auto nonce = "run-" + std::to_string(stamp) + "-" + std::to_string(entropy());
    if (bloom::gpu_route_proof_io::writeRunNonce(directory, nonce) !=
        bloom::gpu_route_proof_io::RouteProofIoStatus::Ok) {
        std::cerr << "FAIL: could not write the run nonce\n";
        return 1;
    }
    std::cout << "route proof run nonce: " << nonce << '\n';
    return 0;
}
