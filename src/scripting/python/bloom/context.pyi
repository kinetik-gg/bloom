from fractions import Fraction
from .data import Proxy
DEFAULTS: dict[str, str]
project: Proxy
composition: Proxy | None
selection: tuple[Proxy, ...]
layers: tuple[Proxy, ...]
time: Fraction
headless: bool
