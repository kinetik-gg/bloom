"""The agent-feedback contracts through Python: one time contract and parameter value kinds."""
import sys
import unittest
from fractions import Fraction

sys.path.insert(0, sys.argv.pop(1))
import _bloom
import bloom
from bloom import _state


class AgentFeedback(unittest.TestCase):
    def setUp(self):
        self.host = _bloom.Host()
        self.token = _state.bind(self.host)

    def tearDown(self):
        _state._host.reset(self.token)

    def duration(self):
        row = next(c for c in bloom.data.snapshot()['compositions']
                   if c['id'] == int(bloom.context.composition))
        return Fraction(*row['duration'])

    def test_integer_times_are_frames(self):
        # The default composition runs at 24 fps: 48 frames are two seconds, not 48.
        self.assertTrue(bloom.ops.composition.set_duration(48)['succeeded'])
        self.assertEqual(self.duration(), Fraction(2))
        # An exact pair is seconds.
        self.assertTrue(bloom.ops.composition.set_duration((3, 1))['succeeded'])
        self.assertEqual(self.duration(), Fraction(3))
        # A frame count follows a frame rate changed by the same call.
        self.assertTrue(bloom.ops.composition.set_settings(
            frameRateNumerator=30, frameRateDenominator=1, duration=60)['succeeded'])
        self.assertEqual(self.duration(), Fraction(2))
        # And a rate changed earlier in the same transaction.
        with bloom.transactions.group('Rate then duration'):
            bloom.ops.composition.set_settings(frameRateNumerator=25, frameRateDenominator=1)
            bloom.ops.composition.set_duration(50)
        self.assertEqual(self.duration(), Fraction(2))

    def test_parameter_values_in_their_own_kind(self):
        bloom.ops.layer.add_shape('star')
        row = next(c for c in bloom.data.snapshot()['compositions']
                   if c['id'] == int(bloom.context.composition))
        shape = next(n for n in row['nodes'] if 'points' in n['parameters'])
        layer_output = next(n for n in reversed(row['nodes']) if 'blendMode' in n['parameters'])
        points = shape['parameters']['points']
        fill = shape['parameters']['fillEnabled']
        blend = layer_output['parameters']['blendMode']
        self.assertTrue(bloom.ops.parameter.set_source(parameter=points, value=7)['succeeded'])
        self.assertTrue(bloom.ops.parameter.set_source(parameter=blend, value='Multiply')['succeeded'])
        self.assertTrue(bloom.ops.parameter.set_source(parameter=fill, value=False)['succeeded'])
        with self.assertRaises(bloom.OperationError) as refused:
            bloom.ops.parameter.set_source(parameter=points, value=99)
        self.assertIn('3..64', str(refused.exception))
        self.assertEqual(refused.exception.diagnostics[0]['operation_index'], 0)


if __name__ == '__main__':
    unittest.main()
