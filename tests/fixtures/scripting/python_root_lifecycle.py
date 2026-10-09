"""Owned roots close before embedded finalization and standalone module teardown."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

PACKAGE = str(pathlib.Path(sys.argv.pop(1)).resolve())
CLI = str(pathlib.Path(sys.argv.pop(1)).resolve())


class RootLifecycle(unittest.TestCase):
    def child(self, source):
        result = subprocess.run(
            [sys.executable, "-I", "-c", f"import sys; sys.path.insert(0, {PACKAGE!r})\n" + source],
            capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("leaked", result.stderr.lower())
        return result

    def test_direct_extension_atexit_without_package(self):
        result = self.child("""
import atexit
def after_retirement():
    assert _bloom._root_retirement_snapshot()['participants'] == 0
    assert 'bloom' not in sys.modules
    print('DIRECT_EXIT_PROVEN')
atexit.register(after_retirement)  # Runs after the extension's later registered close callback.
import _bloom
host = _bloom.Host()
ticket = host.track_task('Retained at exit')
ticket.finish('')
assert _bloom._root_retirement_snapshot()['participants'] == 1
""")
        self.assertIn("DIRECT_EXIT_PROVEN", result.stdout)

    def test_lazy_manual_replacement_gc_and_retained_ticket(self):
        self.child("""
import gc, time, _bloom, bloom
from bloom import _state
default = _state.host()
manual = _bloom.Host()
ticket = manual.track_task('Retained finished ticket')
ticket.finish('')
replacement = _bloom.Host()
token = _state.bind(replacement)
_state._host.reset(token)
del replacement, token
gc.collect()
deadline = time.monotonic() + 2
while _bloom._root_retirement_snapshot()['participants'] > 2 and time.monotonic() < deadline:
    _bloom._collect_retired_roots()
    time.sleep(0.001)
assert _bloom._root_retirement_snapshot()['participants'] == 2
assert _bloom._shutdown_owned_roots(2000)
assert _bloom._root_retirement_snapshot()['roots'] == 0
assert default is not None and manual is not None  # User globals survive explicit closure.
assert ticket.id > 0
ticket.cancel(); ticket.progress(0, None, 'Closed'); ticket.finish('')
for operation in (lambda: manual.track_task('Rejected'), lambda: manual.tasks(),
                  lambda: manual.cancel(ticket.id), lambda: _bloom.Host(),
                  lambda: bloom.tasks.submit(lambda task: None)):
    try:
        operation()
    except RuntimeError:
        pass
    else:
        raise AssertionError('Closed admission must reject safely')
""")

    def test_embedded_script_exception(self):
        with tempfile.TemporaryDirectory(prefix="bloom-python-root-") as directory:
            path = pathlib.Path(directory) / "failure.py"
            path.write_text("import _bloom\nmanual = _bloom.Host()\n"
                            "ticket = manual.track_task('Exception unwind')\n"
                            "ticket.finish('')\nraise RuntimeError('EXPECTED_SCRIPT_FAILURE')\n")
            result = subprocess.run([CLI, "run", str(path)], capture_output=True,
                                    text=True, timeout=20)
            self.assertEqual(result.returncode, 4, result.stdout + result.stderr)
            self.assertIn("EXPECTED_SCRIPT_FAILURE", result.stdout)
            self.assertNotIn("terminate", result.stderr.lower())
            self.assertNotIn("leaked", result.stderr.lower())


if __name__ == "__main__":
    unittest.main()
