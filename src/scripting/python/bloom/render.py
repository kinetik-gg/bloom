"""Reference frame publication through the host render runner."""
from dataclasses import dataclass
from pathlib import Path

from . import _state, context


@dataclass(frozen=True)
class RenderFile:
    path: str
    digest: str


@dataclass(frozen=True)
class RenderResult:
    files: tuple[RenderFile, ...]
    published_frames: int
    preservation_report: str


def _render(first, last, is_range, out, composition, preset):
    if type(first) is not int or type(last) is not int or first < 0 or last < first:
        raise ValueError("Frames must be nonnegative integers in ascending order")
    if composition is None:
        composition = context.composition
    if composition is None:
        raise ValueError("The current project has no composition")
    task = _state._task.get()
    cancellation_task = task._ticket.id if task is not None and task._ticket is not None else 0
    result = _state.host().render(int(composition), first, last, is_range, preset, str(Path(out)),
                                 cancellation_task)
    return RenderResult(tuple(RenderFile(**item) for item in result["files"]),
                        result["published_frames"], result["preservation_report"])


def frame(number: int, *, out, composition=None, preset="PngRgba8SrgbV1") -> RenderResult:
    return _render(number, number, False, out, composition, preset)


def range(first: int, last: int, *, out, composition=None,
          preset="PngRgba8SrgbV1") -> RenderResult:
    return _render(first, last, True, out, composition, preset)
