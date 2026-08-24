#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/validation.hpp>

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bloom::document {

class Composition final {
  public:
    Composition(CompositionId id, std::string name, core::RationalTime duration,
                CanonicalGraph graph, CompositionFormat format = {})
        : id_(id), name_(std::move(name)), duration_(duration), format_(format),
          graph_(std::move(graph)) {}

    [[nodiscard]] CompositionId id() const noexcept { return id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] core::RationalTime duration() const noexcept { return duration_; }
    [[nodiscard]] CompositionFormat format() const noexcept { return format_; }
    [[nodiscard]] const ParameterStore& parameters() const noexcept { return parameters_; }
    [[nodiscard]] ParameterStore& parameters() noexcept { return parameters_; }
    [[nodiscard]] const CanonicalGraph& graph() const noexcept { return graph_; }
    [[nodiscard]] CanonicalGraph& graph() noexcept { return graph_; }

    void setName(std::string name) { name_ = std::move(name); }
    [[nodiscard]] bool setDuration(core::RationalTime duration) noexcept;
    void setFormat(CompositionFormat format) noexcept { format_ = format; }

    [[nodiscard]] ValidationResult validate() const;

  private:
    CompositionId id_;
    std::string name_;
    core::RationalTime duration_;
    CompositionFormat format_;
    ParameterStore parameters_;
    CanonicalGraph graph_;
};

class Project final {
  public:
    Project(ProjectId id, std::string name) : id_(id), name_(std::move(name)) {}

    [[nodiscard]] ProjectId id() const noexcept { return id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] std::span<const Composition> compositions() const noexcept {
        return compositions_;
    }
    [[nodiscard]] const Composition* findComposition(CompositionId id) const noexcept;
    [[nodiscard]] Composition* findComposition(CompositionId id) noexcept;

    void setName(std::string name) { name_ = std::move(name); }
    [[nodiscard]] bool addComposition(Composition composition);
    [[nodiscard]] bool removeComposition(CompositionId id);

    [[nodiscard]] ValidationResult validate() const;

  private:
    ProjectId id_;
    std::string name_;
    std::vector<Composition> compositions_;
};

} // namespace bloom::document
