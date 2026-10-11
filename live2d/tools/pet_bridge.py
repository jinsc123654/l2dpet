#!/usr/bin/env python3
"""Live2D local integration: Codex notify and MCP stdio. Python 3.9+, no dependencies."""
import argparse
import json
import os
from pathlib import Path
import socket
import sys

DEFAULT_PORT = 23333
MAX_LINE = 262144


def saved_port():
    try:
        descriptor = json.loads((Path.home() / '.config/l2dpet/integration/integration.json').read_text(encoding='utf-8'))
        if type(descriptor.get('port')) is int and 1 <= descriptor['port'] <= 65535:
            return descriptor['port']
    except (OSError, ValueError, TypeError, AttributeError):
        pass
    try:
        value = json.loads((Path.home() / '.config/l2dpet/config.json').read_text(encoding='utf-8')).get('ctl_port', DEFAULT_PORT)
        return int(value) if type(value) in (int, float) and 1024 <= value <= 65535 and int(value) == value else DEFAULT_PORT
    except (OSError, ValueError, TypeError, AttributeError):
        return DEFAULT_PORT


def port_number(value):
    try:
        port = int(value)
    except (ValueError, TypeError) as error:
        raise argparse.ArgumentTypeError('invalid port') from error
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError('port must be between 1 and 65535')
    return port


def authenticate(sock, port, home=None):
    """Private per-port capability; consumed locally, never copied into notifications or logs."""
    path = Path(home) if home is not None else Path.home()
    token = (path / '.config/l2dpet' / f'control-token-{port}').read_text(encoding='ascii').strip()
    if len(token) != 64 or any(c not in '0123456789abcdef' for c in token):
        raise ValueError('invalid local control token')
    sock.sendall((json.dumps({'cmd': 'auth', 'token': token}) + '\n').encode())
    raw = bytearray()
    while len(raw) < 256:
        byte = sock.recv(1)
        if not byte:
            raise ValueError('control authentication failed')
        if byte == b'\n':
            reply = json.loads(raw)
            if not isinstance(reply, dict) or reply.get('ok') is not True:
                raise ValueError('control authentication failed')
            return
        raw.extend(byte)
    raise ValueError('invalid authentication reply')


def pet_request(port, command):
    with socket.create_connection(('127.0.0.1', port), timeout=3) as sock:
        sock.settimeout(3)
        authenticate(sock, port)
        sock.sendall((json.dumps(command, ensure_ascii=False) + '\n').encode('utf-8'))
        with sock.makefile('rb') as stream:
            raw = stream.readline(MAX_LINE + 1)
            if not raw or len(raw) > MAX_LINE or not raw.endswith(b'\n'):
                raise ValueError('invalid or oversized pet reply')
            reply = json.loads(raw)
    if not reply.get('ok'):
        raise ValueError(reply.get('msg', 'pet rejected command'))
    return reply


TOOLS = [
    {'name': 'get_state', 'description': 'Read Live2D connection, available actions and expressions. Call before choosing an action.',
     'inputSchema': {'type': 'object', 'properties': {}, 'additionalProperties': False}},
    {'name': 'show_message', 'description': 'Show a short message in the Live2D speech bubble.',
     'inputSchema': {'type': 'object', 'properties': {'text': {'type': 'string', 'minLength': 1, 'maxLength': 120}}, 'required': ['text'], 'additionalProperties': False}},
    {'name': 'play_action', 'description': 'Play an action from get_state.actions using its exact fv identifier.',
     'inputSchema': {'type': 'object', 'properties': {'fv': {'type': 'string', 'minLength': 1, 'maxLength': 128}}, 'required': ['fv'], 'additionalProperties': False}},
    {'name': 'set_expression', 'description': 'Set an expression from get_state.expressions using its exact name.',
     'inputSchema': {'type': 'object', 'properties': {'name': {'type': 'string', 'minLength': 1, 'maxLength': 128}}, 'required': ['name'], 'additionalProperties': False}},
]



EVENT_KINDS = ('started', 'working', 'progress', 'waiting', 'failed', 'completed', 'interrupted')


def companion_event(port, kind, message='', event_id='', source='Codex', client_id=None):
    if kind not in EVENT_KINDS or not isinstance(message, str) or len(message) > 600:
        raise ValueError('invalid companion event')
    if not isinstance(event_id, str) or len(event_id) > 200 or not isinstance(source, str) or len(source) > 40:
        raise ValueError('invalid companion event identity')
    try:
        command = {'cmd': 'companion_event', 'kind': kind, 'message': message, 'event_id': event_id, 'source': source}
        if client_id is not None:
            command['client_id'] = client_id
        return pet_request(port, command)
    except ValueError as error:
        if '未知命令：companion_event' in str(error):
            return {'ok': True, 'shown': False, 'reason': 'player_upgrade_required'}
        raise


def hook_event(payload):
    """Map lifecycle signals without copying shell commands, credentials or raw tool output."""
    event = payload.get('hook_event_name', '')
    tool = str(payload.get('tool_name', ''))
    # Companion calls must not recursively produce progress hooks.
    if 'l2dpet' in tool:
        return None
    kind = {'UserPromptSubmit': 'started', 'PreToolUse': 'working',
            'PermissionRequest': 'waiting', 'Interrupt': 'interrupted'}.get(event)
    if event == 'PostToolUse':
        response = payload.get('tool_response', {})
        if isinstance(response, dict) and (response.get('isError') is True or
                (type(response.get('exit_code')) is int and response['exit_code'] != 0)):
            kind = 'failed'
    if kind is None:
        return None
    message = ''
    if event == 'UserPromptSubmit':
        message = str(payload.get('prompt', ''))[:600]
    elif event == 'PermissionRequest':
        tool_input = payload.get('tool_input', {})
        if isinstance(tool_input, dict):
            message = str(tool_input.get('description', '') or '')[:600]
    elif kind == 'failed':
        message = '工具执行遇到问题，请查看 Codex 的错误信息。'
    # Shell commands and raw tool output are never copied into the companion history.
    turn = str(payload.get('turn_id', ''))
    identity = str(payload.get('session_id', ''))[:80] + ':' + turn[:80] if turn else ''
    if event in ('PreToolUse', 'PostToolUse', 'PermissionRequest'):
        use_id = str(payload.get('tool_use_id', ''))
        if use_id:
            identity += ':' + use_id[:30]
    return kind, message, identity[:200]


TOOLS.insert(0, {
    'name': 'send_event',
    'description': 'Forward a meaningful agent status or progress summary to the companion pet. The pet chooses its own friendly wording and gentle animation. Do not send every tool call; preserve facts and use failed/waiting for problems or user decisions. completed means a turn ended, not guaranteed task success.',
    'inputSchema': {'type': 'object', 'properties': {
        'kind': {'type': 'string', 'enum': list(EVENT_KINDS)},
        'message': {'type': 'string', 'maxLength': 600},
        'event_id': {'type': 'string', 'maxLength': 200},
        'source': {'type': 'string', 'maxLength': 40}},
        'required': ['kind'], 'additionalProperties': False}
})


def call_tool(port, name, args, client_id=None):
    if not isinstance(args, dict):
        raise ValueError('arguments must be an object')
    if name == 'send_event':
        if set(args) - {'kind', 'message', 'event_id', 'source'}:
            raise ValueError('unexpected event argument')
        return companion_event(port, args.get('kind'), args.get('message', ''), args.get('event_id', ''), args.get('source', 'Codex'), client_id)
    if name == 'get_state':
        if args:
            raise ValueError('get_state takes no arguments')
        state = pet_request(port, {'cmd': 'panel_state'})
        # panel_state wraps the public state; do not expose memory or LLM configuration.
        state = state.get('state', state)
        public = {key: state.get(key) for key in ('model', 'visible', 'playing', 'expressions', 'expression')}
        public['actions'] = [{'fv': 'act:' + row[0], 'description': row[1]} for row in state.get('actions', []) if isinstance(row, list) and len(row) == 2 and isinstance(row[0], str)]
        return public
    field, limit = {'show_message': ('text', 120), 'play_action': ('fv', 128), 'set_expression': ('name', 128)}.get(name, ('', 0))
    if not field:
        raise ValueError('unknown tool: ' + str(name))
    value = args.get(field)
    if set(args) != {field} or not isinstance(value, str) or not 1 <= len(value) <= limit:
        raise ValueError('invalid ' + field)
    command = {'cmd': 'say', 'text': value} if name == 'show_message' else {'cmd': 'action' if name == 'play_action' else 'expression', 'fv': value, 'strict': True}
    if client_id is not None:
        command['client_id'] = client_id
    reply = pet_request(port, command)
    return {key: reply[key] for key in ('ok', 'accepted', 'warnings', 'shown', 'reason') if key in reply}


def mcp(port, client_id=None):
    initialized = False
    while True:
        raw = sys.stdin.buffer.readline(MAX_LINE + 1)
        if not raw:
            return
        request = None
        try:
            if len(raw) > MAX_LINE or not raw.endswith(b'\n'):
                raise ValueError('oversized MCP message')
            request = json.loads(raw)
            if not isinstance(request, dict) or request.get('jsonrpc') != '2.0' or not isinstance(request.get('method'), str):
                raise ValueError('invalid JSON-RPC request')
        except (ValueError, UnicodeError):
            emit({'jsonrpc': '2.0', 'id': None, 'error': {'code': -32700, 'message': 'Invalid JSON message'}})
            if len(raw) > MAX_LINE:
                return
            continue
        method = request['method']
        if 'id' not in request:
            continue
        response = {'jsonrpc': '2.0', 'id': request['id']}
        params = request.get('params', {})
        if not isinstance(params, dict):
            response['error'] = {'code': -32602, 'message': 'params must be an object'}
        elif method == 'initialize':
            requested = params.get('protocolVersion')
            supported = ('2024-11-05', '2025-03-26', '2025-06-18', '2025-11-25')
            response['result'] = {'protocolVersion': requested if requested in supported else '2025-06-18', 'capabilities': {'tools': {}}, 'serverInfo': {'name': 'l2dpet', 'version': '1.0.5'}}
            initialized = True
        elif method == 'ping':
            response['result'] = {}
        elif not initialized:
            response['error'] = {'code': -32002, 'message': 'Initialize first'}
        elif method == 'tools/list':
            response['result'] = {'tools': TOOLS}
        elif method == 'tools/call':
            try:
                result = call_tool(port or port_number(os.environ.get('L2DPET_PORT', saved_port())), params.get('name'), params.get('arguments', {}), client_id)
                response['result'] = {'content': [{'type': 'text', 'text': json.dumps(result, ensure_ascii=False)}], 'isError': False}
            except (OSError, ValueError, TypeError, argparse.ArgumentTypeError) as error:
                response['result'] = {'content': [{'type': 'text', 'text': str(error)}], 'isError': True}
        else:
            response['error'] = {'code': -32601, 'message': 'Method not found'}
        emit(response)


def emit(message):
    sys.stdout.buffer.write((json.dumps(message, ensure_ascii=False) + '\n').encode('utf-8'))
    sys.stdout.buffer.flush()


def main():
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=port_number, help='Override the saved pet port (default 23333).')
    parser.add_argument('--client-id', help='Registered assistant ID; use the same ID for hooks and MCP.')
    parser.add_argument('--name', help='Display name for register mode.')
    parser.add_argument('mode', choices=('notify', 'hook', 'mcp', 'test', 'register'))
    parser.add_argument('payload', nargs='?', help='Codex notify JSON argument')
    args = parser.parse_args()
    if args.mode == 'register':
        client_id = args.client_id or ''
        if not 1 <= len(client_id) <= 64 or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789-_' for c in client_id):
            parser.error('client-id must use 1-64 lowercase ASCII letters, digits, - or _')
        folder = Path.home() / '.config/l2dpet/integration/clients' / client_id
        folder.mkdir(parents=True, exist_ok=True)
        path = folder / 'registration.json'
        if not path.exists():
            temp = path.with_suffix('.json.tmp')
            temp.write_text(json.dumps({'schema_version': 1, 'id': client_id, 'name': args.name or client_id}, ensure_ascii=False), encoding='utf-8')
            temp.replace(path)
        emit({'registered': client_id, 'directory': str(folder)})
        return 0
    if args.mode == 'hook':
        try:
            payload = json.loads(sys.stdin.buffer.read(MAX_LINE + 1))
            if not isinstance(payload, dict):
                raise ValueError('hook input must be an object')
            event = hook_event(payload)
            if event is not None:
                companion_event(args.port or port_number(os.environ.get('L2DPET_PORT', saved_port())), *event, client_id=args.client_id)
        except (OSError, ValueError, TypeError, argparse.ArgumentTypeError) as error:
            print('l2dpet hook: ' + str(error), file=sys.stderr)
        # Observational hook only: never approve, block, add model context or continue a turn.
        emit({})
        return 0
    if args.mode == 'mcp':
        mcp(args.port, args.client_id)
        return 0
    try:
        if args.mode == 'notify':
            payload = json.loads(args.payload or '{}')
            if not isinstance(payload, dict):
                raise ValueError('notification must be an object')
            if payload.get('type') != 'agent-turn-complete':
                return 0
        port = args.port or port_number(os.environ.get('L2DPET_PORT', saved_port()))
        if args.mode == 'notify':
            identity = str(payload.get('thread-id', ''))[:80] + ':' + str(payload.get('turn-id', ''))[:80]
            message = str(payload.get('last-assistant-message', ''))[:600]
            companion_event(port, 'completed', message, identity, client_id=args.client_id)
        else:
            companion_event(port, 'progress', '陪伴消息接入成功', client_id=args.client_id)
        return 0
    except (OSError, ValueError, TypeError) as error:
        print('l2dpet bridge: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
