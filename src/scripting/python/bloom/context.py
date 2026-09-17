"""The current session context, read afresh on every access."""
import sys
from fractions import Fraction
from types import ModuleType

from . import _state
from .data import Proxy

# The operation arguments that fall back to these values when a call omits them. The host schema
# carries the same flags, so the CLI and MCP clients report and apply exactly these defaults.
DEFAULTS = {"project": "project", "composition": "composition", "time": "time",
            "layer": "layers[0]", "selection": "selection"}


def _read(name):
    current = _state.host().context()
    if name == "project":
        return Proxy(current["project"], "project")
    if name == "composition":
        return (None if current["composition"] is None
                else Proxy(current["composition"], "compositions"))
    if name == "selection":
        return tuple(Proxy(identity, "nodes", current["composition"])
                     for identity in current["selection"])
    if name == "layers":
        # A node selection is not a layer selection; these are the layers it names.
        return tuple(Proxy(identity, "layers", current["composition"])
                     for identity in current["layers"])
    if name == "time":
        return Fraction(*current["time"])
    if name == "headless":
        return _state.host().headless
    raise AttributeError(name)


class _ContextModule(ModuleType):
    def __getattr__(self, name):
        if name.startswith("_"):
            raise AttributeError(name)
        return _read(name)

    def __dir__(self):
        return ["DEFAULTS", "composition", "headless", "layers", "project", "selection",
                "time"]

    def __repr__(self):
        try:
            return ("<bloom.context project={} composition={} selection={} time={}>".format(
                _read("project"), _read("composition"), len(_read("selection")), _read("time")))
        except Exception as error:  # No live host: say so instead of leaking a traceback.
            return f"<bloom.context unavailable: {error}>"


sys.modules[__name__].__class__ = _ContextModule
