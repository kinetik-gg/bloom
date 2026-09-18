#pragma once

#include <bloom/commands/command_stack.hpp>
#include <bloom/scripting/session.hpp>

#include <utility>

namespace bloom::scripting {

class Subscription final {
  public:
    Subscription() noexcept = default;
    Subscription(commands::CommandStack* stack, commands::CommandObserverId id) noexcept
        : stack_(stack), id_(id) {}
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&& other) noexcept
        : stack_(std::exchange(other.stack_, nullptr)), id_(std::exchange(other.id_, 0)) {}
    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            reset();
            stack_ = std::exchange(other.stack_, nullptr);
            id_ = std::exchange(other.id_, 0);
        }
        return *this;
    }
    ~Subscription() { reset(); }

    void reset() noexcept {
        if (stack_ != nullptr) {
            stack_->removeObserver(id_);
            stack_ = nullptr;
            id_ = 0;
        }
    }

  private:
    commands::CommandStack* stack_ = nullptr;
    commands::CommandObserverId id_ = 0;
};

class Events final {
  public:
    explicit Events(Session& session) noexcept : session_(session) {}

    [[nodiscard]] Subscription subscribe(commands::CommandObserver observer) {
        auto* stack = session_.commandStack();
        return stack == nullptr ? Subscription{}
                                : Subscription(stack, stack->addObserver(std::move(observer)));
    }

  private:
    Session& session_;
};

} // namespace bloom::scripting
