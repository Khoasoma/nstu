"""Dedicated public/private worker. Only the local broker UID may connect."""
import argparse
import base64
import json
import os
import re
import socket
import socketserver
import struct
import signal
import threading
from pathlib import Path

from isolate_runner import BoundedThreads, LocalSandbox, MAX_WORKER_MESSAGE, RunnerBusy, RunnerUnavailable, _wire, config_identity, decode_worker_json


def handle_request(sandbox, config, lane, body):
    if not isinstance(body, dict):
        raise RunnerUnavailable('worker object required')
    operation = body.get('operation')
    if operation == 'ready' and set(body) == {'operation'}:
        return dict(available=sandbox.available, languages=['cpp20', 'python3'],
                    toolchainId=config['toolchainId'], configIdentity=config_identity(config))
    if operation == 'cancel' and set(body) == {'operation', 'jobId'}:
        if not isinstance(body['jobId'], str) or not re.fullmatch('[0-9a-f]{32}', body['jobId']):
            raise RunnerUnavailable('invalid cancellation identity')
        sandbox.stop(body['jobId'])
        return dict(status='cancel_requested')
    if operation != 'run' or set(body) != {'operation', 'jobId', 'language', 'configIdentity', 'sourceBase64', 'stdinBase64', 'timeLimitMs', 'memoryLimitMiB'}:
        raise RunnerUnavailable('unsupported worker message')
    if body['configIdentity'] != config_identity(config):
        raise RunnerUnavailable('frozen runtime mismatch')
    if not isinstance(body['jobId'], str) or not re.fullmatch('[0-9a-f]{32}', body['jobId']) or body['language'] not in {'cpp20', 'python3'}:
        raise RunnerUnavailable('invalid source identity')
    source = base64.b64decode(body['sourceBase64'], validate=True)
    stdin = base64.b64decode(body['stdinBase64'], validate=True)
    decoded = source.decode('utf-8')
    if not 0 < len(source) <= 32768 or len(stdin) > (4096 if lane == 'public' else 65536) or any((ord(c) < 32 and c not in '\t\n\r') or ord(c) == 127 for c in decoded):
        raise RunnerUnavailable('source/input bounds')
    if type(body['timeLimitMs']) is not int or not 100 <= body['timeLimitMs'] <= 10000 or type(body['memoryLimitMiB']) is not int or not 16 <= body['memoryLimitMiB'] <= 512:
        raise RunnerUnavailable('resource bounds')
    ran = sandbox.execute(body['jobId'], source, body['language'], stdin, body['timeLimitMs'], body['memoryLimitMiB'])
    return dict(status=ran['status'], truncated=ran['truncated'],
                **{key + 'Base64': base64.b64encode(ran[key]).decode() for key in ['stdout', 'stderr', 'diagnostics']})


def serve(config, lane):
    sandbox = LocalSandbox(config, lane)
    sandbox.check_environment()
    settings = config['lanes'][lane]
    path = Path(settings['socketPath'])
    path.parent.mkdir(mode=0o750, parents=True, exist_ok=True)
    if path.exists():
        raise RunnerUnavailable('remove the stopped owned worker socket before restart')

    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            _, uid, _ = struct.unpack('3i', self.connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
            if uid != config['brokerUid']:
                return
            self.connection.settimeout(10)
            try:
                raw = self.rfile.readline(MAX_WORKER_MESSAGE + 1)
                if not raw.endswith(b'\n') or len(raw) > MAX_WORKER_MESSAGE:
                    raise RunnerUnavailable('invalid message framing')
                body = decode_worker_json(raw)
                result = handle_request(sandbox, config, lane, body)
            except RunnerBusy:
                result = dict(error='worker_busy')
            except (OSError, ValueError, TypeError, RunnerUnavailable):
                result = dict(error='worker_unavailable')
            self.wfile.write(_wire(result))

    class Server(BoundedThreads, socketserver.UnixStreamServer):
        daemon_threads = False
        max_threads = 4
        request_queue_size = 4
    with Server(str(path), Handler) as server:
        os.chmod(path, 0o660)
        def stop(_signal, _frame):
            sandbox.stop(sandbox.job_id)
            threading.Thread(target=server.shutdown, daemon=True).start()
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        server.serve_forever()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--config', required=True)
    parser.add_argument('--lane', choices=['public', 'private'], required=True)
    args = parser.parse_args()
    os.umask(0o077)
    serve(json.loads(Path(args.config).read_bytes()), args.lane)


if __name__ == '__main__':
    main()
