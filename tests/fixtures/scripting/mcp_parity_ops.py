"""MCP parity operations through the stdio server: an agent adds layers from assets the way an asset
drop does, duplicates and deletes them, and every step is one undoable history entry."""
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

MCP = sys.argv.pop(1)


def write_png(path, width=8, height=4, rgb=(255, 0, 0)):
    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))
    raw = (b'\x00' + bytes(rgb) * width) * height
    header = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) +
                     chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
    return path


class Server:
    """One stdio server kept open, so each call can read the previous call's result."""

    def __init__(self):
        self.process = subprocess.Popen([MCP], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        self.next_id = 1
        self.call('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                                 'clientInfo': {'name': 'Bloom parity', 'version': '1'}})
        self.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})

    def send(self, message):
        self.process.stdin.write((json.dumps(message) + '\n').encode())
        self.process.stdin.flush()

    def call(self, method, params):
        self.next_id += 1
        self.send({'jsonrpc': '2.0', 'id': self.next_id, 'method': method, 'params': params})
        return json.loads(self.process.stdout.readline())

    def tool(self, name, arguments):
        response = self.call('tools/call', {'name': name, 'arguments': arguments})
        return response['result']['structuredContent']

    def revision(self):
        return self.tool('query', {'kind': 'project'})['revision']

    def transact(self, operations):
        return self.tool('transact', {'expectedRevision': self.revision(),
                                      'operations': operations})

    def history(self, action):
        return self.tool('history', {'action': action, 'expectedRevision': self.revision()})

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=60)
        self.process.stdout.close()
        self.process.stderr.close()


def outputs(result, name):
    return [output['id'] for output in result['outputs'] if output['name'] == name]


class ParityOperations(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='bloom-mcp-parity-')
        self.server = Server()

    def tearDown(self):
        self.server.close()
        self.directory.cleanup()

    def node_count(self, composition):
        return len(self.server.tool('query', {'kind': 'nodes', 'composition': composition})[
            'records'])

    def test_layers_from_assets_duplicate_delete_and_undo(self):
        root = pathlib.Path(self.directory.name)
        stills = [write_png(root / 'red.png'), write_png(root / 'blue.png', rgb=(0, 0, 255))]
        imported = self.server.transact([{'op': 'bloom.asset.import',
                                          'args': {'paths': [str(p) for p in stills],
                                                   'projectDirectory': str(root)}}])
        self.assertTrue(imported['succeeded'], imported)
        assets = outputs(imported, 'asset')
        self.assertEqual(len(assets), 2)
        composition = self.server.tool('query', {'kind': 'compositions'})['records'][0]['id']
        nodes = self.node_count(composition)

        added = self.server.transact([{'op': 'bloom.layer.add-from-assets',
                                       'args': {'assets': assets, 'time': 0}}])
        self.assertTrue(added['succeeded'], added)
        layers = outputs(added, 'layer')
        self.assertEqual(len(layers), 2)
        # A footage layer is a source node and a Layer Output node.
        self.assertEqual(self.node_count(composition), nodes + 4)
        self.assertTrue(self.server.history('undo')['succeeded'])
        self.assertEqual(self.node_count(composition), nodes, 'one undo removes both layers')
        self.assertTrue(self.server.history('redo')['succeeded'])

        duplicated = self.server.transact([{'op': 'bloom.layer.duplicate',
                                            'args': {'layers': [layers[0]]}}])
        self.assertTrue(duplicated['succeeded'], duplicated)
        copy = outputs(duplicated, 'layer')
        self.assertEqual(len(copy), 1)
        self.assertNotIn(copy[0], layers)

        deleted = self.server.transact([{'op': 'bloom.layer.delete',
                                         'args': {'layers': layers}}])
        self.assertTrue(deleted['succeeded'], deleted)
        self.assertTrue(self.server.history('undo')['succeeded'])
        again = self.server.transact([{'op': 'bloom.layer.delete', 'args': {'layers': layers}}])
        self.assertTrue(again['succeeded'], 'undo restored the deleted layer ids')

        refused = self.server.transact([{'op': 'bloom.layer.delete', 'args': {'layers': layers}}])
        self.assertFalse(refused['succeeded'], 'deleting deleted layers is refused')

    def test_parity_operations_are_described(self):
        records = self.server.tool('query', {'kind': 'operations'})['records']
        described = {record['id']: record['description'] for record in records}
        for op in ('bloom.layer.delete', 'bloom.layer.duplicate', 'bloom.layer.add-from-assets',
                   'bloom.node.add-asset-source', 'bloom.node.add-composition-source',
                   'bloom.node.add-feeding', 'bloom.node.add-fed-by',
                   'bloom.node.insert-reroute', 'bloom.asset.delete-items',
                   'bloom.composition.add-from-assets', 'bloom.parameter.reset',
                   'bloom.node.set-parameter-value', 'bloom.node.set-video-audio-stream'):
            self.assertIn('Undo:', described[op], op)
            self.assertIn('MCP example:', described[op], op)


if __name__ == '__main__':
    unittest.main()
