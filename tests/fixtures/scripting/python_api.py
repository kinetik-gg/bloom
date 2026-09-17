"""Python/CLI conformance against built artifacts, without user site imports."""
import ast
import keyword
import hashlib
import inspect
import pathlib
import subprocess
import sys
import tempfile
import threading
import time
import unittest
sys.path.insert(0, sys.argv.pop(1))
CLI = sys.argv.pop(1)
import _bloom
import bloom
from bloom import _state


class PythonApi(unittest.TestCase):
    def setUp(self):
        self.host = _bloom.Host()
        self.token = _state.bind(self.host)

    def tearDown(self):
        _state._host.reset(self.token)

    def test_registry(self):
        registry = bloom.ops.registry()
        self.assertEqual({s['id'] for s in self.host.schemas()}, set(registry))
        for schema in self.host.schemas():
            _, domain, verb = schema['id'].split('.')
            verb = verb.replace('-', '_')
            name = verb + '_' if keyword.iskeyword(verb) else verb
            self.assertEqual(getattr(getattr(bloom.ops, domain.replace('-', '_')), name).id, schema['id'])
            signature = inspect.signature(registry[schema['id']])
            self.assertEqual(list(signature.parameters), [a['name'] for a in schema['arguments']])
            for argument in schema['arguments']:
                p = signature.parameters[argument['name']]
                self.assertEqual(p.kind, inspect.Parameter.KEYWORD_ONLY)
                self.assertEqual(p.default is inspect.Parameter.empty, argument['required'])
        with self.assertRaises(bloom.OperationError):
            bloom.ops.layer.add_solid(composition=1, name='bad', color=(0, 0, float('nan'), 1))
        with self.assertRaises(bloom.OperationError):
            bloom.ops.project.set_name(name='bad', unknown=1)
        for field in ('frameRateNumerator', 'frameRateDenominator'):
            for value in (-1, 0, 2**32, 2**63 - 1):
                with self.subTest(field=field, value=value), self.assertRaises(bloom.OperationError):
                    bloom.ops.composition.add(name='Invalid rate', width=64, height=64,
                                              duration=(48, 1), **{field: value})
        self.assertEqual(bloom.data.snapshot()['revision'], 0)

    def test_stubs_parse(self):
        for stub in pathlib.Path(bloom.__file__).parent.glob('*.pyi'):
            ast.parse(stub.read_text(), filename=str(stub))

    def test_removed_node_is_stale(self):
        before = {node.id for node in bloom.data.nodes(1)}
        bloom.ops.node.add(composition=1, nodeType='bloom.value-scalar', position=(0, 0))
        node = next(node for node in bloom.data.nodes(1) if node.id not in before)
        bloom.ops.node.remove(composition=1, nodes=(node.id,))
        with self.assertRaises(bloom.StaleObjectError) as error:
            node.resolve()
        self.assertEqual(error.exception.id, node.id)
        self.assertEqual(error.exception.revision, bloom.data.snapshot()['revision'])

    def test_transactions(self):
        before = bloom.data.snapshot()
        with bloom.transactions.group('Two edits'):
            bloom.ops.project.set_name(name='first')
            bloom.ops.composition.set_name(composition=1, name='second')
        self.assertEqual(bloom.data.snapshot()['revision'], before['revision'] + 1)
        bloom.transactions.undo()
        self.assertEqual(bloom.context.project.name, 'Untitled')
        self.assertEqual(bloom.context.composition.name, 'Composition')
        with self.assertRaises(bloom.OperationError):
            with bloom.transactions.group('Conflict', expected_revision=0):
                bloom.ops.project.set_name(name='must not apply')
        with self.assertRaises(ValueError):
            with bloom.transactions.group('Abort'):
                bloom.ops.project.set_name(name='must not apply')
                raise ValueError('abort')
        self.assertEqual(bloom.context.project.name, 'Untitled')

    def test_proxy(self):
        node = bloom.data.nodes(1)[0]
        with self.assertRaises(TypeError):
            bloom.data.snapshot()['project']['name'] = 'illegal'
        with self.assertRaises(AttributeError):
            node.id = 99
        bloom.ops.composition.add(name='Other', width=64, height=64, duration=(48, 1))
        bloom.ops.composition.delete(composition=1)
        with self.assertRaises(bloom.StaleObjectError) as error:
            node.resolve()
        self.assertEqual(error.exception.id, node.id)
        self.assertEqual(error.exception.revision, bloom.data.snapshot()['revision'])

    def test_events(self):
        called = []
        thread = threading.get_ident()
        with bloom.events.subscribe(lambda event: called.append((threading.get_ident(), event))):
            bloom.ops.project.set_name(name='event')
            self.assertEqual(called, [])
            self.assertGreater(bloom.events.pump(), 0)
            self.assertTrue(all(identity == thread for identity, event in called))
            self.assertTrue(any(event['kind'] == 'revision' for identity, event in called))

    def test_tasks(self):
        started, release = threading.Event(), threading.Event()
        def work(task):
            task.report_progress(1, 2, 'Computing')
            started.set()
            release.wait(2)
            task.check_cancelled()
        handle = bloom.tasks.submit(work)
        self.assertTrue(started.wait(2))
        self.assertEqual(handle.progress.completed, 1)
        handle.cancel()
        release.set()
        with self.assertRaises(bloom.CancelledError):
            handle.result(3)

    def test_event_overflow(self):
        called = []
        with bloom.events.subscribe(called.append):
            for index in range(520):
                bloom.ops.project.set_name(name=f'Event {index}')
            bloom.events.pump()
        self.assertEqual(called[0]['kind'], 'reset')

    def test_worker_render_and_concurrent_read(self):
        with tempfile.TemporaryDirectory(prefix='bloom-python-worker-') as directory:
            started = threading.Event()
            def render(task):
                started.set()
                return bloom.render.frame(12, out=pathlib.Path(directory) / 'worker.png')
            handle = bloom.tasks.submit(render)
            self.assertTrue(started.wait(2))
            for _ in range(10):
                self.assertEqual(bloom.data.snapshot()['revision'], 0)
            self.assertEqual(handle.result(30).published_frames, 1)

    def test_task_cancels_native_range(self):
        with tempfile.TemporaryDirectory(prefix='bloom-python-cancel-') as directory:
            root = pathlib.Path(directory)
            bloom.ops.composition.add(name='Small', width=4, height=4, duration=(48, 1))
            composition = bloom.data.compositions[-1]
            handle = bloom.tasks.submit(lambda task: bloom.render.range(
                0, 999, composition=composition, out=root / 'cancel.png'))
            deadline = time.monotonic() + 10
            while len(self.host.tasks()) < 2 and not handle.done and time.monotonic() < deadline:
                time.sleep(0.001)
            self.assertGreaterEqual(len(self.host.tasks()), 2)
            handle.cancel()
            while not handle.done and time.monotonic() < deadline:
                time.sleep(0.001)
            self.assertTrue(handle.done, 'Native range cooperates with its parent Python task')
            self.assertLess(len(list(root.glob('cancel.*.png'))), 1000)
            with self.assertRaises(bloom.CancelledError):
                handle.result()

    def test_worker_cannot_join_authoring_transaction(self):
        with bloom.transactions.group('Author only'):
            handle = bloom.tasks.submit(lambda task: bloom.ops.project.set_name(name='Wrong thread'))
            with self.assertRaisesRegex(RuntimeError, 'authoring thread'):
                handle.result(3)
            bloom.ops.project.set_name(name='Author')
        self.assertEqual(bloom.context.project.name, 'Author')
        bloom.transactions.undo()
        self.assertEqual(bloom.context.project.name, 'Untitled')

    def test_render_matches_cli(self):
        with tempfile.TemporaryDirectory(prefix='bloom-python-') as directory:
            root = pathlib.Path(directory)
            bloom.ops.layer.add_solid(composition=1, name='Red', color=(1, 0, 0, 1), position=(960, 540))
            self.host.save(str(root / 'scene.bloom'))
            result = bloom.render.frame(12, out=root / 'python.png')
            command = subprocess.run([CLI, 'render', '--project', str(root / 'scene.bloom'),
                                      '--frame', '12', '--preset', 'PngRgba8SrgbV1',
                                      '--out', str(root / 'cli')], capture_output=True, timeout=60)
            self.assertEqual(command.returncode, 0, command.stderr.decode())
            expected = (root / 'cli/frame.png').read_bytes()
            self.assertEqual((root / 'python.png').read_bytes(), expected)
            self.assertEqual(result.files[0].digest, 'sha256:' + hashlib.sha256(expected).hexdigest())
            with self.assertRaises(AttributeError):
                result.published_frames = 2


if __name__ == '__main__':
    unittest.main()
