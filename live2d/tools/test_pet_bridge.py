"""Protocol tests for the dependency-free external integration adapter."""
import importlib.util
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

BRIDGE = Path(__file__).with_name('pet_bridge.py')
spec = importlib.util.spec_from_file_location('pet_bridge', BRIDGE)
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class FakePet:
    def __enter__(self):
        self.requests = []
        self.sock = socket.socket()
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen(5)
        self.sock.settimeout(.1)
        self.port = self.sock.getsockname()[1]
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run)
        self.thread.start()
        return self

    def run(self):
        while not self.stop.is_set():
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            with conn:
                conn.settimeout(3)
                with conn.makefile('rb') as stream:
                    request = json.loads(stream.readline())
                self.requests.append(request)
                if request['cmd'] == 'panel_state':
                    reply = {'ok': True, 'state': {'model': 'Hiyori', 'actions': [['wave', 'Wave']], 'expressions': ['smile'], 'memory': 'private', 'llm': {'key': 'private'}}}
                elif request.get('fv') == 'missing':
                    reply = {'ok': False, 'msg': 'unsupported action'}
                else:
                    reply = {'ok': True, 'accepted': 1}
                conn.sendall((json.dumps(reply) + '\n').encode())

    def __exit__(self, *args):
        self.stop.set()
        self.thread.join(4)
        self.sock.close()


class IntegrationTests(unittest.TestCase):
    def run_bridge(self, *args, input=None):
        return subprocess.run([sys.executable, str(BRIDGE), *args], input=input, capture_output=True, encoding='utf-8', timeout=10)

    def test_notify_complete_and_ignore_other_events(self):
        with FakePet() as pet:
            result = self.run_bridge('--port', str(pet.port), 'notify', json.dumps({'type': 'agent-turn-complete'}))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(pet.requests[0]['cmd'], 'companion_event')
            self.assertEqual(pet.requests[0]['kind'], 'completed')
            result = self.run_bridge('--port', str(pet.port), 'notify', json.dumps({'type': 'other'}))
            self.assertEqual(result.returncode, 0)
            self.assertEqual(len(pet.requests), 1)

    def test_mcp_handshake_list_call_and_errors(self):
        with FakePet() as pet:
            requests = [
                {'id': 1, 'method': 'initialize', 'params': {'protocolVersion': '2025-06-18'}},
                {'method': 'notifications/initialized'},
                {'id': 2, 'method': 'tools/list'},
                {'id': 3, 'method': 'tools/call', 'params': {'name': 'get_state'}},
                {'id': 4, 'method': 'tools/call', 'params': {'name': 'show_message', 'arguments': {'text': '你好'}}},
                {'id': 5, 'method': 'tools/call', 'params': {'name': 'play_action', 'arguments': {'fv': 'act:wave'}}},
                {'id': 6, 'method': 'tools/call', 'params': {'name': 'set_expression', 'arguments': {'name': 'smile'}}},
                {'id': 7, 'method': 'tools/call', 'params': {'name': 'play_action', 'arguments': {'fv': 'missing'}}},
                {'id': 8, 'method': 'tools/call', 'params': {'name': 'show_message', 'arguments': {'text': 'a' * 121}}},
                {'id': 9, 'method': 'unsupported'},
            ]
            body = '\n'.join(json.dumps(dict(jsonrpc='2.0', **r)) for r in requests) + '\n'
            result = self.run_bridge('--port', str(pet.port), 'mcp', input=body)
            self.assertEqual(result.returncode, 0, result.stderr)
            replies = {r['id']: r for r in map(json.loads, result.stdout.splitlines())}
            self.assertEqual(len(replies), 9)
            self.assertEqual(replies[1]['result']['protocolVersion'], '2025-06-18')
            self.assertEqual(len(replies[2]['result']['tools']), 5)
            state = json.loads(replies[3]['result']['content'][0]['text'])
            self.assertNotIn('memory', state)
            self.assertNotIn('llm', state)
            self.assertEqual(state['actions'][0]['fv'], 'act:wave')
            self.assertFalse(replies[4]['result']['isError'])
            self.assertEqual(pet.requests[2]['fv'], 'act:wave')
            self.assertEqual(pet.requests[3]['fv'], 'smile')
            self.assertTrue(replies[7]['result']['isError'])
            self.assertTrue(replies[8]['result']['isError'])
            self.assertEqual(replies[9]['error']['code'], -32601)


    def test_companion_event_validation(self):
        with FakePet() as pet:
            result = bridge.call_tool(pet.port, 'send_event', {'kind': 'waiting', 'message': 'Choose a target'})
            self.assertTrue(result['ok'])
            self.assertEqual(pet.requests[0]['kind'], 'waiting')
            self.assertEqual(pet.requests[0]['message'], 'Choose a target')
            for args in [{'kind': 'unknown'}, {'kind': 'progress', 'message': 'a' * 601}, {'kind': 'progress', 'extra': True}]:
                with self.assertRaises(ValueError):
                    bridge.call_tool(pet.port, 'send_event', args)
            self.assertEqual(len(pet.requests), 1)

    def test_observational_hooks_and_recursion_guard(self):
        with FakePet() as pet:
            payload = {'hook_event_name': 'PreToolUse', 'tool_name': 'Bash', 'session_id': 's', 'turn_id': 't',
                       'tool_use_id': 'u', 'tool_input': {'command': 'secret command'}}
            result = self.run_bridge('--port', str(pet.port), 'hook', input=json.dumps(payload))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout), {})
            self.assertEqual(pet.requests[0]['kind'], 'working')
            self.assertNotIn('secret command', json.dumps(pet.requests[0]))
            payload['tool_name'] = 'mcp__l2dpet__send_event'
            result = self.run_bridge('--port', str(pet.port), 'hook', input=json.dumps(payload))
            self.assertEqual(len(pet.requests), 1)
            self.assertEqual(json.loads(result.stdout), {})
            payload = {'hook_event_name': 'PostToolUse', 'tool_name': 'Bash', 'tool_response': {'exit_code': 2, 'output': 'private output'}}
            result = self.run_bridge('--port', str(pet.port), 'hook', input=json.dumps(payload))
            self.assertEqual(pet.requests[-1]['kind'], 'failed')
            self.assertNotIn('private output', json.dumps(pet.requests[-1]))
            for event, expected in [('UserPromptSubmit', 'started'), ('PermissionRequest', 'waiting'), ('Interrupt', 'interrupted')]:
                result = self.run_bridge('--port', str(pet.port), 'hook', input=json.dumps({'hook_event_name': event}))
                self.assertEqual(pet.requests[-1]['kind'], expected)
                self.assertEqual(json.loads(result.stdout), {})

    def test_hook_failure_does_not_block_or_continue(self):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        result = self.run_bridge('--port', str(port), 'hook', input=json.dumps({'hook_event_name': 'UserPromptSubmit'}))
        self.assertEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout), {})


    def test_saved_port_validation(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(bridge.Path, 'home', return_value=Path(temp)):
            config = Path(temp) / '.config/l2dpet/config.json'
            config.parent.mkdir(parents=True)
            for value, expected in [(24444, 24444), (24444.0, 24444), (0, 23333), (65536, 23333), ('24444', 23333), (True, 23333), (24444.5, 23333)]:
                config.write_text(json.dumps({'ctl_port': value}))
                self.assertEqual(bridge.saved_port(), expected)
            config.write_text('broken')
            self.assertEqual(bridge.saved_port(), 23333)

    def test_invalid_input_and_disconnect(self):
        result = self.run_bridge('--port', '65536', 'test')
        self.assertNotEqual(result.returncode, 0)
        result = self.run_bridge('notify', 'not json')
        self.assertEqual(result.returncode, 1)
        result = self.run_bridge('mcp', input='not json\n')
        self.assertEqual(json.loads(result.stdout)['error']['code'], -32700)
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        result = self.run_bridge('--port', str(port), 'test')
        self.assertEqual(result.returncode, 1)
        self.assertIn('l2dpet bridge:', result.stderr)


if __name__ == '__main__':
    unittest.main()
