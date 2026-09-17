"""Callables, signatures and help text come directly from the host operation registry."""
import inspect
import keyword
import math
import sys
from fractions import Fraction
from types import ModuleType

from . import _state
from .data import Proxy
from .errors import OperationError, checked

_TYPES = {"bool": bool, "int": int, "float": float, "str": str, "id": int,
          "vec2": tuple[float, float], "vec3": tuple[float, float, float],
          "color4": tuple[float, float, float, float], "array": tuple,
          "time": int | tuple[int, int] | Fraction | float,
          "value": float | tuple[float, ...]}

_EXPECTED = {
    "bool": "true or false",
    "int": "a whole number",
    "float": "a finite number",
    "str": "text",
    "id": "a positive stable id or a live proxy",
    "vec2": "two numbers as (x, y)",
    "vec3": "three numbers as (x, y, z)",
    "color4": "four numbers as (r, g, b, a) with alpha in 0..1",
    "array": "a list or tuple",
    "time": "a whole frame, a Fraction, or an exact (numerator, denominator) time",
    "value": "a number, (x, y), (x, y, z) or (r, g, b, a)",
}

_CONTEXT_DEFAULTS = {
    "project": "bloom.context.project",
    "composition": "bloom.context.composition",
    "time": "bloom.context.time",
    "layer": "bloom.context.layers[0]",
    "selection": "bloom.context.selection",
}


def _plain(value):
    """Proxies become their stable id and Fractions become exact (numerator, denominator)."""
    if isinstance(value, Proxy):
        return int(value)
    if isinstance(value, Fraction):
        return (value.numerator, value.denominator)
    if isinstance(value, (list, tuple)):
        return tuple(_plain(item) for item in value)
    return value


def _finite(value):
    return type(value) in (int, float) and math.isfinite(value)


def _sequence(value, size):
    return (isinstance(value, tuple) and len(value) == size
            and all(_finite(item) for item in value))


def _valid(value, kind):
    if kind == "bool":
        return type(value) is bool
    if kind in ("int", "id"):
        return (type(value) is int and -(2 ** 63) <= value < 2 ** 63
                and (kind != "id" or value > 0))
    if kind == "float":
        return _finite(value)
    if kind == "str":
        return type(value) is str
    if kind in ("vec2", "vec3", "color4"):
        return _sequence(value, {"vec2": 2, "vec3": 3, "color4": 4}[kind])
    if kind == "array":
        return isinstance(value, tuple)
    if kind == "time":
        return (type(value) is int
                or (isinstance(value, tuple) and len(value) == 2
                    and all(type(item) is int for item in value)))
    if kind == "value":
        return _finite(value) or any(_sequence(value, size) for size in (2, 3, 4))
    return True


class Operation:
    """One registered host operation, callable positionally or by keyword."""

    def __init__(self, schema):
        self.schema = _state.freeze(schema)
        self.id = schema["id"]
        self.example = schema.get("example", "")
        self.__name__ = self.id.rsplit(".", 1)[-1].replace("-", "_")
        if keyword.iskeyword(self.__name__):
            self.__name__ += "_"
        self.__qualname__ = "bloom.ops." + self.id.split(".")[1].replace("-", "_") + "." \
            + self.__name__
        self._positional = tuple(a for a in self.schema["arguments"] if not a["contextual"])
        self._contextual = tuple(a for a in self.schema["arguments"] if a["contextual"])
        self._by_name = {a["name"]: a for a in self.schema["arguments"]}
        self.__signature__ = inspect.Signature(
            [inspect.Parameter(a["name"], inspect.Parameter.POSITIONAL_OR_KEYWORD,
                               default=inspect.Parameter.empty if a["required"] else None,
                               annotation=_TYPES[a["kind"]])
             for a in self._positional]
            + [inspect.Parameter(a["name"], inspect.Parameter.KEYWORD_ONLY, default=None,
                                 annotation=_TYPES[a["kind"]])
               for a in self._contextual])
        self.__annotations__ = {a["name"]: _TYPES[a["kind"]] for a in self.schema["arguments"]}
        self.__doc__ = self._help()

    def _help(self):
        lines = [f"{self.__qualname__}{self.__signature__}", "",
                 f"Host operation {self.id}."]
        if self.schema["arguments"]:
            lines += ["", "Arguments:"]
            width = max(len(a["name"]) for a in self.schema["arguments"])
            for argument in self.schema["arguments"]:
                default = _CONTEXT_DEFAULTS.get(argument["context"]) if argument["contextual"] \
                    else None
                state = f"defaults to {default}" if default else (
                    "required" if argument["required"] else "optional")
                lines.append("  {name:<{width}}  {kind:<6}  {state}  {summary}".format(
                    name=argument["name"], width=width, kind=argument["kind"], state=state,
                    summary=argument.get("summary", "")).rstrip())
        if self.example:
            lines += ["", "Example:", "  " + self.example]
        return "\n".join(lines)

    def __repr__(self):
        return f"<bloom operation {self.id}{self.__signature__}>"

    def _reject(self, argument, message):
        detail = {"code": "bloom.scripting.invalid-argument", "operation_id": self.id,
                  "argument": argument or "",
                  "message": f"{self.id}: {message}. Example: {self.example}"}
        raise OperationError([detail])

    def _collect(self, args, kwargs):
        if len(args) > len(self._positional):
            names = ", ".join(a["name"] for a in self._positional) or "no positional arguments"
            self._reject("", f"takes {len(self._positional)} positional arguments "
                             f"({names}) but {len(args)} were given")
        given = dict(kwargs)
        for value, argument in zip(args, self._positional):
            if argument["name"] in given:
                self._reject(argument["name"], f"argument '{argument['name']}' was given both "
                                               "positionally and by keyword")
            given[argument["name"]] = value
        for name in given:
            if name not in self._by_name:
                known = ", ".join(a["name"] for a in self.schema["arguments"])
                self._reject(name, f"has no argument '{name}'; it takes {known}")
        return given

    def __call__(self, *args, **kwargs):
        given = self._collect(args, kwargs)
        converted = {}
        for name, raw in given.items():
            if raw is None:
                continue
            argument = self._by_name[name]
            value = _plain(raw)
            if argument["kind"] == "time" and type(value) is float and math.isfinite(value):
                value = value.as_integer_ratio()
            if not _valid(value, argument["kind"]):
                self._reject(name, f"argument '{name}' expects {_EXPECTED[argument['kind']]}, "
                                   f"got {type(raw).__name__}")
            converted[name] = value
        for argument in self.schema["arguments"]:
            if argument["required"] and not argument["contextual"] \
                    and argument["name"] not in converted:
                self._reject(argument["name"], f"argument '{argument['name']}' is required")
        request = {"op": self.id, "args": converted}
        transaction = _state.transaction.get()
        if transaction is not None:
            transaction.operations.append(request)
            return None
        host = _state.host()
        return checked(host.transact([request], self.id, host.snapshot()["revision"]))


class _Domain:
    """One `bloom.<domain>` operation family, such as `bloom.ops.layer`."""

    def __init__(self, name):
        self.__dict__["_name"] = name

    def __dir__(self):
        return sorted(name for name in self.__dict__ if not name.startswith("_"))

    def __repr__(self):
        names = ", ".join(dir(self))
        return f"<bloom.ops.{self._name}: {names}>"


_registry = {}
_domains = {}


def _refresh():
    for schema in _state.host().schemas():
        operation = Operation(schema)
        _registry[operation.id] = operation
        domain = operation.id.split(".")[1].replace("-", "_")
        if domain not in _domains:
            _domains[domain] = _Domain(domain)
            globals()[domain] = _domains[domain]
        setattr(_domains[domain], operation.__name__, operation)


class _OpsModule(ModuleType):
    def __getattr__(self, name):
        _refresh()
        if name in globals():
            return globals()[name]
        raise AttributeError(name)

    def __dir__(self):
        _refresh()
        return sorted(set(_domains) | {"registry", "get", "Operation"})

    def __repr__(self):
        _refresh()
        return "<bloom.ops: " + ", ".join(sorted(_domains)) + ">"


def registry():
    _refresh()
    return _state.freeze(_registry)


def get(operation_id: str) -> Operation:
    _refresh()
    if operation_id not in _registry:
        raise OperationError([{"code": "bloom.scripting.unknown-operation",
                               "operation_id": operation_id, "argument": "",
                               "message": f"Unknown operation id {operation_id!r}"}])
    return _registry[operation_id]


sys.modules[__name__].__class__ = _OpsModule
