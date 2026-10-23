"""Every Python Host of one interpreter shares one GPU device, retired once at exit (#357).

Several Hosts rendering on the main thread, on worker threads and through a cancelled native range
used to leave one device per Host for the exit hook to destroy, and concurrent native device
teardown deadlocked inside the GPU driver. This runs that workload and must exit promptly."""
import hashlib
import pathlib
import sys
import tempfile
import threading
import time
import unittest
sys.path.insert(0, sys.argv.pop(1))
import _bloom
import bloom
from bloom import _state


def render(host, color, out):
    token = _state.bind(host)
    try:
        bloom.ops.layer.add_solid(composition=1, name='Solid', color=color, position=(960, 540))
        bloom.render.frame(12, out=out)
        return hashlib.sha256(out.read_bytes()).hexdigest()
    finally:
        _state._host.reset(token)


class SharedDevice(unittest.TestCase):
    def test_hosts_share_one_root_with_their_own_pixels(self):
        with tempfile.TemporaryDirectory(prefix='bloom-shared-device-') as directory:
            root = pathlib.Path(directory)
            hosts = [_bloom.Host() for _ in range(3)]
            colors = [(1, 0, 0, 1), (0, 1, 0, 1), (0, 0, 1, 1)]
            digests = [render(host, color, root / f'{index}.png')
                       for index, (host, color) in enumerate(zip(hosts, colors))]
            self.assertEqual(len(set(digests)), 3, 'colliding Host identities never share pixels')
            self.assertEqual(_bloom._root_retirement_snapshot()['roots'], 1,
                             'every Host shares the interpreter provider')

            # Two Hosts render at the same time on their own threads through the one device.
            # Authoring stays on this thread; only the renders run concurrently.
            concurrent = [_bloom.Host(), _bloom.Host()]
            for host, color in zip(concurrent, colors):
                token = _state.bind(host)
                try:
                    bloom.ops.layer.add_solid(composition=1, name='Solid', color=color,
                                              position=(960, 540))
                finally:
                    _state._host.reset(token)
            results = [None, None]

            def worker(index):
                token = _state.bind(concurrent[index])
                try:
                    out = root / f'c{index}.png'
                    bloom.render.frame(12, out=out)
                    results[index] = hashlib.sha256(out.read_bytes()).hexdigest()
                finally:
                    _state._host.reset(token)

            threads = [threading.Thread(target=worker, args=(index,)) for index in range(2)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(30)
            self.assertEqual(results, digests[:2], 'concurrent Hosts keep their own results')

            # A worker-task render and a cancelled native range on further Hosts.
            tasked = _bloom.Host()
            token = _state.bind(tasked)
            try:
                handle = bloom.tasks.submit(lambda task: bloom.render.frame(
                    12, out=root / 'task.png'))
                self.assertEqual(handle.result(30).published_frames, 1)
                bloom.ops.composition.add(name='Small', width=4, height=4, duration=(48, 1))
                composition = bloom.data.compositions[-1]
                ranged = bloom.tasks.submit(lambda task: bloom.render.range(
                    0, 999, composition=composition, out=root / 'cancel.png'))
                deadline = time.monotonic() + 10
                while len(tasked.tasks()) < 2 and not ranged.done and time.monotonic() < deadline:
                    time.sleep(0.001)
                ranged.cancel()
                while not ranged.done and time.monotonic() < deadline:
                    time.sleep(0.001)
                self.assertTrue(ranged.done, 'the native range cooperates with cancellation')
            finally:
                _state._host.reset(token)

            participants = _bloom._root_retirement_snapshot()['participants']
            del hosts, concurrent, tasked
            import gc
            gc.collect()
            _bloom._collect_retired_roots()
            snapshot = _bloom._root_retirement_snapshot()
            self.assertLess(snapshot['participants'], participants,
                            'collected Hosts retire their own records before exit')
            self.assertEqual(snapshot['roots'], 1, 'the shared provider outlives its Hosts')

    def test_zz_shutdown_retires_once_and_refuses_new_hosts(self):
        # Runs last (alphabetical): retires the interpreter provider explicitly, as the exit hook
        # would, then a new Host gets a typed refusal instead of a crash.
        self.assertTrue(_bloom._shutdown_owned_roots(20000), 'one serial device retirement')
        self.assertEqual(_bloom._root_retirement_snapshot()['roots'], 0)
        with self.assertRaisesRegex(RuntimeError, 'shutting down'):
            _bloom.Host()


if __name__ == '__main__':
    unittest.main()
