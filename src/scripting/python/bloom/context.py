"""The current session context, read afresh on every access."""
from fractions import Fraction

from . import _state
from .data import Proxy


def __getattr__(name):
    current = _state.host().context()
    if name == "project":
        return Proxy(current["project"], "project")
    if name == "composition":
        return (None if current["composition"] is None
                else Proxy(current["composition"], "compositions"))
    if name == "selection":
        return tuple(Proxy(identity, "nodes", current["composition"])
                     for identity in current["selection"])
    if name == "time":
        return Fraction(*current["time"])
    if name == "headless":
        return _state.host().headless
    raise AttributeError(name)
