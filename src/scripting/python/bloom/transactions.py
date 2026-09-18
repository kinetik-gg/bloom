"""Atomic command groups with optimistic revision checks."""
from . import _state
from .errors import checked


class group:
    def __init__(self, label: str, *, expected_revision: int | None = None):
        self.label = label
        self.expected_revision = expected_revision
        self.operations = []
        self.result = None
        self._token = None

    def __enter__(self):
        if _state.transaction.get() is not None:
            raise RuntimeError("Nested transactions are not supported by facade v1 (ADR 0022)")
        self._host = _state.host()
        if self.expected_revision is None:
            self.expected_revision = self._host.snapshot()["revision"]
        self._token = _state.transaction.set(self)
        return self

    def __exit__(self, kind, value, traceback):
        _state.transaction.reset(self._token)
        if kind is None and self.operations:
            if self._host is not _state.host():
                raise RuntimeError("The active host changed during a transaction")
            self.result = checked(self._host.transact(self.operations, self.label,
                                                     self.expected_revision))
        return False


def undo():
    if _state.transaction.get() is not None:
        raise RuntimeError("Cannot undo inside a transaction")
    return checked(_state.host().history(False))


def redo():
    if _state.transaction.get() is not None:
        raise RuntimeError("Cannot redo inside a transaction")
    return checked(_state.host().history(True))
