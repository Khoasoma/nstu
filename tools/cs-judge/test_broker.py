import base64
import copy
import hashlib
import io
import json
import sqlite3
import tempfile
import unittest
import threading
from pathlib import Path
from unittest.mock import Mock, patch

try:
    import broker
except ModuleNotFoundError:
    broker = None


TOKEN = 'operator-test-secret-with-at-least-32-bytes'
CONTEXT = dict(packageId='cs-test', packageDigestHex='ab' * 32,
               clientIdHex='cd' * 16, sessionIdHex='ef' * 16, candidateId='student-1')
MANIFEST = dict(id='cs-test', title='CS', durationSeconds=600,
                contentMode='computer_science', programmingVersion=1, sessionMode='exam',
                languages=['cpp20', 'python3'], questions=[dict(
                    id='sum', type='programming', revision=1, prompt='Add two integers.',
                    points=100, samples=[dict(id='sample-1', input='1 2\n', output='3\n')],
                    timeLimitMs=1000, memoryLimitMiB=64)])


class Runner:
    def __init__(self):
        self.calls = []
        self.output = b'3\n'

    def ready(self):
        return dict(available=True, languages=['cpp20', 'python3'], toolchainId='test-pinned')

    def execute(self, source, language, stdin, limits, lane, cancel):
        self.calls.append((source, language, stdin, lane))
        return dict(status='completed', stdout=self.output, stderr=b'', diagnostics=b'', truncated=False)


class BrokerTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(broker, 'CS broker implementation is missing')
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.now = 1_800_000_000_000
        self.runner = Runner()
        self.db = Path(self.tmp.name) / 'judge.sqlite3'
        self.app = broker.Broker(self.db, TOKEN, self.runner, clock=lambda: self.now)
        self.addCleanup(lambda: self.app.close())
        self.call('/v1/packages', dict(programmingVersion=1, packageId='cs-test',
                                     packageDigestHex=CONTEXT['packageDigestHex'], manifest=MANIFEST))
        grading = dict(programmingVersion=1, packageId='cs-test',
                       packageDigestHex=CONTEXT['packageDigestHex'], problems=[dict(
                           id='sum', revision=1, cases=[dict(input='SECRET-INPUT\n', output='3\n')])])
        grading['gradingDigestHex'] = hashlib.sha256(broker.canonical_json(grading)).hexdigest()
        self.grading = grading
        self.call('/v1/grading', grading)
        self.call('/v1/sessions', dict(CONTEXT, programmingVersion=1, sessionMode='exam',
                                      gradingDigestHex=grading['gradingDigestHex']))
        self.call('/v1/begin', dict(CONTEXT, programmingVersion=1, requestId='begin-1', durationSeconds=600))

    def call(self, route, body, token=TOKEN):
        return self.app.dispatch(route, copy.deepcopy(body), token)

    def request(self, operation, request_id='request-1', **fields):
        return dict(CONTEXT, programmingVersion=1, operation=operation, requestId=request_id, **fields)

    def solution(self, operation='submit', request_id='solution-1', source='print(3)\n', revision=1):
        return self.request(operation, request_id, problemId='sum', problemRevision=1,
                            language='python3', source=source, draftRevision=revision)

    def test_authentication_precedes_read_or_write(self):
        with self.assertRaises(broker.BrokerError) as error:
            self.call('/v1/programming', self.solution(), 'wrong-token')
        self.assertEqual(error.exception.code, 'unauthorized')

    def test_second_live_broker_cannot_reset_running_work_in_shared_database(self):
        second = None
        try:
            with self.assertRaises(broker.BrokerError) as error:
                second = broker.Broker(self.db, TOKEN, self.runner)
            self.assertEqual(error.exception.code, 'storage_unavailable')
        finally:
            if second:
                second.close()

    def test_received_snapshot_and_receipt_survive_restart_and_lost_ack(self):
        first = self.call('/v1/programming', self.solution())
        self.app.close()
        self.now += 1000
        self.app = broker.Broker(self.db, TOKEN, self.runner, clock=lambda: self.now)
        replay = self.call('/v1/programming', self.solution())
        self.assertEqual(first, replay)
        self.assertEqual(first['status'], 'received')
        self.assertEqual(self.app.private_score(CONTEXT)['score'], 0)

    def test_changed_bytes_under_same_request_id_conflict(self):
        self.call('/v1/programming', self.solution())
        with self.assertRaises(broker.BrokerError) as error:
            self.call('/v1/programming', self.solution(source='print(9)\n'))
        self.assertEqual(error.exception.code, 'conflict')

    def test_foreign_context_and_unknown_fields_are_rejected(self):
        for patch in [dict(candidateId='someone-else'), dict(packageDigestHex='11' * 32),
                      dict(privateTestId='hidden'), dict(programmingVersion=2),
                      dict(problemRevision=2), dict(language='bash')]:
            with self.subTest(patch=patch), self.assertRaises(broker.BrokerError):
                body = self.solution()
                body.update(patch)
                self.call('/v1/programming', body)

    def test_source_size_is_utf8_bytes_and_controls_are_rejected_without_truncation(self):
        self.call('/v1/programming', self.solution(source='x' * 32768))
        for source in ['x' * 32769, 'ế' * 12000, 'x\x00y', 'x\x01y', 'x\x7fy', '\ud800']:
            with self.subTest(source=repr(source[:20])), self.assertRaises(broker.BrokerError):
                self.call('/v1/programming', self.solution(request_id='bad-source', source=source))

    def test_draft_recovery_preserves_exact_bytes_and_monotonic_revision(self):
        source = 'print("ế")\r\n\t# trailing  \n'
        self.call('/v1/programming', self.solution('draft', source=source, revision=2))
        with self.assertRaises(broker.BrokerError):
            self.call('/v1/programming', self.solution('draft', 'older', 'print(1)', 1))
        result = self.call('/v1/programming', self.request('recover', problemId='sum',
                                                        problemRevision=1, language='python3'))
        self.assertEqual(base64.b64decode(result['sourceBase64']), source.encode())
        self.assertEqual(result['draftRevision'], 2)
        self.assertNotIn('cases', json.dumps(result))

    def test_late_source_is_preserved_but_never_eligible(self):
        first = self.call('/v1/programming', self.solution())
        self.now += 600_001
        late = self.call('/v1/programming', self.solution(request_id='late', source='print(4)', revision=2))
        self.assertEqual(late['status'], 'late')
        self.assertEqual(self.call('/v1/programming', self.solution()), first)
        self.assertEqual(self.app.private_score(CONTEXT)['eligibleSubmissions'], 1)

    def test_public_run_uses_canonical_sample_and_never_private_input(self):
        request = self.solution('run', 'run-1')
        request['sampleId'] = 'sample-1'
        result = self.call('/v1/programming', request)
        self.assertEqual(self.runner.calls[0][2], b'1 2\n')
        self.assertEqual(self.runner.calls[0][3], 'public')
        self.assertEqual(result['status'], 'completed')
        self.assertTrue(result['sampleMatched'])
        with self.assertRaises(broker.BrokerError):
            request['sampleId'] = '../../private/input'
            self.call('/v1/programming', request)

    def test_public_output_budget_is_enforced_before_response_serialization(self):
        self.runner.output = b'<script>bad()</script>' * 3000
        request = self.solution('run', 'huge-run')
        request['sampleId'] = 'sample-1'
        result = self.call('/v1/programming', request)
        self.assertLessEqual(sum(len(result[k].encode()) for k in ['stdout', 'stderr', 'diagnostics']), 24576)
        self.assertTrue(result['truncated'])
        self.assertFalse(result['sampleMatched'])

    def test_private_judging_never_changes_receipt_or_candidate_recovery(self):
        receipt = self.call('/v1/programming', self.solution())
        before = self.call('/v1/programming', self.request('recover', problemId='sum',
                                                        problemRevision=1, language='python3'))
        self.assertTrue(self.app.judge_one())
        self.assertEqual(self.runner.calls[-1][2], b'SECRET-INPUT\n')
        self.assertEqual(self.runner.calls[-1][3], 'private')
        self.assertEqual(self.call('/v1/programming', self.solution()), receipt)
        after = self.call('/v1/programming', self.request('recover', problemId='sum',
                                                       problemRevision=1, language='python3'))
        self.assertEqual(before, after)
        self.assertEqual(self.app.private_score(CONTEXT)['score'], 100)

    def test_busy_private_worker_does_not_consume_judging_retries(self):
        from isolate_runner import RunnerBusy
        class BusyRunner(Runner):
            def execute(inner, *args):
                raise RunnerBusy('busy')
        self.call('/v1/programming', self.solution())
        self.app.runner = BusyRunner()
        for _ in range(5):
            self.app.judge_one()
        self.app.runner = self.runner
        self.app.judge_one()
        self.assertEqual(self.app.private_score(CONTEXT)['score'], 100)

    def test_exam_grades_latest_eligible_snapshot_not_best(self):
        self.call('/v1/programming', self.solution())
        self.app.judge_one()
        self.call('/v1/programming', self.solution(request_id='new', source='print(4)', revision=2))
        self.runner.output = b'4\n'
        self.app.judge_one()
        self.assertEqual(self.app.private_score(CONTEXT)['score'], 0)

    def test_received_source_recovers_when_autosave_did_not_arrive(self):
        self.call('/v1/programming', self.solution(source='print("ế")\n', revision=3))
        result = self.call('/v1/programming', self.request('recover', problemId='sum',
                                                        problemRevision=1, language='python3'))
        self.assertEqual(base64.b64decode(result['sourceBase64']), 'print("ế")\n'.encode())
        self.assertEqual(result['draftRevision'], 3)
        self.assertEqual(result['problemRevision'], 1)
        self.assertTrue(result['latestReceiptId'])
        self.assertEqual(result['receiptDigest'], hashlib.sha256('print("ế")\n'.encode()).hexdigest())
        self.assertNotIn('receipts', result)

    def test_cancel_validates_problem_language_and_owned_run_identity(self):
        for patch in [dict(problemId='foreign'), dict(language='bash'), dict(problemRevision=2)]:
            body = self.request('cancel', targetRunId='run-1', problemId='sum', problemRevision=1, language='python3')
            body.update(patch)
            with self.subTest(patch=patch), self.assertRaises(broker.BrokerError):
                self.call('/v1/programming', body)

    def test_contest_counts_first_accepted_and_penalties_internally(self):
        other = dict(CONTEXT, sessionIdHex='11' * 16, packageDigestHex='22' * 32)
        manifest = copy.deepcopy(MANIFEST)
        manifest['sessionMode'] = 'contest'
        self.call('/v1/packages', dict(programmingVersion=1, packageId='cs-test',
                                     packageDigestHex=other['packageDigestHex'], manifest=manifest))
        grading = copy.deepcopy(self.grading)
        grading['packageDigestHex'] = other['packageDigestHex']
        grading.pop('gradingDigestHex')
        grading['gradingDigestHex'] = hashlib.sha256(broker.canonical_json(grading)).hexdigest()
        self.call('/v1/grading', grading)
        self.call('/v1/sessions', dict(other, programmingVersion=1, sessionMode='contest', gradingDigestHex=grading['gradingDigestHex']))
        self.call('/v1/begin', dict(other, programmingVersion=1, requestId='begin', durationSeconds=600))
        first = self.solution(source='print(4)')
        first.update(other)
        self.runner.output = b'4\n'
        self.call('/v1/programming', first)
        self.app.judge_one()
        self.now += 120000
        second = self.solution(request_id='second', revision=2)
        second.update(other)
        self.runner.output = b'3\n'
        self.call('/v1/programming', second)
        self.app.judge_one()
        score = self.app.private_score(other)
        self.assertEqual(score['solved'], 1)
        self.assertEqual(score['penaltyMinutes'], 22)

    def test_finalize_requires_owned_received_inventory_and_remains_idempotent(self):
        receipt = self.call('/v1/programming', self.solution())
        body = self.request('finalize', receiptIds=['foreign'], noSourceProblemIds=[])
        with self.assertRaises(broker.BrokerError):
            self.call('/v1/programming', body)
        body['receiptIds'] = [receipt['receiptId']]
        first = self.call('/v1/programming', body)
        self.assertEqual(first['status'], 'finalized')
        self.assertTrue(first['receiptId'])
        self.now += 10
        self.assertEqual(self.call('/v1/programming', body), first)
        late = self.call('/v1/programming', self.solution(request_id='closed', revision=2))
        self.assertEqual(late['status'], 'late')

    def test_unavailable_runner_cannot_begin_or_fake_a_run(self):
        self.app.runner = None
        ready = self.call('/v1/programming', self.request('ready'))
        self.assertFalse(ready['runnerReady'])
        self.assertTrue(ready['reason'])
        request = self.solution('run', 'no-runner')
        request['sampleId'] = 'sample-1'
        with self.assertRaises(broker.BrokerError) as error:
            self.call('/v1/programming', request)
        self.assertEqual(error.exception.code, 'runner_unavailable')

    def test_js_safe_maximum_draft_revision_roundtrips_without_loss(self):
        self.call('/v1/programming', self.solution('draft', revision=9007199254740991))
        restored = self.call('/v1/programming', self.request('recover', problemId='sum',
                                                          problemRevision=1, language='python3'))
        self.assertEqual(restored['draftRevision'], 9007199254740991)
        with self.assertRaises(broker.BrokerError):
            self.call('/v1/programming', self.solution('draft', 'overflow', revision=9007199254740992))

    def test_storage_failure_never_reports_received_or_replaces_old_draft(self):
        self.call('/v1/programming', self.solution('draft', source='print(1)', revision=1))
        self.app.db.execute('PRAGMA query_only=ON')
        for body in [self.solution('draft', 'failed-draft', 'print(2)', 2), self.solution()]:
            with self.subTest(operation=body['operation']), self.assertRaises(broker.BrokerError) as error:
                self.call('/v1/programming', body)
            self.assertEqual(error.exception.code, 'storage_unavailable')
        self.app.db.execute('PRAGMA query_only=OFF')
        restored = self.call('/v1/programming', self.request('recover', problemId='sum', problemRevision=1, language='python3'))
        self.assertEqual(base64.b64decode(restored['sourceBase64']), b'print(1)')
        self.assertEqual(restored['latestReceiptId'], '')

    def test_clock_rollback_disrupts_eligibility_without_losing_source(self):
        self.now -= 1
        late = self.call('/v1/programming', self.solution())
        self.assertEqual(late['status'], 'late')
        self.now += 1000
        next_receipt = self.call('/v1/programming', self.solution(request_id='after-clock', revision=2))
        self.assertEqual(next_receipt['status'], 'late')
        self.assertEqual(self.app.private_score(CONTEXT)['eligibleSubmissions'], 0)

    def test_full_http_body_receipt_before_deadline_survives_delayed_processing(self):
        deadline = self.now + 600000
        self.now = deadline - 1
        raw = broker.canonical_json(self.solution(request_id='received-before-deadline'))
        responses = []
        class FakeHTTPServer:
            def __init__(inner, address, handler):
                inner.handler = handler
            def serve_forever(inner):
                handler = object.__new__(inner.handler)
                handler.path = '/v1/programming'
                handler.headers = {'Authorization': 'Bearer ' + TOKEN, 'Content-Length': str(len(raw)),
                                   'Content-Type': 'application/json'}
                handler.connection = Mock()
                handler.rfile, handler.wfile = io.BytesIO(raw), io.BytesIO()
                handler.send_response = Mock()
                handler.send_header = handler.end_headers = Mock()
                handler.do_POST()
                responses.append(json.loads(handler.wfile.getvalue()))
        decode = broker.decode_json
        def delayed_decode(value):
            if value == raw:
                self.now = deadline + 1
            return decode(value)
        with patch.object(broker, 'HTTPServer', FakeHTTPServer), patch.object(broker, 'decode_json', delayed_decode):
            broker.serve(self.app, 0)
        self.assertEqual(responses[0]['status'], 'received')
        self.assertEqual(responses[0]['serverTime'], deadline - 1)

    def test_out_of_order_processing_preserves_admission_time_without_clock_disruption(self):
        first = self.app.capture_admission()
        self.now += 1
        second = self.app.capture_admission()
        newer = self.app.dispatch('/v1/programming', self.solution(request_id='newer-ingress', source='print(2)', revision=2), TOKEN, second)
        older = self.app.dispatch('/v1/programming', self.solution(request_id='older-ingress', source='print(1)'), TOKEN, first)
        self.assertEqual((newer['status'], older['status']), ('received', 'received'))
        self.assertEqual(older['serverTime'], self.now - 1)
        self.assertEqual(self.app.db.execute('SELECT state FROM sessions').fetchone()[0], 'active')
        recovered = self.call('/v1/programming', self.request('recover', problemId='sum', problemRevision=1, language='python3'))
        self.assertEqual(base64.b64decode(recovered['sourceBase64']), b'print(2)')
        self.assertEqual(recovered['draftRevision'], 2)
        self.assertEqual(self.call('/v1/programming', self.solution(request_id='after-reordering'))['status'], 'received')

    def test_owned_cancel_interrupts_pending_public_run_without_waiting_for_run(self):
        started = threading.Event()
        class WaitingRunner(Runner):
            def execute(inner, source, language, stdin, limits, lane, cancel):
                started.set()
                if not cancel.wait(3):
                    raise RuntimeError('test cancellation was not delivered')
                return dict(status='cancelled', stdout=b'', stderr=b'', diagnostics=b'', truncated=False)
        self.app.runner = WaitingRunner()
        request = self.solution('run', 'owned-run')
        request['sampleId'] = 'sample-1'
        results = []
        thread = threading.Thread(target=lambda: results.append(self.call('/v1/programming', request)))
        thread.start()
        self.assertTrue(started.wait(1))
        cancel = self.request('cancel', 'cancel-1', targetRunId='owned-run', problemId='sum', problemRevision=1, language='python3')
        self.assertEqual(self.call('/v1/programming', cancel)['status'], 'cancelled')
        thread.join(2)
        self.assertFalse(thread.is_alive())
        self.assertEqual(results[0]['status'], 'cancelled')

    def test_cancel_failure_never_claims_the_owned_job_stopped(self):
        started = threading.Event()
        class FailedRunner(Runner):
            def execute(inner, source, language, stdin, limits, lane, cancel):
                started.set()
                if not cancel.wait(2):
                    raise RuntimeError('test cancellation missing')
                raise RuntimeError('owned cleanup failed')
        self.app.runner = FailedRunner()
        request = dict(self.solution('run', 'broken-run'), sampleId='sample-1')
        errors = []
        def run():
            try:
                self.call('/v1/programming', request)
            except broker.BrokerError as error:
                errors.append(error.code)
        thread = threading.Thread(target=run)
        thread.start()
        self.assertTrue(started.wait(1))
        result = self.call('/v1/programming', self.request('cancel', 'stop-broken',
                           targetRunId='broken-run', problemId='sum', problemRevision=1, language='python3'))
        thread.join(2)
        self.assertFalse(thread.is_alive())
        self.assertEqual(result['status'], 'unavailable')
        self.assertEqual(errors, ['runner_unavailable'])

    def test_public_timeout_and_resource_statuses_use_frozen_protocol(self):
        class LimitedRunner(Runner):
            status = 'time_limit'
            def execute(inner, *args):
                return dict(status=inner.status, stdout=b'', stderr=b'', diagnostics=b'', truncated=False)
        self.app.runner = LimitedRunner()
        for internal, public in [('time_limit', 'timeout'), ('memory_limit', 'runtime_error'), ('output_limit', 'runtime_error')]:
            self.app.runner.status = internal
            result = self.call('/v1/programming', dict(self.solution('run', internal), sampleId='sample-1'))
            self.assertEqual(result['status'], public)
            self.assertEqual(result['problemRevision'], 1)
            self.assertFalse(result['sampleMatched'])

    def test_manifest_bounds_origins_and_id_prefix_match_native(self):
        for patch in [dict(id='.hidden'), dict(id='-hidden'), dict(instruction='x' * 4001)]:
            manifest = copy.deepcopy(MANIFEST)
            manifest['questions'][0].update(patch)
            with self.subTest(patch=patch), self.assertRaises(broker.BrokerError):
                broker.validate_manifest(manifest)
        manifest = copy.deepcopy(MANIFEST)
        manifest['allowedOrigins'] = []
        manifest['documents'] = []
        manifest['questions'][0]['instruction'] = 'x' * 4000
        broker.validate_manifest(manifest)
        manifest['allowedOrigins'] = ['https://candidate.example']
        with self.assertRaises(broker.BrokerError):
            broker.validate_manifest(manifest)
        manifest['allowedOrigins'] = []
        manifest['documents'] = [{'id': 'private-guide'}]
        with self.assertRaises(broker.BrokerError):
            broker.validate_manifest(manifest)

    def test_finalize_empty_draft_preserves_eligible_source_and_failed_ack_is_retryable(self):
        receipt = self.call('/v1/programming', self.solution())
        self.app.judge_one()
        self.call('/v1/programming', self.solution('draft', 'empty-final-draft', '', 2))
        body = self.request('finalize', 'empty-final', receiptIds=[], noSourceProblemIds=['sum'])
        self.app.db.execute('PRAGMA query_only=ON')
        with self.assertRaises(broker.BrokerError) as error:
            self.call('/v1/programming', body)
        self.assertEqual(error.exception.code, 'storage_unavailable')
        self.app.db.execute('PRAGMA query_only=OFF')
        result = self.call('/v1/programming', body)
        self.assertEqual(result['status'], 'finalized')
        self.assertEqual(self.app.private_score(CONTEXT)['score'], 100)
        self.assertEqual(self.call('/v1/programming', self.solution()), receipt)
        recovered = self.call('/v1/programming', self.request('recover', 'post-final-recover',
                              problemId='sum', problemRevision=1, language='python3'))
        self.assertEqual(recovered['status'], 'recovered')
        self.assertEqual(recovered['sourceBase64'], '')
        self.assertEqual(recovered['latestReceiptId'], receipt['receiptId'])
        self.assertTrue(recovered['finalized'])

    def test_invalid_inventory_value_fails_as_protocol_error(self):
        for inventory in [[{}], [[]], [123]]:
            with self.subTest(inventory=inventory), self.assertRaises(broker.BrokerError):
                self.call('/v1/programming', self.request('finalize', receiptIds=inventory, noSourceProblemIds=[]))

    def test_clock_rollback_observed_by_run_disrupts_future_submission_eligibility(self):
        self.now -= 1
        with self.assertRaises(broker.BrokerError):
            self.call('/v1/programming', dict(self.solution('run', 'rollback-run'), sampleId='sample-1'))
        self.now += 1000
        result = self.call('/v1/programming', self.solution())
        self.assertEqual(result['status'], 'late')
        self.assertEqual(self.app.private_score(CONTEXT)['eligibleSubmissions'], 0)

    def test_toolchain_change_does_not_judge_or_run_prepared_session_but_keeps_receipts(self):
        receipt = self.call('/v1/programming', self.solution())
        self.runner.ready = lambda: dict(available=True, languages=['cpp20', 'python3'], toolchainId='other-pinned-runtime')
        self.assertFalse(self.call('/v1/programming', self.request('ready'))['runnerReady'])
        self.assertFalse(self.app.judge_one())
        with self.assertRaises(broker.BrokerError):
            self.call('/v1/programming', dict(self.solution('run', 'changed-toolchain'), sampleId='sample-1'))
        self.assertEqual(self.call('/v1/programming', self.solution()), receipt)
        self.assertEqual(self.call('/v1/programming', self.request('recover', problemId='sum',
                         problemRevision=1, language='python3'))['latestReceiptId'], receipt['receiptId'])

    def test_incompatible_old_session_does_not_block_current_private_work(self):
        old = self.call('/v1/programming', self.solution(request_id='old-pending'))
        self.runner.ready = lambda: dict(available=True, languages=['cpp20', 'python3'], toolchainId='new-runtime')
        ctx = dict(CONTEXT, candidateId='new-candidate')
        self.call('/v1/sessions', dict(ctx, programmingVersion=1, sessionMode='exam',
                                      gradingDigestHex=self.grading['gradingDigestHex']))
        self.call('/v1/begin', dict(ctx, programmingVersion=1, requestId='new-begin', durationSeconds=600))
        current = self.call('/v1/programming', dict(self.solution(request_id='new-pending'), **ctx))
        self.assertTrue(self.app.judge_one())
        self.assertEqual(self.app.private_score(ctx)['score'], 100)
        self.assertEqual(self.app.db.execute('SELECT judge_state FROM submissions WHERE receipt=?',
                                            (old['receiptId'],)).fetchone()[0], 'pending')
        self.assertEqual(self.app.db.execute('SELECT judge_state FROM submissions WHERE receipt=?',
                                            (current['receiptId'],)).fetchone()[0], 'judged')
        self.assertEqual(self.call('/v1/programming', self.solution(request_id='old-pending')), old)

    def test_private_worker_cancellation_retries_without_contest_penalty(self):
        ctx = dict(CONTEXT, sessionIdHex='11' * 16, packageDigestHex='22' * 32)
        manifest = copy.deepcopy(MANIFEST)
        manifest['sessionMode'] = 'contest'
        self.call('/v1/packages', dict(programmingVersion=1, packageId='cs-test',
                                      packageDigestHex=ctx['packageDigestHex'], manifest=manifest))
        grading = copy.deepcopy(self.grading)
        grading['packageDigestHex'] = ctx['packageDigestHex']
        grading.pop('gradingDigestHex')
        grading['gradingDigestHex'] = hashlib.sha256(broker.canonical_json(grading)).hexdigest()
        self.call('/v1/grading', grading)
        self.call('/v1/sessions', dict(ctx, programmingVersion=1, sessionMode='contest',
                                      gradingDigestHex=grading['gradingDigestHex']))
        self.call('/v1/begin', dict(ctx, programmingVersion=1, requestId='contest-begin', durationSeconds=600))
        receipt = self.call('/v1/programming', dict(self.solution(request_id='cancelled-private'), **ctx))
        execute = self.runner.execute
        self.runner.execute = lambda *args: dict(status='cancelled', stdout=b'', stderr=b'', diagnostics=b'', truncated=False)
        self.assertTrue(self.app.judge_one())
        self.assertEqual(self.app.db.execute('SELECT judge_state FROM submissions WHERE receipt=?',
                                            (receipt['receiptId'],)).fetchone()[0], 'pending')
        self.runner.execute = execute
        self.now += 120000
        self.assertTrue(self.app.judge_one())
        self.assertEqual(self.app.private_score(ctx)['penaltyMinutes'], 0)
        self.assertEqual(self.app.private_score(ctx)['solved'], 1)
        self.assertEqual(self.call('/v1/programming', dict(self.solution(request_id='cancelled-private'), **ctx)), receipt)
        self.app.db.execute("UPDATE submissions SET outcome='cancelled' WHERE receipt=?", (receipt['receiptId'],))
        self.call('/v1/programming', dict(self.solution(request_id='after-legacy-cancel', revision=2), **ctx))
        self.assertTrue(self.app.judge_one())
        self.assertEqual(self.app.private_score(ctx)['penaltyMinutes'], 2)
        self.assertEqual(self.app.private_score(ctx)['solved'], 1)

    def test_database_initialization_failure_releases_lease_and_reports_unavailable(self):
        failed = Mock()
        failed.execute.side_effect = sqlite3.OperationalError('disk unavailable')
        path = Path(self.tmp.name) / 'failed.sqlite3'
        with patch.object(broker.sqlite3, 'connect', return_value=failed):
            with self.assertRaises(broker.BrokerError) as error:
                broker.Broker(path, TOKEN, self.runner)
        self.assertEqual(error.exception.code, 'storage_unavailable')
        failed.close.assert_called_once()
        recovered = broker.Broker(path, TOKEN, self.runner)
        recovered.close()

    def test_clock_rollback_recovery_retains_newest_source_without_promoting_eligibility(self):
        self.call('/v1/programming', self.solution())
        self.now -= 1
        late = self.call('/v1/programming', self.solution(request_id='newest-late', source='print(4)', revision=2))
        recovered = self.call('/v1/programming', self.request('recover', problemId='sum', problemRevision=1, language='python3'))
        self.assertEqual(base64.b64decode(recovered['sourceBase64']), b'print(4)')
        self.assertEqual(recovered['latestReceiptId'], late['receiptId'])
        self.assertEqual(late['status'], 'late')
        self.assertEqual(self.app.private_score(CONTEXT)['eligibleSubmissions'], 1)

    def test_context_validation_authenticates_and_reads_only_existing_record(self):
        body = dict(CONTEXT, programmingVersion=1)
        with self.assertRaises(broker.BrokerError) as error:
            self.call('/v1/validate-context', body, 'wrong-token')
        self.assertEqual(error.exception.code, 'unauthorized')
        changes = self.app.db.total_changes
        self.app.db.execute('PRAGMA query_only=ON')
        known = self.call('/v1/validate-context', body)
        self.app.db.execute('PRAGMA query_only=OFF')
        self.assertEqual(known, dict(body, status='known', serverTime=self.now))
        self.assertEqual(self.app.db.total_changes, changes)

    def test_foreign_context_validation_returns_minimal_unknown_without_creating_authority(self):
        before = self.app.db.execute('SELECT count(*) FROM sessions').fetchone()[0]
        for patch_value in [dict(candidateId='never-issued'), dict(packageId='never-issued'),
                            dict(packageDigestHex='33' * 32), dict(clientIdHex='44' * 16), dict(sessionIdHex='55' * 16)]:
            body = dict(CONTEXT, programmingVersion=1, **patch_value)
            with self.subTest(patch=patch_value), self.assertRaises(broker.BrokerError) as error:
                self.call('/v1/validate-context', body)
            self.assertEqual(error.exception.status, 404)
            self.assertEqual(error.exception.response, dict(body, status='unknown', serverTime=self.now))
            self.assertEqual(self.app.db.execute('SELECT count(*) FROM sessions').fetchone()[0], before)
        for extra in [dict(operation='recover'), dict(recoveryOnly=True), dict(context=CONTEXT)]:
            with self.subTest(extra=extra), self.assertRaises(broker.BrokerError):
                self.call('/v1/validate-context', dict(CONTEXT, programmingVersion=1, **extra))

    def test_recovery_only_new_source_is_late_even_before_deadline_and_preserves_eligible_score(self):
        original = self.call('/v1/programming', self.solution())
        self.app.judge_one()
        late = self.call('/v1/programming', dict(self.solution(request_id='recovered-new', source='print(4)', revision=2), recoveryOnly=True))
        self.assertEqual(late['status'], 'late')
        self.assertFalse(self.app.judge_one())
        self.assertEqual(self.app.private_score(CONTEXT)['eligibleSubmissions'], 1)
        self.assertEqual(self.app.private_score(CONTEXT)['score'], 100)
        self.assertEqual(self.call('/v1/programming', dict(self.solution(), recoveryOnly=True)), original)
        recovered = self.call('/v1/programming', dict(self.request('recover', problemId='sum', problemRevision=1, language='python3'), recoveryOnly=True))
        self.assertEqual(base64.b64decode(recovered['sourceBase64']), b'print(4)')
        self.assertEqual(recovered['latestReceiptId'], late['receiptId'])

    def test_recovery_only_denies_execution_and_readiness_and_requires_boolean(self):
        operations = [dict(self.solution('run'), sampleId='sample-1'),
                      self.request('cancel', targetRunId='run', problemId='sum', problemRevision=1, language='python3'),
                      self.request('ready')]
        for body in operations:
            with self.subTest(operation=body['operation']), self.assertRaises(broker.BrokerError):
                self.call('/v1/programming', dict(body, recoveryOnly=True))
        self.assertEqual(self.runner.calls, [])
        for value in [1, 'true', None]:
            with self.subTest(value=value), self.assertRaises(broker.BrokerError):
                self.call('/v1/programming', dict(self.solution('draft'), recoveryOnly=value))
        with self.assertRaises(broker.BrokerError):
            self.call('/v1/begin', dict(CONTEXT, programmingVersion=1, requestId='recovery-begin', durationSeconds=600, recoveryOnly=True))

    def test_recovery_only_draft_and_finalize_keep_existing_timer_and_stable_receipt(self):
        original = self.call('/v1/programming', self.solution())
        before = tuple(self.app.db.execute('SELECT start,deadline FROM sessions').fetchone())
        draft = self.call('/v1/programming', dict(self.solution('draft', 'recovery-draft', 'print(9)', 2), recoveryOnly=True))
        self.assertEqual(draft['status'], 'saved')
        body = self.request('finalize', 'recovery-final', receiptIds=[original['receiptId']], noSourceProblemIds=[])
        finalized = self.call('/v1/programming', dict(body, recoveryOnly=True))
        self.assertEqual(finalized['status'], 'finalized')
        self.assertEqual(self.call('/v1/programming', body), finalized)
        self.assertEqual(tuple(self.app.db.execute('SELECT start,deadline FROM sessions').fetchone()), before)

    def test_recovery_context_and_late_source_survive_restart_and_failed_write(self):
        self.app.close()
        self.app = broker.Broker(self.db, TOKEN, self.runner, clock=lambda: self.now)
        self.assertEqual(self.call('/v1/validate-context', dict(CONTEXT, programmingVersion=1))['status'], 'known')
        body = dict(self.solution(request_id='restart-recovery', revision=2), recoveryOnly=True)
        self.app.db.execute('PRAGMA query_only=ON')
        with self.assertRaises(broker.BrokerError) as error:
            self.call('/v1/programming', body)
        self.assertEqual(error.exception.code, 'storage_unavailable')
        self.app.db.execute('PRAGMA query_only=OFF')
        receipt = self.call('/v1/programming', body)
        self.assertEqual(receipt['status'], 'late')
        self.app.close()
        self.app = broker.Broker(self.db, TOKEN, self.runner, clock=lambda: self.now)
        self.assertEqual(self.call('/v1/programming', body), receipt)
        self.assertEqual(self.app.private_score(CONTEXT)['eligibleSubmissions'], 0)
        self.assertFalse(self.app.judge_one())

    def test_recovery_only_does_not_create_unknown_session_or_promote_prebegin_source(self):
        unknown = dict(self.solution(), candidateId='never-issued', recoveryOnly=True)
        count = self.app.db.execute('SELECT count(*) FROM sessions').fetchone()[0]
        with self.assertRaises(broker.BrokerError) as error:
            self.call('/v1/programming', unknown)
        self.assertEqual(error.exception.code, 'unknown_context')
        self.assertEqual(self.app.db.execute('SELECT count(*) FROM sessions').fetchone()[0], count)
        waiting = dict(CONTEXT, candidateId='waiting-candidate')
        self.call('/v1/sessions', dict(waiting, programmingVersion=1, sessionMode='exam', gradingDigestHex=self.grading['gradingDigestHex']))
        draft = dict(self.solution('draft'), **waiting, recoveryOnly=True)
        self.assertEqual(self.call('/v1/programming', draft)['status'], 'saved')
        submitted = dict(self.solution(request_id='prebegin-recovery'), **waiting, recoveryOnly=True)
        receipt = self.call('/v1/programming', submitted)
        self.assertEqual(receipt['status'], 'late')
        self.call('/v1/begin', dict(waiting, programmingVersion=1, requestId='begin-waiting', durationSeconds=600))
        self.assertEqual(self.call('/v1/programming', submitted), receipt)
        self.assertEqual(self.call('/v1/programming', {k: v for k, v in submitted.items() if k != 'recoveryOnly'}), receipt)
        self.assertEqual(self.app.private_score(waiting)['eligibleSubmissions'], 0)

    def test_private_fields_in_public_package_and_wrong_grading_digest_are_rejected(self):
        public = dict(programmingVersion=1, packageId='cs-test', packageDigestHex='33' * 32,
                      manifest=copy.deepcopy(MANIFEST))
        public['manifest']['questions'][0]['privateCases'] = [{'input': 'secret'}]
        with self.assertRaises(broker.BrokerError):
            self.call('/v1/packages', public)
        bad = copy.deepcopy(self.grading)
        bad['gradingDigestHex'] = '00' * 32
        with self.assertRaises(broker.BrokerError):
            self.call('/v1/grading', bad)

    def test_duplicate_json_fields_and_nonfinite_numbers_fail_closed(self):
        for body in [b'{"source":"first","source":"second"}', b'{"x":NaN}', b'{"x":Infinity}']:
            with self.assertRaises(broker.BrokerError):
                broker.decode_json(body)


if __name__ == '__main__':
    unittest.main()
