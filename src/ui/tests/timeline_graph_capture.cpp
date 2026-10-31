// Lane T1 review captures of the timeline Graph Editor (a tool, not a test):
// bloom_timeline_graph_capture <dir>. Run once per QT_SCALE_FACTOR (1 and 1.5). It hosts the real
// window, maximizes the Timeline at about 2000 and 900 logical pixels wide, and captures: the
// Value and the Speed graphs with their coloured curves and grid, Normalize on, the key menu with
// Easing open, a live drag in progress, a Shift-drag, and frame-0 keys close up.
#include "window_fixture.hpp"

#include <timeline_graph_math.hpp>

#include <bloom/document/graph.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/timeline_graph_view.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <QAction>
#include <QDir>
#include <QImage>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>

#include <iostream>

namespace {
using namespace bloom;
using namespace bloom::ui;

core::RationalTime seconds(const std::int64_t numerator, const std::int64_t denominator = 1) {
    return *core::RationalTime::create(numerator, denominator);
}

void author(test::WindowFixture& fixture) {
    auto& session = fixture.session;
    if (!session.addSolidLayer(QStringLiteral("Title"), {0.86, 0.42, 0.18, 1.0}))
        throw std::runtime_error("capture layer failed");
    const auto* position = session.parameterForSelection(document::kPositionParameterRole);
    const auto* opacity = session.parameterForSelection(document::kOpacityParameterRole);
    const auto* rotation = session.parameterForSelection(document::kRotationParameterRole);
    if (position == nullptr || opacity == nullptr || rotation == nullptr)
        throw std::runtime_error("capture parameters failed");
    const auto x = document::AnimationComponent::X;
    const auto y = document::AnimationComponent::Y;
    using Mode = document::KeyframeInterpolation;
    if (!session.pasteKeyframes(
            {
                {position->id, seconds(0), 260.0, Mode::Linear, x},
                {position->id, seconds(1), 980.0, Mode::Linear, x},
                {position->id, seconds(5, 2), 1460.0, Mode::Linear, x},
                {position->id, seconds(4), 1660.0, Mode::Linear, x},
                {position->id, seconds(0), 820.0, Mode::Linear, y},
                {position->id, seconds(3, 2), 300.0, Mode::Linear, y},
                {position->id, seconds(4), 520.0, Mode::Linear, y},
                {opacity->id, seconds(0), 0.0},
                {opacity->id, seconds(1), 1.0},
                {rotation->id, seconds(1, 2), 0.0},
                {rotation->id, seconds(3), 180.0},
            },
            session.snapshot().revision()))
        throw std::runtime_error("capture keys failed");
}

EditorArea* timelineArea(MainWindow& window) {
    for (auto* area : window.findChildren<EditorArea*>())
        if (area->editorId() == "bloom.timeline")
            return area;
    return nullptr;
}

void settle(const int milliseconds = 60) {
    QTest::qWait(milliseconds);
    QApplication::processEvents();
}

void sizeArea(MainWindow& window, EditorArea& area, const QSize target) {
    for (int pass = 0; pass < 4; ++pass) {
        settle(30);
        const QSize delta = target - area.size();
        if (delta.isNull())
            return;
        window.resize(window.size() + delta);
    }
}

void mouse(QWidget& widget, const QEvent::Type type, const QPointF point,
           const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(type, point, widget.mapToGlobal(point), Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                      modifiers);
    QCoreApplication::sendEvent(&widget, &event);
}

// A popup is its own top-level window, so a panel grab never contains it: paint each open popup
// over the panel image at the place it occupies on screen.
QImage withPopups(EditorArea& area, const std::vector<QWidget*>& popups) {
    // The canvas grows to hold any popup that opens past the panel's edge, as it would on screen.
    const QPoint origin = area.mapToGlobal(QPoint(0, 0));
    QRect bounds(QPoint(0, 0), area.size());
    for (auto* popup : popups)
        if (popup != nullptr && popup->isVisible())
            bounds |= QRect(popup->mapToGlobal(QPoint(0, 0)) - origin, popup->size());
    const QPixmap panel = area.grab();
    const qreal ratio = panel.devicePixelRatio();
    QImage image((bounds.size() * ratio), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(kit::color(kit::Color::Background));
    QPainter painter(&image);
    painter.drawPixmap(-bounds.topLeft(), panel);
    for (auto* popup : popups)
        if (popup != nullptr && popup->isVisible())
            painter.drawPixmap(popup->mapToGlobal(QPoint(0, 0)) - origin - bounds.topLeft(),
                               popup->grab());
    painter.end();
    return image;
}

int run(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomGrammarTests");
    app.setApplicationName("TimelineGraphCapture");
    kit::installKinetikTheme(app);
    const QString directory = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                       : QStringLiteral(BLOOM_GRAMMAR_ARTIFACT_DIR);
    QDir().mkpath(directory);
    test::WindowFixture fixture(author);
    const auto ratio = fixture.window->devicePixelRatioF();
    const QString dpr = ratio == 1.0 ? QStringLiteral("dpr1") : QStringLiteral("dpr15");
    auto* area = timelineArea(*fixture.window);
    auto* editor = area != nullptr ? area->findChild<TimelineEditor*>() : nullptr;
    if (area == nullptr || editor == nullptr) {
        std::cerr << "no Timeline\n";
        return 1;
    }
    Q_EMIT area->maximizeRequested(area);
    settle(80);
    auto& session = fixture.session;
    std::optional<document::LayerId> layer;
    for (const auto& output : session.composition()->graph().layerOutputs())
        if (output.name == "Title")
            layer = output.layerId;
    if (!layer) {
        std::cerr << "no capture layer\n";
        return 1;
    }
    Q_EMIT editor->layerStackForTest()->expansionRequested(*layer);
    settle();
    auto* graphPane = editor->findChild<QAction*>(QStringLiteral("timelineGraphPaneAction"));
    if (graphPane == nullptr) {
        std::cerr << "no graph pane action\n";
        return 1;
    }
    if (!graphPane->isChecked())
        graphPane->trigger();
    settle();
    auto* view = editor->laneRegionForTest()->graphViewForTest();
    if (view == nullptr || view->curves().empty()) {
        std::cerr << "no graph curves\n";
        return 1;
    }
    std::optional<GraphCurveId> curveX;
    for (const auto& curve : view->curves())
        if (curve.component == document::AnimationComponent::X)
            curveX = curve;
    if (!curveX) {
        std::cerr << "no X curve\n";
        return 1;
    }
    const auto keysOf = [&](const GraphCurveId& curve) {
        const auto* record = session.composition()->animationCurves().findVec2(curve.curveId);
        return record->component(*curve.component)->keyframes;
    };
    // Easy Ease the X keys so the captures carry Bezier segments and handles.
    {
        std::vector<KeyframeSelection> all;
        for (const auto& key : keysOf(*curveX))
            all.push_back({curveX->curveId, key.id, curveX->component});
        session.selectKeyframes(all);
        (void)session.easeSelectedKeyframes(true, true);
    }
    const auto selectOne = [&](const std::size_t index) {
        const auto keys = keysOf(*curveX);
        session.selectKeyframe(curveX->curveId, *curveX->component, keys[index].id);
    };
    static_cast<void>(session.setCurrentTime(seconds(2)));

    int failures = 0;
    const auto save = [&](const QImage& image, const QString& scene, const char* width) {
        const QString label =
            QStringLiteral("graph-%1-%2-%3").arg(scene, QString::fromLatin1(width), dpr);
        if (!image.save(directory + QLatin1Char('/') + label + QStringLiteral(".png")))
            ++failures;
        std::cout << label.toStdString() << ' ' << area->width() << 'x' << area->height() << '\n';
    };
    for (const auto& [width, size] :
         {std::pair{"wide", QSize(2000, 640)}, std::pair{"narrow", QSize(900, 560)}}) {
        sizeArea(*fixture.window, *area, size);
        editor->rulerForTest()->zoomToFit();
        view->setGraphType(GraphType::Value);
        view->setNormalized(false);
        view->fitCurves();
        selectOne(1);
        fixture.clearInteraction();
        settle();
        save(area->grab().toImage(), QStringLiteral("value"), width);

        view->setGraphType(GraphType::Speed);
        view->fitCurves();
        settle();
        save(area->grab().toImage(), QStringLiteral("speed"), width);

        view->setGraphType(GraphType::Value);
        view->setNormalized(true);
        view->fitCurves();
        settle();
        save(area->grab().toImage(), QStringLiteral("normalize"), width);
        view->setNormalized(false);
        view->fitCurves();
        settle();

        // The key menu with Easing open, over two selected keys.
        {
            const auto keys = keysOf(*curveX);
            session.selectKeyframes({{curveX->curveId, keys[1].id, curveX->component},
                                     {curveX->curveId, keys[2].id, curveX->component}});
            settle();
            const auto center = view->keyCenter(*curveX, keys[1].id);
            std::unique_ptr<QMenu> menu(kit::makeMenu(view));
            view->populateContextMenu(*menu);
            menu->popup(view->mapToGlobal(center.value_or(QPointF()).toPoint()));
            settle(120);
            QMenu* easing = menu->findChild<QMenu*>(QStringLiteral("graphEasingMenu"));
            if (easing != nullptr) {
                menu->setActiveAction(easing->menuAction());
                const QRect row = menu->actionGeometry(easing->menuAction());
                easing->popup(menu->mapToGlobal(QPoint(row.right(), row.top())));
                settle(120);
            }
            save(withPopups(*area, {menu.get(), easing}), QStringLiteral("menu-easing"), width);
            if (easing != nullptr)
                easing->close();
            menu->close();
            settle();
        }

        // A live drag in progress, then a Shift-drag (value-only), both held for the capture.
        for (const auto& [scene, modifiers, offset] :
             {std::tuple{"drag", Qt::KeyboardModifiers(Qt::NoModifier), QPointF(46, -70)},
              std::tuple{"shift-drag", Qt::KeyboardModifiers(Qt::ShiftModifier),
                         QPointF(14, -90)}}) {
            selectOne(1);
            settle();
            const auto center = view->keyCenter(*curveX, keysOf(*curveX)[1].id);
            if (!center) {
                ++failures;
                continue;
            }
            mouse(*view, QEvent::MouseButtonPress, *center, modifiers);
            mouse(*view, QEvent::MouseMove, *center + offset / 2.0, modifiers);
            mouse(*view, QEvent::MouseMove, *center + offset, modifiers);
            settle();
            save(area->grab().toImage(), QString::fromLatin1(scene), width);
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QCoreApplication::sendEvent(view, &escape);
            mouse(*view, QEvent::MouseButtonRelease, *center + offset, modifiers);
            settle();
        }

        // Frame-0 keys, close in: the gutter leads the axis, so nothing covers them.
        selectOne(0);
        editor->rulerForTest()->zoomToRange(0.0, 1.5);
        settle();
        save(area->grab().toImage(), QStringLiteral("frame-zero"), width);
    }
    return failures == 0 ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
