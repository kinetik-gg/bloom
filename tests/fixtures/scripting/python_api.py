"""Python/CLI conformance against built artifacts, without user site imports."""
import ast
import json
import keyword
import hashlib
import inspect
import pathlib
import pydoc
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

STUB_TYPES = {"bool": "bool", "int": "int", "float": "float", "str": "str",
              "id": "int | Proxy", "vec2": "tuple[float, float]",
              "vec3": "tuple[float, float, float]",
              "color4": "tuple[float, float, float, float]", "array": "tuple",
              "time": "Time", "value": "Value"}


def operation_stub(schemas):
    """The exact text of bloom/ops.pyi for a registry, so the stub can never drift from it."""
    lines = ["# Generated from the host operation registry by the bloom.scripting.python-api test.",
             "from collections.abc import Mapping",
             "from fractions import Fraction",
             "from typing import Any",
             "",
             "from .data import Proxy",
             "",
             "Time = int | tuple[int, int] | Fraction | float",
             "Value = float | tuple[float, ...]",
             "",
             "class Operation:",
             "    id: str",
             "    example: str",
             "    schema: Mapping[str, Any]",
             "    def __call__(self, *args: Any, **kwargs: Any) -> Mapping[str, Any] | None: ...",
             ""]
    domains = {}
    for schema in schemas:
        domains.setdefault(schema["id"].split(".")[1], []).append(schema)
    for domain in sorted(domains):
        name = "".join(part.capitalize() for part in domain.split("-")) + "Ops"
        lines.append(f"class {name}:")
        for schema in sorted(domains[domain], key=lambda item: item["id"]):
            verb = schema["id"].split(".")[2].replace("-", "_")
            if keyword.iskeyword(verb):
                verb += "_"
            params = ["self"]
            for argument in schema["arguments"]:
                if argument["contextual"]:
                    continue
                default = "" if argument["required"] else " = ..."
                params.append(f"{argument['name']}: {STUB_TYPES[argument['kind']]}{default}")
            contextual = [a for a in schema["arguments"] if a["contextual"]]
            if contextual:
                params.append("*")
                params += [f"{a['name']}: {STUB_TYPES[a['kind']]} = ..." for a in contextual]
            joined = ", ".join(params)
            lines.append(f"    def {verb}({joined}) -> Mapping[str, Any] | None: ...")
        lines += ["", f"{domain.replace('-', '_')}: {name}", ""]
    lines += ["def registry() -> Mapping[str, Operation]: ...",
              "def get(operation_id: str) -> Operation: ..."]
    return "\n".join(lines) + "\n"


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
            positional = [a for a in schema['arguments'] if not a['contextual']]
            contextual = [a for a in schema['arguments'] if a['contextual']]
            self.assertEqual(list(signature.parameters),
                             [a['name'] for a in positional + contextual])
            optional_seen = False
            for argument in positional:
                p = signature.parameters[argument['name']]
                self.assertEqual(p.kind, inspect.Parameter.POSITIONAL_OR_KEYWORD)
                self.assertEqual(p.default is inspect.Parameter.empty, argument['required'])
                # Positional calls bind in schema order, so required never follows optional.
                optional_seen = optional_seen or not argument['required']
                self.assertFalse(optional_seen and argument['required'], argument['name'])
            for argument in contextual:
                p = signature.parameters[argument['name']]
                self.assertEqual(p.kind, inspect.Parameter.KEYWORD_ONLY)
                self.assertIsNone(p.default)
            # Every operation publishes a call an artist can paste.
            self.assertTrue(schema['example'].startswith('bloom.ops.'), schema['id'])
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

    def test_every_operation_is_constructible(self):
        """SCRIPT-1's registry had 69 ids with no argument schema at all; none may remain."""
        for schema in self.host.schemas():
            self.assertTrue(schema['arguments'], schema['id'])
            for argument in schema['arguments']:
                self.assertTrue(argument['summary'], f"{schema['id']}.{argument['name']}")
                self.assertIn(argument['kind'], STUB_TYPES)
                self.assertIsNotNone(argument['example'], f"{schema['id']}.{argument['name']}")

    def test_context_defaults(self):
        """The first three minutes: omit composition, time and selection and they come from context."""
        composition = bloom.context.composition
        self.assertIsNotNone(composition)
        result = bloom.ops.layer.add_solid(name='Red', color=(1, 0, 0, 1))
        self.assertTrue(result['succeeded'])
        layer = next(o['id'] for o in result['outputs'] if o['name'] == 'layer')
        # Contextual arguments stay keyword-only, so an explicit target reads as one.
        self.assertTrue(
            bloom.ops.layer.rename('Renamed', layer=layer, composition=composition)['succeeded'])
        self.assertTrue(bloom.ops.layer.set_enabled(False, layer=layer)['succeeded'])
        # Schema and context agree about which arguments default and where from.
        schema = bloom.ops.get('bloom.layer.set-enabled').schema
        defaults = {a['name']: a['context'] for a in schema['arguments'] if a['contextual']}
        self.assertEqual(defaults, {'composition': 'composition', 'layer': 'layer'})
        for source in defaults.values():
            self.assertIn(source, bloom.context.DEFAULTS)

    def test_positional_and_proxy_arguments(self):
        composition = bloom.context.composition
        self.assertTrue(bloom.ops.layer.add_solid('Red', (1, 0, 0, 1))['succeeded'])
        self.assertTrue(bloom.ops.layer.add_solid('Green', [0, 1, 0, 1], (10, 20))['succeeded'])
        # Proxies and raw ids are interchangeable for id arguments.
        self.assertTrue(bloom.ops.composition.set_name('By proxy', composition=composition)['succeeded'])
        self.assertTrue(bloom.ops.composition.set_name('By id', composition=int(composition))['succeeded'])
        self.assertEqual(bloom.context.composition.name, 'By id')
        # Times accept whole frames, exact pairs, Fractions and floats.
        from fractions import Fraction
        for moment in (12, (1, 2), Fraction(3, 4), 1.5):
            with self.subTest(time=moment):
                self.assertTrue(bloom.ops.composition.set_duration(moment)['succeeded'])

    def test_owner_first_minutes(self):
        """The three lines from the owner's first session in the Script editor."""
        # 1. No composition argument: the current composition is the default.
        with self.assertRaises(bloom.OperationError) as error:
            bloom.ops.layer.add_solid()
        message = str(error.exception)
        self.assertIn('bloom.layer.add-solid', message)
        self.assertIn("'name'", message)
        self.assertIn('Example: bloom.ops.layer.add_solid(name="Red", color=(1, 0, 0, 1))', message)
        self.assertNotIn('keyword-only', message)
        # 2. A positional call is accepted, and a wrong type says what was expected.
        with self.assertRaises(bloom.OperationError) as error:
            bloom.ops.layer.add_solid(1)
        message = str(error.exception)
        self.assertIn("argument 'name' expects text, got int", message)
        self.assertIn('Example: bloom.ops.layer.add_solid', message)
        self.assertTrue(bloom.ops.layer.add_solid('Red', (1, 0, 0, 1))['succeeded'])
        # 3. The context repr summarises the session; no owner pointer leaks.
        text = repr(bloom.context)
        self.assertTrue(text.startswith('<bloom.context '), text)
        for field in ('project=', 'composition=', 'selection=', 'time='):
            self.assertIn(field, text)
        self.assertNotIn('_owner', text)
        self.assertNotIn('Host object', text)

    def test_layers_are_addressable(self):
        """Layer operations need a LayerId, so the snapshot has to publish one."""
        result = bloom.ops.layer.add_solid('Red', (1, 0, 0, 1))
        layer = next(o['id'] for o in result['outputs'] if o['name'] == 'layer')
        composition = bloom.context.composition
        found = bloom.data.layers(composition).get(layer)
        self.assertEqual(found.name, 'Red')
        self.assertEqual(repr(found), f"<Layer {layer} 'Red'>")
        self.assertTrue(bloom.ops.layer.rename('Renamed', layer=found)['succeeded'])
        self.assertEqual(bloom.data.layers(composition).get(layer).name, 'Renamed')
        # Headless runs have no selection, so a layer argument stays unanswered and says so.
        self.assertEqual(bloom.context.layers, ())
        with self.assertRaises(bloom.OperationError) as error:
            bloom.ops.layer.set_enabled(False)
        self.assertIn('bloom.context', str(error.exception))

    def test_reprs(self):
        self.assertEqual(repr(bloom.context.composition), "<Composition 1 'Composition'>")
        self.assertEqual(repr(bloom.context.project), "<Project 1 'Untitled'>")
        node = bloom.data.nodes(1)[0]
        self.assertRegex(repr(node), r"^<Node \d+ '[^']+'>$")
        self.assertNotIn('_owner', repr(node))
        bloom.ops.node.add(nodeType='bloom.value-scalar', position=(0, 0))
        added = bloom.data.nodes(1)[-1]
        bloom.ops.node.remove(nodes=(added.id,))
        self.assertEqual(repr(added), f'<Node {added.id} (stale)>')

    def test_help_and_dir(self):
        document = pydoc.render_doc(bloom.ops.layer.add_solid).replace('\b', '')
        for fragment in ('bloom.ops.layer.add_solid', 'Arguments:', 'The layer name.',
                         'defaults to bloom.context.composition', 'Example:',
                         'bloom.ops.layer.add_solid(name="Red", color=(1, 0, 0, 1))'):
            self.assertIn(fragment, document)
        names = dir(bloom.ops)
        for domain in ('animation', 'asset', 'composition', 'data_block', 'layer', 'merge',
                       'node', 'node_group', 'parameter', 'project'):
            self.assertIn(domain, names)
        self.assertEqual(names, sorted(names))
        self.assertIn('add_solid', dir(bloom.ops.layer))
        self.assertIn('bloom.ops.layer', repr(bloom.ops.layer))

    def test_stubs_parse(self):
        for stub in pathlib.Path(bloom.__file__).parent.glob('*.pyi'):
            ast.parse(stub.read_text(), filename=str(stub))

    def test_operation_stub_matches_registry(self):
        stub = pathlib.Path(bloom.__file__).parent / 'ops.pyi'
        self.assertEqual(stub.read_text(), operation_stub(self.host.schemas()),
                         'regenerate bloom/ops.pyi from the registry')

    def test_cli_applies_the_same_context_defaults(self):
        with tempfile.TemporaryDirectory(prefix='bloom-cli-context-') as directory:
            script = pathlib.Path(directory) / 'script.json'
            script.write_text(json.dumps([
                {'op': 'bloom.layer.add-solid',
                 'args': {'name': 'Red', 'color': [1, 0, 0, 1]}},
                {'op': 'bloom.composition.set-name', 'args': {'name': 'From context'}}]))
            command = subprocess.run([CLI, 'run', str(script)], capture_output=True, timeout=60)
            self.assertEqual(command.returncode, 0, command.stderr.decode())
            self.assertIn(b'executed 2 transaction(s)', command.stdout)

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
