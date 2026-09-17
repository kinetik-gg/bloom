#include "support.hpp"
#include <array>
#include <limits>

using namespace bloom::media::provider;
namespace {
// Independent encoder: concatenates hex, with no production wire helpers or field traversal.
struct Oracle {
    std::string hex;
    void integer(std::uint64_t n, unsigned bytes) {
        constexpr std::string_view digits = "0123456789abcdef";
        for (unsigned i = 0; i < bytes; ++i) {
            const auto b = (n >> (i * 8U)) & 255U;
            hex += digits[b >> 4U];
            hex += digits[b & 15U];
        }
    }
    void text(std::string_view s) {
        integer(s.size(), 4);
        for (char c : s)
            integer(static_cast<unsigned char>(c), 1);
    }
    void hash(const Digest& d) {
        for (auto b : d.bytes())
            integer(b, 1);
    }
    void domain(std::string_view s) {
        text(s);
        integer(1, 2);
    }
    Digest finish() {
        Bytes bytes;
        for (std::size_t i = 0; i < hex.size(); i += 2)
            bytes.push_back(static_cast<std::byte>(std::stoul(hex.substr(i, 2), nullptr, 16)));
        return digestBytes(bytes);
    }
};
void absentAuthority(Oracle& o) {
    for (int i = 0; i < 6; ++i)
        o.text("");
}
Digest fixtureDigest(std::uint8_t byte) {
    Digest::Bytes bytes{};
    bytes.fill(byte);
    return Digest::fromBytes(bytes);
}
void repeatedDigest(Oracle& oracle, std::uint8_t byte) {
    for (unsigned index = 0; index < 32; ++index)
        oracle.integer(byte, 1);
}
void distinctFields() {
    // Distinct values make swaps of equal-width fields visible, including attributed authority,
    // optional tolerance identities and the five otherwise easy-to-confuse quality-evidence digest
    // positions.
    MediaCapabilityKeyV1 c;
    c.role = Role::VideoDecode;
    c.purpose = Purpose::Proxy;
    c.container = "container";
    c.mapping = "mapping";
    c.codec = "codec";
    c.profile = "profile";
    c.level = "level";
    c.tier = "tier";
    c.sampleEntry = "sampleEntry";
    c.range = "range";
    c.chroma = "chroma";
    c.chromaLocation = "chromaLocation";
    c.alpha = "alpha";
    c.fieldMode = "fieldMode";
    c.audioFormat = "audioFormat";
    c.channelLayout = "channelLayout";
    c.colorFeatures = "colorFeatures";
    c.hdrFeatures = "hdrFeatures";
    c.timecodeFeatures = "timecodeFeatures";
    c.metadataFeatures = "metadataFeatures";
    c.timing = "timing";
    c.surfaceSemantics = "surfaceSemantics";
    c.bitDepth = 10;
    c.maxWidth = 1920;
    c.maxHeight = 1080;
    c.maxRate = {30000, 1001};
    Oracle a;
    a.domain("MediaCapabilityKeyV1");
    a.integer(3, 1);
    a.integer(2, 1);
    a.text("container");
    a.text("mapping");
    a.text("codec");
    a.text("profile");
    a.text("level");
    a.text("tier");
    a.text("sampleEntry");
    a.integer(10, 4);
    a.text("range");
    a.text("chroma");
    a.text("chromaLocation");
    a.text("alpha");
    a.text("fieldMode");
    a.integer(1920, 4);
    a.integer(1080, 4);
    a.integer(30000, 8);
    a.integer(1001, 8);
    a.text("audioFormat");
    a.text("channelLayout");
    a.text("colorFeatures");
    a.text("hdrFeatures");
    a.text("timecodeFeatures");
    a.text("metadataFeatures");
    a.text("timing");
    a.text("surfaceSemantics");
    test::check(a.finish() == std::get<Digest>(digest(c)), "distinct capability field order");
    ProviderExecutionKeyV1 e;
    e.provider = "provider";
    e.build = "build";
    e.dependencyLock = fixtureDigest(10);
    e.os = "OS";
    e.architecture = "arch";
    e.implementation = Implementation::Hardware;
    e.sdk = "SDK";
    e.driver = "driver";
    e.device = "device";
    e.generation = 0x0102030405060708ULL;
    e.transport = Transport::SharedMemorySlabsReserved;
    e.synchronization = "fence";
    e.resourceProfile = "bounded";
    e.entitlement = "test";
    e.trustDomain = "sdk-domain";
    e.availability = Availability::Unavailable;
    Oracle b;
    b.domain("ProviderExecutionKeyV1");
    b.text("provider");
    b.text("build");
    repeatedDigest(b, 10);
    b.integer(1, 2);
    b.text("OS");
    b.text("arch");
    b.integer(2, 1);
    b.text("SDK");
    b.text("driver");
    b.text("device");
    b.integer(0x0102030405060708ULL, 8);
    b.integer(1, 1);
    b.text("fence");
    b.text("bounded");
    b.text("test");
    b.text("sdk-domain");
    b.integer(2, 1);
    test::check(b.finish() == std::get<Digest>(digest(e)), "distinct execution field order");
    QualificationEvidenceV1 q;
    q.fixtures = fixtureDigest(11);
    q.qualification = Qualification::ExportQualified;
    q.result = QcResult::Pass;
    q.reviewDate = "2026-09-17";
    q.reviewDeadline = "2027-09-17";
    q.authority = {"issuer", "reference", "product", "scope", "2026-09-16", "2027-09-16"};
    Oracle d;
    d.domain("QualificationEvidenceV1");
    repeatedDigest(d, 11);
    d.integer(4, 1);
    d.integer(1, 1);
    d.text("2026-09-17");
    d.text("2027-09-17");
    d.text("issuer");
    d.text("reference");
    d.text("product");
    d.text("scope");
    d.text("2026-09-16");
    d.text("2027-09-16");
    test::check(d.finish() == std::get<Digest>(digest(q)), "attributed authority field order");
    PipelineQualificationV1 p;
    p.steps = {{a.finish(), b.finish(), d.finish()},
               {fixtureDigest(12), fixtureDigest(13), fixtureDigest(14)}};
    p.purpose = Purpose::Export;
    p.profile = "preset";
    p.fixtures = fixtureDigest(15);
    p.conversionVersions = {"convert-a", "convert-b"};
    p.determinism = MediaDeterminismV1::DecodedSemanticTolerance;
    p.toleranceProfile = fixtureDigest(16);
    p.reopenPolicy = "independent";
    p.qcProfile = "recipient";
    p.result = QcResult::Pass;
    Oracle f;
    f.domain("PipelineQualificationV1");
    f.integer(2, 4);
    f.hash(a.finish());
    f.hash(b.finish());
    f.hash(d.finish());
    repeatedDigest(f, 12);
    repeatedDigest(f, 13);
    repeatedDigest(f, 14);
    f.integer(4, 1);
    f.text("preset");
    repeatedDigest(f, 15);
    f.integer(2, 4);
    f.text("convert-a");
    f.text("convert-b");
    f.integer(3, 1);
    repeatedDigest(f, 16);
    f.text("independent");
    f.text("recipient");
    f.integer(1, 1);
    test::check(f.finish() == std::get<Digest>(digest(p)), "ordered steps and tolerance identity");
    MediaQcEvidenceV1 qc;
    qc.artifact = fixtureDigest(1);
    qc.snapshot = fixtureDigest(2);
    qc.preset = fixtureDigest(3);
    qc.pipeline = fixtureDigest(4);
    qc.execution = fixtureDigest(5);
    qc.tool = "tool";
    qc.version = "version";
    qc.profile = "profile";
    qc.coverage = "coverage";
    qc.independentReader = true;
    qc.externalQc = q.authority;
    qc.result = QcResult::Fail;
    Oracle g;
    g.domain("MediaQcEvidenceV1");
    repeatedDigest(g, 1);
    repeatedDigest(g, 2);
    repeatedDigest(g, 3);
    repeatedDigest(g, 4);
    repeatedDigest(g, 5);
    g.text("tool");
    g.text("version");
    g.text("profile");
    g.text("coverage");
    g.integer(1, 1);
    g.text("issuer");
    g.text("reference");
    g.text("product");
    g.text("scope");
    g.text("2026-09-16");
    g.text("2027-09-16");
    g.integer(2, 1);
    test::check(g.finish() == std::get<Digest>(digest(qc)),
                "distinct quality evidence field order");
}
void run() {
    distinctFields();
    const auto c = test::capability();
    const auto e = test::execution();
    const auto q = test::evidence();
    Oracle a;
    a.domain("MediaCapabilityKeyV1");
    a.integer(1, 1);
    a.integer(1, 1);
    a.text("bloom.synthetic");
    a.text("none");
    a.text("fake.rgba8");
    for (int i = 0; i < 4; ++i)
        a.text("none");
    a.integer(8, 4);
    for (int i = 0; i < 5; ++i)
        a.text("none");
    a.integer(64, 4);
    a.integer(64, 4);
    a.integer(24, 8);
    a.integer(1, 8);
    for (int i = 0; i < 8; ++i)
        a.text("none");
    test::check(a.finish() == std::get<Digest>(digest(c)), "capability independent encoding");
    Oracle b;
    b.domain("ProviderExecutionKeyV1");
    b.text("bloom.fake");
    b.text("v1");
    b.hash({});
    b.integer(1, 2);
    b.text("Linux");
    b.text("test");
    b.integer(1, 1);
    b.text("none");
    b.text("none");
    b.text("cpu");
    b.integer(1, 8);
    b.integer(0, 1);
    b.text("pipe-order");
    b.text("media-v1");
    b.text("none");
    b.text("fake-only");
    b.integer(1, 1);
    test::check(b.finish() == std::get<Digest>(digest(e)), "execution independent encoding");
    Oracle d;
    d.domain("QualificationEvidenceV1");
    d.hash({});
    d.integer(2, 1);
    d.integer(1, 1);
    d.text("2026-09-17");
    d.text("");
    absentAuthority(d);
    test::check(d.finish() == std::get<Digest>(digest(q)), "qualification independent encoding");
    PipelineQualificationV1 p;
    p.steps.push_back({a.finish(), b.finish(), d.finish()});
    p.profile = "synthetic-v1";
    p.conversionVersions = {"identity-v1"};
    p.reopenPolicy = "none";
    p.qcProfile = "none";
    p.result = QcResult::Pass;
    Oracle f;
    f.domain("PipelineQualificationV1");
    f.integer(1, 4);
    f.hash(a.finish());
    f.hash(b.finish());
    f.hash(d.finish());
    f.integer(1, 1);
    f.text("synthetic-v1");
    f.hash({});
    f.integer(1, 4);
    f.text("identity-v1");
    f.integer(4, 1);
    f.hash({});
    f.text("none");
    f.text("none");
    f.integer(1, 1);
    test::check(f.finish() == std::get<Digest>(digest(p)), "pipeline independent encoding");
    MediaQcEvidenceV1 qc;
    qc.tool = "fake-qc";
    qc.version = "1";
    qc.profile = "synthetic-v1";
    qc.coverage = "all";
    Oracle g;
    g.domain("MediaQcEvidenceV1");
    for (int i = 0; i < 5; ++i)
        g.hash({});
    g.text("fake-qc");
    g.text("1");
    g.text("synthetic-v1");
    g.text("all");
    g.integer(0, 1);
    absentAuthority(g);
    g.integer(3, 1);
    test::check(g.finish() == std::get<Digest>(digest(qc)), "quality-control independent encoding");
    auto bad = c;
    bad.role = static_cast<Role>(0);
    test::check(std::holds_alternative<Unavailable>(digest(bad)), "closed enum");
    bad = c;
    bad.codec.clear();
    test::check(std::holds_alternative<Unavailable>(digest(bad)), "missing field");
    bad = c;
    bad.codec = std::string(Limits::stringBytes + 1, 'x');
    test::check(std::holds_alternative<Unavailable>(digest(bad)), "string limit");
    bad = c;
    bad.codec = "\xc0\x80";
    test::check(std::holds_alternative<Unavailable>(digest(bad)), "UTF-8");
    test::check(!valid(Rational{1, 0}) && !valid(Rational{2, 2}) &&
                    valid(Rational{std::numeric_limits<std::int64_t>::min(), 1}),
                "rational bounds");
    FrameProduct frame;
    CpuPlane plane;
    plane.width = 2;
    plane.height = 2;
    plane.stride = 8;
    plane.bytes.resize(16);
    plane.digest = digestBytes(plane.bytes);
    frame.planes.push_back(plane);
    test::check(valid(frame), "bounded plane");
    frame.planes[0].stride = 0;
    test::check(!valid(frame), "bad stride");
    AudioBlock audio;
    audio.sampleRate = 48000;
    audio.channelLayout = {"L", "R"};
    audio.channels = {{0.0F}, {1.0F}};
    test::check(valid(audio), "planar audio");
    audio.channels[0][0] = std::numeric_limits<float>::infinity();
    test::check(!valid(audio), "finite audio");
}
} // namespace
int main() {
    try {
        run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
