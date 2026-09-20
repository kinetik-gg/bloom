// CPU reference replay for the GPU scene preparation tests: replaying the prepared commands with
// the EXISTING CPU primitives (test oracle only) to prove bit-for-bit output parity.
// Included by gpu_scene_preparation_test_support.hpp inside its anonymous namespace.

// --- Replay with the existing CPU primitives (test-only)
// ------------------------------------------
[[nodiscard]] std::shared_ptr<const Rgba32fImage> freeze(Rgba32fImageBuilder& builder) {
    auto frozen = std::move(builder).freeze();
    return std::make_shared<const Rgba32fImage>(std::move(*frozen.value()));
}

[[maybe_unused, nodiscard]] bool
replayScene(const PreparedGpuScene& scene, std::vector<std::shared_ptr<const Rgba32fImage>>& images,
            const double hScale = 1.0, const double vScale = 1.0) {
    constexpr std::size_t kBudget = 1U << 28U;
    images.assign(scene.commands().size(), nullptr);
    for (const auto& command : scene.commands()) {
        if (const auto* solid = std::get_if<bloom::runtime::GpuSceneSolidCommand>(&command)) {
            const auto descriptor = Rgba32fImageDescriptor::create(
                solid->dataWindow, solid->displayWindow, solid->pixelAspect);
            if (!descriptor) {
                return false;
            }
            auto builder = Rgba32fImageBuilder::create(*descriptor.value(), kBudget);
            if (!builder) {
                return false;
            }
            for (std::int64_t y = solid->dataWindow.originY();
                 y < solid->dataWindow.maxYExclusive(); ++y) {
                auto row = builder.value()->row(y);
                if (!row) {
                    return false;
                }
                bloom::render::fillSolidRow(*row.value(), solid->pixel);
            }
            images[solid->index] = freeze(*builder.value());
            continue;
        }
        if (const auto* translation =
                std::get_if<bloom::runtime::GpuSceneTranslationCommand>(&command)) {
            const auto* input = images[translation->input].get();
            if (input == nullptr) {
                return false;
            }
            const auto sourceView = input->view();
            if (!sourceView) {
                return false;
            }
            const auto params = bloom::render::TranslationOpacity::create(
                translation->translationX, translation->translationY,
                static_cast<double>(translation->opacity));
            if (!params) {
                return false;
            }
            const auto descriptor = Rgba32fImageDescriptor::create(
                translation->outputWindow, input->descriptor()->displayWindow(),
                input->descriptor()->pixelAspect());
            if (!descriptor) {
                return false;
            }
            auto builder = Rgba32fImageBuilder::create(*descriptor.value(), kBudget);
            if (!builder) {
                return false;
            }
            for (std::int64_t y = translation->outputWindow.originY();
                 y < translation->outputWindow.maxYExclusive(); ++y) {
                auto row = builder.value()->row(y);
                if (!row) {
                    return false;
                }
                if (const auto status = bloom::render::translateOpacityBilinearRow(
                        *sourceView.value(), translation->outputWindow, y, *params.value(),
                        *row.value())) {
                    (void)status;
                    return false;
                }
            }
            images[translation->index] = freeze(*builder.value());
            continue;
        }
        if (const auto* coverage =
                std::get_if<bloom::runtime::GpuSceneCoverageSolidCommand>(&command)) {
            const auto descriptor = Rgba32fImageDescriptor::create(
                coverage->outputWindow, coverage->displayWindow, coverage->pixelAspect);
            if (!descriptor) {
                return false;
            }
            auto builder =
                Rgba32fImageBuilder::create(*descriptor.value(), kBudget, Rgba32f::transparent());
            if (!builder) {
                return false;
            }
            const auto width = coverage->outputWindow.extent().width();
            const std::vector<std::uint8_t> mask =
                bloom::gpu_scene_coverage_test::coverageMaskBytes(*coverage);
            if (mask.empty()) {
                return false;
            }
            for (std::int64_t y = coverage->outputWindow.originY();
                 y < coverage->outputWindow.maxYExclusive(); ++y) {
                auto row = builder.value()->row(y);
                if (!row) {
                    return false;
                }
                const auto offset =
                    static_cast<std::size_t>(y - coverage->outputWindow.originY()) * width;
                const auto coverageRow = std::span<const std::uint8_t>(mask.data() + offset, width);
                if (const auto status = bloom::render::coverageSolidRow(
                        coverageRow, coverage->pixel, *row.value())) {
                    (void)status;
                    return false;
                }
                for (auto& value : *row.value()) {
                    const auto faded = Rgba32f::fromPremultiplied(
                        value.red() * coverage->opacity, value.green() * coverage->opacity,
                        value.blue() * coverage->opacity, value.alpha() * coverage->opacity);
                    if (!faded) {
                        return false;
                    }
                    value = *faded.value();
                }
            }
            images[coverage->index] = freeze(*builder.value());
            continue;
        }
        if (const auto* upload = std::get_if<bloom::runtime::GpuSceneUploadCommand>(&command)) {
            // The upload command already carries the frozen converted source, so replay is an
            // alias: this is exactly the immutability the native upload would rely on.
            images[upload->index] = upload->image;
            continue;
        }
        if (const auto* merge = std::get_if<bloom::runtime::GpuSceneMergeCommand>(&command)) {
            const auto descriptor = Rgba32fImageDescriptor::create(
                merge->outputWindow, merge->displayWindow, merge->pixelAspect);
            if (!descriptor) {
                return false;
            }
            auto builder =
                Rgba32fImageBuilder::create(*descriptor.value(), kBudget, Rgba32f::transparent());
            if (!builder) {
                return false;
            }
            const auto destinationWindow = merge->outputWindow;
            for (const auto foreground : merge->foregrounds) {
                const auto* source = images[foreground].get();
                if (source == nullptr) {
                    continue;
                }
                const auto sourceView = source->view();
                if (!sourceView) {
                    return false;
                }
                const auto sourceWindow = source->descriptor()->dataWindow();
                const auto firstColumn =
                    std::max(sourceWindow.originX(), destinationWindow.originX());
                const auto lastColumn =
                    std::min(sourceWindow.maxXExclusive(), destinationWindow.maxXExclusive());
                if (lastColumn <= firstColumn) {
                    continue;
                }
                const auto sourceOffset = firstColumn - sourceWindow.originX();
                const auto columnOffset = firstColumn - destinationWindow.originX();
                const auto columnCount = static_cast<std::size_t>(lastColumn - firstColumn);
                for (std::int64_t y = std::max(sourceWindow.originY(), destinationWindow.originY());
                     y < std::min(sourceWindow.maxYExclusive(), destinationWindow.maxYExclusive());
                     ++y) {
                    auto sourceRow = sourceView.value()->row(y);
                    auto destinationRow = builder.value()->row(y);
                    if (!sourceRow || !destinationRow) {
                        return false;
                    }
                    if (const auto status = bloom::render::sourceOverLinearRec709SceneRow(
                            sourceRow.value()->subspan(static_cast<std::size_t>(sourceOffset),
                                                       columnCount),
                            destinationRow.value()->subspan(static_cast<std::size_t>(columnOffset),
                                                            columnCount))) {
                        (void)status;
                        return false;
                    }
                }
            }
            images[merge->index] = freeze(*builder.value());
            continue;
        }
        if (const auto* affine = std::get_if<bloom::runtime::GpuSceneAffineCommand>(&command)) {
            const auto* input = images[affine->input].get();
            if (input == nullptr) {
                return false;
            }
            const auto view = input->view();
            if (!view) {
                return false;
            }
            const auto descriptor = Rgba32fImageDescriptor::create(
                affine->outputWindow, input->descriptor()->displayWindow(),
                input->descriptor()->pixelAspect());
            if (!descriptor) {
                return false;
            }
            auto builder =
                Rgba32fImageBuilder::create(*descriptor.value(), kBudget, Rgba32f::transparent());
            if (!builder) {
                return false;
            }
            // Recover the AUTHOR-space LayerMatrix from the device-space GpuAffine matrix and
            // replay with the exact CPU oracle the evaluator's parented raster arm uses.
            const auto& g = affine->matrix;
            const double b = g.b * (vScale / hScale);
            const double c = g.c * (hScale / vScale);
            const double ox = static_cast<double>(affine->sourceWindow.originX()) + 0.5;
            const double oy = static_cast<double>(affine->sourceWindow.originY()) + 0.5;
            const double mx = (g.tx - g.a * ox - g.b * oy + 0.5) / hScale;
            const double my = (g.ty - g.c * ox - g.d * oy + 0.5) / vScale;
            bloom::runtime::detail::LayerMatrix matrix{g.a, b, c, g.d, mx, my};
            bloom::runtime::detail::ParentedLayerTransform parented(
                matrix, affine->sourceWindow, hScale, vScale, affine->opacity);
            for (std::int64_t y = affine->outputWindow.originY();
                 y < affine->outputWindow.maxYExclusive(); ++y) {
                auto row = builder.value()->row(y);
                if (!row) {
                    return false;
                }
                if (const auto status =
                        parented.row(*view.value(), affine->outputWindow, y, *row.value())) {
                    (void)status;
                    return false;
                }
            }
            images[affine->index] = freeze(*builder.value());
            continue;
        }
        if (const auto* resample =
                std::get_if<bloom::runtime::GpuScenePointResampleCommand>(&command)) {
            const auto* input = images[resample->input].get();
            if (input == nullptr) {
                return false;
            }
            const auto view = input->view();
            if (!view) {
                return false;
            }
            const auto descriptor = Rgba32fImageDescriptor::create(
                resample->outputWindow, resample->displayWindow, resample->pixelAspect);
            if (!descriptor) {
                return false;
            }
            auto builder = Rgba32fImageBuilder::create(*descriptor.value(), kBudget);
            if (!builder) {
                return false;
            }
            // The exact CPU media-image proxy oracle: sourceX = min(extent-1, (uint32)(x/scale)).
            const auto sourceWindow = input->descriptor()->dataWindow();
            const auto outputWidth = resample->outputWindow.extent().width();
            const auto outputHeight = resample->outputWindow.extent().height();
            for (std::uint32_t y = 0; y < outputHeight; ++y) {
                const auto sourceY = std::min(
                    sourceWindow.extent().height() - 1,
                    static_cast<std::uint32_t>(static_cast<double>(y) / resample->verticalScale));
                auto outputRow = builder.value()->row(resample->outputWindow.originY() +
                                                      static_cast<std::int64_t>(y));
                if (!outputRow) {
                    return false;
                }
                for (std::uint32_t x = 0; x < outputWidth; ++x) {
                    const auto sourceX =
                        std::min(sourceWindow.extent().width() - 1,
                                 static_cast<std::uint32_t>(static_cast<double>(x) /
                                                            resample->horizontalScale));
                    (*outputRow.value())[x] = input->pixels()[static_cast<std::size_t>(sourceY) *
                                                                  sourceWindow.extent().width() +
                                                              sourceX];
                }
            }
            images[resample->index] = freeze(*builder.value());
            continue;
        }
        if (const auto* blend = std::get_if<bloom::runtime::GpuSceneBlendCommand>(&command)) {
            const auto* source = images[blend->source].get();
            const auto* destination = images[blend->destination].get();
            if (source == nullptr || destination == nullptr) {
                return false;
            }
            const auto sourceView = source->view();
            const auto destinationView = destination->view();
            if (!sourceView || !destinationView) {
                return false;
            }
            const auto descriptor = Rgba32fImageDescriptor::create(
                blend->outputWindow, destination->descriptor()->displayWindow(),
                destination->descriptor()->pixelAspect());
            if (!descriptor) {
                return false;
            }
            auto builder =
                Rgba32fImageBuilder::create(*descriptor.value(), kBudget, Rgba32f::transparent());
            if (!builder) {
                return false;
            }
            const auto outputWindow = blend->outputWindow;
            const auto destinationWindow = destination->descriptor()->dataWindow();
            const auto firstColumn = std::max(destinationWindow.originX(), outputWindow.originX());
            const auto lastColumn =
                std::min(destinationWindow.maxXExclusive(), outputWindow.maxXExclusive());
            const auto firstRow = std::max(destinationWindow.originY(), outputWindow.originY());
            const auto lastRow =
                std::min(destinationWindow.maxYExclusive(), outputWindow.maxYExclusive());
            if (lastColumn > firstColumn && lastRow > firstRow) {
                const auto count = static_cast<std::size_t>(lastColumn - firstColumn);
                const auto destinationOffset = firstColumn - destinationWindow.originX();
                const auto outputOffset = firstColumn - outputWindow.originX();
                for (std::int64_t y = firstRow; y < lastRow; ++y) {
                    auto destinationRow = destinationView.value()->row(y);
                    auto outputRow = builder.value()->row(y);
                    if (!destinationRow || !outputRow) {
                        return false;
                    }
                    std::copy_n(destinationRow.value()->begin() + destinationOffset, count,
                                outputRow.value()->begin() + outputOffset);
                }
            }
            const auto sourceWindow = source->descriptor()->dataWindow();
            const auto blendFirstColumn = std::max(sourceWindow.originX(), outputWindow.originX());
            const auto blendLastColumn =
                std::min(sourceWindow.maxXExclusive(), outputWindow.maxXExclusive());
            const auto blendFirstRow = std::max(sourceWindow.originY(), outputWindow.originY());
            const auto blendLastRow =
                std::min(sourceWindow.maxYExclusive(), outputWindow.maxYExclusive());
            if (blendLastColumn > blendFirstColumn && blendLastRow > blendFirstRow) {
                const auto count = static_cast<std::size_t>(blendLastColumn - blendFirstColumn);
                const auto sourceOffset = blendFirstColumn - sourceWindow.originX();
                const auto outputOffset = blendFirstColumn - outputWindow.originX();
                for (std::int64_t y = blendFirstRow; y < blendLastRow; ++y) {
                    auto sourceRow = sourceView.value()->row(y);
                    auto outputRow = builder.value()->row(y);
                    if (!sourceRow || !outputRow) {
                        return false;
                    }
                    if (const auto status = bloom::render::blendLinearRec709SceneRow(
                            blend->mode,
                            sourceRow.value()->subspan(static_cast<std::size_t>(sourceOffset),
                                                       count),
                            outputRow.value()->subspan(static_cast<std::size_t>(outputOffset),
                                                       count))) {
                        (void)status;
                        return false;
                    }
                }
            }
            images[blend->index] = freeze(*builder.value());
            continue;
        }
        if (const auto* output =
                std::get_if<bloom::runtime::GpuSceneCompositionOutputCommand>(&command)) {
            const auto descriptor = Rgba32fImageDescriptor::create(
                output->dataWindow, output->displayWindow, output->pixelAspect);
            if (!descriptor) {
                return false;
            }
            auto builder =
                Rgba32fImageBuilder::create(*descriptor.value(), kBudget, Rgba32f::transparent());
            if (!builder) {
                return false;
            }
            if (output->input != bloom::runtime::kInvalidGpuSceneCommand) {
                const auto* source = images[output->input].get();
                if (source != nullptr) {
                    const auto sourceWindow = source->descriptor()->dataWindow();
                    const auto firstColumn =
                        std::max(sourceWindow.originX(), output->dataWindow.originX());
                    const auto lastColumn =
                        std::min(sourceWindow.maxXExclusive(), output->dataWindow.maxXExclusive());
                    const auto firstRow =
                        std::max(sourceWindow.originY(), output->dataWindow.originY());
                    const auto lastRow =
                        std::min(sourceWindow.maxYExclusive(), output->dataWindow.maxYExclusive());
                    if (lastColumn > firstColumn && lastRow > firstRow) {
                        const auto count = static_cast<std::size_t>(lastColumn - firstColumn);
                        const auto sourceOffset = firstColumn - sourceWindow.originX();
                        const auto destinationOffset = firstColumn - output->dataWindow.originX();
                        for (std::int64_t y = firstRow; y < lastRow; ++y) {
                            const auto sourceView = source->view();
                            auto sourceRow = sourceView.value()->row(y);
                            auto destinationRow = builder.value()->row(y);
                            std::copy_n(sourceRow.value()->begin() + sourceOffset, count,
                                        destinationRow.value()->begin() + destinationOffset);
                        }
                    }
                }
            }
            images[output->index] = freeze(*builder.value());
            continue;
        }
        return false;
    }
    return true;
}
