"""MCP parity operations from Python: the same registry ops an agent runs over MCP, bound as
bloom.ops.*, each one undoable step."""
import pathlib
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, sys.argv.pop(1))
import _bloom
import bloom
from bloom import _state


def write_png(path, width=8, height=4, rgb=(255, 0, 0)):
    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))
    raw = (b'\x00' + bytes(rgb) * width) * height
    header = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) +
                     chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
    return path


def outputs(result, name):
    return [output['id'] for output in result['outputs'] if output['name'] == name]


def layer_ids():
    composition = bloom.data.snapshot()['compositions'][0]
    return sorted(layer['id'] for layer in composition['layers'])


class ParityOperations(unittest.TestCase):
    def setUp(self):
        self.host = _bloom.Host()
        self.token = _state.bind(self.host)
        self.directory = tempfile.TemporaryDirectory(prefix='bloom-python-parity-')

    def tearDown(self):
        _state._host.reset(self.token)
        self.directory.cleanup()

    def test_layer_operations(self):
        root = pathlib.Path(self.directory.name)
        still = write_png(root / 'still.png')
        imported = bloom.ops.asset.import_([str(still)], str(root))
        asset = outputs(imported, 'asset')[0]
        added = bloom.ops.layer.add_from_assets(assets=[asset], time=0)
        layers = outputs(added, 'layer')
        self.assertEqual(layer_ids(), layers)

        copy = outputs(bloom.ops.layer.duplicate(layers=layers), 'layer')
        self.assertEqual(layer_ids(), sorted(layers + copy))
        bloom.transactions.undo()
        self.assertEqual(layer_ids(), layers, 'undo removes the copy')
        bloom.transactions.redo()
        self.assertEqual(layer_ids(), sorted(layers + copy), 'redo restores it')

        bloom.ops.layer.delete(layers=copy)
        self.assertEqual(layer_ids(), layers)
        bloom.transactions.undo()
        self.assertEqual(layer_ids(), sorted(layers + copy), 'undo restores the deleted layer')
        with self.assertRaises(bloom.OperationError):
            bloom.ops.layer.add_from_assets(time=0)

    def test_node_source_from_asset(self):
        root = pathlib.Path(self.directory.name)
        still = write_png(root / 'node.png')
        asset = outputs(bloom.ops.asset.import_([str(still)], str(root)), 'asset')[0]
        composition = bloom.data.snapshot()['compositions'][0]
        before = len(composition['nodes'])
        node = outputs(bloom.ops.node.add_asset_source(asset, position=(40, 0)), 'node')
        self.assertEqual(len(node), 1)
        self.assertEqual(len(bloom.data.snapshot()['compositions'][0]['nodes']), before + 1)
        bloom.transactions.undo()
        self.assertEqual(len(bloom.data.snapshot()['compositions'][0]['nodes']), before)

    def test_new_composition_from_assets_and_delete(self):
        root = pathlib.Path(self.directory.name)
        still = write_png(root / 'plate.png', width=64, height=32)
        asset = outputs(bloom.ops.asset.import_([str(still)], str(root)), 'asset')[0]
        created = bloom.ops.composition.add_from_assets([asset])
        composition = outputs(created, 'composition')[0]
        record = next(c for c in bloom.data.snapshot()['compositions'] if c['id'] == composition)
        self.assertEqual((record['width'], record['height']), (64, 32), 'sized from the footage')
        self.assertEqual(len(record['layers']), 1)
        bloom.transactions.undo()
        self.assertNotIn(composition, [c['id'] for c in bloom.data.snapshot()['compositions']])
        bloom.transactions.redo()
        bloom.ops.asset.delete_items([asset], compositions=[composition])
        snapshot = bloom.data.snapshot()
        self.assertNotIn(composition, [c['id'] for c in snapshot['compositions']])
        bloom.transactions.undo()
        self.assertIn(composition, [c['id'] for c in bloom.data.snapshot()['compositions']])

    def test_descriptions_are_published(self):
        schemas = {schema['id']: schema for schema in self.host.schemas()}
        for op in ('bloom.layer.delete', 'bloom.layer.duplicate', 'bloom.layer.add-from-assets',
                   'bloom.node.add-asset-source'):
            self.assertIn('Errors:', schemas[op]['description'], op)


if __name__ == '__main__':
    unittest.main()
