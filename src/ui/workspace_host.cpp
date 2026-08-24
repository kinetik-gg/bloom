#include <bloom/ui/workspace_host.hpp>

#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QString>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace {

constexpr auto layoutFormat = "bloom.workspace-layout";
constexpr int layoutSchema = 1;
constexpr int maximumLayoutDepth = 64;
constexpr int maximumAreaCount = 64;
constexpr int defaultSplitWeight = 1000;

bool containsArea(QWidget* widget, const bloom::ui::EditorArea* area) {
    if (widget == area) {
        return true;
    }

    const auto* splitter = qobject_cast<const QSplitter*>(widget);
    if (splitter == nullptr) {
        return false;
    }

    for (int index = 0; index < splitter->count(); ++index) {
        if (containsArea(splitter->widget(index), area)) {
            return true;
        }
    }
    return false;
}

int countAreas(QWidget* widget) {
    if (qobject_cast<bloom::ui::EditorArea*>(widget) != nullptr) {
        return 1;
    }

    auto* splitter = qobject_cast<QSplitter*>(widget);
    if (splitter == nullptr) {
        return 0;
    }

    int count = 0;
    for (int index = 0; index < splitter->count(); ++index) {
        count += countAreas(splitter->widget(index));
    }
    return count;
}

bloom::ui::EditorArea* firstArea(QWidget* widget) {
    if (auto* area = qobject_cast<bloom::ui::EditorArea*>(widget); area != nullptr) {
        return area;
    }

    auto* splitter = qobject_cast<QSplitter*>(widget);
    if (splitter == nullptr) {
        return nullptr;
    }

    for (int index = 0; index < splitter->count(); ++index) {
        if (auto* area = firstArea(splitter->widget(index)); area != nullptr) {
            return area;
        }
    }
    return nullptr;
}

void collectAreas(QWidget* widget, QList<bloom::ui::EditorArea*>& areas) {
    if (auto* area = qobject_cast<bloom::ui::EditorArea*>(widget); area != nullptr) {
        areas.push_back(area);
        return;
    }

    auto* splitter = qobject_cast<QSplitter*>(widget);
    if (splitter == nullptr) {
        return;
    }

    for (int index = 0; index < splitter->count(); ++index) {
        collectAreas(splitter->widget(index), areas);
    }
}

QList<int> usableSizes(const QSplitter& splitter) {
    auto sizes = splitter.sizes();
    if (sizes.size() != splitter.count() ||
        std::ranges::all_of(sizes, [](int size) { return size <= 0; })) {
        sizes.fill(defaultSplitWeight, splitter.count());
    }
    return sizes;
}

void captureSplitterSizes(QWidget* widget, QHash<QSplitter*, QList<int>>& sizes) {
    auto* splitter = qobject_cast<QSplitter*>(widget);
    if (splitter == nullptr) {
        return;
    }

    sizes.insert(splitter, usableSizes(*splitter));
    for (int index = 0; index < splitter->count(); ++index) {
        captureSplitterSizes(splitter->widget(index), sizes);
    }
}

bool showOnlyArea(QWidget* widget, const bloom::ui::EditorArea* area) {
    if (qobject_cast<bloom::ui::EditorArea*>(widget) != nullptr) {
        const bool matches = widget == area;
        widget->setVisible(matches);
        return matches;
    }

    auto* splitter = qobject_cast<QSplitter*>(widget);
    if (splitter == nullptr) {
        widget->hide();
        return false;
    }

    bool contains = false;
    for (int index = 0; index < splitter->count(); ++index) {
        auto* child = splitter->widget(index);
        const bool childContains = containsArea(child, area);
        child->setVisible(childContains);
        if (childContains) {
            showOnlyArea(child, area);
            contains = true;
        }
    }
    splitter->setVisible(contains);
    return contains;
}

void showEntireTree(QWidget* widget) {
    widget->show();
    auto* splitter = qobject_cast<QSplitter*>(widget);
    if (splitter == nullptr) {
        return;
    }

    for (int index = 0; index < splitter->count(); ++index) {
        showEntireTree(splitter->widget(index));
    }
}

QJsonObject serializeNode(QWidget* widget, const bloom::ui::EditorArea* activeArea,
                          const QHash<QSplitter*, QList<int>>& preservedSizes) {
    if (const auto* area = qobject_cast<const bloom::ui::EditorArea*>(widget); area != nullptr) {
        return {{"type", "area"},
                {"id", area->areaId()},
                {"editor", QString::fromStdString(area->editorId())},
                {"active", area == activeArea}};
    }

    const auto* splitter = qobject_cast<const QSplitter*>(widget);
    if (splitter == nullptr) {
        return {};
    }

    QJsonArray children;
    for (int index = 0; index < splitter->count(); ++index) {
        children.push_back(serializeNode(splitter->widget(index), activeArea, preservedSizes));
    }

    const auto storedSizes = preservedSizes.constFind(const_cast<QSplitter*>(splitter));
    const QList<int> sizes =
        storedSizes == preservedSizes.cend() ? usableSizes(*splitter) : storedSizes.value();
    qint64 totalSize = 0;
    for (int size : sizes) {
        totalSize += std::max(size, 0);
    }
    if (totalSize <= 0) {
        totalSize = sizes.size();
    }

    QJsonArray weights;
    for (int size : sizes) {
        const double weight =
            totalSize == sizes.size() && size <= 0
                ? 1.0 / static_cast<double>(sizes.size())
                : static_cast<double>(std::max(size, 0)) / static_cast<double>(totalSize);
        weights.push_back(weight);
    }

    return {{"type", "split"},
            {"orientation", splitter->orientation() == Qt::Horizontal ? "horizontal" : "vertical"},
            {"weights", weights},
            {"children", children}};
}

} // namespace

namespace bloom::ui {

WorkspaceHost::WorkspaceHost(const EditorRegistry& editorRegistry, QWidget* parent)
    : QFrame(parent), editorRegistry_(editorRegistry) {
    setObjectName("workspaceHost");
    setFrameShape(QFrame::NoFrame);

    rootLayout_ = new QVBoxLayout(this);
    rootLayout_->setContentsMargins(0, 0, 0, 0);
    rootLayout_->setSpacing(0);

    rootWidget_ = createArea();
    rootLayout_->addWidget(rootWidget_);
    setActiveArea(qobject_cast<EditorArea*>(rootWidget_));
    updateAreaControls();
}

EditorArea* WorkspaceHost::activeArea() const noexcept { return activeArea_; }

int WorkspaceHost::areaCount() const { return countAreas(rootWidget_); }

void WorkspaceHost::setActiveArea(EditorArea* area) {
    if (area == nullptr || !containsArea(rootWidget_, area) || activeArea_ == area ||
        (maximizedArea_ != nullptr && maximizedArea_ != area)) {
        return;
    }

    if (activeArea_ != nullptr) {
        activeArea_->setAreaActive(false);
    }
    activeArea_ = area;
    activeArea_->setAreaActive(true);
    emit activeAreaChanged(activeArea_);
}

void WorkspaceHost::resetToSingleArea(std::string editorId) {
    restoreMaximizedArea();
    auto* area = createArea(std::move(editorId));
    replaceRoot(area);
    activeArea_.clear();
    setActiveArea(area);
    updateAreaControls();
    emit areaCountChanged(1);
}

EditorArea* WorkspaceHost::splitActiveArea(Qt::Orientation orientation) {
    if (activeArea_ == nullptr) {
        return nullptr;
    }
    return splitArea(*activeArea_, orientation, activeArea_->editorId());
}

EditorArea* WorkspaceHost::splitArea(EditorArea& area, Qt::Orientation orientation,
                                     std::string initialEditorId, double newAreaFraction) {
    if (isAreaMaximized() || areaCount() >= maximumAreaCount || !containsArea(rootWidget_, &area)) {
        return nullptr;
    }

    if (initialEditorId.empty()) {
        initialEditorId = area.editorId();
    }
    newAreaFraction = std::clamp(newAreaFraction, 0.1, 0.9);

    auto* newArea = createArea(std::move(initialEditorId));
    auto* parentSplitter = qobject_cast<QSplitter*>(area.parentWidget());
    auto* newSplitter = createSplitter(orientation);
    QList<int> parentSizes;
    int parentIndex = -1;

    if (parentSplitter != nullptr) {
        parentSizes = usableSizes(*parentSplitter);
        parentIndex = parentSplitter->indexOf(&area);
        parentSplitter->insertWidget(parentIndex, newSplitter);
        area.hide();
        area.setParent(nullptr);
        parentSplitter->setSizes(parentSizes);
    } else {
        rootLayout_->removeWidget(&area);
        area.hide();
        area.setParent(nullptr);
        rootWidget_ = newSplitter;
        rootLayout_->addWidget(rootWidget_);
    }

    newSplitter->addWidget(&area);
    newSplitter->addWidget(newArea);
    area.show();
    const int newSize = static_cast<int>(std::lround(defaultSplitWeight * newAreaFraction));
    newSplitter->setSizes({defaultSplitWeight - newSize, newSize});

    setActiveArea(newArea);
    updateAreaControls();
    emit areaCountChanged(areaCount());
    return newArea;
}

bool WorkspaceHost::closeActiveArea() {
    if (activeArea_ == nullptr) {
        return false;
    }
    return closeArea(*activeArea_);
}

bool WorkspaceHost::closeArea(EditorArea& area) {
    if (isAreaMaximized() || areaCount() <= 1 || !containsArea(rootWidget_, &area)) {
        return false;
    }
    auto* parentSplitter = qobject_cast<QSplitter*>(area.parentWidget());
    if (parentSplitter == nullptr || parentSplitter->count() != 2) {
        return false;
    }

    const int areaIndex = parentSplitter->indexOf(&area);
    const int neighborIndex =
        areaIndex + 1 < parentSplitter->count() ? areaIndex + 1 : areaIndex - 1;
    auto* nextActiveArea = firstArea(parentSplitter->widget(neighborIndex));
    if (nextActiveArea == nullptr) {
        return false;
    }

    if (activeArea_ == &area) {
        activeArea_->setAreaActive(false);
        activeArea_.clear();
    }

    auto* survivor = parentSplitter->widget(1 - areaIndex);
    auto* grandparentSplitter = qobject_cast<QSplitter*>(parentSplitter->parentWidget());

    area.hide();
    area.setParent(nullptr);
    survivor->hide();
    survivor->setParent(nullptr);

    if (grandparentSplitter != nullptr) {
        const int parentIndex = grandparentSplitter->indexOf(parentSplitter);
        const auto grandparentSizes = usableSizes(*grandparentSplitter);
        grandparentSplitter->insertWidget(parentIndex, survivor);
        parentSplitter->hide();
        parentSplitter->setParent(nullptr);
        grandparentSplitter->setSizes(grandparentSizes);
    } else {
        rootLayout_->removeWidget(parentSplitter);
        parentSplitter->hide();
        parentSplitter->setParent(nullptr);
        rootWidget_ = survivor;
        rootLayout_->addWidget(rootWidget_);
    }

    survivor->show();
    parentSplitter->deleteLater();
    area.deleteLater();

    setActiveArea(nextActiveArea);
    updateAreaControls();
    emit areaCountChanged(areaCount());
    return true;
}

bool WorkspaceHost::isAreaMaximized() const noexcept { return maximizedArea_ != nullptr; }

void WorkspaceHost::toggleMaximizeActiveArea() {
    if (maximizedArea_ != nullptr) {
        restoreMaximizedArea();
        return;
    }
    if (activeArea_ == nullptr || areaCount() <= 1) {
        return;
    }

    preMaximizeSizes_.clear();
    captureSplitterSizes(rootWidget_, preMaximizeSizes_);
    maximizedArea_ = activeArea_;
    showOnlyArea(rootWidget_, maximizedArea_);
    updateAreaControls();
    emit maximizeStateChanged(true);
}

QByteArray WorkspaceHost::saveLayoutState() const {
    const QJsonObject documentRoot = {
        {"format", layoutFormat},
        {"schema", layoutSchema},
        {"root", serializeNode(rootWidget_, activeArea_, preMaximizeSizes_)},
    };
    return QJsonDocument(documentRoot).toJson(QJsonDocument::Compact);
}

WorkspaceLayoutRestoreResult WorkspaceHost::restoreLayoutState(const QByteArray& state) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(state, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return WorkspaceLayoutRestoreResult::Invalid;
    }

    const auto documentRoot = document.object();
    if (documentRoot.value("format").toString() != layoutFormat) {
        return WorkspaceLayoutRestoreResult::Invalid;
    }
    const int serializedSchema = documentRoot.value("schema").toInt(-1);
    if (serializedSchema > layoutSchema) {
        return WorkspaceLayoutRestoreResult::UnsupportedVersion;
    }
    if (serializedSchema != layoutSchema || !documentRoot.value("root").isObject()) {
        return WorkspaceLayoutRestoreResult::Invalid;
    }

    int createdAreaCount = 0;
    int activeAreaCount = 0;
    QSet<QString> restoredAreaIds;
    EditorArea* restoredActiveArea = nullptr;
    bool valid = true;

    std::function<QWidget*(const QJsonObject&, int)> buildNode = [&](const QJsonObject& node,
                                                                     int depth) -> QWidget* {
        if (!valid || depth > maximumLayoutDepth) {
            valid = false;
            return nullptr;
        }

        const auto type = node.value("type").toString();
        if (type == "area") {
            if (++createdAreaCount > maximumAreaCount) {
                valid = false;
                return nullptr;
            }

            const auto areaIdValue = node.value("id");
            const auto editorIdValue = node.value("editor");
            if (!areaIdValue.isString() || !editorIdValue.isString()) {
                valid = false;
                return nullptr;
            }

            const auto areaId = areaIdValue.toString();
            const auto editorId = editorIdValue.toString();
            const auto normalizedAreaId = areaId.toLower();
            if (!EditorArea::isValidAreaId(areaId) || restoredAreaIds.contains(normalizedAreaId) ||
                editorId.isEmpty() || !node.value("active").isBool()) {
                valid = false;
                return nullptr;
            }
            restoredAreaIds.insert(normalizedAreaId);

            const bool isActive = node.value("active").toBool(false);
            if (isActive && ++activeAreaCount > 1) {
                valid = false;
                return nullptr;
            }

            auto* area = createArea(editorId.toStdString(), areaId);
            if (isActive) {
                restoredActiveArea = area;
            }
            return area;
        }

        if (type != "split") {
            valid = false;
            return nullptr;
        }

        const auto orientationValue = node.value("orientation").toString();
        if (orientationValue != "horizontal" && orientationValue != "vertical") {
            valid = false;
            return nullptr;
        }

        const auto children = node.value("children").toArray();
        if (children.size() != 2) {
            valid = false;
            return nullptr;
        }

        auto* splitter =
            createSplitter(orientationValue == "horizontal" ? Qt::Horizontal : Qt::Vertical);
        for (const auto& childValue : children) {
            if (!childValue.isObject()) {
                valid = false;
                break;
            }
            auto* child = buildNode(childValue.toObject(), depth + 1);
            if (child == nullptr) {
                valid = false;
                break;
            }
            splitter->addWidget(child);
        }

        if (!valid) {
            delete splitter;
            return nullptr;
        }

        QList<int> sizes;
        const auto weights = node.value("weights").toArray();
        bool validWeights = weights.size() == children.size();
        double totalWeight = 0.0;
        for (const auto& weightValue : weights) {
            const double weight = weightValue.toDouble(-1.0);
            if (!std::isfinite(weight) || weight < 0.0) {
                validWeights = false;
                break;
            }
            totalWeight += weight;
        }
        validWeights = validWeights && std::isfinite(totalWeight) && totalWeight > 0.0;

        if (validWeights) {
            for (const auto& weightValue : weights) {
                sizes.push_back(std::max(0, static_cast<int>(std::lround(weightValue.toDouble() /
                                                                         totalWeight * 10000.0))));
            }
        } else {
            sizes.fill(defaultSplitWeight, children.size());
        }
        splitter->setSizes(sizes);
        return splitter;
    };

    auto* restoredRoot = buildNode(documentRoot.value("root").toObject(), 0);
    if (!valid || restoredRoot == nullptr || createdAreaCount < 1) {
        delete restoredRoot;
        return WorkspaceLayoutRestoreResult::Invalid;
    }

    restoreMaximizedArea();
    replaceRoot(restoredRoot);
    activeArea_.clear();
    setActiveArea(restoredActiveArea != nullptr ? restoredActiveArea : firstArea(rootWidget_));
    updateAreaControls();
    emit areaCountChanged(areaCount());
    return WorkspaceLayoutRestoreResult::Restored;
}

void WorkspaceHost::persistLayout(QSettings& settings, const QString& key) const {
    settings.setValue(key, saveLayoutState());
}

WorkspaceLayoutRestoreResult WorkspaceHost::restorePersistedLayout(QSettings& settings,
                                                                   const QString& key) {
    const auto state = settings.value(key);
    if (!state.isValid() || !state.canConvert<QByteArray>()) {
        return WorkspaceLayoutRestoreResult::Missing;
    }
    return restoreLayoutState(state.toByteArray());
}

EditorArea* WorkspaceHost::createArea(std::string initialEditorId, QString areaId) {
    auto* area = new EditorArea(editorRegistry_, initialEditorId, std::move(areaId));
    connect(area, &EditorArea::activationRequested, this,
            [this](EditorArea* requestedArea) { setActiveArea(requestedArea); });
    connect(area, &EditorArea::splitRequested, this,
            [this](EditorArea* requestedArea, Qt::Orientation orientation) {
                if (requestedArea != nullptr) {
                    setActiveArea(requestedArea);
                    splitArea(*requestedArea, orientation);
                }
            });
    connect(area, &EditorArea::closeRequested, this, [this](EditorArea* requestedArea) {
        if (requestedArea != nullptr) {
            setActiveArea(requestedArea);
            (void)closeArea(*requestedArea);
        }
    });
    connect(area, &EditorArea::maximizeRequested, this, [this](EditorArea* requestedArea) {
        if (requestedArea != nullptr) {
            setActiveArea(requestedArea);
            toggleMaximizeActiveArea();
        }
    });
    return area;
}

QSplitter* WorkspaceHost::createSplitter(Qt::Orientation orientation) const {
    auto* splitter = new QSplitter(orientation);
    splitter->setObjectName("workspaceSplitter");
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(2);
    splitter->setOpaqueResize(true);
    return splitter;
}

void WorkspaceHost::replaceRoot(QWidget* newRoot) {
    if (rootWidget_ != nullptr) {
        rootLayout_->removeWidget(rootWidget_);
        rootWidget_->hide();
        rootWidget_->setParent(nullptr);
        rootWidget_->deleteLater();
    }
    rootWidget_ = newRoot;
    rootLayout_->addWidget(rootWidget_);
    rootWidget_->show();
}

void WorkspaceHost::updateAreaControls() {
    QList<EditorArea*> areas;
    collectAreas(rootWidget_, areas);
    const bool canClose = areas.size() > 1;
    const bool canChangeStructure = maximizedArea_ == nullptr;
    for (auto* area : areas) {
        area->setSplitEnabled(canChangeStructure);
        area->setCloseEnabled(canClose && canChangeStructure);
        area->setMaximizedAppearance(area == maximizedArea_);
    }
}

void WorkspaceHost::restoreMaximizedArea() {
    if (maximizedArea_ == nullptr) {
        return;
    }

    showEntireTree(rootWidget_);
    for (auto iterator = preMaximizeSizes_.cbegin(); iterator != preMaximizeSizes_.cend();
         ++iterator) {
        if (containsArea(rootWidget_, firstArea(iterator.key()))) {
            iterator.key()->setSizes(iterator.value());
        }
    }
    maximizedArea_.clear();
    preMaximizeSizes_.clear();
    updateAreaControls();
    emit maximizeStateChanged(false);
}

} // namespace bloom::ui
