import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
EXE = Path(os.environ.get('L2DPET_TEST_PLAYER', str(Path.home() / 'l2dpet-build/build-win/Release/l2dtest.exe')))
MODEL = ROOT / 'live2d/bundled-models/Hiyori/Hiyori.model3.json'
spec = importlib.util.spec_from_file_location('bridge', ROOT / 'live2d/tools/pet_bridge.py')
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def exercise(home, port, extra=(), env_port=None, new_port=None):
    env = os.environ.copy()
    env.update(HOME=str(home), USERPROFILE=str(home), L2D_NO_AGENT='1')
    env.pop('L2DPET_PORT', None)
    if env_port is not None:
        env['L2DPET_PORT'] = str(env_port)
    with (home / 'player.log').open('wb') as log:
        proc = subprocess.Popen([str(EXE), str(MODEL), '0', '0.0333', '--port', 'headless', '--no-ui', *extra], env=env, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 12
            while time.monotonic() < deadline:
                try:
                    result = bridge.pet_request(port, {'cmd': 'config'})
                    break
                except (OSError, ValueError):
                    if proc.poll() is not None:
                        raise AssertionError((home / 'player.log').read_text(encoding='utf-8', errors='replace')[-2000:])
                    time.sleep(.1)
            else:
                raise AssertionError('control server did not start')
            # Registry contract: explicit IDs, per-client routing and persistent preferences.
            bridge.pet_request(port, {'cmd': 'integration_client', 'action': 'register', 'client_id': 'test-assistant', 'name': 'Test Assistant'})
            bridge.pet_request(port, {'cmd': 'integration_client', 'action': 'set_enabled', 'client_id': 'test-assistant', 'enabled': False})
            muted = bridge.companion_event(port, 'failed', 'Muted notification', client_id='test-assistant')
            assert muted['reason'] == 'client_disabled' and not muted['shown'], muted
            direct = bridge.call_tool(port, 'show_message', {'text': 'Muted direct message'}, 'test-assistant')
            assert direct['ok'] and direct['reason'] == 'client_disabled', direct
            folder = home / '.config/l2dpet/integration/clients/test-assistant'
            (folder / 'registration.json').write_text(json.dumps({'schema_version': 1, 'id': 'test-assistant', 'name': 'Updated assistant', 'enabled': True}), encoding='utf-8')
            assert bridge.companion_event(port, 'failed', client_id='test-assistant')['reason'] == 'client_disabled'
            try:
                bridge.companion_event(port, 'failed', client_id='unregistered')
            except ValueError:
                pass
            else:
                raise AssertionError('unknown explicit client accepted')
            bridge.pet_request(port, {'cmd': 'integration_client', 'action': 'set_enabled', 'client_id': 'test-assistant', 'enabled': True})
            routed = bridge.companion_event(port, 'failed', 'Registered event', 'registry-event', 'Spoofed source', 'test-assistant')
            assert routed['shown'], routed
            public_state = bridge.pet_request(port, {'cmd': 'panel_state'})['state']
            assert public_state['companion_events'][-1]['source'] == 'Updated assistant'
            descriptor = json.loads((home / '.config/l2dpet/integration/integration.json').read_text(encoding='utf-8'))
            assert descriptor['port'] == port and descriptor['model']['name'] == 'Hiyori'
            assert 'capabilities' in descriptor and 'llm' not in descriptor

            # Exercise the real player using the same adapter as MCP/notify.
            state = bridge.call_tool(port, 'get_state', {})
            assert state['model'] == 'Hiyori', state
            bridge.call_tool(port, 'show_message', {'text': '外部接入测试成功'})

            result = bridge.call_tool(port, 'send_event', {'kind': 'started', 'message': 'Native companion test', 'event_id': 'test-start'})
            assert result['shown'], result
            duplicate = bridge.call_tool(port, 'send_event', {'kind': 'started', 'message': 'Native companion test', 'event_id': 'test-start'})
            assert not duplicate['shown'] and duplicate['reason'] == 'duplicate', duplicate
            throttled = bridge.call_tool(port, 'send_event', {'kind': 'progress', 'message': 'Early progress'})
            assert not throttled['shown'] and throttled['reason'] == 'rate_limited', throttled
            failure = bridge.call_tool(port, 'send_event', {'kind': 'failed', 'message': 'A real test failure', 'event_id': 'test-error'})
            assert failure['shown'], failure
            state = bridge.pet_request(port, {'cmd': 'panel_state'})['state']
            assert state['companion_events'][-1]['message'] == 'A real test failure', state['companion_events']
            bridge.pet_request(port, {'cmd': 'config', 'set': {'companion': 0}})
            disabled = bridge.call_tool(port, 'send_event', {'kind': 'completed', 'event_id': 'test-disabled'})
            assert not disabled['shown'] and disabled['reason'] == 'disabled', disabled
            bridge.pet_request(port, {'cmd': 'config', 'set': {'companion': 1}})
            bridge.pet_request(port, {'cmd': 'say', 'text': '请选择', 'options': ['甲', '乙']})
            protected = bridge.call_tool(port, 'send_event', {'kind': 'waiting', 'message': 'Keep the choices', 'event_id': 'test-protect'})
            assert not protected['shown'] and protected['reason'] == 'awaiting_choice', protected

            if new_port is not None:
                bridge.pet_request(port, {'cmd': 'config', 'set': {'ctl_port': new_port}})
                cfg = json.loads((home / '.config/l2dpet/config.json').read_text(encoding='utf-8'))
                assert cfg['ctl_port'] == new_port, cfg
                # Invalid ports must be rejected without changing the saved value.
                for value in [0, 65536, 24444.5]:
                    try:
                        bridge.pet_request(port, {'cmd': 'config', 'set': {'ctl_port': value}})
                    except ValueError:
                        pass
                    else:
                        raise AssertionError('invalid port accepted')
                assert json.loads((home / '.config/l2dpet/config.json').read_text())['ctl_port'] == new_port
        finally:
            proc.terminate()
            proc.wait(timeout=10)


with tempfile.TemporaryDirectory(prefix='l2dpet-external-') as temp:
    home = Path(temp)
    config = home / '.config/l2dpet/config.json'
    config.parent.mkdir(parents=True)
    first, second, override, cli = [free_port() for _ in range(4)]
    config.write_text(json.dumps({'ctl_port': first}))
    exercise(home, first, new_port=second)
    print('PASS saved port startup, real MCP commands, port validation and persistence')
    exercise(home, second)
    print('PASS changed port after restart')
    exercise(home, override, env_port=override)
    print('PASS environment override')
    exercise(home, cli, extra=['--ctl', str(cli)], env_port=override)
    print('PASS explicit CLI port overrides environment')
