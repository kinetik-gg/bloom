"""Native task handles and cooperative work on a dedicated Python worker."""
import sys

from concurrent.futures import ThreadPoolExecutor
from contextvars import copy_context
from dataclasses import dataclass
from threading import BoundedSemaphore, Event, Lock

from . import _state
from .errors import CancelledError

_executor = None
_executor_lock = Lock()
_admission = BoundedSemaphore(128)


@dataclass(frozen=True)
class Progress:
    completed: int = 0
    total: int | None = None
    phase: str = ""


class TaskHandle:
    def __init__(self):
        self._cancelled = Event()
        self._progress = Progress()
        self._future = None
        self._ticket = None
        self._host = None
        self._epoch = 0

    @property
    def progress(self):
        return self._progress

    @property
    def cancellation_requested(self):
        return (self._cancelled.is_set()
                or (self._ticket is not None and self._ticket.cancelled)
                or (self._host is not None and self._host.cancellation_epoch != self._epoch))

    @property
    def done(self):
        return self._future is not None and self._future.done()

    def cancel(self):
        self._cancelled.set()
        if self._ticket is not None:
            self._ticket.cancel()
        return self._future.cancel() if self._future is not None else True

    def check_cancelled(self):
        if self.cancellation_requested:
            raise CancelledError("Task was cancelled")

    def report_progress(self, completed=0, total=None, phase=""):
        if type(completed) is not int or completed < 0:
            raise ValueError("Completed work must be a nonnegative integer")
        if total is not None and (type(total) is not int or total < completed):
            raise ValueError("Total work must be an integer at least equal to completed work")
        self._progress = Progress(completed, total, phase)
        if self._ticket is not None:
            self._ticket.progress(completed, total, phase)
        self.check_cancelled()

    def result(self, timeout=None):
        self.check_cancelled()
        return self._future.result(timeout)


def submit(function, *args, **kwargs) -> TaskHandle:
    """Run function(task, *args, **kwargs); call task.check_cancelled() between slices.

    Durable edits belong in an authoring-thread continuation after result() returns.
    """
    global _executor
    if not callable(function):
        raise TypeError("Task work must be callable")
    if not _admission.acquire(blocking=False):
        raise RuntimeError("The Python task queue is full (128 tasks)")
    with _executor_lock:
        if _executor is None:
            _executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="bloom-python")
    handle = TaskHandle()
    ctx = copy_context()
    # Ensure a host has been selected before crossing to another thread.
    host = _state.host()
    handle._host = host
    handle._epoch = host.cancellation_epoch

    def run():
        token = _state.bind(host)
        transaction_token = _state.transaction.set(None)
        task_token = _state._task.set(handle)
        try:
            handle.check_cancelled()
            handle._ticket = host.track_task(getattr(function, "__name__", "Python task"))
            try:
                previous = sys.gettrace()
                def trace(frame, event, arg):
                    frame.f_trace_opcodes = True
                    handle.check_cancelled()
                    return trace
                try:
                    sys.settrace(trace)
                    result = function(handle, *args, **kwargs)
                finally:
                    sys.settrace(previous)
                handle._ticket.finish("")
                return result
            except BaseException as error:
                handle._ticket.finish(str(error))
                raise
        finally:
            _state._task.reset(task_token)
            _state.transaction.reset(transaction_token)
            _state._host.reset(token)

    handle._future = _executor.submit(ctx.run, run)
    handle._future.add_done_callback(lambda future: _admission.release())
    return handle


def snapshots():
    return _state.freeze(_state.host().tasks())


def cancel(task_id: int) -> bool:
    return _state.host().cancel(task_id)


def _shutdown():
    global _executor
    if _executor is not None:
        _executor.shutdown(wait=True, cancel_futures=True)
        _executor = None
