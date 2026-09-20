void testEmbeddedSpirvDigest(Expectations& expectations) {
    static_assert(bloom::render::vulkan_detail::kNeutralDisplaySpirvWordCount * 4U ==
                      bloom::render::vulkan_detail::kNeutralDisplaySpirvByteCount,
                  "SPIR-V word count must exactly cover the byte count");
    const auto* raw =
        reinterpret_cast<const std::byte*>(bloom::render::vulkan_detail::kNeutralDisplaySpirvCode);
    const std::span<const std::byte> bytes(
        raw, bloom::render::vulkan_detail::kNeutralDisplaySpirvByteCount);
    const auto digest = bloom::core::Sha256Hasher::hash(bytes);
    expectations.expect(digest.has_value(), "the embedded SPIR-V hashes");
    if (!digest.has_value()) {
        return;
    }
    const auto hex = digest->toLowercaseHex();
    const std::string_view measured(hex.data(), hex.size());
    expectations.expect(measured == BLOOM_NEUTRAL_DISPLAY_SPV_SHA256,
                        "the embedded SPIR-V array hashes to the pinned SPIR-V digest");
    std::cout << "embedded SPIR-V sha256=" << measured << '\n';
}

struct Options final {
    std::filesystem::path loader_path;
    bool require_device = false;
    bool benchmark = false;
    bool valid = true;
};

[[nodiscard]] Options parseOptions(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader") {
            if (index + 1 >= argc) {
                std::cerr << "--loader requires a path argument\n";
                options.valid = false;
                return options;
            }
            options.loader_path = argv[++index];
        } else if (argument == "--require-device") {
            options.require_device = true;
        } else if (argument == "--benchmark") {
            options.benchmark = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            options.valid = false;
            return options;
        }
    }
    return options;
}

[[nodiscard]] Rgba32f pixel(const float red, const float green, const float blue,
                            const float alpha) {
    const auto result = Rgba32f::fromPremultiplied(red, green, blue, alpha);
    return result ? *result.value() : Rgba32f::transparent();
}

[[nodiscard]] std::optional<Rgba32fImage> makeImage(const std::span<const Rgba32f> pixels,
                                                    const std::uint32_t width,
                                                    const std::uint32_t height) {
    const auto windowResult = ImageWindow::create(0, 0, width, height);
    if (!windowResult) {
        return std::nullopt;
    }
    const auto descriptorResult = Rgba32fImageDescriptor::create(
        *windowResult.value(), *windowResult.value(), bloom::core::PixelAspectRatio::square());
    if (!descriptorResult) {
        return std::nullopt;
    }
    auto builderResult = Rgba32fImageBuilder::create(*descriptorResult.value(), 1ULL << 30ULL);
    if (!builderResult) {
        return std::nullopt;
    }
    auto builder = std::move(*builderResult.value());
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto x = static_cast<std::int64_t>(index % width);
        const auto y = static_cast<std::int64_t>(index / width);
        if (builder.write(x, y, pixels[index]).has_value()) {
            return std::nullopt;
        }
    }
    auto frozen = std::move(builder).freeze();
    if (!frozen) {
        return std::nullopt;
    }
    return std::move(*frozen.value());
}

[[nodiscard]] std::optional<bloom::color::PreparedCpuDisplayProcessorHandle> buildCpuHandle() {
    auto resolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    if (resolution.outcome() != bloom::color::OcioBuiltInRegistryOutcome::Ready) {
        return std::nullopt;
    }
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    auto buildResult = bloom::color::buildBloomNeutralCpuDisplayProcessor(*resolved);
    if (!buildResult) {
        return std::nullopt;
    }
    return std::move(buildResult).takeHandle();
}

[[nodiscard]] std::optional<std::vector<Rgba8>>
cpuOracle(const bloom::color::PreparedCpuDisplayProcessorHandle& handle,
          const std::span<const Rgba32f> pixels, const std::uint32_t width,
          const std::uint32_t height) {
    auto image = makeImage(pixels, width, height);
    if (!image.has_value()) {
        return std::nullopt;
    }
    const auto view = image->view();
    if (!view) {
        return std::nullopt;
    }
    auto result = bloom::color::produceBloomNeutralDisplayFrame(handle, *view.value(),
                                                                pixels.size(), 1ULL << 30ULL);
    if (!result) {
        return std::nullopt;
    }
    const auto span = result.value()->pixels();
    return std::vector<Rgba8>(span.begin(), span.end());
}

struct GpuRunResult final {
    bool success = false;
    GpuNeutralDisplayDiagnosticCode code = GpuNeutralDisplayDiagnosticCode::None;
    std::string message;
    std::vector<Rgba8> pixels;
};

// begin() copies and submits; poll() is a non-blocking fence query driven to completion by this
// bounded spin. No thread blocks on a Vulkan wait.
[[nodiscard]] GpuRunResult runGpu(GpuNeutralDisplay& display, const std::span<const Rgba32f> source,
                                  const std::uint64_t budget) {
    GpuRunResult result;
    const auto beginDiagnostic = display.begin(source, budget);
    if (beginDiagnostic.code != GpuNeutralDisplayDiagnosticCode::None) {
        result.code = beginDiagnostic.code;
        result.message = beginDiagnostic.message;
        return result;
    }
    for (int attempt = 0; attempt < 1000000; ++attempt) {
        const auto poll = display.poll();
        if (poll == GpuNeutralDisplayPollResult::Pending) {
            std::this_thread::yield();
            continue;
        }
        if (poll == GpuNeutralDisplayPollResult::Failure) {
            result.code = display.diagnostic().code;
            result.message = display.diagnostic().message;
            return result;
        }
        auto readback = display.readback();
        if (!readback) {
            result.code = readback.diagnostic.code;
            result.message = readback.diagnostic.message;
            return result;
        }
        result.success = true;
        result.pixels = std::move(readback.pixels);
        return result;
    }
    result.code = GpuNeutralDisplayDiagnosticCode::DeviceUnavailable;
    result.message = "the poll spin did not observe completion";
    return result;
}

[[nodiscard]] std::string hex(const Rgba8 value) {
    std::ostringstream stream;
    stream << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(value.red)
           << static_cast<int>(value.green) << static_cast<int>(value.blue)
           << static_cast<int>(value.alpha);
    return stream.str();
}

// Benchmark sinks, file-scope so the per-run lambdas can write them without capturing by reference
// through a mutable lambda; reading them back keeps each timed result from being optimized away.
double g_benchmarkSink = 0.0;
double g_benchmarkGpuMs = 0.0;
double g_benchmarkCpuMs = 0.0;

// GPU vs CPU gate: RGB within one 8-bit code, alpha exact. Returns the number of mismatches.
[[nodiscard]] std::size_t compareToOracle(const Expectations& expectations,
                                          const std::string_view name,
                                          const std::span<const Rgba8> gpu,
                                          const std::span<const Rgba8> cpu) {
    std::size_t mismatches = 0;
    for (std::size_t index = 0; index < gpu.size(); ++index) {
        const int dr =
            std::abs(static_cast<int>(gpu[index].red) - static_cast<int>(cpu[index].red));
        const int dg =
            std::abs(static_cast<int>(gpu[index].green) - static_cast<int>(cpu[index].green));
        const int db =
            std::abs(static_cast<int>(gpu[index].blue) - static_cast<int>(cpu[index].blue));
        const bool alphaExact = gpu[index].alpha == cpu[index].alpha;
        if (dr > 1 || dg > 1 || db > 1 || !alphaExact) {
            if (mismatches < 8) {
                std::cerr << "MISMATCH " << name << " pixel " << index << " gpu=" << hex(gpu[index])
                          << " cpu=" << hex(cpu[index]) << '\n';
            }
            ++mismatches;
        }
    }
    (void)expectations;
    return mismatches;
}

// Deterministic xorshift so the "random" fixture is reproducible across runs and machines.
class Rng final {
  public:
    explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}
    [[nodiscard]] std::uint32_t next() noexcept {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 7U;
        state_ ^= state_ << 17U;
        return static_cast<std::uint32_t>(state_);
    }
    [[nodiscard]] float unit() noexcept {
        return static_cast<float>(next() % 1000000U) / 1000000.0F;
    }

  private:
    std::uint64_t state_;
};

[[nodiscard]] std::vector<Rgba32f> randomFixture(const std::size_t count,
                                                 const std::uint64_t seed) {
    Rng rng(seed);
    std::vector<Rgba32f> pixels;
    pixels.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const float alpha = rng.unit();
        const float red = (rng.unit() * 2.0F - 0.5F) * alpha;
        const float green = (rng.unit() * 2.0F - 0.5F) * alpha;
        const float blue = (rng.unit() * 2.0F - 0.5F) * alpha;
        pixels.push_back(pixel(red, green, blue, alpha));
    }
    return pixels;
}

struct Fixture final {
    std::string name;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::vector<Rgba32f> pixels;
};

void runFixture(Expectations& expectations, GpuNeutralDisplay& display,
                const bloom::color::PreparedCpuDisplayProcessorHandle& handle,
                const Fixture& fixture) {
    const auto cpu = cpuOracle(handle, fixture.pixels, fixture.width, fixture.height);
    expectations.expect(cpu.has_value(), fixture.name + ": CPU oracle succeeds");
    const auto gpu = runGpu(display, fixture.pixels, 1ULL << 30ULL);
    expectations.expect(gpu.success, fixture.name + ": GPU dispatch succeeds: " + gpu.message);
    if (!cpu.has_value() || !gpu.success) {
        return;
    }
    expectations.expect(gpu.pixels.size() == cpu->size(), fixture.name + ": pixel count matches");
    const std::size_t mismatches = compareToOracle(expectations, fixture.name, gpu.pixels, *cpu);
    expectations.expect(mismatches == 0, fixture.name + ": GPU matches CPU oracle");
    if (mismatches == 0) {
        std::cout << "PASS " << fixture.name << " (" << fixture.pixels.size() << " px)\n";
    }
}

// Boundary sweep: for every one of the 256 alpha quantization transitions, the two adjacent
// binary32 values must pack to exactly the CPU alpha byte.
[[nodiscard]] Fixture alphaBoundaryFixture() {
    Fixture fixture;
    fixture.name = "alpha-boundary-adjacent";
    std::vector<Rgba32f> pixels;
    for (int byteValue = 1; byteValue <= 255; ++byteValue) {
        const float boundary = (static_cast<float>(byteValue) - 0.5F) / 255.0F;
        const float below = std::nextafter(boundary, 0.0F);
        const float above = std::nextafter(boundary, 1.0F);
        for (const float alpha : {below, boundary, above}) {
            if (alpha < 0.0F || alpha > 1.0F) {
                continue;
            }
            pixels.push_back(pixel(0.25F * alpha, 0.5F * alpha, 0.75F * alpha, alpha));
        }
    }
    fixture.width = 1;
    fixture.height = static_cast<std::uint32_t>(pixels.size());
    fixture.pixels = std::move(pixels);
    return fixture;
}
