"""Structured exceptions from the language-neutral host boundary (ADR 0022)."""
from ._state import freeze


class OperationError(RuntimeError):
    def __init__(self, diagnostics):
        self.diagnostics = freeze(diagnostics)
        super().__init__("; ".join(item.get("message", "Operation rejected")
                                  for item in self.diagnostics))


class StaleObjectError(ReferenceError):
    def __init__(self, id, revision):
        self.id = id
        self.revision = revision
        super().__init__(f"Object {id} is stale at revision {revision}")


class CancelledError(RuntimeError):
    pass


def checked(result):
    if not result["succeeded"]:
        raise OperationError(result["diagnostics"])
    return freeze(result)
