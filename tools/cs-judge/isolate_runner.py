"""Two fixed Linux Isolate lanes; no Windows or uncontained execution fallback."""
import base64
import hashlib
import json
import os
import platform
import selectors
import signal
import socket
import socketserver
import stat
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path
from pathlib import PurePosixPath

MAX_PUBLIC_OUTPUT = 24576
MAX_WORKER_MESSAGE = 512 * 1024


class RunnerUnavailable(RuntimeError):
    pass


class RunnerBusy(RunnerUnavailable):
    pass


class BoundedThreads(socketserver.ThreadingMixIn):
    daemon_threads = True
    max_threads = 16

    def __init__(self, *args, **kwargs):
        self.capacity = threading.BoundedSemaphore(self.max_threads)
        super().__init__(*args, **kwargs)

    def process_request(self, request, client_address):
        if not self.capacity.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self.capacity.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.capacity.release()


def decode_worker_json(raw):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('duplicate worker field')
            result[key] = value
        return result
    return json.loads(raw, object_pairs_hook=pairs,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError('invalid worker number')))


def public_output(stdout, stderr, diagnostics):
    result, remaining, truncated = [], MAX_PUBLIC_OUTPUT, False
    for raw in [stdout, stderr, diagnostics]:
        value = raw.decode('utf-8', 'replace')
        value = ''.join(c if ord(c) >= 32 and ord(c) != 127 or c in '\t\n\r' else '\ufffd' for c in value)
        encoded = value.encode()
        if len(encoded) > remaining:
            truncated = True
            encoded = encoded[:remaining]
            value = encoded.decode('utf-8', 'ignore')
        result.append(value)
        remaining -= len(value.encode())
    return *result, truncated


def validate_config(config):
    required = {'isolate', 'isolateSha256', 'toolchainRoot', 'toolchainId',
                'toolchainManifestSha256', 'kernelRelease', 'brokerUid', 'lanes'}
    if not isinstance(config, dict) or set(config) != required:
        raise ValueError('invalid Isolate configuration fields')
    for key in ['isolate', 'toolchainRoot']:
        path = config[key]
        if not isinstance(path, str) or not path.startswith('/') or any(c in path for c in '\n\r:='):
            raise ValueError('invalid fixed deployment path')
    for key in ['isolateSha256', 'toolchainManifestSha256']:
        if not isinstance(config[key], str) or len(config[key]) != 64 or any(c not in '0123456789abcdef' for c in config[key]):
            raise ValueError('missing deployment digest')
    if type(config['brokerUid']) is not int or config['brokerUid'] <= 0 or not isinstance(config['toolchainId'], str) or not config['toolchainId']:
        raise ValueError('invalid broker identity/toolchain')
    kernel = config['kernelRelease']
    if not isinstance(kernel, str) or not kernel or any(c.isspace() for c in kernel):
        raise ValueError('missing installed kernel pin')
    if not isinstance(config['lanes'], dict) or set(config['lanes']) != {'public', 'private'}:
        raise ValueError('both execution lanes are required')
    for lane in config['lanes'].values():
        if not isinstance(lane, dict) or set(lane) != {'boxId', 'workerUid', 'socketPath', 'stateRoot', 'boxRoot'}:
            raise ValueError('invalid lane configuration')
        if type(lane['boxId']) is not int or not 0 <= lane['boxId'] <= 999 or type(lane['workerUid']) is not int or lane['workerUid'] <= 0:
            raise ValueError('invalid lane identity')
        for key in ['socketPath', 'stateRoot', 'boxRoot']:
            if not isinstance(lane[key], str) or not lane[key].startswith('/') or any(c in lane[key] for c in '\n\r:=') or '..' in PurePosixPath(lane[key]).parts:
                raise ValueError('invalid lane path')
    public, private = config['lanes']['public'], config['lanes']['private']
    if public['boxId'] == private['boxId'] or len({public['workerUid'], private['workerUid'], config['brokerUid']}) != 3:
        raise ValueError('public/private/broker identities must be disjoint')
    for key in ['socketPath', 'stateRoot']:
        first, second = PurePosixPath(public[key]), PurePosixPath(private[key])
        if first == second or first in second.parents or second in first.parents:
            raise ValueError('lane workspaces must be disjoint')
    return config


def _wire(value):
    return json.dumps(value, separators=(',', ':'), allow_nan=False).encode() + b'\n'


def config_identity(config):
    return hashlib.sha256(json.dumps(config, sort_keys=True, separators=(',', ':'),
                                     ensure_ascii=False, allow_nan=False).encode()).hexdigest()


def _receive(connection, cancel=None, cancel_call=None):
    data = bytearray()
    cancelled = False
    deadline = time.monotonic() + 120
    while b'\n' not in data:
        if time.monotonic() >= deadline:
            raise RunnerUnavailable('worker response timed out')
        if cancel and cancel.is_set() and not cancelled:
            cancelled = True
            cancel_call()
        try:
            chunk = connection.recv(4096)
        except socket.timeout:
            continue
        if not chunk or len(data) + len(chunk) > MAX_WORKER_MESSAGE:
            raise RunnerUnavailable('invalid worker response')
        data.extend(chunk)
    if data[-1:] != b'\n' or b'\n' in data[:-1]:
        raise RunnerUnavailable('invalid worker framing')
    try:
        return decode_worker_json(data)
    except (ValueError, UnicodeError):
        raise RunnerUnavailable('invalid worker JSON') from None


class IsolateRunner:
    def __init__(self, config):
        self.config = validate_config(config) if config else None

    def _call(self, lane, body, cancel=None):
        if self.config is None or sys.platform != 'linux':
            raise RunnerUnavailable('physical Linux workers are unavailable')
        lane_config = self.config['lanes'][lane]
        path = Path(lane_config['socketPath'])
        info = path.lstat()
        if not stat.S_ISSOCK(info.st_mode) or info.st_uid != lane_config['workerUid'] or info.st_mode & 0o007:
            raise RunnerUnavailable('worker socket ownership is invalid')
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(0.2)
            connection.connect(str(path))
            connection.sendall(_wire(body))
            result = _receive(connection, cancel, lambda: self._call(lane, dict(operation='cancel', jobId=body['jobId'])))
        if isinstance(result, dict) and result.get('error') == 'worker_busy':
            raise RunnerBusy('worker lane is busy')
        if not isinstance(result, dict) or result.get('error'):
            raise RunnerUnavailable('worker refused execution')
        return result

    def ready(self):
        if self.config is None or sys.platform != 'linux':
            return dict(available=False, languages=['cpp20', 'python3'])
        try:
            reports = [self._call(lane, dict(operation='ready')) for lane in ['public', 'private']]
            good = all(r.get('available') and r.get('toolchainId') == self.config['toolchainId'] and
                       r.get('configIdentity') == config_identity(self.config) and
                       r.get('languages') == ['cpp20', 'python3'] for r in reports)
            return dict(available=good, languages=['cpp20', 'python3'], toolchainId=self.config['toolchainId'])
        except (OSError, RunnerUnavailable):
            return dict(available=False, languages=['cpp20', 'python3'])

    def execute(self, source, language, stdin, limits, lane, cancel):
        if lane not in {'public', 'private'} or language not in {'cpp20', 'python3'}:
            raise RunnerUnavailable('unsupported execution request')
        body = dict(operation='run', jobId=uuid.uuid4().hex, language=language,
                    configIdentity=config_identity(self.config),
                    sourceBase64=base64.b64encode(source).decode(), stdinBase64=base64.b64encode(stdin).decode(),
                    timeLimitMs=limits['timeLimitMs'], memoryLimitMiB=limits['memoryLimitMiB'])
        result = self._call(lane, body, cancel)
        allowed = {'status', 'stdoutBase64', 'stderrBase64', 'diagnosticsBase64', 'truncated'}
        if set(result) != allowed or result['status'] not in {'completed', 'compilation_error', 'runtime_error',
                'time_limit', 'memory_limit', 'output_limit', 'cancelled', 'judge_error'} or type(result['truncated']) is not bool:
            raise RunnerUnavailable('invalid result projection')
        if result['status'] == 'judge_error':
            raise RunnerUnavailable('worker containment failed')
        try:
            decoded = {name: base64.b64decode(result[name + 'Base64'], validate=True) for name in ['stdout', 'stderr', 'diagnostics']}
        except (ValueError, TypeError):
            raise RunnerUnavailable('invalid worker output') from None
        if sum(len(v) for v in decoded.values()) > 98304:
            raise RunnerUnavailable('worker output bounds exceeded')
        return dict(decoded, status=result['status'], truncated=result['truncated'])


class LocalSandbox:
    """Called only by the dedicated lane worker, never by the broker."""
    def __init__(self, config, lane):
        self.config = validate_config(config)
        self.lane = lane
        self.settings = config['lanes'][lane]
        self.available = False
        self.lock = threading.Lock()
        self.cancel = None
        self.job_id = None

    def _control(self, action, *args):
        return subprocess.run([self.config['isolate'], '--cg', '--box-id=' + str(self.settings['boxId']), *args, action],
                              stdin=subprocess.DEVNULL, capture_output=True, timeout=10, check=True,
                              env={'PATH': '/usr/bin:/bin'}, close_fds=True)

    def check_environment(self):
        if sys.platform != 'linux' or os.getuid() != self.settings['workerUid'] or platform.release() != self.config['kernelRelease']:
            raise RunnerUnavailable('worker identity mismatch')
        binary = Path(self.config['isolate'])
        info = binary.stat()
        if info.st_uid != 0 or not info.st_mode & stat.S_ISUID or info.st_mode & 0o022 or hashlib.sha256(binary.read_bytes()).hexdigest() != self.config['isolateSha256']:
            raise RunnerUnavailable('untrusted Isolate installation')
        state_info = Path(self.settings['stateRoot']).lstat()
        if not stat.S_ISDIR(state_info.st_mode) or state_info.st_uid != self.settings['workerUid'] or state_info.st_mode & 0o077:
            raise RunnerUnavailable('worker state ownership is invalid')
        root = Path(self.config['toolchainRoot']).resolve(strict=True)
        root_info = root.stat()
        if root_info.st_uid != 0 or root_info.st_mode & 0o022:
            raise RunnerUnavailable('writable toolchain root')
        manifest_file = root / 'toolchain-manifest.json'
        raw = manifest_file.read_bytes()
        if hashlib.sha256(raw).hexdigest() != self.config['toolchainManifestSha256']:
            raise RunnerUnavailable('toolchain manifest mismatch')
        manifest = decode_worker_json(raw)
        if not isinstance(manifest, dict) or set(manifest) != {'toolchainId', 'files'} or manifest.get('toolchainId') != self.config['toolchainId'] or not isinstance(manifest.get('files'), dict):
            raise RunnerUnavailable('toolchain identity mismatch')
        actual = set()
        for path in root.rglob('*'):
            resolved = path.resolve(strict=True)
            if not resolved.is_relative_to(root):
                raise RunnerUnavailable('toolchain link escapes root')
            info = resolved.stat()
            if info.st_uid != 0 or info.st_mode & 0o022:
                raise RunnerUnavailable('writable toolchain')
            if path.is_file() and path != manifest_file:
                relative = path.relative_to(root).as_posix()
                actual.add(relative)
                if hashlib.sha256(path.read_bytes()).hexdigest() != manifest['files'].get(relative):
                    raise RunnerUnavailable('toolchain file mismatch')
            elif not path.is_dir() and not path.is_file():
                raise RunnerUnavailable('toolchain special files are unsupported')
        if actual != set(manifest['files']) or not {'usr/bin/g++', 'usr/bin/python3'} <= actual:
            raise RunnerUnavailable('toolchain inventory mismatch')
        if not Path('/sys/fs/cgroup/cgroup.controllers').is_file() or Path('/proc/swaps').read_text().count('\n') > 1:
            raise RunnerUnavailable('cgroup v2/no-swap requirements missing')
        self._control('--check-config')
        self.available = True
        try:
            for language, source in [('cpp20', b'#include <iostream>\nint main(){std::cout<<"NSTU\\n";}'),
                                     ('python3', b'print("NSTU")\n')]:
                result = self.execute(uuid.uuid4().hex, source, language, b'', 1000, 64)
                if result['status'] != 'completed' or result['stdout'] != b'NSTU\n':
                    raise RunnerUnavailable('language qualification failed')
        except Exception:
            self.available = False
            raise

    def stop(self, job_id):
        if self.job_id == job_id and self.cancel is not None:
            self.cancel.set()

    def _phase(self, argv, work, metadata, cpu, memory, processes, cancel, stdin=b''):
        root = Path(self.config['toolchainRoot'])
        mounts = ['--no-default-dirs', '--dir=/box=' + str(work) + ':rw']
        for directory in ['bin', 'lib', 'lib64', 'usr']:
            if (root / directory).is_dir():
                mounts.append('--dir=/' + directory + '=' + str(root / directory) + ':norec')
        mounts += ['--dir=/proc=proc:fs', '--env=PATH=/usr/bin:/bin', '--env=LANG=C.UTF-8',
                   '--env=HOME=/box', '--env=TMPDIR=/box']
        command = [self.config['isolate'], '--cg', '--box-id=' + str(self.settings['boxId']), '--silent',
                   '--meta=' + str(metadata), '--time=' + str(cpu), '--wall-time=' + str(cpu * 3 + 1),
                   '--cg-mem=' + str(memory * 1024), '--processes=' + str(processes), '--open-files=64',
                   '--fsize=16384', '--core=0', *mounts, '--run', '--', *argv]
        limit = 65536 if self.lane == 'private' else MAX_PUBLIC_OUTPUT
        output = [bytearray(), bytearray()]
        truncated = False
        timed_out = False
        with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              env={'PATH': '/usr/bin:/bin'}, close_fds=True, start_new_session=True) as process:
            def write_input():
                try:
                    process.stdin.write(stdin)
                    process.stdin.close()
                except (OSError, BrokenPipeError):
                    pass
            writer = threading.Thread(target=write_input, daemon=True)
            writer.start()
            poller = selectors.DefaultSelector()
            for index, stream in enumerate([process.stdout, process.stderr]):
                os.set_blocking(stream.fileno(), False)
                poller.register(stream, selectors.EVENT_READ, index)
            deadline = time.monotonic() + cpu * 3 + 6
            terminated = None
            while poller.get_map():
                if cancel.is_set() or truncated or time.monotonic() >= deadline:
                    timed_out |= time.monotonic() >= deadline
                    if terminated is None:
                        process.send_signal(signal.SIGTERM)
                        terminated = time.monotonic()
                    elif time.monotonic() - terminated > 5 and process.poll() is None:
                        process.kill()
                        self.available = False
                for key, _ in poller.select(0.1):
                    raw = os.read(key.fileobj.fileno(), 4096)
                    if not raw:
                        poller.unregister(key.fileobj)
                        continue
                    stream_limit = limit if key.data == 0 else 16384
                    remaining = stream_limit - len(output[key.data])
                    output[key.data].extend(raw[:max(0, remaining)])
                    truncated |= len(raw) > remaining
                if terminated and time.monotonic() - terminated > 8:
                    self.available = False
                    raise RunnerUnavailable('owned job did not close its streams')
            poller.close()
            process.wait(timeout=5)
            writer.join(timeout=1)
        meta = {}
        if metadata.exists():
            for line in metadata.read_text().splitlines():
                key, _, value = line.partition(':')
                if key in meta:
                    raise RunnerUnavailable('invalid Isolate metadata')
                meta[key] = value
        if cancel.is_set():
            status = 'cancelled'
        elif truncated:
            status = 'output_limit'
        elif timed_out or meta.get('status') == 'TO':
            status = 'time_limit'
        elif meta.get('cg-oom-killed') == '1':
            status = 'memory_limit'
        elif meta.get('status') == 'XX' or process.returncode not in {0, 1} or not meta:
            status = 'judge_error'
        elif process.returncode:
            status = 'runtime_error'
        else:
            status = 'completed'
        return dict(status=status, stdout=bytes(output[0]), stderr=bytes(output[1]), diagnostics=b'', truncated=truncated)

    def execute(self, job_id, source, language, stdin, cpu_ms, memory_mib):
        if not self.available:
            raise RunnerUnavailable('lane unavailable')
        if not self.lock.acquire(blocking=False):
            raise RunnerBusy('lane busy')
        self.job_id, self.cancel = job_id, threading.Event()
        state = Path(self.settings['stateRoot'])
        metadata = state / (uuid.uuid4().hex + '.meta')
        try:
            state.mkdir(mode=0o700, parents=True, exist_ok=True)
            initialized = self._control('--init', '--quota=65536,256')
            base = Path(initialized.stdout.decode().strip()).resolve(strict=True)
            expected = (Path(self.settings['boxRoot']) / str(self.settings['boxId'])).resolve(strict=True)
            if base != expected:
                self.available = False
                raise RunnerUnavailable('unexpected owned box path')
            work = base / 'box'
            filename = 'main.cpp' if language == 'cpp20' else 'main.py'
            (work / filename).write_bytes(source)
            compile_argv = ['/usr/bin/g++', '-std=c++20', '-O2', '-pipe', '/box/main.cpp', '-o', '/box/main'] if language == 'cpp20' else ['/usr/bin/python3', '-I', '-B', '-m', 'py_compile', '/box/main.py']
            compiled = self._phase(compile_argv, work, metadata, 20, 1024, 32, self.cancel)
            if compiled['status'] != 'completed':
                if compiled['status'] == 'runtime_error':
                    compiled['status'] = 'compilation_error'
                compiled['diagnostics'] = compiled.pop('stderr')
                compiled['stderr'] = b''
                return compiled
            metadata.unlink(missing_ok=True)
            argv = ['/box/main'] if language == 'cpp20' else ['/usr/bin/python3', '-I', '-B', '/box/main.py']
            return self._phase(argv, work, metadata, cpu_ms / 1000, memory_mib, 16, self.cancel, stdin)
        except (OSError, ValueError, subprocess.SubprocessError, RunnerUnavailable) as error:
            self.available = False
            raise RunnerUnavailable('Isolate containment unavailable') from error
        finally:
            try:
                self._control('--cleanup')
            except (OSError, subprocess.SubprocessError):
                self.available = False
            try:
                metadata.unlink(missing_ok=True)
            except OSError:
                self.available = False
            self.job_id, self.cancel = None, None
            self.lock.release()
            if not self.available:
                raise RunnerUnavailable('owned sandbox cleanup failed')
