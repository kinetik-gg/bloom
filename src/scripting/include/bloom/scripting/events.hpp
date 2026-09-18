#pragma once

#include <bloom/commands/command_stack.hpp>
#include <bloom/scripting/session.hpp>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace bloom::scripting {

class Subscription final {
  public:
    Subscription() noexcept = default;
    explicit Subscription(std::function<void()> release) : release_(std::move(release)) {}
    Subscription(commands::CommandStack* stack, commands::CommandObserverId id)
        : release_([stack, id] { stack->removeObserver(id); }) {}
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&& other) noexcept : release_(std::exchange(other.release_, {})) {}
    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            reset();
            release_ = std::exchange(other.release_, {});
        }
        return *this;
    }
    ~Subscription() { reset(); }
    void reset() noexcept {
        if (auto release = std::exchange(release_, {}))
            release();
    }

  private:
    std::function<void()> release_;
};

// The live host publishes native command events here. Subscriptions may be released after
// project replacement; no borrowed CommandStack pointer survives that replacement.
class EventStream final {
    struct State {
        std::mutex mutex;
        std::uint64_t next = 0;
        std::map<std::uint64_t, commands::CommandObserver> observers;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();

  public:
    [[nodiscard]] Subscription subscribe(commands::CommandObserver observer) {
        const std::lock_guard lock(state_->mutex);
        const auto id = ++state_->next;
        state_->observers.emplace(id, std::move(observer));
        return Subscription([weak = std::weak_ptr(state_), id] {
            if (auto state = weak.lock()) {
                const std::lock_guard guard(state->mutex);
                state->observers.erase(id);
            }
        });
    }
    // Callbacks only enqueue native values, never Python or UI work. Holding the mutex
    // through delivery makes unsubscribe a lifetime barrier for the callback's owner.
    void publish(const commands::CommandEvent& event) {
        const std::lock_guard lock(state_->mutex);
        for (const auto& [id, observer] : state_->observers) {
            static_cast<void>(id);
            observer(event);
        }
    }
};

class Events final {
  public:
    explicit Events(Session& session) noexcept : session_(session) {}
    [[nodiscard]] Subscription subscribe(commands::CommandObserver observer) {
        if (auto stream = session_.eventStream())
            return stream->subscribe(std::move(observer));
        auto* stack = session_.commandStack();
        return stack == nullptr ? Subscription{}
                                : Subscription(stack, stack->addObserver(std::move(observer)));
    }

  private:
    Session& session_;
};

} // namespace bloom::scripting
