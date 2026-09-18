"""Build and host capabilities."""
import _bloom
from . import _state

version = _bloom.version
facade_version = _bloom.facade_version


def __getattr__(name):
    if name in ("headless", "gui"):
        value = _state.host().headless
        return value if name == "headless" else not value
    if name == "capabilities":
        return frozenset(("python", "queries", "transactions", "events", "tasks", "render",
                          "headless" if _state.host().headless else "gui"))
    raise AttributeError(name)
