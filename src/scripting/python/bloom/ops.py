"""Callables and keyword signatures come directly from the host operation registry."""
import inspect
import math
import keyword
from fractions import Fraction
from types import SimpleNamespace

from . import _state
from .data import Proxy
from .errors import OperationError, checked

_TYPES = {"bool": bool, "int": int, "float": float, "str": str, "id": int,
          "vec2": tuple[float, float], "vec3": tuple[float, float, float],
          "color4": tuple[float, float, float, float], "array": tuple}


def _argument(value, schema, operation):
    kind = schema["kind"]
    if isinstance(value, Proxy):
        value = int(value)
    if isinstance(value, Fraction):
        value = (value.numerator, value.denominator)
    valid = True
    if kind == "bool":
        valid = type(value) is bool
    elif kind in ("int", "id"):
        valid = type(value) is int and -(2**63) <= value < 2**63
        valid = valid and (kind != "id" or value > 0)
    elif kind == "float":
        valid = type(value) in (float, int) and math.isfinite(value)
    elif kind == "str":
        valid = type(value) is str
    elif kind in ("vec2", "vec3", "color4"):
        size = {"vec2": 2, "vec3": 3, "color4": 4}[kind]
        valid = (isinstance(value, (list, tuple)) and len(value) == size
                 and all(type(v) in (int, float) and math.isfinite(v) for v in value))
    elif kind == "array":
        valid = isinstance(value, (list, tuple))
    if not valid:
        raise OperationError([{"code": "bloom.scripting.invalid-argument",
                               "operation_id": operation, "argument": schema["name"],
                               "message": f"Expected {kind} for {schema['name']}"}])
    return value


class Operation:
    def __init__(self, schema):
        self.schema = _state.freeze(schema)
        self.id = schema["id"]
        self.__name__ = self.id.rsplit(".", 1)[-1].replace("-", "_")
        self.__doc__ = f"Host operation {self.id}; see schema for required keyword arguments."
        self.__signature__ = inspect.Signature([
            inspect.Parameter(arg["name"], inspect.Parameter.KEYWORD_ONLY,
                              default=inspect.Parameter.empty if arg["required"] else None,
                              annotation=_TYPES[arg["kind"]])
            for arg in schema["arguments"]])
        self.__annotations__ = {arg["name"]: _TYPES[arg["kind"]]
                                for arg in schema["arguments"]}

    def __call__(self, **kwargs):
        try:
            self.__signature__.bind(**kwargs)
        except TypeError as error:
            raise OperationError([{"code": "bloom.scripting.invalid-argument",
                                   "operation_id": self.id, "message": str(error)}]) from error
        args = {arg["name"]: _argument(kwargs[arg["name"]], arg, self.id)
                for arg in self.schema["arguments"] if arg["name"] in kwargs}
        request = {"op": self.id, "args": args}
        transaction = _state.transaction.get()
        if transaction is not None:
            transaction.operations.append(request)
            return None
        host = _state.host()
        return checked(host.transact([request], self.id, host.snapshot()["revision"]))


_registry = {}


def _refresh():
    for schema in _state.host().schemas():
        operation = Operation(schema)
        _registry[operation.id] = operation
        _, domain, verb = operation.id.split(".")
        domain = domain.replace("-", "_")
        if domain not in globals():
            globals()[domain] = SimpleNamespace()
        name = verb.replace("-", "_")
        setattr(globals()[domain], name + "_" if keyword.iskeyword(name) else name, operation)


def __getattr__(name):
    _refresh()
    if name in globals():
        return globals()[name]
    raise AttributeError(name)


def registry():
    _refresh()
    return _state.freeze(_registry)


def get(operation_id: str) -> Operation:
    return registry()[operation_id]
