#pragma once

#include <bloom/scripting/events.hpp>
#include <bloom/scripting/operation_registry.hpp>
#include <bloom/scripting/query.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>
#include <bloom/scripting/tasks.hpp>

namespace bloom::scripting {

// The language-neutral host surface. Python/MCP bindings consume these typed seams; they do not
// receive Qt objects, mutable document containers, renderer internals, or raw C++ pointers.
struct Facade final {
    explicit Facade(Session& session, Tasks* tasks = nullptr)
        : session(session), operations(session.operations()), query(session), events(session),
          tasks(tasks) {}

    Session& session;
    const OperationRegistry& operations;
    Query query;
    Events events;
    Tasks* tasks = nullptr;
};

} // namespace bloom::scripting
