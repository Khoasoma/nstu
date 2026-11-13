import tempfile
import threading
import unittest
import copy
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

try:
    import isolate_runner
except ModuleNotFoundError:
    isolate_runner = None


class IsolationTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(isolate_runner, 'Isolate adapter implementation is missing')

    def test_missing_physical_configuration_reports_unavailable(self):
        runner = isolate_runner.IsolateRunner(None)
        self.assertFalse(runner.ready()['available'])
        with self.assertRaises(isolate_runner.RunnerUnavailable):
            runner.execute(b'print(3)', 'python3', b'', dict(timeLimitMs=1000, memoryLimitMiB=64),
                           'public', None)

    def test_sample_and_private_lanes_cannot_share_box_or_worker_identity(self):
        config = dict(isolate='/usr/local/bin/isolate', toolchainRoot='/opt/nstu/toolchain',
                      toolchainId='pinned', isolateSha256='ab' * 32, toolchainManifestSha256='cd' * 32,
                      kernelRelease='6.12.1-reviewed', brokerUid=999,
                      lanes=dict(public=dict(boxId=1, workerUid=1000, socketPath='/run/public/worker.sock', stateRoot='/state/public', boxRoot='/boxes'),
                                 private=dict(boxId=2, workerUid=1001, socketPath='/run/private/worker.sock', stateRoot='/state/private', boxRoot='/boxes')))
        isolate_runner.validate_config(config)
        config['lanes']['private']['boxId'] = 1
        with self.assertRaises(ValueError):
            isolate_runner.validate_config(config)
        config['lanes']['private']['boxId'] = 2
        config['lanes']['private']['workerUid'] = 1001
        config['lanes']['private']['stateRoot'] = '/state/public/private'
        with self.assertRaises(ValueError):
            isolate_runner.validate_config(config)
        config['lanes']['private']['stateRoot'] = '/state/private'
        config['lanes']['private']['workerUid'] = 1000
        with self.assertRaises(ValueError):
            isolate_runner.validate_config(config)

    def test_bounded_output_is_valid_utf8_and_control_sequences_are_inert(self):
        stdout, stderr, diagnostics, truncated = isolate_runner.public_output(
            b'\x1b[31m\x00bad\xff' + 'ế'.encode() * 12000, b'error', b'compiler')
        self.assertLessEqual(sum(len(x.encode()) for x in [stdout, stderr, diagnostics]), 24576)
        self.assertNotIn('\x1b', stdout)
        self.assertNotIn('\x00', stdout)
        self.assertTrue(truncated)

    def runner_config(self):
        return dict(isolate='/usr/local/bin/isolate', toolchainRoot='/opt/nstu/toolchain',
                    toolchainId='pinned', isolateSha256='ab' * 32, toolchainManifestSha256='cd' * 32,
                    kernelRelease='6.12.1-reviewed', brokerUid=999,
                    lanes=dict(public=dict(boxId=1, workerUid=1000, socketPath='/run/public/worker.sock', stateRoot='/state/public', boxRoot='/boxes'),
                               private=dict(boxId=2, workerUid=1001, socketPath='/run/private/worker.sock', stateRoot='/state/private', boxRoot='/boxes')))

    def config_digest(self, config):
        return hashlib.sha256(json.dumps(config, sort_keys=True, separators=(',', ':'),
                                         ensure_ascii=False, allow_nan=False).encode()).hexdigest()

    def test_readiness_requires_full_worker_config_pin_with_same_rootfs_id(self):
        config = self.runner_config()
        worker_config = copy.deepcopy(config)
        runner = isolate_runner.IsolateRunner(config)
        runner._call = lambda *args: dict(available=True, languages=['cpp20', 'python3'],
                                         toolchainId=worker_config['toolchainId'], configIdentity=self.config_digest(worker_config))
        with patch.object(isolate_runner.sys, 'platform', 'linux'):
            self.assertTrue(runner.ready()['available'])
            for field, value in [('isolateSha256', 'ef' * 32), ('kernelRelease', '6.12.2-reviewed'),
                                 ('toolchainManifestSha256', '12' * 32)]:
                with self.subTest(field=field):
                    worker_config = dict(config, **{field: value})
                    self.assertFalse(runner.ready()['available'])

    def test_job_carries_full_pin_and_worker_rejects_restart_drift(self):
        import worker
        config = self.runner_config()
        runner = isolate_runner.IsolateRunner(config)
        sandbox = Mock(available=True)
        sandbox.execute.return_value = dict(status='completed', stdout=b'3\n', stderr=b'', diagnostics=b'', truncated=False)
        delivered = []
        def call(lane, body, cancel=None):
            delivered.append(body)
            return worker.handle_request(sandbox, config, lane, body)
        runner._call = call
        result = runner.execute(b'print(3)', 'python3', b'', dict(timeLimitMs=1000, memoryLimitMiB=64),
                                'public', threading.Event())
        self.assertEqual(result['stdout'], b'3\n')
        self.assertEqual(delivered[0]['configIdentity'], self.config_digest(config))
        sandbox.execute.reset_mock()
        changed = dict(config, isolateSha256='ef' * 32)
        with self.assertRaises(isolate_runner.RunnerUnavailable):
            worker.handle_request(sandbox, changed, 'public', delivered[0])
        sandbox.execute.assert_not_called()
        ready = worker.handle_request(sandbox, changed, 'public', dict(operation='ready'))
        self.assertEqual(ready['configIdentity'], self.config_digest(changed))

    def test_worker_json_rejects_duplicates_nonfinite_and_invalid_utf8(self):
        for raw in [b'{"jobId":"one","jobId":"two"}', b'{"time":NaN}', b'{"time":Infinity}', b'"\xff"']:
            with self.subTest(raw=raw), self.assertRaises((ValueError, UnicodeError)):
                isolate_runner.decode_worker_json(raw)

    def sandbox_fixture(self, path):
        sandbox = object.__new__(isolate_runner.LocalSandbox)
        sandbox.config = {}
        sandbox.lane = 'public'
        sandbox.settings = dict(boxId=1, stateRoot=str(path / 'state'), boxRoot=str(path / 'boxes'))
        sandbox.available = True
        sandbox.lock = threading.Lock()
        sandbox.job_id = sandbox.cancel = None
        base = path / 'boxes' / '1'
        (base / 'box').mkdir(parents=True)
        sandbox._control = Mock(return_value=SimpleNamespace(stdout=str(base).encode()))
        sandbox._phase = Mock(return_value=dict(status='completed', stdout=b'3\n', stderr=b'', diagnostics=b'', truncated=False))
        return sandbox, base / 'box'

    def test_fixture_execution_passes_stdin_as_bytes_without_host_box_input_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            sandbox, work = self.sandbox_fixture(Path(temporary))
            sandbox.execute('test-job', b'print(3)', 'python3', b'1 2\n', 1000, 64)
            self.assertEqual(sandbox._phase.call_args.args[-1], b'1 2\n')
            self.assertFalse((work / 'input').exists())
            sandbox._control.assert_any_call('--cleanup')

    def test_cleanup_failure_replaces_success_and_disables_lane(self):
        with tempfile.TemporaryDirectory() as temporary:
            sandbox, _ = self.sandbox_fixture(Path(temporary))
            initialized = sandbox._control.return_value
            sandbox._control.side_effect = [initialized, OSError('cleanup unavailable')]
            with self.assertRaises(isolate_runner.RunnerUnavailable):
                sandbox.execute('test-job', b'print(3)', 'python3', b'', 1000, 64)
            self.assertFalse(sandbox.available)
            self.assertIsNone(sandbox.job_id)
            self.assertFalse(sandbox.lock.locked())

    def test_busy_lane_preserves_existing_job(self):
        with tempfile.TemporaryDirectory() as temporary:
            sandbox, _ = self.sandbox_fixture(Path(temporary))
            sandbox.job_id = 'owned-job'
            sandbox.lock.acquire()
            with self.assertRaises(isolate_runner.RunnerBusy):
                sandbox.execute('other-job', b'print(3)', 'python3', b'', 1000, 64)
            self.assertEqual(sandbox.job_id, 'owned-job')
            sandbox._control.assert_not_called()
            sandbox.lock.release()

    def test_worker_storage_failure_disables_lane_and_releases_owned_lock(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            sandbox, _ = self.sandbox_fixture(path)
            (path / 'state').write_bytes(b'not a directory')
            with self.assertRaises(isolate_runner.RunnerUnavailable):
                sandbox.execute('test-job', b'print(3)', 'python3', b'', 1000, 64)
            self.assertFalse(sandbox.available)
            self.assertIsNone(sandbox.job_id)
            self.assertFalse(sandbox.lock.locked())
            sandbox._phase.assert_not_called()

    def test_request_thread_capacity_rejects_overflow_and_releases_after_completion(self):
        started, finish = threading.Event(), threading.Event()
        closed = []
        class Base:
            def __init__(inner):
                pass
            def finish_request(inner, request, address):
                started.set()
                finish.wait(1)
            def shutdown_request(inner, request):
                closed.append(request)
        class Server(isolate_runner.BoundedThreads, Base):
            max_threads = 1
        server = Server()
        server.process_request('owned', None)
        self.assertTrue(started.wait(1))
        server.process_request('overflow', None)
        self.assertEqual(closed, ['overflow'])
        finish.set()
        self.assertTrue(server.capacity.acquire(timeout=1))
        self.assertEqual(closed, ['overflow', 'owned'])
        server.capacity.release()


if __name__ == '__main__':
    unittest.main()
