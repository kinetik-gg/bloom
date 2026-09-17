from fractions import Fraction
from .data import Proxy
project: Proxy
composition: Proxy | None
selection: tuple[Proxy, ...]
time: Fraction
headless: bool
