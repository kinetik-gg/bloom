"""bloom.mcp.http-conformance (ADR 0034, section 10): Streamable HTTP over a real loopback socket
against a headless McpServerService (bloom_mcp_http_test_host) on a free test port."""
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time
import unittest

HOST_BINARY = sys.argv[1]
del sys.argv[1]

INIT = {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize',
        'params': {'protocolVersion': '2025-11-25', 'capabilities': {},
                   'clientInfo': {'name': 'conformance', 'version': '1', 'title': 'HTTP test'}}}
READY = {'jsonrpc': '2.0', 'method': 'notifications/initialized'}


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


class Host:
    def __init__(self, port=None, folder=None, extra=()):
        self.port = port or free_port()
        self.temporary = None
        if folder is None:
            self.temporary = tempfile.TemporaryDirectory(prefix='bloom-mcp-http-')
            folder = self.temporary.name
        self.folder = pathlib.Path(folder)
        self.process = subprocess.Popen(
            [HOST_BINARY, '--port', str(self.port), '--folder', str(self.folder), *extra],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.status = self.process.stdout.readline().strip()

    @property
    def token(self):
        return (self.folder / 'mcp' / 'token').read_text()

    def command(self, line):
        self.process.stdin.write(line + '\n')
        self.process.stdin.flush()
        while True:
            answer = self.process.stdout.readline().strip()
            if answer == 'DONE ' + line or not answer:
                return

    def close(self):
        if self.process.poll() is None:
            try:
                self.process.stdin.write('quit\n')
                self.process.stdin.close()
            except BrokenPipeError:
                pass
            try:
                self.process.wait(timeout=60)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if self.temporary:
            self.temporary.cleanup()


def raw(port, data, timeout=10):
    """Sends raw bytes and reads the whole answer (until the server closes or times out)."""
    with socket.create_connection(('127.0.0.1', port), timeout=timeout) as connection:
        connection.sendall(data)
        chunks = []
        try:
            while True:
                chunk = connection.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
                if b'\r\n\r\n' in b''.join(chunks):
                    head, _, body = b''.join(chunks).partition(b'\r\n\r\n')
                    if head.startswith(b'HTTP/1.1 204'):
                        break
                    length = [line for line in head.split(b'\r\n')
                              if line.lower().startswith(b'content-length:')]
                    if length and len(body) >= int(length[0].split(b':')[1]):
                        break
        except socket.timeout:
            pass
    return b''.join(chunks)


def parse(answer):
    head, _, body = answer.partition(b'\r\n\r\n')
    lines = head.decode().split('\r\n')
    status = int(lines[0].split()[1]) if lines and lines[0] else 0
    headers = {}
    for line in lines[1:]:
        name, _, value = line.partition(':')
        headers[name.strip().lower()] = value.strip()
    return status, headers, body


class Client:
    def __init__(self, host, token=None, origin=None, host_header=None):
        self.host = host
        self.token = token if token is not None else host.token
        self.origin = origin
        self.host_header = host_header or f'127.0.0.1:{host.port}'
        self.session = None

    def request(self, method='POST', body=None, headers=None, path='/mcp'):
        payload = b'' if body is None else (body if isinstance(body, bytes)
                                             else json.dumps(body).encode())
        lines = [f'{method} {path} HTTP/1.1', f'Host: {self.host_header}']
        if self.token:
            lines.append(f'Authorization: Bearer {self.token}')
        if self.origin:
            lines.append(f'Origin: {self.origin}')
        if method == 'POST':
            lines.append('Content-Type: application/json')
            lines.append('Accept: application/json, text/event-stream')
        if self.session:
            lines.append(f'Mcp-Session-Id: {self.session}')
        for name, value in (headers or {}).items():
            lines = [line for line in lines if not line.lower().startswith(name.lower() + ':')]
            if value is not None:
                lines.append(f'{name}: {value}')
        if method == 'POST' or payload:
            lines.append(f'Content-Length: {len(payload)}')
        return parse(raw(self.host.port, ('\r\n'.join(lines) + '\r\n\r\n').encode() + payload))

    def initialize(self):
        status, headers, body = self.request(body=INIT)
        assert status == 200, (status, body)
        self.session = headers['mcp-session-id']
        status, _, _ = self.request(body=READY)
        assert status == 202, status
        return json.loads(body)

    def call(self, request_id, tool, arguments, meta=None):
        params = {'name': tool, 'arguments': arguments}
        if meta:
            params['_meta'] = meta
        return self.request(body={'jsonrpc': '2.0', 'id': request_id, 'method': 'tools/call',
                                  'params': params})


class Conformance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.host = Host(extra=('--header-timeout-ms', '400'))
        assert cls.host.status.startswith('READY'), cls.host.status

    @classmethod
    def tearDownClass(cls):
        cls.host.close()

    def test_lifecycle(self):
        client = Client(self.host)
        initialized = client.initialize()
        self.assertEqual(initialized['result']['protocolVersion'], '2025-11-25')
        self.assertFalse(initialized['result']['capabilities']['experimental']['bloom']['headless'])
        self.assertEqual(len(client.session), 32)
        status, headers, body = client.request(body={'jsonrpc': '2.0', 'id': 2,
                                                     'method': 'tools/list'})
        self.assertEqual(status, 200)
        self.assertEqual(headers['content-type'], 'application/json')
        self.assertNotIn('access-control-allow-origin', headers)
        names = {tool['name'] for tool in json.loads(body)['result']['tools']}
        self.assertTrue({'query', 'transact', 'history', 'events', 'render', 'export', 'capture',
                         'export.status', 'export.wait'} <= names)
        status, _, body = client.call(3, 'query', {'kind': 'project'})
        self.assertEqual(status, 200)
        result = json.loads(body)['result']
        self.assertFalse(result['isError'], result)
        status, _, body = client.call(4, 'query', {'kind': 'context'})
        self.assertTrue(json.loads(body)['result']['structuredContent']['live'])
        status, _, _ = client.request('DELETE')
        self.assertEqual(status, 204)
        status, _, body = client.call(5, 'query', {'kind': 'project'})
        self.assertEqual(status, 404)

    def test_transact_history_and_events(self):
        client = Client(self.host)
        client.initialize()
        _, _, body = client.call(10, 'query', {'kind': 'project'})
        revision = json.loads(body)['result']['structuredContent']['revision']
        _, _, body = client.call(11, 'transact', {'expectedRevision': revision, 'operations': [
            {'op': 'bloom.project.set-name', 'args': {'name': 'Over HTTP'}}]})
        result = json.loads(body)['result']
        self.assertFalse(result['isError'], result)
        _, _, body = client.call(12, 'history', {'action': 'undo',
                                                 'expectedRevision': revision + 1})
        self.assertFalse(json.loads(body)['result']['isError'], body)
        _, _, body = client.call(13, 'query', {'kind': 'project'})
        self.assertNotEqual(json.loads(body)['result']['structuredContent']['project']['name'],
                            'Over HTTP')
        _, _, body = client.call(14, 'events', {'sinceRevision': 0})
        self.assertGreaterEqual(len(json.loads(body)['result']['structuredContent']['events']), 2)
        client.request('DELETE')

    def test_progress_render_streams(self):
        if not sys.platform.startswith('linux'):
            self.skipTest('export runs on Linux in this version')
        client = Client(self.host)
        client.initialize()
        with tempfile.TemporaryDirectory(prefix='bloom-mcp-render-') as directory:
            destination = str(pathlib.Path(directory) / 'frame.png')
            status, headers, body = client.call(
                20, 'render', {'composition': 1, 'frame': 0, 'preset': 'PngRgba8SrgbV1',
                               'destination': destination},
                meta={'progressToken': 'p1'})
            self.assertEqual(status, 200)
            self.assertEqual(headers['content-type'], 'text/event-stream')
            messages = [json.loads(line[len(b'data: '):]) for line in body.split(b'\r\n')
                        if line.startswith(b'data: ')]
            self.assertGreaterEqual(len(messages), 1)
            final = messages[-1]
            self.assertEqual(final['id'], 20)
            self.assertFalse(final['result']['isError'], final)
            for message in messages[:-1]:
                self.assertEqual(message['method'], 'notifications/progress')
                self.assertEqual(message['params']['progressToken'], 'p1')
            self.assertTrue(pathlib.Path(destination).exists())
        client.request('DELETE')

    def test_failed_transient_render_carries_its_log(self):
        # #388: a failed render, export or capture answers with its transient job id and the run's
        # log, because the job is retired as soon as the call returns.
        if not sys.platform.startswith('linux'):
            self.skipTest('export runs on Linux in this version')
        client = Client(self.host)
        client.initialize()

        def result(body):
            message = json.loads(body)
            return message['result']['structuredContent']

        with tempfile.TemporaryDirectory(prefix='bloom-mcp-failing-') as directory:
            source = pathlib.Path(directory) / 'source.png'
            _, _, body = client.call(30, 'render', {'composition': 1, 'frame': 0,
                                                    'preset': 'PngRgba8SrgbV1',
                                                    'destination': str(source)})
            rendered = result(body)
            self.assertIn('job', rendered)
            self.assertIsInstance(rendered['log'], list)
            _, _, body = client.call(31, 'query', {'kind': 'project'})
            revision = result(body)['revision']
            _, _, body = client.call(32, 'transact', {'expectedRevision': revision, 'operations': [
                {'op': 'bloom.asset.import', 'args': {'paths': [str(source)],
                                                      'projectDirectory': directory}}]})
            imported = result(body)
            self.assertTrue(imported['succeeded'], imported)
            asset = [o['id'] for o in imported['outputs'] if o['name'] == 'asset'][0]
            _, _, body = client.call(33, 'transact', {
                'expectedRevision': imported['revision'],
                'operations': [{'op': 'bloom.layer.add-image',
                                'args': {'composition': 1, 'asset': asset}}]})
            self.assertTrue(result(body)['succeeded'])
            source.unlink()
            _, _, body = client.call(34, 'render', {'composition': 1, 'frame': 0,
                                                    'preset': 'PngRgba8SrgbV1',
                                                    'destination': directory})
            failed = json.loads(body)['result']
            self.assertTrue(failed['isError'], failed)
            content = failed['structuredContent']
            self.assertIsInstance(content.get('job'), int, content)
            self.assertTrue(content['log'], content)
            self.assertTrue(all({'severity', 'code', 'message'} <= set(row)
                                for row in content['log']))
        client.request('DELETE')

    def test_admission(self):
        port = self.host.port
        good = Client(self.host)
        self.assertEqual(Client(self.host, host_header='evil.example').request(body=INIT)[0], 403)
        self.assertEqual(Client(self.host, host_header=f'127.0.0.1:{port + 1}')
                         .request(body=INIT)[0], 403)
        self.assertEqual(Client(self.host, origin='http://evil.example').request(body=INIT)[0], 403)
        self.assertEqual(Client(self.host, origin='null').request(body=INIT)[0], 403)
        status, headers, _ = Client(self.host, token='').request(body=INIT)
        self.assertEqual(status, 401)
        self.assertEqual(headers['www-authenticate'], 'Bearer realm="bloom"')
        status, headers, _ = Client(self.host, token='bloom_' + 'x' * 43).request(body=INIT)
        self.assertEqual((status, headers['www-authenticate']), (401, 'Bearer realm="bloom"'))
        status, headers, _ = good.request('GET')
        self.assertEqual((status, headers['allow']), (405, 'POST, DELETE'))
        self.assertEqual(good.request(path='/.well-known/oauth-authorization-server')[0], 404)
        self.assertEqual(good.request(body=INIT, headers={'Content-Type': 'text/plain'})[0], 415)
        self.assertEqual(good.request(body=INIT, headers={'Accept': 'application/json'})[0], 406)
        self.assertEqual(good.request(body=[INIT])[0], 400)
        self.assertEqual(good.request(body=b' ' * (1024 * 1024 + 1))[0], 413)
        client = Client(self.host)
        client.initialize()
        self.assertEqual(client.request(body={'jsonrpc': '2.0', 'id': 3, 'method': 'tools/list'},
                                        headers={'MCP-Protocol-Version': '2024-01-01'})[0], 400)
        self.assertEqual(client.request(body={'jsonrpc': '2.0', 'id': 4, 'method': 'tools/list'},
                                        headers={'MCP-Protocol-Version': '2025-11-25'})[0], 200)
        missing = Client(self.host)
        self.assertEqual(missing.request(body={'jsonrpc': '2.0', 'id': 5,
                                               'method': 'tools/list'})[0], 400)
        missing.session = 'f' * 32
        self.assertEqual(missing.request(body={'jsonrpc': '2.0', 'id': 6,
                                               'method': 'tools/list'})[0], 404)
        client.request('DELETE')

    def test_slow_header_closes(self):
        with socket.create_connection(('127.0.0.1', self.host.port), timeout=10) as connection:
            connection.sendall(b'POST /mcp HTTP/1.1\r\nHost: 127.0.0.1')
            started = time.monotonic()
            self.assertEqual(connection.recv(1024), b'')  # Closed at the header deadline.
            self.assertLess(time.monotonic() - started, 8)

    def test_connection_limit(self):
        sockets = []
        try:
            for _ in range(16):
                sockets.append(socket.create_connection(('127.0.0.1', self.host.port), timeout=10))
            time.sleep(0.3)
            with socket.create_connection(('127.0.0.1', self.host.port), timeout=10) as extra:
                answer = extra.recv(65536)
                self.assertTrue(answer.startswith(b'HTTP/1.1 503'), answer)
        finally:
            for item in sockets:
                item.close()
        time.sleep(0.3)

    def test_client_limit(self):
        clients = [Client(self.host) for _ in range(8)]
        try:
            for client in clients:
                client.initialize()
            status, _, body = Client(self.host).request(body=INIT)
            self.assertEqual(status, 503)
            self.assertEqual(json.loads(body)['error']['code'], -32002)
        finally:
            for client in clients:
                client.request('DELETE')


class Lifecycle(unittest.TestCase):
    def test_port_in_use_and_regenerate(self):
        first = Host()
        try:
            self.assertTrue(first.status.startswith('READY'), first.status)
            second = Host(port=first.port)
            try:
                self.assertEqual(second.status,
                                 f'REFUSED Port {first.port} is in use. Choose another port or '
                                 'stop the other program.')
            finally:
                second.close()
            old = first.token
            client = Client(first)
            client.initialize()
            first.command('regenerate')
            for _ in range(200):
                if first.token != old:
                    break
                time.sleep(0.02)
            time.sleep(0.2)
            self.assertNotEqual(first.token, old)
            self.assertEqual(Client(first, token=old).request(body=INIT)[0], 401)
            # The regenerated token ended the client's session.
            client.token = first.token
            self.assertEqual(client.request(body={'jsonrpc': '2.0', 'id': 2,
                                                  'method': 'tools/list'})[0], 404)
            self.assertEqual(Client(first).initialize()['result']['serverInfo']['name'], 'Bloom')
            if os.name == 'posix':
                self.assertEqual((first.folder / 'mcp' / 'token').stat().st_mode & 0o777, 0o600)
        finally:
            first.close()


if __name__ == '__main__':
    unittest.main()
