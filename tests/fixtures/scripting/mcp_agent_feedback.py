"""Regressions from live-testing Bloom's MCP server with an agent, over real stdio pipes."""
import json
import os
import struct
import zlib
import subprocess
import sys
import tempfile
import unittest

MCP = sys.argv[1]
del sys.argv[1]
CACHE = tempfile.TemporaryDirectory(prefix='bloom-mcp-feedback-cache-')
os.environ['XDG_CACHE_HOME'] = CACHE.name
os.environ['LOCALAPPDATA'] = CACHE.name


class Server:
    """One bloom-mcp process, initialized, answering one call at a time."""

    def __init__(self, *arguments):
        self.process = subprocess.Popen([MCP, *arguments], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.next_id = 1
        self.send({'jsonrpc': '2.0', 'id': 0, 'method': 'initialize',
                   'params': {'protocolVersion': '2025-11-25', 'capabilities': {},
                              'clientInfo': {'name': 'feedback', 'version': '1'}}})
        self.receive()
        self.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})

    def send(self, message):
        self.process.stdin.write((json.dumps(message) + '\n').encode())
        self.process.stdin.flush()

    def receive(self):
        line = self.process.stdout.readline()
        if not line:
            raise AssertionError(self.process.stderr.read().decode(errors='replace'))
        return json.loads(line)

    def raw(self, name, arguments):
        self.next_id += 1
        self.send({'jsonrpc': '2.0', 'id': self.next_id, 'method': 'tools/call',
                   'params': {'name': name, 'arguments': arguments}})
        return self.receive()

    def call(self, name, arguments):
        response = self.raw(name, arguments)
        if 'error' in response:
            raise AssertionError(response['error'])
        return response['result']['structuredContent']

    def revision(self):
        return self.call('query', {'kind': 'project'})['revision']

    def transact(self, *operations):
        return self.call('transact', {'expectedRevision': self.revision(),
                                      'operations': [{'op': op, 'args': args}
                                                     for op, args in operations]})

    def nodes(self, composition=1):
        return self.call('query', {'kind': 'nodes', 'composition': composition})['records']

    def parameter(self, parameter, composition=1):
        return self.call('query', {'kind': 'parameter', 'composition': composition,
                                   'parameter': parameter})['record']

    def role(self, role, type_part=None):
        """The newest node's parameter with this role."""
        for node in reversed(self.nodes()):
            if role in node['parameters'] and (type_part is None or type_part in node['typeId']):
                return node['parameters'][role]
        raise AssertionError(f'no node has a {role} parameter')

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=60)
        self.process.stdout.close()
        self.process.stderr.close()


def png_pixel(path, x, y):
    """One RGBA8 pixel of a non-interlaced 8-bit RGBA PNG, decoded with the standard library."""
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    offset, chunks, width, height = 8, b'', 0, 0
    while offset < len(data):
        length, kind = struct.unpack('>I4s', data[offset:offset + 8])
        body = data[offset + 8:offset + 8 + length]
        if kind == b'IHDR':
            width, height, depth, colour = struct.unpack('>IIBB', body[:10])
            assert (depth, colour) == (8, 6), (depth, colour)
        elif kind == b'IDAT':
            chunks += body
        offset += 12 + length
    raw = zlib.decompress(chunks)
    stride = width * 4
    previous = bytearray(stride)
    for row in range(height):
        start = row * (stride + 1)
        kind, line = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        for i in range(stride):
            left = line[i - 4] if i >= 4 else 0
            up, corner = previous[i], previous[i - 4] if i >= 4 else 0
            if kind == 1:
                line[i] = (line[i] + left) & 255
            elif kind == 2:
                line[i] = (line[i] + up) & 255
            elif kind == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                line[i] = (line[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 255
        if row == y:
            return tuple(line[x * 4:x * 4 + 4])
        previous = line
    raise AssertionError('row out of range')


def set_source(parameter, **source):
    return ('bloom.parameter.set-source', {'composition': 1, 'parameter': parameter, **source})


class AgentFeedback(unittest.TestCase):
    def setUp(self):
        self.server = Server()

    def tearDown(self):
        self.server.close()

    def constant(self, parameter):
        return self.server.parameter(parameter)['value']

    def test_integer_constants_for_integer_and_enum_parameters(self):
        server = self.server
        result = server.transact(('bloom.layer.add-shape', {'composition': 1, 'kind': 'star'}))
        self.assertTrue(result['succeeded'], result)
        blend = server.role('blendMode')
        points = server.role('points')
        self.assertTrue(server.transact(set_source(blend, value=3))['succeeded'])
        self.assertEqual(self.constant(blend), 3)
        self.assertTrue(server.transact(set_source(points, value=7))['succeeded'])
        self.assertEqual(self.constant(points), 7)
        # Range validation names the parameter, its node and what it accepts.
        refused = server.transact(set_source(points, value=2))
        self.assertFalse(refused['succeeded'])
        message = refused['diagnostics'][0]['message']
        self.assertIn('points', message)
        self.assertIn('3..64', message)
        refused = server.transact(set_source(blend, value=42))
        self.assertIn('Screen', refused['diagnostics'][0]['message'])

    def test_bool_and_string_values(self):
        server = self.server
        server.transact(('bloom.layer.add-shape', {'composition': 1, 'kind': 'rectangle'}))
        fill = server.role('fillEnabled')
        blend = server.role('blendMode')
        self.assertTrue(server.transact(set_source(fill, value=False))['succeeded'])
        self.assertIs(self.constant(fill), False)
        # An enumeration member by its label, in any case or spelling.
        self.assertTrue(server.transact(set_source(blend, value='screen'))['succeeded'])
        self.assertEqual(self.constant(blend), 3)
        server.transact(('bloom.layer.add-text', {'composition': 1, 'name': 'T', 'text': 'Hi'}))
        content = server.role('text')
        self.assertTrue(server.transact(set_source(content, value='Hello'))['succeeded'])
        self.assertEqual(self.constant(content), 'Hello')
        refused = server.transact(set_source(blend, value='sparkle'))
        self.assertFalse(refused['succeeded'])
        self.assertIn('blendMode', refused['diagnostics'][0]['message'])

    def test_text_defaults_to_the_composition_centre(self):
        server = self.server
        result = server.transact(('bloom.layer.add-text', {'composition': 1, 'name': 'Title',
                                                           'text': 'Hello', 'size': 48.0}))
        self.assertTrue(result['succeeded'], result)
        position = server.role('position')
        self.assertEqual(self.constant(position), [960.0, 540.0])

    def test_rejections_carry_operation_index_and_reason(self):
        server = self.server
        server.transact(('bloom.layer.add-solid', {'composition': 1, 'name': 'S',
                                                   'color': [1, 0, 0, 1]}))
        blend = server.role('blendMode')
        refused = server.transact(('bloom.project.set-name', {'name': 'Fine'}),
                                  set_source(blend, value=1.5))
        self.assertFalse(refused['succeeded'])
        diagnostic = refused['diagnostics'][0]
        self.assertEqual(diagnostic['operationIndex'], 1)
        self.assertNotEqual(diagnostic['message'], 'The host refused the transaction')
        self.assertIn('cannot take 1.5', diagnostic['message'])
        self.assertIn('blendMode', diagnostic['message'])
        stale = server.call('transact', {'expectedRevision': 999, 'operations': [
            {'op': 'bloom.project.set-name', 'args': {'name': 'Late'}}]})
        self.assertFalse(stale['succeeded'])
        self.assertEqual(stale['diagnostics'][0]['code'], 'bloom.scripting.stale-revision')
        self.assertIsNone(stale['diagnostics'][0]['operationIndex'])
        self.assertIn('revision', stale['diagnostics'][0]['message'])
        # A wrong argument names the operation's index too.
        response = server.raw('transact', {'expectedRevision': server.revision(), 'operations': [
            {'op': 'bloom.project.set-name', 'args': {'name': 'A'}},
            {'op': 'bloom.project.set-name', 'args': {'title': 'B'}}]})
        self.assertIn('operations[1]', json.dumps(response))

    def test_driver_on_a_missing_port_is_refused_with_its_outputs(self):
        server = self.server
        server.transact(('bloom.layer.add-solid', {'composition': 1, 'name': 'S',
                                                   'color': [1, 0, 0, 1]}))
        blend = server.role('blendMode')
        added = server.transact(('bloom.node.add', {'composition': 1,
                                                    'nodeType': 'bloom.scalar-to-integer'}))
        self.assertTrue(added['succeeded'], added)
        node = [output['id'] for output in added['outputs'] if output['name'] == 'node'][0]
        refused = server.transact(set_source(blend, driverNode=node, driverPort='value'))
        self.assertFalse(refused['succeeded'])
        message = refused['diagnostics'][0]['message']
        self.assertIn('no output "value"', message)
        self.assertIn('result (integer)', message)
        self.assertEqual(refused['diagnostics'][0]['operationIndex'], 0)
        accepted = server.transact(set_source(blend, driverNode=node, driverPort='result'))
        self.assertTrue(accepted['succeeded'], accepted)

    def test_project_lifecycle_on_stdio(self):
        server = self.server
        folder = tempfile.TemporaryDirectory(prefix='bloom-mcp-lifecycle-')
        self.addCleanup(folder.cleanup)
        path = os.path.join(folder.name, 'study.bloom')

        def code(result):
            return result['diagnostic']['code']

        self.assertEqual(code(server.call('project.save', {})), 'bloom.mcp.no-path')
        self.assertEqual(code(server.call('project.save-as', {'path': 'relative.bloom'})),
                         'bloom.mcp.path-invalid')
        server.transact(('bloom.project.set-name', {'name': 'Study'}))
        saved = server.call('project.save-as', {'path': path})
        self.assertTrue(saved['succeeded'], saved)
        self.assertEqual(saved['event'], 'project.saved')
        self.assertTrue(os.path.exists(path))
        self.assertEqual(code(server.call('project.save-as', {'path': path})),
                         'bloom.mcp.destination-exists')
        self.assertTrue(server.call('project.save-as', {'path': path, 'replace': True})['succeeded'])
        server.transact(('bloom.project.set-name', {'name': 'Edited'}))
        refused = server.call('project.new', {})
        self.assertEqual(code(refused), 'bloom.mcp.unsaved-changes')
        self.assertIn('Edited', refused['diagnostic']['message'])
        created = server.call('project.new', {'discardChanges': True})
        self.assertTrue(created['succeeded'], created)
        self.assertNotEqual(server.call('query', {'kind': 'project'})['project']['name'], 'Edited')
        events = server.call('events', {'sinceRevision': 0})
        self.assertTrue(events['resetRequired'])
        self.assertIn('project.created', [event['kind'] for event in events['events']])
        opened = server.call('project.open', {'path': path})
        self.assertTrue(opened['succeeded'], opened)
        self.assertEqual(server.call('query', {'kind': 'project'})['project']['name'], 'Study')
        # The reopened project edits and saves like any other.
        server.transact(('bloom.project.set-name', {'name': 'Reopened'}))
        self.assertTrue(server.call('project.save', {})['succeeded'])
        self.assertTrue(server.call('project.close', {})['succeeded'])

    def test_window_tools_are_live_only(self):
        for name, arguments in (('editor.reveal', {'composition': 1, 'time': 12}),
                                ('selection.set', {'composition': 1, 'layers': []}),
                                ('composition.activate', {'composition': 1}),
                                ('transport', {'action': 'play'}),
                                ('viewer.set', {'exposure': 1.0})):
            result = self.server.call(name, arguments)
            self.assertFalse(result['succeeded'])
            self.assertEqual(result['diagnostic']['code'], 'bloom.mcp.not-live', name)

    def render(self, frame):
        folder = tempfile.TemporaryDirectory(prefix='bloom-mcp-render-')
        self.addCleanup(folder.cleanup)
        # A folder is a valid destination: the file is frame.png inside it (#388 N1).
        result = self.server.call('render', {'composition': 1, 'frame': frame,
                                             'preset': 'PngRgba8SrgbV1',
                                             'destination': folder.name})
        self.assertNotIn('diagnostic', result, result)
        self.assertEqual(os.path.basename(result['files'][0]['path']), 'frame.png')
        return result['files'][0]['path']

    def test_integer_times_are_frames_and_keys_animate_constants(self):
        server = self.server
        self.assertTrue(server.transact(('bloom.layer.add-solid', {
            'composition': 1, 'name': 'S', 'color': [1, 1, 1, 1]}))['succeeded'])
        opacity = server.role('opacity')
        # One transaction keys a constant parameter twice: it becomes animated in the same step.
        keyed = server.transact(
            ('bloom.animation.set-keyframe-at-time-for-parameter',
             {'composition': 1, 'parameter': opacity, 'time': 0, 'value': 1.0}),
            ('bloom.animation.set-keyframe-at-time-for-parameter',
             {'composition': 1, 'parameter': opacity, 'time': 24, 'value': 0.0}))
        self.assertTrue(keyed['succeeded'], keyed)
        self.assertEqual(server.parameter(opacity)['source'], 'curve')
        # Frame 24 at 24 fps is one second: the key is fully transparent there. Read as seconds it
        # would sit at frame 576 and frame 24 would still be almost opaque.
        self.assertLess(png_pixel(self.render(24), 960, 540)[3], 8)
        self.assertGreater(png_pixel(self.render(12), 960, 540)[3], 100)
        # set-duration counts frames too, and an exact pair is seconds.
        self.assertTrue(server.transact(('bloom.composition.set-duration',
                                         {'composition': 1, 'duration': 48}))['succeeded'])
        self.assertTrue(server.transact(('bloom.composition.set-duration',
                                         {'composition': 1, 'duration': [2, 1]}))['succeeded'])

    def test_shape_switches_follow_a_boolean_driver(self):
        server = self.server
        server.transact(('bloom.layer.add-shape', {'composition': 1, 'kind': 'rectangle',
                                                   'size': [800, 600]}))
        fill = server.role('fillEnabled')
        added = server.transact(('bloom.node.add', {'composition': 1,
                                                    'nodeType': 'bloom.scalar-to-boolean'}))
        node = [output['id'] for output in added['outputs'] if output['name'] == 'node'][0]
        driven = server.transact(set_source(fill, driverNode=node, driverPort='result'))
        self.assertTrue(driven['succeeded'], driven)
        self.assertEqual(driven['compileFailures'], [])
        self.assertEqual(png_pixel(self.render(0), 960, 540)[3], 0)
        operand = [n for n in server.nodes() if n['id'] == node][0]['parameters']['value']
        self.assertTrue(server.transact(set_source(operand, value=1.0))['succeeded'])
        self.assertEqual(png_pixel(self.render(0), 960, 540)[3], 255)

    def test_render_destinations(self):
        server = self.server
        folder = tempfile.TemporaryDirectory(prefix='bloom-mcp-sequence-')
        self.addCleanup(folder.cleanup)
        # A sequence pattern substitutes the frame number instead of keeping #### literally.
        result = server.call('render', {'composition': 1, 'first': 0, 'last': 1,
                                        'preset': 'PngRgba8SrgbV1',
                                        'destination': os.path.join(folder.name, 'seq.####.png')})
        names = sorted(os.path.basename(file['path']) for file in result['files'])
        self.assertEqual(names, ['seq.0000.png', 'seq.0001.png'])
        # A wrong extension is an argument error that says what is expected.
        wrong = server.raw('render', {'composition': 1, 'frame': 0, 'preset': 'PngRgba8SrgbV1',
                                      'destination': os.path.join(folder.name, 'still.exr')})
        self.assertIn('.png', json.dumps(wrong))
        self.assertNotIn('export-unavailable', json.dumps(wrong))


if __name__ == '__main__':
    unittest.main()
