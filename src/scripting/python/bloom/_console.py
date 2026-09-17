"""Embedding support; trusted source runs on the scripting thread."""
import code
import contextlib
import io
import traceback
import sys
import time

from . import _state
from .errors import CancelledError

import bloom


class _Output(io.StringIO):
    """Keep runaway prints from exhausting the panel or CLI's memory."""
    def write(self, text):
        remaining = max(0, 262144 - self.tell())
        super().write(text[:remaining])
        return len(text)


class Console:
    def __init__(self):
        self.globals = {"__name__": "__main__", "bloom": bloom}
        self.compiler = code.CommandCompiler()

    def execute(self, source, interactive=False):
        output = _Output()
        succeeded = True
        incomplete = False
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            try:
                compiled = (self.compiler(source, "<Bloom Script>", "single") if interactive
                            else compile(source, "<Bloom Script>", "exec"))
                if compiled is None:
                    incomplete = True
                else:
                    host = _state.host()
                    previous = sys.gettrace()
                    started = time.thread_time()

                    def trace(frame, event, arg):
                        frame.f_trace_opcodes = True
                        if host.cancellation_requested:
                            raise CancelledError("Script cancelled")
                        if not host.headless and time.thread_time() - started > 2:
                            raise TimeoutError("Script exceeded its CPU slice; use bloom.tasks")
                        return trace

                    try:
                        sys.settrace(trace)
                        exec(compiled, self.globals, self.globals)
                    finally:
                        sys.settrace(previous)
            except BaseException:
                succeeded = False
                traceback.print_exc()
        return output.getvalue(), succeeded, incomplete
