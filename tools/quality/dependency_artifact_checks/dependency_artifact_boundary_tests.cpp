#include "dependency_artifact_checks_test_support.hpp"

#include <filesystem>
#include <string>

namespace bloom::quality::dependencies::tests {
namespace {

void testPathsAndSymlinks(const std::filesystem::path& root, Expectations& expectations) {
    expectations.expect(timestampFromEpoch(253'402'300'799ULL) == "9999-12-31T23:59:59Z",
                        "epoch upper bound formats independently of platform time APIs");

    const auto fixtureRoot = root / "dependencies/tests/fixtures";
    const auto expected = root / "dependencies/dependencies.lock.json";
    expectations.rejects(
        "fixture-separation",
        [&] {
            rejectFixtureAsProductionPath(fixtureRoot / "valid-lock.json", expected, fixtureRoot);
        },
        "production path guard rejects fixture resolution");
    expectations.rejects(
        "production-path",
        [&] {
            rejectFixtureAsProductionPath(root / "dependencies/some-other-lock.json", expected,
                                          fixtureRoot);
        },
        "production path guard rejects a different lexical path");
    expectations.rejects(
        "production-path",
        [&] {
            rejectFixtureAsProductionPath(root / "dependencies/alias/../dependencies.lock.json",
                                          expected, fixtureRoot);
        },
        "production path guard rejects parent aliases");

    TemporaryDirectory temporary;
    temporary.write("dependencies/dependencies.lock.json", "fixture");
    std::error_code error;
    std::filesystem::create_symlink(temporary.root() / "dependencies/dependencies.lock.json",
                                    temporary.root() / "lock-alias.json", error);
    if (!error) {
        expectations.rejects(
            "production-path",
            [&] {
                rejectFixtureAsProductionPath(temporary.root() / "lock-alias.json",
                                              temporary.root() /
                                                  "dependencies/dependencies.lock.json",
                                              temporary.root() / "fixtures");
            },
            "production path guard rejects symlink aliases");
    }

    TemporaryDirectory symlinkFixture;
    symlinkFixture.write("target.txt", "synthetic");
    const auto payloadRoot = symlinkFixture.root() / "dependencies/tests/fixtures/payloads";
    std::filesystem::create_directories(payloadRoot);
    error.clear();
    std::filesystem::create_symlink(symlinkFixture.root() / "target.txt", payloadRoot / "link.txt",
                                    error);
    if (!error) {
        const FixtureContext context{
            .root = symlinkFixture.root(),
            .payloadRoot = payloadRoot,
            .payloadDigests = {},
            .payloadSizes = {},
        };
        expectations.rejects(
            "fixture-evidence",
            [&] {
                verifyFixturePayload("dependencies/tests/fixtures/payloads/link.txt",
                                     "sha256:" + std::string(64, '0'), "fixture", context);
            },
            "fixture evidence rejects symlink path components");
    }
}

void testPublicSurface(const std::filesystem::path& root, Expectations& expectations) {
    const auto header = readBounded(
        root / "tools/quality/dependency_artifact_checks/dependency_artifact_checks.hpp",
        std::size_t{64} * 1024U);
    expectations.expect(
        header.find("validates Bloom's checked-in synthetic contract fixtures") !=
                std::string::npos &&
            header.find("still not an installed-prefix validator") != std::string::npos,
        "tooling API states its production-lock scope and prefix-validation boundary");
    expectations.expect(header.find("lockIdentity(") == std::string::npos &&
                            header.find("prefixIdentity(") == std::string::npos,
                        "tooling API exposes no trusted production identity capability");
    const auto productionHeader =
        readBounded(root / "tools/quality/dependency_artifact_checks/production_lock_checks.hpp",
                    std::size_t{64} * 1024U);
    expectations.expect(
        productionHeader.find("ASCII-STRICT v1 TIGHTENING") != std::string::npos,
        "production lock validator documents the deliberate ASCII-strict v1 tightening");
}

} // namespace

auto runBoundaryTests(const std::filesystem::path& repositoryRoot) -> int {
    Expectations expectations;
    testPathsAndSymlinks(repositoryRoot, expectations);
    testPublicSurface(repositoryRoot, expectations);
    return expectations.failures();
}

} // namespace bloom::quality::dependencies::tests
