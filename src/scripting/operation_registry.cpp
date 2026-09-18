#include <bloom/commands/operations.hpp>
#include <bloom/scripting/operation_registry.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <utility>

namespace bloom::scripting {
namespace {

[[nodiscard]] OperationCreateResult failure(const std::string& operationId, std::string argument,
                                            std::string message) {
    return OperationCreateResult(nullptr,
                                 OperationDiagnostic{.code = "bloom.scripting.invalid-argument",
                                                     .operationId = operationId,
                                                     .argument = std::move(argument),
                                                     .message = std::move(message)});
}

[[nodiscard]] const Value* findArgument(const Arguments& arguments, const std::string& name) {
    const auto iterator = arguments.find(name);
    return iterator == arguments.end() ? nullptr : &iterator->second;
}

[[nodiscard]] std::optional<double> numeric(const Value& value) {
    if (const auto* result = std::get_if<double>(&value.storage)) {
        return *result;
    }
    if (const auto* result = std::get_if<std::int64_t>(&value.storage)) {
        return static_cast<double>(*result);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t> integer(const Arguments& arguments,
                                                  const std::string& operationId,
                                                  const std::string& name,
                                                  OperationCreateResult& error) {
    const auto* value = findArgument(arguments, name);
    if (value == nullptr) {
        error = failure(operationId, name, "Missing integer argument");
        return std::nullopt;
    }
    if (const auto* result = std::get_if<std::int64_t>(&value->storage)) {
        return *result;
    }
    error = failure(operationId, name, "Expected an integer");
    return std::nullopt;
}

[[nodiscard]] std::optional<double> number(const Arguments& arguments,
                                           const std::string& operationId, const std::string& name,
                                           OperationCreateResult& error,
                                           const bool required = true) {
    const auto* value = findArgument(arguments, name);
    if (value == nullptr) {
        if (!required) {
            return std::nullopt;
        }
        error = failure(operationId, name, "Missing number argument");
        return std::nullopt;
    }
    if (const auto* result = std::get_if<double>(&value->storage)) {
        return *result;
    }
    if (const auto* result = std::get_if<std::int64_t>(&value->storage)) {
        return static_cast<double>(*result);
    }
    error = failure(operationId, name, "Expected a number");
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> text(const Arguments& arguments,
                                              const std::string& operationId,
                                              const std::string& name,
                                              OperationCreateResult& error) {
    const auto* value = findArgument(arguments, name);
    if (value == nullptr) {
        error = failure(operationId, name, "Missing string argument");
        return std::nullopt;
    }
    if (const auto* result = std::get_if<std::string>(&value->storage)) {
        return *result;
    }
    error = failure(operationId, name, "Expected a string");
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint64_t> id(const Arguments& arguments,
                                              const std::string& operationId,
                                              const std::string& name,
                                              OperationCreateResult& error) {
    const auto* value = findArgument(arguments, name);
    if (value == nullptr) {
        error = failure(operationId, name, "Missing stable id argument");
        return std::nullopt;
    }
    if (const auto* result = std::get_if<StableId>(&value->storage)) {
        return result->value;
    }
    if (const auto* result = std::get_if<std::int64_t>(&value->storage);
        result != nullptr && *result > 0) {
        return static_cast<std::uint64_t>(*result);
    }
    error = failure(operationId, name, "Expected a positive stable id");
    return std::nullopt;
}

[[nodiscard]] std::optional<document::Vec2d>
vec2(const Arguments& arguments, const std::string& operationId, const std::string& name,
     OperationCreateResult& error, const bool required = true) {
    const auto* value = findArgument(arguments, name);
    if (value == nullptr) {
        if (!required) {
            return std::nullopt;
        }
        error = failure(operationId, name, "Missing vec2 argument");
        return std::nullopt;
    }
    if (const auto* result = std::get_if<document::Vec2d>(&value->storage)) {
        return *result;
    }
    if (const auto* array = std::get_if<ValueArray>(&value->storage);
        array != nullptr && array->size() == 2) {
        const auto x = numeric((*array)[0]);
        const auto y = numeric((*array)[1]);
        if (!x.has_value() || !y.has_value()) {
            error = failure(operationId, name, "Vec2 components must be numbers");
            return std::nullopt;
        }
        return document::Vec2d{*x, *y};
    }
    error = failure(operationId, name, "Expected a vec2 value");
    return std::nullopt;
}

[[nodiscard]] std::optional<core::Color4d>
color4(const Arguments& arguments, const std::string& operationId, const std::string& name,
       OperationCreateResult& error, const bool required = true) {
    const auto* value = findArgument(arguments, name);
    if (value == nullptr) {
        if (!required) {
            return std::nullopt;
        }
        error = failure(operationId, name, "Missing color4 argument");
        return std::nullopt;
    }
    if (const auto* result = std::get_if<core::Color4d>(&value->storage)) {
        return *result;
    }
    if (const auto* array = std::get_if<ValueArray>(&value->storage);
        array != nullptr && array->size() == 4) {
        std::array<double, 4> channels{};
        for (std::size_t index = 0; index < channels.size(); ++index) {
            const auto channel = numeric((*array)[index]);
            if (!channel.has_value()) {
                error = failure(operationId, name, "Color components must be numbers");
                return std::nullopt;
            }
            channels[index] = *channel;
        }
        const core::Color4d result{channels[0], channels[1], channels[2], channels[3]};
        if (!result.isValid()) {
            error = failure(operationId, name, "Color alpha must be in the range 0 to 1");
            return std::nullopt;
        }
        return result;
    }
    error = failure(operationId, name, "Expected a color4 value");
    return std::nullopt;
}

[[nodiscard]] std::optional<core::RationalTime> rational(const Arguments& arguments,
                                                         const std::string& operationId,
                                                         const std::string& name,
                                                         OperationCreateResult& error) {
    const auto* value = findArgument(arguments, name);
    if (value == nullptr) {
        error = failure(operationId, name, "Missing rational time argument");
        return std::nullopt;
    }
    if (const auto* integerValue = std::get_if<std::int64_t>(&value->storage)) {
        return core::RationalTime::fromInteger(*integerValue);
    }
    if (const auto* array = std::get_if<ValueArray>(&value->storage);
        array != nullptr && array->size() == 2) {
        const auto* numerator = std::get_if<std::int64_t>(&(*array)[0].storage);
        const auto* denominator = std::get_if<std::int64_t>(&(*array)[1].storage);
        if (numerator != nullptr && denominator != nullptr) {
            return core::RationalTime::create(*numerator, *denominator);
        }
    }
    error = failure(operationId, name, "Expected an integer frame or [numerator, denominator]");
    return std::nullopt;
}

[[nodiscard]] std::optional<document::CompositionFormat>
format(const Arguments& arguments, const std::string& operationId, OperationCreateResult& error) {
    const auto width = integer(arguments, operationId, "width", error);
    const auto height = integer(arguments, operationId, "height", error);
    if (!width.has_value() || !height.has_value() || *width <= 0 || *height <= 0 ||
        static_cast<std::uint64_t>(*width) > std::numeric_limits<std::uint32_t>::max() ||
        static_cast<std::uint64_t>(*height) > std::numeric_limits<std::uint32_t>::max()) {
        if (!error) {
            error = failure(operationId, "format", "Invalid composition dimensions");
        }
        return std::nullopt;
    }
    const auto frameRateNumerator =
        number(arguments, operationId, "frameRateNumerator", error, false);
    const auto frameRateDenominator =
        number(arguments, operationId, "frameRateDenominator", error, false);
    const auto frameRate = document::FrameRate::create(
        frameRateNumerator.has_value() ? static_cast<std::uint32_t>(*frameRateNumerator) : 24U,
        frameRateDenominator.has_value() ? static_cast<std::uint32_t>(*frameRateDenominator) : 1U);
    if (!frameRate.has_value()) {
        error = failure(operationId, "frameRate", "Invalid frame rate");
        return std::nullopt;
    }
    return document::CompositionFormat::create(static_cast<std::uint32_t>(*width),
                                               static_cast<std::uint32_t>(*height),
                                               core::PixelAspectRatio::square(), *frameRate);
}

[[nodiscard]] OperationFactory unsupportedFactory() {
    return [](const std::string& operationId, const Arguments&) {
        return failure(operationId, {},
                       "The operation is registered but its argument adapter is "
                       "not available in this host build");
    };
}

void addDescriptor(std::vector<OperationDescriptor>& descriptors, std::string id,
                   std::vector<ArgumentSchema> arguments, OperationFactory factory) {
    descriptors.push_back({.schema = {.typeId = std::move(id), .arguments = std::move(arguments)},
                           .factory = std::move(factory)});
}

} // namespace

OperationRegistry OperationRegistry::builtIn() {
    OperationRegistry registry;
    const auto unsupported = unsupportedFactory();
    const auto ids = std::to_array<std::string_view>(
        {"bloom.animation.convert-to-constant",
         "bloom.animation.create-for-parameter",
         "bloom.animation.delete-keyframe",
         "bloom.animation.delete-keyframes",
         "bloom.animation.insert-color4-keyframe",
         "bloom.animation.insert-scalar-keyframe",
         "bloom.animation.insert-vec2-keyframe",
         "bloom.animation.insert-vec3-keyframe",
         "bloom.animation.move-keyframes",
         "bloom.animation.paste-keyframes",
         "bloom.animation.set-keyframe-at-time",
         "bloom.animation.set-keyframe-at-time-for-parameter",
         "bloom.animation.set-keyframe-at-time-for-parameter-component",
         "bloom.animation.set-keyframe-handles",
         "bloom.animation.set-keyframe-interpolation",
         "bloom.animation.set-keyframe-values",
         "bloom.animation.set-keyframes-interpolation",
         "bloom.animation.update-color4-keyframe",
         "bloom.animation.update-scalar-keyframe",
         "bloom.animation.update-vec2-keyframe",
         "bloom.animation.update-vec3-keyframe",
         "bloom.asset.create-folder",
         "bloom.asset.ensure-font",
         "bloom.asset.import",
         "bloom.asset.move",
         "bloom.asset.relink",
         "bloom.asset.relink-font",
         "bloom.asset.remove",
         "bloom.asset.remove-folder",
         "bloom.asset.rename",
         "bloom.asset.rename-folder",
         "bloom.asset.reorder",
         "bloom.asset.set-tags",
         "bloom.data-block.create",
         "bloom.data-block.replace-payload",
         "bloom.data-block.remove",
         "bloom.data-block.relink",
         "bloom.data-block.set-tags",
         "bloom.composition.add",
         "bloom.composition.clear-work-area",
         "bloom.composition.delete",
         "bloom.composition.duplicate",
         "bloom.composition.set-background-color",
         "bloom.composition.set-duration",
         "bloom.composition.set-format",
         "bloom.composition.set-name",
         "bloom.composition.set-safe-areas",
         "bloom.composition.set-work-area",
         "bloom.layer.add-audio",
         "bloom.layer.add-image",
         "bloom.layer.add-shape",
         "bloom.layer.add-solid",
         "bloom.layer.add-text",
         "bloom.layer.move-before",
         "bloom.layer.rename",
         "bloom.layer.set-enabled",
         "bloom.layer.set-label-color",
         "bloom.layer.set-locked",
         "bloom.layer.set-parent",
         "bloom.layer.set-range",
         "bloom.layer.set-solo",
         "bloom.layer.split-at-time",
         "bloom.merge.reorder-input",
         "bloom.merge.set-enabled",
         "bloom.node-group.create",
         "bloom.node-group.remove",
         "bloom.node-group.rename",
         "bloom.node-group.set-members",
         "bloom.node.add",
         "bloom.node.connect",
         "bloom.node.disconnect-input",
         "bloom.node.dissolve",
         "bloom.node.duplicate",
         "bloom.node.move",
         "bloom.node.remove",
         "bloom.node.set-collapsed",
         "bloom.node.set-muted",
         "bloom.node.set-width",
         "bloom.parameter.set-source",
         "bloom.project.set-name"});
    for (const auto id : ids) {
        addDescriptor(registry.descriptors_, std::string(id), {}, unsupported);
    }

    const auto replaceFactory = [&registry](std::string_view id, OperationFactory factory,
                                            std::vector<ArgumentSchema> arguments) {
        const auto iterator =
            std::ranges::find(registry.descriptors_, id,
                              [](const auto& descriptor) { return descriptor.schema.typeId; });
        if (iterator != registry.descriptors_.end()) {
            iterator->schema.arguments = std::move(arguments);
            iterator->factory = std::move(factory);
        }
    };

    replaceFactory("bloom.project.set-name",
                   [](const std::string& operationId, const Arguments& arguments) {
                       OperationCreateResult error(nullptr, std::nullopt);
                       const auto name = text(arguments, operationId, "name", error);
                       return name.has_value()
                                  ? OperationCreateResult(
                                        std::make_unique<commands::SetProjectName>(*name),
                                        std::nullopt)
                                  : std::move(error);
                   },
                   {{"name", ValueKind::String, true}});

    replaceFactory(
        "bloom.composition.add",
        [](const std::string& operationId, const Arguments& arguments) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto name = text(arguments, operationId, "name", error);
            const auto compositionFormat = format(arguments, operationId, error);
            const auto duration = rational(arguments, operationId, "duration", error);
            if (!name.has_value() || !compositionFormat.has_value() || !duration.has_value()) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::AddComposition>(*name, *compositionFormat, *duration),
                std::nullopt);
        },
        {{"name", ValueKind::String, true},
         {"width", ValueKind::Integer, true},
         {"height", ValueKind::Integer, true},
         {"duration", ValueKind::Array, true},
         {"frameRateNumerator", ValueKind::Integer, false},
         {"frameRateDenominator", ValueKind::Integer, false}});

    replaceFactory("bloom.composition.set-name",
                   [](const std::string& operationId, const Arguments& arguments) {
                       OperationCreateResult error(nullptr, std::nullopt);
                       const auto composition = id(arguments, operationId, "composition", error);
                       const auto name = text(arguments, operationId, "name", error);
                       if (!composition.has_value() || !name.has_value()) {
                           return error;
                       }
                       return OperationCreateResult(
                           std::make_unique<commands::SetCompositionName>(
                               document::CompositionId::fromRaw(*composition), *name),
                           std::nullopt);
                   },
                   {{"composition", ValueKind::Id, true}, {"name", ValueKind::String, true}});

    replaceFactory("bloom.composition.delete",
                   [](const std::string& operationId, const Arguments& arguments) {
                       OperationCreateResult error(nullptr, std::nullopt);
                       const auto composition = id(arguments, operationId, "composition", error);
                       return composition.has_value()
                                  ? OperationCreateResult(
                                        std::make_unique<commands::DeleteComposition>(
                                            document::CompositionId::fromRaw(*composition)),
                                        std::nullopt)
                                  : std::move(error);
                   },
                   {{"composition", ValueKind::Id, true}});

    replaceFactory("bloom.composition.duplicate",
                   [](const std::string& operationId, const Arguments& arguments) {
                       OperationCreateResult error(nullptr, std::nullopt);
                       const auto composition = id(arguments, operationId, "composition", error);
                       return composition.has_value()
                                  ? OperationCreateResult(
                                        std::make_unique<commands::DuplicateComposition>(
                                            document::CompositionId::fromRaw(*composition)),
                                        std::nullopt)
                                  : std::move(error);
                   },
                   {{"composition", ValueKind::Id, true}});

    replaceFactory("bloom.layer.add-solid",
                   [](const std::string& operationId, const Arguments& arguments) {
                       OperationCreateResult error(nullptr, std::nullopt);
                       const auto composition = id(arguments, operationId, "composition", error);
                       const auto name = text(arguments, operationId, "name", error);
                       const auto color = color4(arguments, operationId, "color", error);
                       const auto position = vec2(arguments, operationId, "position", error, false);
                       const auto opacity = number(arguments, operationId, "opacity", error, false);
                       if (!composition.has_value() || !name.has_value() || !color.has_value()) {
                           return error;
                       }
                       const auto compositionId = document::CompositionId::fromRaw(*composition);
                       if (position.has_value()) {
                           return OperationCreateResult(
                               std::make_unique<commands::AddSolidLayer>(
                                   compositionId, *name, *color, *position, opacity.value_or(1.0)),
                               std::nullopt);
                       }
                       return OperationCreateResult(
                           std::make_unique<commands::AddSolidLayer>(compositionId, *name, *color),
                           std::nullopt);
                   },
                   {{"composition", ValueKind::Id, true},
                    {"name", ValueKind::String, true},
                    {"color", ValueKind::Color4, true},
                    {"position", ValueKind::Vec2, false},
                    {"opacity", ValueKind::Double, false}});

    replaceFactory("bloom.layer.add-text",
                   [](const std::string& operationId, const Arguments& arguments) {
                       OperationCreateResult error(nullptr, std::nullopt);
                       const auto composition = id(arguments, operationId, "composition", error);
                       const auto name = text(arguments, operationId, "name", error);
                       const auto content = text(arguments, operationId, "text", error);
                       const auto position = vec2(arguments, operationId, "position", error, false);
                       const auto opacity = number(arguments, operationId, "opacity", error, false);
                       const auto size = number(arguments, operationId, "size", error, false);
                       const auto color = color4(arguments, operationId, "color", error, false);
                       if (!composition.has_value() || !name.has_value() || !content.has_value()) {
                           return error;
                       }
                       const auto compositionId = document::CompositionId::fromRaw(*composition);
                       if (position.has_value() || opacity.has_value() || size.has_value() ||
                           color.has_value()) {
                           return OperationCreateResult(
                               std::make_unique<commands::AddTextLayer>(
                                   compositionId, *name, *content,
                                   position.value_or(document::Vec2d{}), opacity.value_or(1.0),
                                   size.value_or(72.0),
                                   color.value_or(core::Color4d{1.0, 1.0, 1.0, 1.0})),
                               std::nullopt);
                       }
                       return OperationCreateResult(
                           std::make_unique<commands::AddTextLayer>(compositionId, *name, *content),
                           std::nullopt);
                   },
                   {{"composition", ValueKind::Id, true},
                    {"name", ValueKind::String, true},
                    {"text", ValueKind::String, true},
                    {"position", ValueKind::Vec2, false},
                    {"opacity", ValueKind::Double, false},
                    {"size", ValueKind::Double, false},
                    {"color", ValueKind::Color4, false}});

    replaceFactory(
        "bloom.node.add",
        [](const std::string& operationId, const Arguments& arguments) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(arguments, operationId, "composition", error);
            const auto nodeType = text(arguments, operationId, "nodeType", error);
            const auto position = vec2(arguments, operationId, "position", error);
            if (!composition.has_value() || !nodeType.has_value() || !position.has_value()) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::AddNode>(document::CompositionId::fromRaw(*composition),
                                                    *nodeType, *position),
                std::nullopt);
        },
        {{"composition", ValueKind::Id, true},
         {"nodeType", ValueKind::String, true},
         {"position", ValueKind::Vec2, true}});

    replaceFactory("bloom.data-block.remove",
                   [](const std::string& operationId, const Arguments& arguments) {
                       OperationCreateResult error(nullptr, std::nullopt);
                       const auto block = id(arguments, operationId, "dataBlock", error);
                       if (!block.has_value())
                           return error;
                       return OperationCreateResult(
                           std::make_unique<commands::RemoveDataBlock>(
                               document::DataBlockRecordId::fromRaw(*block)),
                           std::nullopt);
                   },
                   {{"dataBlock", ValueKind::Id, true}});

    replaceFactory("bloom.data-block.set-tags",
                   [](const std::string& operationId, const Arguments& arguments) {
                       OperationCreateResult error(nullptr, std::nullopt);
                       const auto block = id(arguments, operationId, "dataBlock", error);
                       const auto* value = findArgument(arguments, "tags");
                       if (!block.has_value() || value == nullptr)
                           return error;
                       const auto* array = std::get_if<ValueArray>(&value->storage);
                       if (array == nullptr)
                           return failure(operationId, "tags", "Expected an array of strings");
                       std::vector<std::string> tags;
                       for (const auto& entry : *array) {
                           const auto* tag = std::get_if<std::string>(&entry.storage);
                           if (tag == nullptr)
                               return failure(operationId, "tags", "Expected an array of strings");
                           tags.push_back(*tag);
                       }
                       return OperationCreateResult(
                           std::make_unique<commands::SetDataBlockTags>(
                               document::DataBlockRecordId::fromRaw(*block), std::move(tags)),
                           std::nullopt);
                   },
                   {{"dataBlock", ValueKind::Id, true}, {"tags", ValueKind::Array, true}});

    return registry;
}

const OperationDescriptor* OperationRegistry::find(const std::string_view typeId) const noexcept {
    const auto iterator = std::ranges::find(
        descriptors_, typeId, [](const auto& descriptor) { return descriptor.schema.typeId; });
    return iterator == descriptors_.end() ? nullptr : &*iterator;
}

OperationCreateResult OperationRegistry::create(const std::string_view typeId,
                                                const Arguments& arguments) const {
    const auto* descriptor = find(typeId);
    if (descriptor == nullptr) {
        return failure(std::string(typeId), {}, "Unknown operation id");
    }
    for (const auto& argument : descriptor->schema.arguments) {
        if (argument.required && arguments.find(argument.name) == arguments.end()) {
            return failure(descriptor->schema.typeId, argument.name, "Missing required argument");
        }
    }
    return descriptor->factory(descriptor->schema.typeId, arguments);
}

} // namespace bloom::scripting
