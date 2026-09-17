from collections.abc import Mapping, Sequence
from typing import Any
class OperationError(RuntimeError):
    diagnostics: Sequence[Mapping[str, Any]]
class StaleObjectError(ReferenceError):
    id: int
    revision: int
class CancelledError(RuntimeError): ...
