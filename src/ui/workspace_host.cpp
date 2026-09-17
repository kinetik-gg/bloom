#include <bloom/ui/workspace_host.hpp>

#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>

#include <QAction>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QKeySequence>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QTimer>

#include <QString>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>
#include <bloom/ui/kit/tokens.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <numeric>
#include <utility>

namespace {

constexpr auto layoutFormat = "bloom.workspace-layout";
constexpr int layoutSchema = bloom::ui::kit::Layout::WorkspaceVersion;
constexpr int maximumLayoutDepth = 64;
constexpr int maximumAreaCount = 64;
constexpr int defaultSplitWeight = 1000;
// First-run / Reset Workspace proportions in per mille of the usable splitter extent: the top
// row is Assets 16%, Viewer 31%, Nodes 32%, Properties 19%; the bottom Timeline row is 32% below
// the 68% top row. These are weights, never pixels, so the arrangement follows the window size.
constexpr std::array<int, 4> defaultTopRowWeights{160, 310, 320, 190};
constexpr std::array<int, 2> defaultWorkspaceRowWeights{680, 320};

template <std::size_t count>
void setWeightedSizes(QSplitter& splitter, const std::array<int, count>& weights) {
    QList<int> initialSizes;
    initialSizes.reserve(static_cast<qsizetype>(count));
    for (const int weight : weights) {
        initialSizes.push_back(weight);
    }
    splitter.setSizes(initialSizes);

    // Minimum widths are meaningful only after the complete default tree has received its window
    // extent. Reapply the same weights from the real usable extent on the next event turn; the
    // splitter itself then performs its normal minimum-size adjustment.
    QTimer::singleShot(0, &splitter, [&splitter, weights] {
        const int extent =
            splitter.orientation() == Qt::Horizontal ? splitter.width() : splitter.height();
        const int available = std::max(0, extent - splitter.handleWidth());
        const int totalWeight = std::accumulate(weights.cbegin(), weights.cend(), 0);
        if (available <= 0 || totalWeight <= 0) {
            return;
        }

        QList<int> sizes;
        sizes.reserve(static_cast<qsizetype>(count));
        int assigned = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const bool last = index + 1 == count;
            const int size =
                last
                    ? available - assigned
                    : static_cast<int>(std::lround(static_cast<double>(available) * weights[index] /
                                                   static_cast<double>(totalWeight)));
            sizes.push_back(std::max(0, size));
            assigned += size;
        }
        splitter.setSizes(sizes);
    });
}

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
    // Window inner padding (task C1, item C4; owner: "panels inside window is overlapping with
    // actual window border at the edges; window needs inner padding"): autoFillBackground() paints
    // this frame's own QPalette::Window role, which kinetikPalette() already sets to
    // Color::Background -- the exact color the Gutter inset below must reveal on every side.
    setAutoFillBackground(true);

    rootLayout_ = new QVBoxLayout(this);
    // A uniform Spacing::Gutter (6px) inset on all four sides, so the window's own Background is
    // visible around the outermost panels the same way it already shows BETWEEN panels (the
    // splitter handle width just below): no panel touches the window edge, and the visible gutter
    // is identical on every side.
    const int gutter = kit::px(kit::Spacing::Gutter);
    rootLayout_->setContentsMargins(gutter, gutter, gutter, gutter);
    rootLayout_->setSpacing(0);

    rootWidget_ = createArea();
    rootLayout_->addWidget(rootWidget_);
    setActiveArea(qobject_cast<EditorArea*>(rootWidget_));
    updateAreaControls();

    // task U8, issue #131, fix 6: a window-scope backtick toggles fullscreen for the ACTIVE panel,
    // wired through the exact same toggleMaximizeActiveArea() routing the header button and the
    // Window/View menu actions already use. Same WindowShortcut + text-entry-defers idiom as
    // main_window.cpp's own window-level shortcuts and composition_editors.cpp's Space-key
    // transport shortcut (QAction + setShortcutContext(Qt::WindowShortcut) + addAction(this)):
    // Qt::WindowShortcut fires whenever this widget's top-level window is active regardless of
    // which descendant holds focus, EXCEPT that a focused text-entry widget accepts the
    // ShortcutOverride event for an ordinary key like backtick first, so typing a literal backtick
    // in a text field wins over this shortcut -- Qt's own mechanism, no bespoke focus check here.
    auto* toggleFullscreenAction = new QAction(this);
    toggleFullscreenAction->setObjectName(QStringLiteral("toggleActivePanelFullscreenAction"));
    toggleFullscreenAction->setShortcut(QKeySequence(Qt::Key_QuoteLeft));
    toggleFullscreenAction->setShortcutContext(Qt::WindowShortcut);
    addAction(toggleFullscreenAction);
    connect(toggleFullscreenAction, &QAction::triggered, this,
            &WorkspaceHost::toggleMaximizeActiveArea);
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

void WorkspaceHost::resetToSingleArea(const std::string_view editorId) {
    restoreMaximizedArea();
    auto* area = createArea(editorId);
    replaceRoot(area);
    activeArea_.clear();
    setActiveArea(area);
    updateAreaControls();
    emit areaCountChanged(1);
}

void WorkspaceHost::resetToDefaultLayout(const std::array<std::string_view, 4>& topRowEditorIds,
                                         const std::string_view bottomRowEditorId,
                                         const std::size_t activeTopRowIndex) {
    restoreMaximizedArea();

    auto* topRow = createSplitter(Qt::Horizontal);
    std::array<EditorArea*, 4> topRowAreas{};
    for (std::size_t index = 0; index < topRowEditorIds.size(); ++index) {
        topRowAreas[index] = createArea(topRowEditorIds[index]);
        topRow->addWidget(topRowAreas[index]);
    }
    setWeightedSizes(*topRow, defaultTopRowWeights);

    auto* bottomRow = createArea(bottomRowEditorId);
    auto* root = createSplitter(Qt::Vertical);
    root->addWidget(topRow);
    root->addWidget(bottomRow);
    setWeightedSizes(*root, defaultWorkspaceRowWeights);

    replaceRoot(root);
    activeArea_.clear();
    const auto selectedIndex = std::min(activeTopRowIndex, topRowAreas.size() - 1);
    setActiveArea(topRowAreas[selectedIndex]);
    updateAreaControls();
    emit areaCountChanged(areaCount());
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
    // The complete tree must have its window extent before minima can be applied.
    // Otherwise constructing a sidebar in an unshown 640px shell clamps it to half the window.
    QTimer::singleShot(0, newSplitter, [newSplitter, newAreaFraction] {
        const int extent = newSplitter->orientation() == Qt::Horizontal ? newSplitter->width()
                                                                        : newSplitter->height();
        const int available = std::max(0, extent - newSplitter->handleWidth());
        const int trailing = static_cast<int>(std::lround(available * newAreaFraction));
        newSplitter->setSizes({available - trailing, trailing});
    });

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
    if (parentSplitter == nullptr || parentSplitter->count() < 2) {
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

    if (parentSplitter->count() > 2) {
        const auto parentSizes = usableSizes(*parentSplitter);
        area.hide();
        area.setParent(nullptr);
        QList<int> remainingSizes;
        remainingSizes.reserve(parentSizes.size() - 1);
        for (int index = 0; index < parentSizes.size(); ++index) {
            if (index != areaIndex) {
                remainingSizes.push_back(parentSizes[index]);
            }
        }
        parentSplitter->setSizes(remainingSizes);
        area.deleteLater();
        setActiveArea(nextActiveArea);
        updateAreaControls();
        emit areaCountChanged(areaCount());
        return true;
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
    if ((serializedSchema != 1 && serializedSchema != layoutSchema) ||
        !documentRoot.value("root").isObject()) {
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
        if (children.size() < 2 || children.size() > maximumAreaCount) {
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

EditorArea* WorkspaceHost::createArea(const std::string_view initialEditorId, QString areaId) {
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
    // The gutter (task U1, issue #117): panels are separated by a visible Background gap of
    // Spacing::Gutter, not by a hairline seam. The handle's own fill comes from the theme's
    // QSplitter::handle rule; this is the width that makes the gap real and grabbable.
    splitter->setHandleWidth(kit::px(kit::Spacing::Gutter));
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
