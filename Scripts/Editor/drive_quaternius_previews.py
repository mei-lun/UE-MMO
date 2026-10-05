"""Drive Scripts/Editor/import_quaternius.py inside a RUNNING GUI editor
through the official PythonScriptPlugin remote-execution protocol.

The script's scene-capture previews need editor world ticks between setting
an animation pose time and capturing the frame; -ExecutePythonScript shuts
the editor down as soon as the one-shot script returns, so the state machine
never gets a tick. Remote execution keeps the editor alive: every command is
executed on the editor's game thread with normal ticking in between.

Protocol (Engine/Plugins/Experimental/PythonScriptPlugin, version 1):
  UDP multicast 239.255.0.0.1:6766, JSON frames {"version":1, "magic":
  "ue_py", "type": ping|pong|open_connection, "source", "dest", "data"}.
  The client hosts the TCP command server; the editor connects out after an
  open_connection message, then each {"type": "command", "data": {"command",
  "unattended", "exec_mode"}} is answered by a command_result.

The editor must be launched with:
  -ini:Engine:[/Script/PythonScriptPlugin.PythonScriptPluginSettings]:bRemoteExecution=true

ASCII only. Never writes UE assets itself.
"""
import json
import socket
import struct
import sys
import time
from pathlib import Path

MULTICAST_GROUP = '239.0.0.1'
MULTICAST_PORT = 6766
NODE_ID = 'zcode-m3-032-driver'
PROJECT_ROOT = Path(__file__).resolve().parents[2]
REPORT_PATH = PROJECT_ROOT / 'Artifacts' / 'Logs' / 'quaternius-import-report.json'
SCRIPT_PATH = PROJECT_ROOT / 'Scripts' / 'Editor' / 'import_quaternius.py'


def open_multicast():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('', MULTICAST_PORT))
    group = struct.pack('!4sL', socket.inet_aton(MULTICAST_GROUP), 0)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, group)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    sock.settimeout(2.0)
    return sock


def frame(message):
    return (json.dumps(message) + '\x00').encode('utf-8')


def discover(sock, deadline_seconds=120):
    print('discovering editor on %s:%d ...' % (MULTICAST_GROUP, MULTICAST_PORT))
    deadline = time.time() + deadline_seconds
    while time.time() < deadline:
        try:
            sock.sendto(frame({
                'version': 1,
                'magic': 'ue_py',
                'type': 'ping',
                'source': NODE_ID,
            }), (MULTICAST_GROUP, MULTICAST_PORT))
        except OSError as error:
            print('  ping send failed: %s' % error)
        try:
            while True:
                payload = sock.recv(65536)
                text = payload.rstrip(b'\x00').decode('utf-8', 'replace')
                try:
                    message = json.loads(text)
                except ValueError:
                    continue
                if message.get('magic') == 'ue_py' and message.get('type') == 'pong' \
                        and message.get('dest', '') == NODE_ID:
                    editor_node = message.get('source', '')
                    info = message.get('data') or {}
                    print('found editor node %s (engine %s, project %s)' % (
                        editor_node, info.get('engine_version', '?'), info.get('project_name', '?')))
                    return editor_node
        except socket.timeout:
            continue
    raise RuntimeError('no editor answered the multicast ping within %ds' % deadline_seconds)


def run_commands(sock, editor_node, commands, result_timeout=300):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('127.0.0.1', 0))
    server.listen(1)
    port = server.getsockname()[1]
    sock.sendto(frame({
        'version': 1,
        'magic': 'ue_py',
        'type': 'open_connection',
        'source': NODE_ID,
        'dest': editor_node,
        'data': {'command_ip': '127.0.0.1', 'command_port': port},
    }), (MULTICAST_GROUP, MULTICAST_PORT))
    print('waiting for the editor to connect to TCP port %d ...' % port)
    server.settimeout(60)
    conn, address = server.accept()
    conn.settimeout(5.0)
    print('editor connected from %s:%d' % address)
    buffer = b''

    def read_result():
        nonlocal buffer
        deadline = time.time() + result_timeout
        while time.time() < deadline:
            # The editor sends the JSON without a trailing terminator, so a
            # complete message is whatever parses as one JSON object.
            text = buffer.decode('utf-8', 'replace').strip().rstrip('\x00')
            if text:
                try:
                    message = json.loads(text)
                    buffer = b''
                    return message
                except ValueError:
                    pass
            try:
                chunk = conn.recv(65536)
                if not chunk:
                    raise RuntimeError('editor closed the command connection')
                buffer += chunk
            except socket.timeout:
                continue
        raise RuntimeError('timed out waiting for command_result')

    results = []
    for label, code in commands:
        conn.sendall(frame({
            'version': 1,
            'magic': 'ue_py',
            'type': 'command',
            'source': NODE_ID,
            'dest': editor_node,
            'data': {
                'command': code,
                'unattended': True,
                'exec_mode': 'ExecuteStatement',
            },
        }))
        started = time.time()
        result = read_result()
        success = (result.get('data') or {}).get('success', False)
        results.append({'label': label, 'success': success,
                        'seconds': round(time.time() - started, 1)})
        print('command %-12s success=%s (%.1fs)' % (label, success, results[-1]['seconds']))
        if not success:
            for entry in (result.get('data') or {}).get('output', []):
                print('  [%s] %s' % (entry.get('type', '?'), entry.get('output', '').rstrip()))
    try:
        conn.close()
    except OSError:
        pass
    server.close()
    return results


def main():
    if not REPORT_PATH.parent.exists():
        REPORT_PATH.parent.mkdir(parents=True, exist_ok=True)
    sock = open_multicast()
    try:
        editor_node = discover(sock)
        statement = "import runpy; runpy.run_path(r'%s', run_name='__main__')" % str(SCRIPT_PATH)
        results = run_commands(sock, editor_node, [('run_import', statement)])
        if not results[0]['success']:
            print('remote import script reported failure', file=sys.stderr)
            return 1
    finally:
        sock.close()

    # The state machine finalizes (writes the report and quits the editor)
    # over the following editor ticks; wait for the fresh report.
    started_at = time.time()
    import os
    stamp_before = REPORT_PATH.stat().st_mtime if REPORT_PATH.exists() else 0
    while time.time() - started_at < 300:
        if REPORT_PATH.exists() and REPORT_PATH.stat().st_mtime > stamp_before:
            time.sleep(2)
            report = json.loads(REPORT_PATH.read_text(encoding='utf-8'))
            frames = [e for e in (report.get('previews') or {}).get('entries', []) if e.get('png')]
            print('report refreshed: success=%s frames=%d' % (report.get('success'), len(frames)))
            return 0 if report.get('success') else 1
        time.sleep(2)
    print('report was not refreshed within 300s', file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
