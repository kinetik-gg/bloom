#include <bloom/ui/compile_failure.hpp>

#include <bloom/document/project.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QClipboard>
#include <QHBoxLayout>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QVBoxLayout>

#include <string_view>

namespace bloom::ui::node_editor {
// Declared in node_editor_items.hpp (private to src/ui); defined in node_editor_items.cpp.
QString nodeTypeDisplayName(std::string_view typeId);
} // namespace bloom::ui::node_editor

namespace bloom::ui {

namespace {

constexpr std::string_view kCompileCodePrefix = "bloom.runtime.compile.";

// "blendMode" -> "Blending Mode", "line-height" -> "Line height". The Properties panel spells its
// rows by hand; the few roles whose row label differs from the role are listed here.
QString roleLabel(const std::string& role) {
    if (role == "blendMode")
        return CompileFailurePanel::tr("Blending Mode");
    if (role == "timeRemap")
        return CompileFailurePanel::tr("Time Remap");
    QString out;
    bool upperNext = true;
    for (const QChar c : QString::fromUtf8(role.data(), static_cast<qsizetype>(role.size()))) {
        if (c == QLatin1Char('-') || c == QLatin1Char('_') || c == QLatin1Char('.')) {
            out += QLatin1Char(' ');
            continue;
        }
        if (c.isUpper() && !out.isEmpty() && !out.endsWith(QLatin1Char(' '))) {
            out += QLatin1Char(' ');
            out += c;
            continue;
        }
        out += upperNext ? c.toUpper() : c;
        upperNext = false;
    }
    return out;
}

std::optional<std::uint64_t> takeId(QString& text, const QString& label) {
    const QRegularExpression pattern(QStringLiteral("\\s*\\b%1=(\\d+)\\b").arg(label));
    const auto match = pattern.match(text);
    if (!match.hasMatch())
        return std::nullopt;
    const auto value = match.captured(1).toULongLong();
    text.remove(match.capturedStart(0), match.capturedLength(0));
    return value;
}

} // namespace

// ENGINE SWAP POINT (see the header). Reads the first compile TaskDiagnostic of a failed preview.
// Replace this body with the engine's structured diagnostic: headline from its summary, `detail`
// as is, subject.composition/node/parameter straight from its ids, `raw` from its code and detail.
std::optional<CompileFailure> compileFailureFromPreview(const CompositionPreviewState& preview) {
    if (preview.activity != PreviewActivity::Failed || preview.diagnostics.empty())
        return std::nullopt;
    const auto& diagnostic = preview.diagnostics.front();
    if (!std::string_view(diagnostic.code).starts_with(kCompileCodePrefix))
        return std::nullopt;

    CompileFailure failure;
    failure.headline = QString::fromStdString(diagnostic.summary);
    QString detail = QString::fromStdString(diagnostic.detail);
    failure.raw = QString::fromStdString(diagnostic.code) + QLatin1Char('\n') + failure.headline;
    if (!detail.isEmpty())
        failure.raw += QLatin1Char('\n') + detail;
    failure.subject.node = takeId(detail, QStringLiteral("node"));
    failure.subject.parameter = takeId(detail, QStringLiteral("parameter"));
    // Layer, slot and edge ids are not shown; drop them from the sentence all the same.
    for (const auto* label : {"edge", "layer", "slot"})
        takeId(detail, QString::fromLatin1(label));
    failure.detail = detail.trimmed();
    if (preview.desiredIdentity)
        failure.subject.composition = preview.desiredIdentity->compositionId.value();
    return failure;
}

QString compileFailureSubjectText(const CompileFailure& failure, const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr)
        return {};
    QString owner;
    std::optional<document::NodeId> nodeId;
    if (failure.subject.node)
        nodeId = document::NodeId::fromRaw(*failure.subject.node);
    const document::NodeRecord* node = nodeId ? composition->graph().findNode(*nodeId) : nullptr;

    QString parameter;
    if (failure.subject.parameter) {
        const auto parameterId = document::ParameterId::fromRaw(*failure.subject.parameter);
        if (node != nullptr) {
            for (const auto& binding : node->parameters) {
                if (binding.parameterId == parameterId)
                    parameter = roleLabel(binding.role);
            }
        }
    }
    if (node != nullptr) {
        const auto layerId = session.layerForNode(node->id);
        const auto* layer = layerId ? composition->graph().findLayer(*layerId) : nullptr;
        owner = layer != nullptr
                    ? CompileFailurePanel::tr("Layer '%1'").arg(QString::fromStdString(layer->name))
                    : CompileFailurePanel::tr("Node '%1'").arg(node_editor::nodeTypeDisplayName(node->typeId));
    }
    if (owner.isEmpty())
        return {};
    return parameter.isEmpty() ? owner : owner + QStringLiteral(" · ") + parameter;
}

bool selectCompileFailureSubject(CompositionSession& session, const CompileFailure& failure) {
    const auto* composition = session.composition();
    if (composition == nullptr || !failure.subject.node)
        return false;
    const auto nodeId = document::NodeId::fromRaw(*failure.subject.node);
    if (composition->graph().findNode(nodeId) == nullptr)
        return false;
    if (const auto layerId = session.layerForNode(nodeId)) {
        session.selectLayer(*layerId);
        return true;
    }
    session.selectNode(nodeId);
    return true;
}

QSize CompileFailurePanel::preferredSize() const {
    const int width = kit::px(kit::Size::CompileFailurePopupWidth);
    return {width, layout()->totalHeightForWidth(width)};
}

CompileFailurePanel::CompileFailurePanel(CompileFailure failure, CompositionSession& session,
                                         QWidget* const parent)
    : QWidget(parent), failure_(std::move(failure)), session_(session) {
    setObjectName(QStringLiteral("compileFailurePanel"));
    const int pad = kit::px(kit::Spacing::M);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(kit::px(kit::Spacing::S));
    setFixedWidth(kit::px(kit::Size::CompileFailurePopupWidth));

    auto* headline = new kit::KLabel(failure_.headline, this, kit::TypeRole::UiSmallStrong);
    headline->setObjectName(QStringLiteral("compileFailureHeadline"));
    headline->setTone(kit::Color::Error);
    headline->setWordWrap(true);
    layout->addWidget(headline);

    if (!failure_.detail.isEmpty()) {
        auto* detail = new kit::KLabel(failure_.detail, this, kit::TypeRole::UiSmall);
        detail->setObjectName(QStringLiteral("compileFailureDetail"));
        detail->setWordWrap(true);
        layout->addWidget(detail);
    }
    const QString subject = compileFailureSubjectText(failure_, session_);
    if (!subject.isEmpty()) {
        auto* label = new kit::KLabel(subject, this, kit::TypeRole::UiSmall);
        label->setObjectName(QStringLiteral("compileFailureSubject"));
        label->setTone(kit::Color::Muted);
        label->setWordWrap(true);
        layout->addWidget(label);
    }

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(kit::px(kit::Spacing::S));
    buttons->addStretch(1);
    auto* copy = new kit::KButton(tr("Copy details"), this);
    copy->setObjectName(QStringLiteral("compileFailureCopy"));
    copy->setVariant(kit::KButton::Variant::Secondary);
    connect(copy, &QAbstractButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(failure_.raw);
        Q_EMIT dismissed();
    });
    buttons->addWidget(copy);
    auto* select = new kit::KButton(tr("Select"), this);
    select->setObjectName(QStringLiteral("compileFailureSelect"));
    select->setVariant(kit::KButton::Variant::Primary);
    select->setEnabled(!subject.isEmpty());
    connect(select, &QAbstractButton::clicked, this, [this] {
        selectCompileFailureSubject(session_, failure_);
        Q_EMIT dismissed();
    });
    buttons->addWidget(select);
    layout->addLayout(buttons);
}

} // namespace bloom::ui
