// OpenEXR ingest group (#362): data/display window registration and the non-finite repair policy,
// through the CPU evaluator and GPU preparation. Included by gpu_media_scene_preparation_tests.cpp
// inside its anonymous namespace.

// Writes a premultiplied RGBA EXR whose data window is `data` inside the display window `display`.
void writeWindowedExr(const std::filesystem::path& path, const Imath::Box2i& data,
                      const Imath::Box2i& display, const std::vector<ExrPixel>& pixels) {
    const auto width = data.max.x - data.min.x + 1;
    const auto height = data.max.y - data.min.y + 1;
    if (static_cast<int>(pixels.size()) != width * height)
        throw std::logic_error("windowed EXR fixture pixel count does not match its data window");
    Imf::Header header(display, data);
    header.insert("alphaAssociation", Imf::StringAttribute("premultiplied"));
    for (const auto* name : {"R", "G", "B", "A"})
        header.channels().insert(name, Imf::Channel(Imf::FLOAT));
    std::array<std::vector<float>, 4> planes;
    for (auto& plane : planes)
        plane.resize(pixels.size());
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        planes[0][index] = pixels[index].red;
        planes[1][index] = pixels[index].green;
        planes[2][index] = pixels[index].blue;
        planes[3][index] = pixels[index].alpha;
    }
    Imf::FrameBuffer frameBuffer;
    const auto stride = static_cast<std::size_t>(width) * sizeof(float);
    const std::array<const char*, 4> names{"R", "G", "B", "A"};
    for (std::size_t channel = 0; channel < 4; ++channel) {
        // OpenEXR addresses slices by absolute data-window coordinates.
        auto* base =
            reinterpret_cast<char*>(planes[channel].data()) -
            static_cast<std::ptrdiff_t>(data.min.x) * static_cast<std::ptrdiff_t>(sizeof(float)) -
            static_cast<std::ptrdiff_t>(data.min.y) * static_cast<std::ptrdiff_t>(stride);
        frameBuffer.insert(names[channel], Imf::Slice(Imf::FLOAT, base, sizeof(float), stride));
    }
    Imf::OutputFile file(path.string().c_str(), header);
    file.setFrameBuffer(frameBuffer);
    file.writePixels(height);
}

[[nodiscard]] Rgba32f premultiplied(const float red, const float green, const float blue,
                                    const float alpha) {
    const auto pixel = Rgba32f::fromPremultiplied(red, green, blue, alpha);
    return *pixel.value();
}

[[nodiscard]] Rgba32f pixelAt(const Rgba32fImage& image, const std::int64_t x,
                              const std::int64_t y) {
    const auto window = image.descriptor()->dataWindow();
    const auto column = static_cast<std::size_t>(x - window.originX());
    const auto row = static_cast<std::size_t>(y - window.originY());
    return image.pixels()[row * window.extent().width() + column];
}

// A cropped pass (data window 3x2 at (2,1) inside a 8x6 frame) placed with the default layer
// transform at the frame centre lands at its own offset, and GPU preparation reproduces the CPU
// frame bit for bit at full and proxy resolution.
void testExrDataWindowRegistration(Expectations& expectations) {
    const auto directory = std::filesystem::temp_directory_path() / "bloom_exr_ingest_windows";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto path = directory / "cropped.exr";
    std::vector<ExrPixel> pixels;
    for (int index = 0; index < 6; ++index)
        pixels.push_back({0.1F * static_cast<float>(index + 1), 0.25F, 0.5F, 1.0F});
    writeWindowedExr(path, Imath::Box2i(Imath::V2i(2, 1), Imath::V2i(4, 2)),
                     Imath::Box2i(Imath::V2i(0, 0), Imath::V2i(7, 5)), pixels);
    const auto asset = imageAsset(path, "cropped", 60000);
    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(directory);
    const auto plan = mediaPlan(format(8, 6), asset, LayerValues{.position = {4.0, 3.0}}, 60000);
    const auto frame = evaluator.evaluate(plan, requestFor(*plan), {});
    expectations.expect(frame.frame() != nullptr, "a cropped EXR pass evaluates");
    if (frame.frame()) {
        const auto& image = frame.frame()->processImage();
        expectations.expect(pixelAt(image, 2, 1) == premultiplied(0.1F, 0.25F, 0.5F, 1.0F) &&
                                pixelAt(image, 4, 2) == premultiplied(0.6F, 0.25F, 0.5F, 1.0F) &&
                                pixelAt(image, 1, 1).alpha() == 0.0F &&
                                pixelAt(image, 5, 2).alpha() == 0.0F,
                            "the data window keeps its offset inside the frame");
        const auto& bounds = frame.frame()->evaluatedBounds().front().local;
        expectations.expect(bounds.left == 0.0 && bounds.top == 0.0 && bounds.right == 8.0 &&
                                bounds.bottom == 6.0,
                            "a cropped pass reports its frame as local content bounds");
    }
    checkMediaParity(expectations, evaluator, plan, requestFor(*plan), "cropped EXR full");
    auto proxy = requestFor(*plan);
    const auto extent = bloom::render::ImageExtent::create(4, 3);
    if (extent) {
        proxy.resolution = bloom::runtime::ProxyResolution{*extent.value()};
        checkMediaParity(expectations, evaluator, plan, proxy, "cropped EXR half proxy");
    }
    // Overscan: data larger than the frame, starting left of and above it.
    const auto overscanPath = directory / "overscan.exr";
    std::vector<ExrPixel> overscan(10 * 8, ExrPixel{0.5F, 0.5F, 0.5F, 1.0F});
    writeWindowedExr(overscanPath, Imath::Box2i(Imath::V2i(-1, -1), Imath::V2i(8, 6)),
                     Imath::Box2i(Imath::V2i(0, 0), Imath::V2i(7, 5)), overscan);
    const auto overscanAsset = imageAsset(overscanPath, "overscan", 61000);
    const auto overscanPlan =
        mediaPlan(format(8, 6), overscanAsset, LayerValues{.position = {4.0, 3.0}}, 61000);
    checkMediaParity(expectations, evaluator, overscanPlan, requestFor(*overscanPlan),
                     "overscan EXR full");
    std::filesystem::remove_all(directory);
}

// NaN/Inf components and out-of-range alpha are repaired, reported and never fail the frame.
void testExrNonFiniteRepair(Expectations& expectations) {
    const auto directory = std::filesystem::temp_directory_path() / "bloom_exr_ingest_repair";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto path = directory / "nonfinite.exr";
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto inf = std::numeric_limits<float>::infinity();
    writeExrRgba(path, 2, 2,
                 {{nan, 0.5F, 0.5F, 1.0F},
                  {0.25F, inf, 0.25F, 1.0F},
                  {0.25F, 0.25F, 0.25F, 1.5F},
                  {0.25F, 0.25F, 0.25F, 0.0F}});
    const auto asset = imageAsset(path, "nonfinite", 62000);
    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(directory);
    const auto plan = mediaPlan(format(2, 2), asset, LayerValues{.position = {1.0, 1.0}}, 62000);
    auto request = requestFor(*plan);
    request.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, request, {});
    const bool warned = std::ranges::any_of(frame.diagnostics(), [](const auto& diagnostic) {
        return diagnostic.summary.find("repaired") != std::string::npos;
    });
    expectations.expect(frame.frame() != nullptr && warned,
                        "non-finite EXR pixels render with a repair warning");
    if (frame.frame()) {
        const auto& image = frame.frame()->processImage();
        expectations.expect(
            pixelAt(image, 0, 0).red() == 0.0F && pixelAt(image, 1, 0).green() == 0.0F &&
                pixelAt(image, 0, 1).alpha() == 1.0F &&
                pixelAt(image, 1, 1) == Rgba32f::transparent(),
            "NaN/Inf become zero, alpha is clamped, zero-alpha emission is dropped");
    }
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    context.assetBaseDirectory = directory;
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared && prepared.scene->mediaStatistics().imageRepairedPixels == 4,
                        "GPU preparation records the repaired pixel count");
    checkMediaParity(expectations, evaluator, plan, requestFor(*plan), "repaired EXR");
    std::filesystem::remove_all(directory);
}
