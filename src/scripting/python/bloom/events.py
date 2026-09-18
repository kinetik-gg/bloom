"""Subscriptions run only when their creating thread calls pump()."""
from collections import deque
from dataclasses import dataclass, field
from threading import Lock, get_ident

from . import _state

_lock = Lock()
_subscriptions = []


@dataclass(eq=False)
class Subscription:
    callback: object
    kinds: frozenset
    owner: object
    thread: int = field(default_factory=get_ident)
    pending: deque = field(default_factory=lambda: deque(maxlen=1024))
    active: bool = True
    reset_required: bool = False

    def close(self):
        with _lock:
            self.active = False
            self.pending.clear()
            if self in _subscriptions:
                _subscriptions.remove(self)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


def subscribe(callback, *, kinds=("revision", "history", "rejected")) -> Subscription:
    if not callable(callback):
        raise TypeError("An event callback must be callable")
    subscription = Subscription(callback, frozenset(kinds), _state.host())
    with _lock:
        _subscriptions.append(subscription)
    return subscription


def pump(*, limit: int = 256) -> int:
    if type(limit) is not int or not 1 <= limit <= 1024:
        raise ValueError("Event pump limit must be 1..1024")
    host = _state.host()
    with _lock:
        for event in host.events():
            event = _state.freeze(event)
            for subscription in _subscriptions:
                if subscription.owner is host:
                    if event["kind"] == "reset":
                        subscription.reset_required = True
                    elif event["kind"] in subscription.kinds:
                        if len(subscription.pending) == subscription.pending.maxlen:
                            subscription.reset_required = True
                        subscription.pending.append(event)
        delivery = []
        for subscription in _subscriptions:
            if subscription.thread == get_ident() and subscription.owner is host:
                if subscription.reset_required and len(delivery) < limit:
                    subscription.reset_required = False
                    delivery.append((subscription, _state.freeze({"kind": "reset"})))
                while subscription.pending and len(delivery) < limit:
                    delivery.append((subscription, subscription.pending.popleft()))
    for subscription, event in delivery:
        if subscription.active:
            subscription.callback(event)
    return len(delivery)
