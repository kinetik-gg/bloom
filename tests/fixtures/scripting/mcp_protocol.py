"""Drive the Qt-free MCP server through actual stdin/stdout pipes."""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import time
import unittest

MCP, CLI = sys.argv[1:3]
del sys.argv[1:3]


def request(id, method, params=None):
    result = {'jsonrpc': '2.0', 'id': id, 'method': method}
    if params is not None:
        result['params'] = params
    return json.dumps(result)


def tool(id, name, arguments):
    return request(id, 'tools/call', {'name': name, 'arguments': arguments})


INIT = request(1, 'initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                               'clientInfo': {'name': 'Bloom conformance', 'version': '1'}})
READY = json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'})


def exchange(lines, project=None):
    command = [MCP] + (['--project', str(project)] if project else [])
    result = subprocess.run(command, input=('\n'.join(lines) + '\n').encode(),
                            capture_output=True, timeout=90)
    if result.returncode != 0:
        raise AssertionError(result.stderr.decode(errors='replace'))
    return [json.loads(line) for line in result.stdout.splitlines()]


class Protocol(unittest.TestCase):
    def test_parameter_projection(self):
        color = [0.2, 0.3, 0.4, 1.0]
        responses = exchange([INIT, READY, tool(2, 'transact', {
            'expectedRevision': 0, 'operations': [{'op': 'bloom.layer.add-solid',
                'args': {'composition': 1, 'name': 'Read values', 'color': color}}]}),
            tool(3, 'query', {'kind': 'parameters', 'composition': 1})])
        content = responses[-1]['result']['structuredContent']
        self.assertEqual(content['revision'], 1)
        self.assertTrue(any(record['source'] == 'constant' and record['value'] == color
                            for record in content['records']))

    def test_handshake_transactions_and_events(self):
        responses = exchange([
            INIT, READY, request(2, 'tools/list', {}),
            tool(3, 'transact', {'expectedRevision': 0, 'operations': [
                {'op': 'bloom.project.set-name', 'args': {'name': 'MCP'}},
                {'op': 'bloom.composition.set-name', 'args': {'composition': 1, 'name': 'Atomic'}}]}),
            tool(4, 'query', {'kind': 'project'}),
            tool(5, 'transact', {'expectedRevision': 0, 'operations': [
                {'op': 'bloom.project.set-name', 'args': {'name': 'Refused'}}]}),
            tool(6, 'query', {'kind': 'project'}),
            tool(7, 'events', {'sinceRevision': 0}),
            tool(8, 'query', {'kind': 'operations'})])
        self.assertEqual(responses[0]['result']['serverInfo']['name'], 'Bloom')
        self.assertEqual({t['name'] for t in responses[1]['result']['tools']},
                         {'query', 'transact', 'render', 'export', 'events'})
        self.assertTrue(responses[2]['result']['structuredContent']['succeeded'])
        self.assertEqual(responses[3]['result']['structuredContent']['project']['name'], 'MCP')
        self.assertFalse(responses[4]['result']['structuredContent']['succeeded'])
        self.assertEqual(responses[5]['result']['structuredContent']['revision'], 1)
        self.assertTrue(responses[6]['result']['structuredContent']['events'])
        records = responses[7]['result']['structuredContent']['records']
        ids = [record['id'] for record in records]
        # The registry grows with every command lane (DATA-1 added the data-block operations
        # right after SCRIPT-1 pinned 75); pin uniqueness and a floor, not the exact count.
        self.assertGreaterEqual(len(ids), 75)
        self.assertEqual(len(ids), len(set(ids)))
        self.assertIn('bloom.data-block.remove', ids)
        # Every id is constructible: it names its arguments, describes them, and publishes a
        # pasteable example. An entry with no schema is an id an agent can see and never call.
        for record in records:
            self.assertTrue(record['arguments'], record['id'])
            self.assertTrue(record['example'].startswith('bloom.ops.'), record['id'])
            for argument in record['arguments']:
                self.assertTrue(argument['summary'], f"{record['id']}.{argument['name']}")
                self.assertEqual(argument['contextual'], argument['context'] != '')

    def test_context_defaults_match_the_python_client(self):
        """Omitted contextual arguments come from the session, exactly as they do in Python."""
        responses = exchange([
            INIT, READY,
            tool(2, 'query', {'kind': 'operations'}),
            tool(3, 'transact', {'expectedRevision': 0, 'operations': [
                {'op': 'bloom.layer.add-solid', 'args': {'name': 'Red', 'color': [1, 0, 0, 1]}},
                {'op': 'bloom.composition.set-name', 'args': {'name': 'From context'}}]}),
            tool(4, 'query', {'kind': 'compositions'}),
            tool(5, 'transact', {'expectedRevision': 1, 'operations': [
                {'op': 'bloom.layer.set-enabled', 'args': {'enabled': False}}]})])
        schema = next(record for record in responses[1]['result']['structuredContent']['records']
                      if record['id'] == 'bloom.layer.add-solid')
        composition = next(a for a in schema['arguments'] if a['name'] == 'composition')
        self.assertTrue(composition['contextual'])
        self.assertEqual(composition['context'], 'composition')
        self.assertTrue(responses[2]['result']['structuredContent']['succeeded'])
        self.assertEqual(responses[3]['result']['structuredContent']['records'][0]['name'],
                         'From context')
        # A headless session has no selection, so a layer argument stays unanswered and the
        # rejection says which context value was missing.
        rejected = responses[4]['result']['structuredContent']
        self.assertFalse(rejected['succeeded'])
        self.assertIn('bloom.context', rejected['diagnostics'][0]['message'])

    def test_hostile_requests(self):
        hostile = [
            '{}', '[]', 'null', '{"jsonrpc":"2.0","id":null,"method":"ping"}',
            '{"jsonrpc":"2.0","id":1,"id":2,"method":"ping"}',
            '{"jsonrpc":"2.0","id":1,"\\u0069d":2,"method":"ping"}',
            request(10, 'unknown'), request(11, 'tools/call', {'name': 'query', 'arguments': []}),
            tool(12, 'query', {'kind': 'project', 'unknown': 1}),
            tool(13, 'transact', {'expectedRevision': True, 'operations': []}),
            tool(14, 'transact', {'expectedRevision': 0, 'operations': [
                {'op': 'bloom.project.set-name', 'args': {'name': 'No mutation'}},
                {'op': 'bloom.project.set-name', 'args': {'name': 42}}]}),
            '[' * 1000 + '0' + ']' * 1000,
            ' ' * (1024 * 1024 + 1), '{"x":"\\ud800"}',
            tool(15, 'render', {'composition': 1, 'frame': -1, 'preset': 'PngRgba8SrgbV1', 'destination': 'bad'}),
            tool(16, 'export', {'composition': 1, 'frame': 12, 'preset': 'PngRgba8SrgbV1',
                               'destination': 'bad', 'audio': True}),
            tool(17, 'query', {'kind': 'project', 'composition': 1}),
        ]
        for field in ('frameRateNumerator', 'frameRateDenominator'):
            for value in (-1, 0, 2**32, 2**63 - 1):
                hostile.append(tool(18, 'transact', {'expectedRevision': 0, 'operations': [
                    {'op': 'bloom.composition.add', 'args': {'name': 'Invalid rate', 'width': 64,
                        'height': 64, 'duration': [48, 1], field: value}}]}))
        responses = exchange([INIT, READY, *hostile, tool(99, 'query', {'kind': 'project'})])
        self.assertEqual(len(responses), len(hostile) + 2)
        self.assertTrue(all('error' in response for response in responses[1:-1]))
        self.assertEqual(responses[-1]['result']['structuredContent']['revision'], 0)
        self.assertEqual(responses[-1]['result']['structuredContent']['project']['name'], 'Untitled')

    def test_cancel_active_render(self):
        with tempfile.TemporaryDirectory(prefix='bloom-mcp-cancel-') as directory:
            process = subprocess.Popen([MCP], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE)
            try:
                process.stdin.write((INIT + '\n').encode())
                process.stdin.flush()
                self.assertEqual(json.loads(process.stdout.readline())['id'], 1)
                render = tool(2, 'render', {'composition': 1, 'first': 0, 'last': 1000,
                                          'preset': 'PngRgba8SrgbV1',
                                          'destination': str(pathlib.Path(directory) / 'frame.png')})
                process.stdin.write((READY + '\n' + render + '\n').encode())
                process.stdin.flush()
                time.sleep(0.15)
                cancellation = {'jsonrpc': '2.0', 'method': 'notifications/cancelled',
                                'params': {'requestId': 2}}
                output, diagnostic = process.communicate((json.dumps(cancellation) + '\n').encode(),
                                                          timeout=30)
                self.assertEqual(process.returncode, 0, diagnostic.decode())
                result = json.loads(output.splitlines()[-1])
                self.assertTrue(result['result']['isError'], result)
                self.assertLess(len(list(pathlib.Path(directory).glob('*.png'))), 1001)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_media4_tiff_export(self):
        with tempfile.TemporaryDirectory(prefix='bloom-mcp-export-') as directory:
            destination = pathlib.Path(directory) / 'frame.tiff'
            responses = exchange([INIT, READY, tool(2, 'export', {
                'composition': 1, 'frame': 12, 'preset': 'TiffRgba16SrgbV1',
                'destination': str(destination)})])
            result = responses[-1]['result']
            if sys.platform != 'linux':
                self.assertTrue(result['isError'], result)  # The MEDIA-4 worker fallback.
                return
            self.assertFalse(result['isError'], result)
            self.assertEqual(result['structuredContent']['files'][0]['digest'],
                             'sha256:' + hashlib.sha256(destination.read_bytes()).hexdigest())

    def test_render_digest_and_startup_project(self):
        with tempfile.TemporaryDirectory(prefix='bloom-mcp-') as directory:
            root = pathlib.Path(directory)
            project = root / 'scene.bloom'
            subprocess.run([CLI, 'new', str(project)], check=True, capture_output=True, timeout=30)
            responses = exchange([INIT, READY, tool(2, 'render', {
                'composition': 1, 'frame': 12, 'preset': 'PngRgba8SrgbV1',
                'destination': str(root / 'mcp.png')})], project)
            result = responses[-1]['result']
            self.assertFalse(result['isError'], result)
            subprocess.run([CLI, 'render', '--project', str(project), '--frame', '12',
                            '--preset', 'PngRgba8SrgbV1', '--out', str(root / 'cli')],
                           check=True, capture_output=True, timeout=60)
            image = (root / 'cli/frame.png').read_bytes()
            self.assertEqual((root / 'mcp.png').read_bytes(), image)
            self.assertEqual(result['structuredContent']['files'][0]['digest'],
                             'sha256:' + hashlib.sha256(image).hexdigest())

    def test_media4_composition_export(self):
        with tempfile.TemporaryDirectory(prefix='bloom-mcp-media-') as directory:
            destination = pathlib.Path(directory) / 'preview.mov'
            responses = exchange([INIT, READY, tool(2, 'transact', {
                'expectedRevision': 0, 'operations': [{'op': 'bloom.composition.add', 'args': {
                    'name': 'Export', 'width': 256, 'height': 128, 'duration': [2, 1]}}]}),
                tool(3, 'export', {'composition': 2, 'first': 0, 'last': 1,
                    'preset': 'ProResMovV1', 'audio': False, 'destination': str(destination)})])
            result = responses[-1]['result']
            if sys.platform != 'linux':
                self.assertTrue(result['isError'], result)  # The MEDIA-4 worker fallback.
                return
            self.assertFalse(result['isError'], result)
            content = result['structuredContent']
            self.assertEqual(content['publishedFrames'], 2)
            self.assertIn('not an Apple-authorized', content['preservationReport'])
            self.assertEqual(content['files'][0]['digest'],
                             'sha256:' + hashlib.sha256(destination.read_bytes()).hexdigest())


if __name__ == '__main__':
    unittest.main()
