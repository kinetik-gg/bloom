"""Runtime-local host selection. No document objects are exposed here."""
from contextvars import ContextVar
from types import MappingProxyType

import _bloom

_host = ContextVar("bloom_host", default=None)
_default_host = None
_task = ContextVar("bloom_task", default=None)
transaction = ContextVar("bloom_transaction", default=None)


def host():
    value = _host.get()
    if value is not None:
        return value
    global _default_host
    if _default_host is None:
        _default_host = _bloom.Host()
    return _default_host


def bind(value):
    return _host.set(value)


def freeze(value):
    if isinstance(value, dict):
        return MappingProxyType({key: freeze(child) for key, child in value.items()})
    if isinstance(value, (list, tuple)):
        return tuple(freeze(child) for child in value)
    return value
