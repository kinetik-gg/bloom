#include "project_change_check.hpp"

#include <bloom/ui/composition_session.hpp>

#include <QObject>

namespace bloom::ui {

ProjectChangeCheck::ProjectChangeCheck(CompositionSession& session)
    : session_(session), snapshot_(session.snapshot()), context_(std::make_unique<QObject>()) {
    QObject::connect(&session, &CompositionSession::documentRebound, context_.get(),
                     [rebound = rebound_] { *rebound = true; });
}

ProjectChangeCheck::~ProjectChangeCheck() = default;

bool ProjectChangeCheck::projectChanged() const {
    return *rebound_ || &snapshot_.project() != &session_.snapshot().project();
}

} // namespace bloom::ui
