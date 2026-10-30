#include <bloom/ui/workspace_presets.hpp>

#include <bloom/ui/kit/dialog_prompts.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <functional>

namespace bloom::ui {
namespace {

constexpr auto userPresetsKey = "workspace/presets/user";
constexpr auto activePresetKey = "workspace/presets/active";
// How far a divider may sit from its preset's weight before the layout counts as edited. A
// splitter rounds to pixels and honours panel minimum widths, so an exact match is not expected.
constexpr double weightTolerance = 0.06;

QJsonObject area(const char* editorId, const bool active = false) {
    return {{"type", "area"},
            {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"editor", QString::fromLatin1(editorId)},
            {"active", active}};
}

QJsonObject split(const Qt::Orientation orientation, std::initializer_list<double> weights,
                  std::initializer_list<QJsonObject> children) {
    QJsonArray weightArray;
    for (const double weight : weights)
        weightArray.push_back(weight);
    QJsonArray childArray;
    for (const auto& child : children)
        childArray.push_back(child);
    return {{"type", "split"},
            {"orientation", orientation == Qt::Horizontal ? "horizontal" : "vertical"},
            {"weights", weightArray},
            {"children", childArray}};
}

QByteArray layout(const QJsonObject& root) {
    return QJsonDocument(
               QJsonObject{{"format", "bloom.workspace-layout"}, {"schema", 2}, {"root", root}})
        .toJson(QJsonDocument::Compact);
}

// The arrangement of a layout state with identities removed: topology, editor types and weights.
struct Shape final {
    bool isArea = false;
    QString editor;
    bool horizontal = false;
    std::vector<double> weights;
    std::vector<Shape> children;
};

std::optional<Shape> shapeOf(const QJsonObject& node) {
    Shape shape;
    if (node.value("type").toString() == "area") {
        shape.isArea = true;
        shape.editor = node.value("editor").toString();
        return shape;
    }
    shape.horizontal = node.value("orientation").toString() == "horizontal";
    for (const auto& weight : node.value("weights").toArray())
        shape.weights.push_back(weight.toDouble());
    for (const auto& child : node.value("children").toArray()) {
        auto childShape = shapeOf(child.toObject());
        if (!childShape)
            return std::nullopt;
        shape.children.push_back(std::move(*childShape));
    }
    if (shape.weights.size() != shape.children.size())
        return std::nullopt;
    return shape;
}

bool matches(const Shape& a, const Shape& b) {
    if (a.isArea != b.isArea)
        return false;
    if (a.isArea)
        return a.editor == b.editor;
    if (a.horizontal != b.horizontal || a.children.size() != b.children.size())
        return false;
    for (std::size_t index = 0; index < a.children.size(); ++index)
        if (std::abs(a.weights[index] - b.weights[index]) > weightTolerance ||
            !matches(a.children[index], b.children[index]))
            return false;
    return true;
}

std::optional<Shape> shapeOf(const QByteArray& state) {
    const auto document = QJsonDocument::fromJson(state);
    if (!document.isObject())
        return std::nullopt;
    return shapeOf(document.object().value("root").toObject());
}

} // namespace

WorkspacePresets::WorkspacePresets(WorkspaceHost& host, QObject* parent)
    : QObject(parent), host_(host) {
    using Qt::Horizontal;
    using Qt::Vertical;
    // Default: Assets left, Viewer centre, Properties right; the Timeline under Viewer and Assets.
    builtIn_.push_back(
        {tr("Default"), true,
         layout(split(Horizontal, {0.8, 0.2},
                      {split(Vertical, {0.66, 0.34},
                             {split(Horizontal, {0.25, 0.75},
                                    {area("bloom.assets"), area("bloom.viewer", true)}),
                              area("bloom.timeline")}),
                       area("bloom.properties")}))});
    // Node: Viewer left, Nodes right, Timeline across the bottom.
    builtIn_.push_back({tr("Node"), true,
                        layout(split(Vertical, {0.8, 0.2},
                                     {split(Horizontal, {0.5, 0.5},
                                            {area("bloom.viewer", true), area("bloom.nodes")}),
                                      area("bloom.timeline")}))});
    // Export: Assets left, Export right.
    builtIn_.push_back({tr("Export"), true,
                        layout(split(Horizontal, {0.25, 0.75},
                                     {area("bloom.assets", true), area("bloom.export")}))});
    // Animation: Viewer and Properties over a tall Timeline.
    builtIn_.push_back({tr("Animation"), true,
                        layout(split(Vertical, {0.5, 0.5},
                                     {split(Horizontal, {0.75, 0.25},
                                            {area("bloom.viewer", true), area("bloom.properties")}),
                                      area("bloom.timeline")}))});
    load();
    connect(&host_, &WorkspaceHost::layoutChanged, this, [this] {
        if (!applying_)
            refreshEdited();
    });
}

const WorkspacePreset* WorkspacePresets::find(const QString& name) const {
    for (const auto* list : {&builtIn_, &user_}) {
        const auto found = std::ranges::find(*list, name, &WorkspacePreset::name);
        if (found != list->end())
            return &*found;
    }
    return nullptr;
}

QString WorkspacePresets::label() const {
    if (active_.isEmpty())
        return tr("Custom");
    return edited_ ? tr("%1 (edited)").arg(active_) : active_;
}

bool WorkspacePresets::activeIsUser() const {
    return !active_.isEmpty() && find(active_) != nullptr && !find(active_)->builtIn;
}

bool WorkspacePresets::apply(const QString& name, const bool remember) {
    const auto* preset = find(name);
    if (preset == nullptr)
        return false;
    applying_ = true;
    const auto result = host_.applyArrangement(preset->layout);
    applying_ = false;
    if (result != WorkspaceLayoutRestoreResult::Restored &&
        result != WorkspaceLayoutRestoreResult::Deferred)
        return false;
    active_ = preset->name;
    edited_ = false;
    if (remember)
        persist();
    emit changed();
    return true;
}

std::optional<QString> WorkspacePresets::validateName(const QString& name) const {
    const auto trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return tr("Give the layout a name.");
    if (trimmed.compare(tr("Custom"), Qt::CaseInsensitive) == 0 ||
        std::ranges::any_of(builtIn_, [&](const WorkspacePreset& preset) {
            return preset.name.compare(trimmed, Qt::CaseInsensitive) == 0;
        }))
        return tr("That name belongs to a built-in workspace.");
    if (std::ranges::any_of(user_, [&](const WorkspacePreset& preset) {
            return preset.name.compare(trimmed, Qt::CaseInsensitive) == 0;
        }))
        return tr("A saved layout already has that name.");
    return std::nullopt;
}

bool WorkspacePresets::saveCurrentAs(const QString& name) {
    if (validateName(name))
        return false;
    user_.push_back({name.trimmed(), false, host_.saveLayoutState()});
    active_ = user_.back().name;
    edited_ = false;
    persist();
    emit changed();
    return true;
}

bool WorkspacePresets::updateActive() {
    const auto found = std::ranges::find(user_, active_, &WorkspacePreset::name);
    if (found == user_.end())
        return false;
    found->layout = host_.saveLayoutState();
    edited_ = false;
    persist();
    emit changed();
    return true;
}

bool WorkspacePresets::removeUser(const QString& name) {
    const auto found = std::ranges::find(user_, name, &WorkspacePreset::name);
    if (found == user_.end())
        return false;
    user_.erase(found);
    if (active_ == name) {
        active_.clear();
        edited_ = false;
    }
    persist();
    emit changed();
    return true;
}

void WorkspacePresets::clearActive() {
    if (active_.isEmpty())
        return;
    active_.clear();
    edited_ = false;
    persist();
    emit changed();
}

void WorkspacePresets::promptSaveAs(QWidget& owner) {
    kit::KTextPromptSpec spec;
    spec.title = tr("Save workspace");
    spec.objectName = QStringLiteral("workspaceSavePrompt");
    spec.label = tr("Name");
    spec.placeholder = tr("My layout");
    spec.acceptText = tr("Save");
    spec.validate = [this](const QString& text) { return validateName(text); };
    kit::promptText(owner, spec, [this](const std::optional<QString> name) {
        if (name)
            (void)saveCurrentAs(*name);
    });
}

void WorkspacePresets::promptDeleteActive(QWidget& owner) {
    if (!activeIsUser())
        return;
    kit::KConfirmSpec spec;
    spec.title = tr("Delete workspace");
    spec.objectName = QStringLiteral("workspaceDeletePrompt");
    spec.tone = kit::KPromptTone::Warning;
    spec.heading = tr("Delete the workspace “%1”?").arg(active_);
    spec.detail = tr("The panels on screen stay as they are.");
    spec.destructiveText = tr("Delete");
    spec.rejectText = tr("Cancel");
    spec.defaultRole = kit::KDialog::Role::Reject;
    const auto name = active_;
    kit::confirm(owner, spec, [this, name](const kit::KDialog::Role role) {
        if (role == kit::KDialog::Role::Destructive)
            (void)removeUser(name);
    });
}

void WorkspacePresets::load() {
    QSettings settings;
    user_.clear();
    const auto document = QJsonDocument::fromJson(settings.value(userPresetsKey).toByteArray());
    for (const auto& value : document.array()) {
        const auto object = value.toObject();
        const auto name = object.value("name").toString();
        const auto state =
            QJsonDocument(object.value("layout").toObject()).toJson(QJsonDocument::Compact);
        if (!name.isEmpty() && !shapeOf(state).has_value())
            continue;
        if (!name.isEmpty() && find(name) == nullptr)
            user_.push_back({name, false, state});
    }
    // Nothing stored yet (first run, or a layout from before presets): the layout is the Default's.
    active_ = settings.contains(activePresetKey) ? settings.value(activePresetKey).toString()
                                                 : tr("Default");
    if (find(active_) == nullptr)
        active_.clear();
    refreshEdited();
}

void WorkspacePresets::persist() const {
    QSettings settings;
    QJsonArray list;
    for (const auto& preset : user_)
        list.push_back(QJsonObject{{"name", preset.name},
                                   {"layout", QJsonDocument::fromJson(preset.layout).object()}});
    settings.setValue(userPresetsKey, QJsonDocument(list).toJson(QJsonDocument::Compact));
    settings.setValue(activePresetKey, active_);
}

void WorkspacePresets::refreshEdited() {
    bool edited = false;
    if (const auto* preset = find(active_); preset != nullptr) {
        const auto wanted = shapeOf(preset->layout);
        const auto live = shapeOf(host_.saveLayoutState());
        edited = !(wanted && live && matches(*wanted, *live));
    }
    if (edited != edited_) {
        edited_ = edited;
        emit changed();
    }
}

} // namespace bloom::ui
