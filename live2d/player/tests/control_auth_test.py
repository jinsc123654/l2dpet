import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix='l2d-auth-test-') as directory:
    with socket.socket() as reserved:
        reserved.bind(('127.0.0.1', 0))
        port = reserved.getsockname()[1]
    server = subprocess.Popen([sys.argv[1], str(port)], env=dict(os.environ, HOME=directory))
    connections = []
    def connect():
        sock = socket.create_connection(('127.0.0.1', port), timeout=3)
        connections.append(sock)
        return sock
    def request(sock, value):
        sock.sendall((json.dumps(value) + '\n').encode())
        result = bytearray()
        while not result.endswith(b'\n'):
            data = sock.recv(1)
            assert data
            result.extend(data)
        return json.loads(result)
    try:
        token_file = Path(directory) / '.config/l2dpet' / f'control-token-{port}'
        deadline = time.monotonic() + 5
        while not token_file.exists() and time.monotonic() < deadline:
            time.sleep(.02)
        token = token_file.read_text()
        assert token_file.stat().st_mode & 0o777 == 0o600
        assert request(connect(), {'cmd': 'llm_config', 'base_url': 'https://example.invalid'})['code'] == 'unauthorized'
        assert request(connect(), {'cmd': 'auth', 'token': 'wrong'})['code'] == 'unauthorized'
        accepted = connect()
        assert request(accepted, {'cmd': 'auth', 'token': token})['ok']
        assert request(accepted, {'cmd': 'status'})['executed']
        connect() # Idle unauthenticated connection must not block server shutdown.
        assert request(accepted, {'cmd': 'quit'})['ok']
        assert server.wait(timeout=3) == 0
        print('PASS missing/wrong auth rejected, private token, authenticated requests and clean shutdown')
    finally:
        for sock in connections:
            sock.close()
        if server.poll() is None:
            server.terminate()
            server.wait(timeout=3)
