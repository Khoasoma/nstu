"""Room authority tests; FixtureRunner never represents a qualified Linux judge."""
import copy
import hashlib
import json
import shutil
import threading
import unittest
import uuid
from pathlib import Path

import broker
from test_broker import TOKEN, CONTEXT, MANIFEST


class FixtureRunner:
    def __init__(self):
        self.config = {'fixtureRuntime': 'one'}
        self.available = True
        self.on_ready = None

    def ready(self):
        if self.on_ready:
            self.on_ready()
        return dict(available=self.available, languages=broker.LANGUAGES)


class ContestRoomsTests(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[2]
        self.tmp = root / ('room-test-' + uuid.uuid4().hex)
        self.tmp.mkdir()
        self.addCleanup(self.cleanup_directory)
        self.now = 1_800_000_000_000
        self.runner = FixtureRunner()
        self.database = self.tmp / 'broker.sqlite3'
        self.app = broker.Broker(self.database, TOKEN, self.runner, clock=lambda: self.now,
                                 owner_id='teacher-one')
        self.addCleanup(lambda: self.app.close())
        manifest = dict(copy.deepcopy(MANIFEST), sessionMode='contest')
        self.call('/v1/packages', dict(programmingVersion=1, packageId=CONTEXT['packageId'],
                  packageDigestHex=CONTEXT['packageDigestHex'], manifest=manifest))
        grading = dict(programmingVersion=1, packageId=CONTEXT['packageId'],
                       packageDigestHex=CONTEXT['packageDigestHex'],
                       problems=[dict(id='sum', revision=1, cases=[dict(input='PRIVATE', output='3')])])
        self.grading = hashlib.sha256(broker.canonical_json(grading)).hexdigest()
        self.call('/v1/grading', dict(grading, gradingDigestHex=self.grading))
        self.contexts = [dict(CONTEXT), dict(CONTEXT, clientIdHex='12' * 16,
                                           sessionIdHex='34' * 16, candidateId='student-2')]
        for ctx in self.contexts:
            self.call('/v1/sessions', dict(ctx, programmingVersion=1, sessionMode='contest',
                                         gradingDigestHex=self.grading))
        self.base = dict(programmingVersion=1, roomId='11' * 16, roundId='22' * 16)
        self.create = dict(self.base, packageId=CONTEXT['packageId'],
                           packageDigestHex=CONTEXT['packageDigestHex'], gradingDigestHex=self.grading,
                           publicationPolicy='teacher_only')
        self.room('create', self.create)

    def cleanup_directory(self):
        root = Path(__file__).resolve().parents[2]
        target = self.tmp.resolve()
        if target.parent != root or not target.name.startswith('room-test-'):
            raise RuntimeError('unsafe fixture cleanup')
        shutil.rmtree(target)

    def call(self, path, body):
        return self.app.dispatch(path, copy.deepcopy(body), TOKEN)

    def room(self, route, body=None, **extra):
        return self.call('/v1/contest-rooms/' + route, body or dict(self.base, **extra))

    def state(self):
        return self.room('state')

    def member(self, action, index=0, **extra):
        body = dict(self.base, action=action, expectedRevision=self.state()['revision'],
                    context=self.contexts[index], **extra)
        if action == 'approve':
            body['displayName'] = 'Student ' + str(index + 1)
        else:
            body['memberVersion'] = next(m['memberVersion'] for m in self.state()['members']
                                         if m['context'] == self.contexts[index])
        if action == 'prepare':
            body['packageDigestHex'] = CONTEXT['packageDigestHex']
        return self.room('members', body)

    def prepare(self):
        for i in range(2):
            self.member('approve', i)
            self.member('prepare', i)

    def begin(self, request_id='begin-one'):
        return self.room('begin', requestId=request_id, expectedRevision=self.state()['revision'])

    def programming(self, operation, index=0, **extra):
        return self.call('/v1/programming', dict(self.contexts[index], programmingVersion=1,
                         operation=operation, requestId='source-one', **extra))

    def assert_error(self, code, callback):
        with self.assertRaises(broker.BrokerError) as error:
            callback()
        self.assertEqual(error.exception.code, code)

    def test_common_clock_begin_replay_restart_and_read_only_reconciliation(self):
        self.prepare()
        before = self.state()
        body = dict(self.base, requestId='begin-one', expectedRevision=before['revision'])
        first = self.room('begin', body)
        self.assertEqual(first['state'], 'Running')
        self.assertEqual(first['deadlineUnixMilliseconds'], self.now + 600_000)
        times = self.app.db.execute('SELECT start,deadline FROM sessions').fetchall()
        self.assertEqual({tuple(row) for row in times}, {(self.now, self.now + 600_000)})
        self.now += 5000
        self.assertEqual(first, self.room('begin', body))
        self.app.close()
        self.app = broker.Broker(self.database, TOKEN, self.runner, clock=lambda: self.now,
                                 owner_id='teacher-one')
        self.assertEqual(first, self.room('begin', body))
        changes = self.app.db.total_changes
        restored = self.state()
        self.assertEqual(restored['ownerId'], 'teacher-one')
        self.assertEqual(len(restored['members']), 2)
        self.assertEqual(restored['runtimeIdentity'], broker.config_identity(self.runner.config))
        self.assertEqual(self.app.db.total_changes, changes)

    def test_unready_or_unavailable_begin_has_no_partial_start(self):
        self.member('approve')
        self.assert_error('members_unprepared', self.begin)
        self.member('prepare')
        self.runner.available = False
        self.assert_error('runner_unavailable', self.begin)
        self.assertEqual(self.state()['serverStartUnixMilliseconds'], 0)
        self.assertEqual(self.app.db.execute('SELECT sum(start) FROM sessions').fetchone()[0], 0)

    def test_owner_schema_version_and_immutable_pins(self):
        self.assert_error('invalid_fields', lambda: self.room('create', dict(self.create, ownerId='other')))
        self.assert_error('unsupported_version', lambda: self.room('state', dict(self.base, programmingVersion=True)))
        self.assert_error('conflict', lambda: self.room('create', dict(self.create, publicationPolicy='live')))
        self.assert_error('invalid_fields', lambda: self.room('state', dict(self.base, context=self.contexts[0])))
        self.app.close()
        self.assert_error('owner_mismatch', lambda: broker.Broker(self.database, TOKEN, self.runner,
                                                                owner_id='teacher-two'))

    def test_foreign_tuple_pins_duplicates_and_membership_versions_rejected(self):
        foreign = dict(self.contexts[0], candidateId='forged')
        self.assert_error('unknown_context', lambda: self.room('members', action='approve',
                          expectedRevision=1, context=foreign, displayName='Forged'))
        state = self.member('approve')
        self.assert_error('stale_revision', lambda: self.room('members', action='approve',
                          expectedRevision=1, context=self.contexts[1], displayName='Second'))
        self.assert_error('stale_member', lambda: self.room('members', action='prepare',
                          expectedRevision=state['revision'], memberVersion=9, context=self.contexts[0],
                          packageDigestHex=CONTEXT['packageDigestHex']))
        self.assert_error('package_mismatch', lambda: self.room('members', action='prepare',
                          expectedRevision=state['revision'], memberVersion=1, context=self.contexts[0],
                          packageDigestHex='ff' * 32))

    def test_generic_lifecycle_and_removed_tuple_cannot_bypass_authority(self):
        self.member('approve')
        for path, extra in [('/v1/begin', dict(durationSeconds=600)), ('/v1/stop', {})]:
            self.assert_error('room_authority_required', lambda p=path, e=extra: self.call(p,
                              dict(self.contexts[0], programmingVersion=1, requestId='bypass', **e)))
        self.member('remove')
        self.assert_error('room_authority_required', lambda: self.call('/v1/begin', dict(
                          self.contexts[0], programmingVersion=1, requestId='removed', durationSeconds=600)))
        self.assert_error('member_revoked', lambda: self.programming('ready'))

    def test_post_begin_membership_and_closed_recovery_never_grant_run_or_ready(self):
        self.prepare()
        self.begin()
        self.assert_error('room_closed', lambda: self.member('remove'))
        submit = dict(problemId='sum', problemRevision=1, language='python3', source='print(3)', draftRevision=1)
        first = self.programming('submit', **submit)
        stop_body = dict(self.base, requestId='stop-one', expectedRevision=self.state()['revision'])
        stopped = self.room('stop', stop_body)
        self.assertEqual(stopped['state'], 'Closed')
        self.assertEqual(stopped, self.room('stop', stop_body))
        self.assertEqual(first, self.programming('submit', **submit))
        late = self.call('/v1/programming', dict(self.contexts[0], programmingVersion=1,
                         operation='submit', requestId='late', **dict(submit, draftRevision=2)))
        self.assertEqual(late['status'], 'late')
        self.assert_error('recovery_only', lambda: self.programming('ready'))
        self.assert_error('recovery_only', lambda: self.programming('run', sampleId='sample-1', **submit))
        self.assert_error('room_authority_required', lambda: self.call('/v1/sessions', dict(
                          self.contexts[0], programmingVersion=1, sessionMode='contest', gradingDigestHex=self.grading)))

    def test_deadline_and_clock_regression_close_room_authority(self):
        self.prepare()
        self.begin()
        self.now += 600_000
        self.assertEqual(self.state()['state'], 'Closed')
        self.assert_error('recovery_only', lambda: self.programming('ready'))
        self.assertEqual(self.state()['serverStartUnixMilliseconds'], self.now - 600_000)

    def test_observed_expiry_survives_restart_with_rolled_back_clock(self):
        self.prepare()
        begun = self.begin()
        start = begun['serverStartUnixMilliseconds']
        self.now = begun['deadlineUnixMilliseconds']
        self.assertEqual(self.state()['state'], 'Closed')
        self.assert_error('recovery_only', lambda: self.programming('ready'))
        self.app.close()
        self.now = start + 60_000
        self.app = broker.Broker(self.database, TOKEN, self.runner, clock=lambda: self.now,
                                 owner_id='teacher-one')
        self.assertEqual(self.state()['state'], 'Closed')
        self.assert_error('recovery_only', lambda: self.programming('ready'))
        result = self.programming('submit', problemId='sum', problemRevision=1, language='python3',
                                  source='print(3)', draftRevision=1)
        self.assertEqual(result['status'], 'late')
        self.assertEqual(self.app.db.execute('SELECT eligible FROM submissions').fetchone()[0], 0)

    def test_predeadline_admitted_source_survives_later_expiry_checkpoint(self):
        self.prepare()
        begun = self.begin()
        self.now = begun['deadlineUnixMilliseconds'] - 1
        admission = self.app.capture_admission()
        body = dict(self.contexts[0], programmingVersion=1, operation='submit', requestId='admitted-early',
                    problemId='sum', problemRevision=1, language='python3', source='print(3)', draftRevision=1)
        self.now += 2
        self.assertEqual(self.state()['state'], 'Closed')
        fresh = self.call('/v1/programming', dict(body, requestId='fresh-after-expiry'))
        self.assertEqual(fresh['status'], 'late')
        result = self.app.dispatch('/v1/programming', body, TOKEN, admission)
        self.assertEqual(result['status'], 'received')
        self.assertEqual(result['serverTime'], admission[0])
        self.assertEqual(self.app.db.execute('SELECT eligible FROM submissions WHERE request=?',
                         ('admitted-early',)).fetchone()[0], 1)
        self.assertEqual(self.state()['state'], 'Closed')
        self.assert_error('recovery_only', lambda: self.programming('ready'))
        recovery = self.call('/v1/programming', dict(body, requestId='recovery-after-expiry', recoveryOnly=True))
        self.assertEqual(recovery['status'], 'late')

    def test_explicit_stop_does_not_preserve_unreceipted_admission_eligibility(self):
        self.prepare()
        self.begin()
        self.now += 1
        admission = self.app.capture_admission()
        self.now += 1
        self.room('stop', requestId='explicit-stop', expectedRevision=self.state()['revision'])
        body = dict(self.contexts[0], programmingVersion=1, operation='submit', requestId='before-stop',
                    problemId='sum', problemRevision=1, language='python3', source='print(3)', draftRevision=1)
        self.assertEqual(self.app.dispatch('/v1/programming', body, TOKEN, admission)['status'], 'late')

    def test_explicit_finalize_does_not_preserve_unreceipted_admission_eligibility(self):
        self.prepare()
        self.begin()
        self.now += 1
        admission = self.app.capture_admission()
        self.now += 1
        self.programming('finalize', receiptIds=[], noSourceProblemIds=['sum'])
        body = dict(self.contexts[0], programmingVersion=1, operation='submit', requestId='before-finalize',
                    problemId='sum', problemRevision=1, language='python3', source='print(3)', draftRevision=1)
        self.assertEqual(self.app.dispatch('/v1/programming', body, TOKEN, admission)['status'], 'late')

    def test_authenticated_clock_checkpoint_precedes_read_only_projection(self):
        self.prepare()
        begun = self.begin()
        membership = self.state()['members']
        self.now += 10_000
        self.app.dispatch('/v1/contest-rooms/state', self.base, TOKEN)
        changes = self.app.db.total_changes
        projected = self.state()
        self.assertEqual(projected['members'], membership)
        self.assertEqual(projected['serverStartUnixMilliseconds'], begun['serverStartUnixMilliseconds'])
        self.assertEqual(self.app.db.total_changes, changes)
        self.now += 10_000
        with self.assertRaises(broker.BrokerError):
            self.app.dispatch('/v1/contest-rooms/state', self.base, 'wrong-token')
        self.assertEqual(self.app.db.total_changes, changes)

    def test_remove_unready_member_restores_prepared_with_unlocked_probe(self):
        self.member('approve', 0)
        self.member('prepare', 0)
        self.member('approve', 1)
        self.assertEqual(self.state()['state'], 'Joining')
        def prove_unlocked():
            obtained = threading.Event()
            def observe():
                with self.app.lock:
                    obtained.set()
            thread = threading.Thread(target=observe)
            thread.start()
            thread.join(2)
            self.assertTrue(obtained.is_set(), 'remove readiness held broker lock')
        self.runner.on_ready = prove_unlocked
        self.assertEqual(self.member('remove', 1)['state'], 'Prepared')

    def test_display_name_limit_is_sixty_four_utf8_bytes(self):
        self.room('members', action='approve', expectedRevision=1, context=self.contexts[0],
                  displayName='ế' * 21 + 'x')
        self.assert_error('text_bounds', lambda: self.room('members', action='approve',
                          expectedRevision=self.state()['revision'], context=self.contexts[1],
                          displayName='ế' * 21 + 'xx'))

    def test_runtime_probe_holds_no_database_lock_and_change_is_revalidated(self):
        self.prepare()
        def mutate_runtime():
            obtained = threading.Event()
            def observe():
                with self.app.lock:
                    obtained.set()
            thread = threading.Thread(target=observe)
            thread.start()
            thread.join(2)
            self.assertTrue(obtained.is_set(), 'ready probe held broker lock')
            self.runner.config = {'fixtureRuntime': 'changed'}
        self.runner.on_ready = mutate_runtime
        self.assert_error('runtime_mismatch', self.begin)
        self.assertEqual(self.state()['serverStartUnixMilliseconds'], 0)

    def test_old_unlinked_session_lifecycle_stays_compatible(self):
        result = self.call('/v1/begin', dict(self.contexts[1], programmingVersion=1,
                           requestId='old-begin', durationSeconds=600))
        self.assertEqual(result['status'], 'begun')
        self.assertEqual(self.programming('ready', 1)['status'], 'ready')

    def test_runtime_configuration_removal_during_probe_cannot_begin(self):
        self.prepare()
        self.runner.on_ready = lambda: setattr(self.runner, 'config', None)
        self.assert_error('runtime_mismatch', self.begin)
        self.assertEqual(self.state()['serverStartUnixMilliseconds'], 0)

    def test_membership_change_during_readiness_revalidates_revision(self):
        self.prepare()
        def remove_during_probe():
            self.runner.on_ready = None
            self.member('remove', 1)
        self.runner.on_ready = remove_during_probe
        self.assert_error('stale_revision', self.begin)
        self.assertEqual(self.state()['serverStartUnixMilliseconds'], 0)

    def test_storage_failure_rolls_back_entire_shared_begin(self):
        self.prepare()
        key = broker.session_key(self.contexts[1])
        self.app.db.execute("CREATE TEMP TRIGGER fail_second BEFORE UPDATE OF start ON sessions "
                            "WHEN NEW.key='" + key + "' BEGIN SELECT RAISE(ABORT,'fixture disk failure'); END")
        self.assert_error('storage_unavailable', self.begin)
        self.assertEqual(self.state()['serverStartUnixMilliseconds'], 0)
        self.assertEqual({tuple(row) for row in self.app.db.execute('SELECT start,deadline FROM sessions')}, {(0, 0)})

    def test_sixty_four_member_cap_and_utf8_display_name_limit(self):
        self.assert_error('text_bounds', lambda: self.room('members', action='approve',
                          expectedRevision=1, context=self.contexts[0], displayName='ế' * 43))
        cohort = []
        for index in range(65):
            ctx = dict(CONTEXT, candidateId='cohort-' + str(index),
                       clientIdHex=format(index + 100, '032x'), sessionIdHex=format(index + 1000, '032x'))
            cohort.append(ctx)
            self.call('/v1/sessions', dict(ctx, programmingVersion=1, sessionMode='contest', gradingDigestHex=self.grading))
            body = dict(self.base, action='approve', expectedRevision=self.state()['revision'],
                        context=ctx, displayName='Member ' + str(index))
            if index == 64:
                self.assert_error('member_bounds', lambda: self.room('members', body))
            else:
                self.room('members', body)
        self.assertEqual(len(self.state()['members']), 64)
        self.room('members', action='remove', expectedRevision=self.state()['revision'],
                  context=cohort[0], memberVersion=1)
        self.room('members', action='approve', expectedRevision=self.state()['revision'],
                  context=cohort[64], displayName='Replacement')
        self.assertEqual(len(self.state()['members']), 64)
        self.assertTrue(all(m['approved'] for m in self.state()['members']))
        self.assert_error('room_authority_required', lambda: self.call('/v1/begin', dict(
                          cohort[0], programmingVersion=1, requestId='removed-at-cap', durationSeconds=600)))

    def test_historical_cap_preserves_tombstones_and_current_recovery(self):
        history = []
        for index in range(257):
            ctx = dict(CONTEXT, candidateId='history-' + str(index),
                       clientIdHex=format(index + 1000, '032x'), sessionIdHex=format(index + 2000, '032x'))
            history.append(ctx)
            self.call('/v1/sessions', dict(ctx, programmingVersion=1, sessionMode='contest', gradingDigestHex=self.grading))
            body = dict(self.base, action='approve', expectedRevision=self.state()['revision'],
                        context=ctx, displayName='Member ' + str(index))
            if index == 256:
                self.assert_error('member_history_bounds', lambda: self.room('members', body))
            else:
                self.room('members', body)
                if index < 254:
                    self.room('members', action='remove', expectedRevision=self.state()['revision'],
                              context=ctx, memberVersion=1)
        self.assertEqual(self.app.db.execute('SELECT count(*) FROM contest_members').fetchone()[0], 256)
        self.room('members', action='remove', expectedRevision=self.state()['revision'],
                  context=history[255], memberVersion=1)
        self.room('stop', requestId='cap-stop', expectedRevision=self.state()['revision'])
        self.contexts[0] = history[254]
        result = self.programming('recover', problemId='sum', problemRevision=1, language='python3')
        self.assertEqual(result['status'], 'recovered')
        self.assertEqual(len(self.state()['members']), 1)

    def test_clock_regression_survives_restart_and_never_starts_room(self):
        self.prepare()
        self.app.close()
        self.now -= 1
        self.app = broker.Broker(self.database, TOKEN, self.runner, clock=lambda: self.now,
                                 owner_id='teacher-one')
        self.assert_error('room_closed', self.begin)
        self.assertEqual(self.state()['serverStartUnixMilliseconds'], 0)

    def test_room_stop_and_begin_ids_reject_changed_retries(self):
        self.prepare()
        before = self.state()['revision']
        self.begin()
        self.assert_error('conflict', lambda: self.room('begin', requestId='changed', expectedRevision=before))
        revision = self.state()['revision']
        self.room('stop', requestId='stop-one', expectedRevision=revision)
        self.assert_error('conflict', lambda: self.room('stop', requestId='other-stop', expectedRevision=revision))

    def test_finish_only_closes_room_after_all_members_finish(self):
        self.prepare()
        self.begin()
        self.programming('finalize', receiptIds=[], noSourceProblemIds=['sum'])
        self.assertEqual(self.state()['state'], 'Running')
        self.assert_error('recovery_only', lambda: self.programming('ready'))
        self.programming('finalize', 1, receiptIds=[], noSourceProblemIds=['sum'])
        self.assertEqual(self.state()['state'], 'Closed')

    def test_authentication_precedes_room_reads_and_no_owner_is_unavailable(self):
        with self.assertRaises(broker.BrokerError) as error:
            self.app.dispatch('/v1/contest-rooms/state', self.base, 'wrong-token')
        self.assertEqual(error.exception.code, 'unauthorized')
        other = broker.Broker(self.tmp / 'unconfigured.sqlite3', TOKEN)
        try:
            with self.assertRaises(broker.BrokerError) as error:
                other.dispatch('/v1/contest-rooms/state', self.base, TOKEN)
            self.assertEqual(error.exception.code, 'room_owner_unconfigured')
        finally:
            other.close()

    def test_unconfigured_real_runner_cannot_start_a_room(self):
        self.room('stop', requestId='discard-draft', expectedRevision=1)
        self.app.runner = broker.IsolateRunner(None)
        self.base = dict(self.base, roomId='55' * 16, roundId='66' * 16)
        self.room('create', dict(self.create, **self.base))
        self.contexts[0] = dict(CONTEXT, candidateId='missing-judge', clientIdHex='77' * 16,
                                sessionIdHex='88' * 16)
        self.call('/v1/sessions', dict(self.contexts[0], programmingVersion=1,
                   sessionMode='contest', gradingDigestHex=self.grading))
        self.member('approve')
        self.member('prepare')
        self.assertEqual(self.state()['state'], 'Joining')
        self.assert_error('runner_unavailable', self.begin)
        self.assertEqual(self.state()['serverStartUnixMilliseconds'], 0)

    def standings_cohort(self, count=3, policy='teacher_only'):
        """Populate a score fixture; no source is executed or claimed to be judged."""
        self.room('stop', requestId='discard-for-standings', expectedRevision=1)
        digest = '91' * 32
        manifest = dict(copy.deepcopy(MANIFEST), id='standings', sessionMode='contest')
        manifest['questions'] += [dict(copy.deepcopy(manifest['questions'][0]), id='second')]
        self.call('/v1/packages', dict(programmingVersion=1, packageId='standings',
                  packageDigestHex=digest, manifest=manifest))
        grading = dict(programmingVersion=1, packageId='standings', packageDigestHex=digest,
                       problems=[dict(id=p['id'], revision=1, cases=[dict(
                           input='PRIVATE-CASE-CANARY', output='PRIVATE-OUTPUT-CANARY')])
                           for p in manifest['questions']])
        self.grading = hashlib.sha256(broker.canonical_json(grading)).hexdigest()
        self.call('/v1/grading', dict(grading, gradingDigestHex=self.grading))
        self.base = dict(programmingVersion=1, roomId='92' * 16, roundId='93' * 16)
        self.create = dict(self.base, packageId='standings', packageDigestHex=digest,
                           gradingDigestHex=self.grading, publicationPolicy=policy)
        self.room('create', self.create)
        self.contexts = []
        for index in range(count):
            ctx = dict(CONTEXT, packageId='standings', packageDigestHex=digest,
                       candidateId='identity-canary-' + str(index),
                       clientIdHex=format(index + 500, '032x'), sessionIdHex=format(index + 600, '032x'))
            self.contexts.append(ctx)
            self.call('/v1/sessions', dict(ctx, programmingVersion=1, sessionMode='contest',
                                         gradingDigestHex=self.grading))
            self.member('approve', index)
            self.room('members', action='prepare', expectedRevision=self.state()['revision'],
                      context=ctx, memberVersion=1, packageDigestHex=digest)
        self.begin()
        self.started = self.now

    def score_fixture(self, index, minute, outcome, problem='sum', judge_state='judged'):
        self.now = self.started + minute * 60000
        receipt = self.call('/v1/programming', dict(self.contexts[index], programmingVersion=1,
                    operation='submit', requestId='score-' + uuid.uuid4().hex,
                    problemId=problem, problemRevision=1, language='python3',
                    source='SOURCE-CANARY', draftRevision=1))
        with self.app.lock, self.app.db:
            self.app.db.execute('UPDATE submissions SET judge_state=?,outcome=? WHERE receipt=?',
                                (judge_state, outcome, receipt['receiptId']))
        return receipt

    def test_standings_scoring_competition_ranks_and_exact_secret_free_dto(self):
        self.standings_cohort(count=4)
        self.score_fixture(0, 0, 'compile_error')
        self.score_fixture(0, 0, 'judge_error')
        self.score_fixture(0, 0, 'wrong_answer')
        self.score_fixture(0, 1, 'accepted')
        self.score_fixture(1, 2, 'accepted')
        self.score_fixture(2, 2, 'accepted')
        self.score_fixture(0, 3, 'accepted', problem='second')
        # A retried infrastructure failure no longer makes judging unavailable.
        with self.app.lock, self.app.db:
            self.app.db.execute("UPDATE submissions SET outcome='compile_error' WHERE outcome='judge_error'")
        def refuse_submission_payload(action, table, column, *_):
            if action == broker.sqlite3.SQLITE_READ and table == 'submissions' and column in {'source', 'response'}:
                return broker.sqlite3.SQLITE_DENY
            return broker.sqlite3.SQLITE_OK
        self.app.db.set_authorizer(refuse_submission_payload)
        try:
            result = self.room('standings')
        finally:
            self.app.db.set_authorizer(None)
        self.assertEqual(set(result), {'roomId', 'roundId', 'revision', 'generatedTime', 'freshness', 'rows'})
        self.assertEqual((result['roomId'], result['roundId']), (self.base['roomId'], self.base['roundId']))
        self.assertEqual(result['freshness'], 'current')
        self.assertEqual([(r['rank'], r['solved'], r['penaltyMinutes']) for r in result['rows']],
                         [(1, 2, 24), (2, 1, 2), (2, 1, 2), (4, 0, 0)])
        tied = sorted(self.contexts[1:3], key=broker.session_key)
        self.assertEqual([r['displayName'] for r in result['rows'][1:3]],
                         ['Student ' + str(self.contexts.index(ctx) + 1) for ctx in tied])
        for row in result['rows']:
            self.assertEqual(set(row), {'rank', 'displayName', 'solved', 'penaltyMinutes'})
        wire = json.dumps(result)
        for canary in ['PRIVATE-CASE-CANARY', 'PRIVATE-OUTPUT-CANARY', 'SOURCE-CANARY', TOKEN,
                       'identity-canary', self.grading, 'queuePosition', 'judge_state', 'outcome']:
            self.assertNotIn(canary, wire)
        for ctx in self.contexts:
            for field in broker.CONTEXT:
                self.assertNotIn(ctx[field], wire)

    def test_standings_pending_blocks_later_acceptance_and_marks_provisional(self):
        self.standings_cohort()
        pending = self.score_fixture(0, 0, None, judge_state='pending')
        self.score_fixture(0, 1, 'accepted')
        result = self.room('standings')
        self.assertEqual(result['freshness'], 'pending')
        self.assertEqual({r['solved'] for r in result['rows']}, {0})
        with self.app.lock, self.app.db:
            self.app.db.execute("UPDATE submissions SET judge_state='judged',outcome='wrong_answer' WHERE receipt=?",
                                (pending['receiptId'],))
        current = self.room('standings')
        self.assertEqual(current['freshness'], 'current')
        self.assertEqual((current['rows'][0]['solved'], current['rows'][0]['penaltyMinutes']), (1, 21))
        self.assertGreater(current['revision'], result['revision'])

    def test_standings_infrastructure_error_and_missing_judge_preserve_last_known(self):
        self.standings_cohort()
        self.score_fixture(0, 1, 'accepted')
        last = self.room('standings')
        self.score_fixture(1, 2, 'judge_error')
        unavailable = self.room('standings')
        self.assertEqual(unavailable['freshness'], 'unavailable')
        self.assertEqual(unavailable['rows'], last['rows'])
        self.assertEqual(unavailable['generatedTime'], last['generatedTime'])
        self.app.runner = broker.IsolateRunner(None)
        self.assertEqual(self.room('standings'), unavailable)

    def test_standings_unavailable_initially_never_invents_accepted_results(self):
        self.standings_cohort()
        self.score_fixture(0, 1, 'accepted')
        self.app.runner = None
        result = self.room('standings')
        self.assertEqual(result['freshness'], 'unavailable')
        self.assertEqual({r['solved'] for r in result['rows']}, {0})

    def test_standings_revision_survives_restart_and_reordered_admission(self):
        self.standings_cohort()
        first = self.room('standings')
        self.assertIs(type(first['revision']), int)
        self.assertGreaterEqual(first['revision'], 1)
        old_admission = self.app.capture_admission()
        self.score_fixture(0, 1, 'accepted')
        second = self.room('standings')
        self.assertGreater(second['revision'], first['revision'])
        reordered = self.app.dispatch('/v1/contest-rooms/standings', self.base, TOKEN, old_admission)
        self.assertEqual(reordered, second)
        self.now += 5000
        self.assertEqual(self.room('standings')['revision'], second['revision'])
        self.app.close()
        self.app = broker.Broker(self.database, TOKEN, self.runner, clock=lambda: self.now, owner_id='teacher-one')
        restored = self.room('standings')
        self.assertEqual(restored['revision'], second['revision'])
        self.assertEqual(restored['rows'], second['rows'])

    def test_standings_probe_is_unlocked_and_runtime_change_is_revalidated(self):
        self.standings_cohort()
        last = self.room('standings')
        self.score_fixture(0, 1, 'accepted')
        def change_runtime():
            obtained = threading.Event()
            def observe():
                with self.app.lock:
                    obtained.set()
            thread = threading.Thread(target=observe)
            thread.start()
            thread.join(2)
            self.assertTrue(obtained.is_set(), 'standings readiness held broker lock')
            self.runner.config = {'fixtureRuntime': 'changed'}
        self.runner.on_ready = change_runtime
        result = self.room('standings')
        self.assertEqual(result['freshness'], 'unavailable')
        self.assertEqual(result['rows'], last['rows'])

    def test_standings_strict_owner_room_version_and_publication_policy(self):
        self.standings_cohort(policy='live')
        self.assertEqual(self.state()['publicationPolicy'], 'live')
        self.assertEqual(self.room('standings')['freshness'], 'current')
        self.assert_error('conflict', lambda: self.room('create', dict(self.create, publicationPolicy='teacher_only')))
        for field in ['ownerId', 'publicationPolicy', 'context', 'requestId', 'privateScore', 'rows']:
            self.assert_error('invalid_fields', lambda field=field: self.room('standings', dict(self.base, **{field: 'forged'})))
        self.assert_error('unsupported_version', lambda: self.room('standings', dict(self.base, programmingVersion=True)))
        for field in ['roomId', 'roundId']:
            self.assert_error('unknown_room', lambda field=field: self.room('standings', dict(self.base, **{field: '99' * 16})))
            with self.assertRaises(broker.BrokerError):
                self.room('standings', dict(self.base, **{field: 'x' * 32769}))
        with self.assertRaises(broker.BrokerError) as error:
            self.app.dispatch('/v1/contest-rooms/standings', self.base, 'wrong-token')
        self.assertEqual(error.exception.code, 'unauthorized')
        unconfigured = broker.Broker(self.tmp / 'no-owner.sqlite3', TOKEN)
        try:
            self.assert_error('room_owner_unconfigured', lambda: unconfigured.dispatch('/v1/contest-rooms/standings', self.base, TOKEN))
        finally:
            unconfigured.close()
        self.app.close()
        self.assert_error('owner_mismatch', lambda: broker.Broker(self.database, TOKEN, self.runner, owner_id='teacher-two'))

    def test_standings_current_members_only_and_full_sixty_four_row_bound(self):
        self.standings_cohort(count=64)
        result = self.room('standings')
        self.assertEqual(len(result['rows']), 64)
        self.assertEqual({r['rank'] for r in result['rows']}, {1})
        self.assertLess(len(broker.canonical_json(result)), broker.MAX_BODY)
        # A closed previous room and its old sessions cannot enter this projection.
        old = self.call('/v1/contest-rooms/standings', dict(programmingVersion=1,
                        roomId='11' * 16, roundId='22' * 16))
        self.assertEqual(old['rows'], [])

    def test_standings_does_not_expand_sealed_programming_replies(self):
        self.standings_cohort(policy='teacher_only')
        receipt = self.score_fixture(0, 1, 'accepted')
        expected = broker.CONTEXT | {'type', 'programmingVersion', 'operation', 'requestId', 'status',
                    'serverTime', 'receiptId', 'problemId', 'problemRevision', 'language', 'draftRevision', 'sourceDigest'}
        self.assertEqual(set(receipt), expected)
        result = self.room('standings')
        self.assertEqual(self.state()['publicationPolicy'], 'teacher_only')
        self.assertEqual(result['rows'][0]['solved'], 1)
        ready = self.programming('ready')
        self.assertEqual(set(ready), broker.CONTEXT | {'type', 'programmingVersion', 'operation',
                         'requestId', 'status', 'serverTime', 'reason', 'runnerReady'})

    def test_standings_revalidates_membership_after_unlocked_probe(self):
        self.member('approve', 0)
        self.member('approve', 1)
        self.assertEqual(len(self.room('standings')['rows']), 2)
        def remove_member():
            self.runner.on_ready = None
            self.member('remove', 1)
        self.runner.on_ready = remove_member
        result = self.room('standings')
        self.assertEqual([r['displayName'] for r in result['rows']], ['Student 1'])
        self.runner.available = False
        self.assertEqual([r['displayName'] for r in self.room('standings')['rows']], ['Student 1'])

    def test_standings_delayed_probe_cannot_overwrite_newer_snapshot(self):
        self.standings_cohort()
        self.room('standings')
        entered, release = threading.Event(), threading.Event()
        results, errors = [], []
        def delayed_probe():
            if threading.current_thread().name == 'older-standings':
                entered.set()
                self.assertTrue(release.wait(3))
        self.runner.on_ready = delayed_probe
        def older_request():
            try:
                results.append(self.room('standings'))
            except BaseException as error:
                errors.append(error)
        thread = threading.Thread(target=older_request, name='older-standings')
        thread.start()
        try:
            self.assertTrue(entered.wait(3))
            self.score_fixture(0, 1, 'accepted')
            newest = self.room('standings')
        finally:
            release.set()
            thread.join(3)
        self.assertFalse(thread.is_alive())
        self.assertEqual(errors, [])
        self.assertEqual(results, [newest])

    def test_standings_revision_overflow_and_storage_failure_do_not_erase_snapshot(self):
        self.standings_cohort()
        original = self.room('standings')
        with self.app.lock, self.app.db:
            self.app.db.execute('UPDATE contest_standings SET revision=?', (9007199254740991,))
        self.score_fixture(0, 1, 'accepted')
        self.assert_error('snapshot_revision_exhausted', lambda: self.room('standings'))
        with self.app.lock, self.app.db:
            self.app.db.execute('UPDATE contest_standings SET revision=?', (original['revision'],))
            self.app.db.execute("CREATE TEMP TRIGGER fail_standings BEFORE INSERT ON contest_standings "
                                "BEGIN SELECT RAISE(ABORT,'storage-failure-secret-canary'); END")
        self.assert_error('storage_unavailable', lambda: self.room('standings'))
        stored = self.app.db.execute('SELECT revision,scores FROM contest_standings').fetchone()
        self.assertEqual(stored['revision'], original['revision'])
        self.assertEqual({s['solved'] for s in broker.decode_json(stored['scores'])}, {0})

    def standings_health_race(self, old_available, new_available):
        self.standings_cohort()
        self.room('standings')
        entered, release = threading.Event(), threading.Event()
        results, errors = [], []
        def capture_health():
            available = self.runner.available
            if threading.current_thread().name == 'older-health':
                entered.set()
                self.assertTrue(release.wait(3))
            return dict(available=available, languages=broker.LANGUAGES)
        self.runner.ready = capture_health
        self.runner.available = old_available
        def older_request():
            try:
                results.append(self.room('standings'))
            except BaseException as error:
                errors.append(error)
        thread = threading.Thread(target=older_request, name='older-health')
        thread.start()
        try:
            self.assertTrue(entered.wait(3))
            self.runner.available = new_available
            newest = self.room('standings')
        finally:
            release.set()
            thread.join(3)
        self.assertFalse(thread.is_alive())
        self.assertEqual(errors, [])
        self.assertEqual(newest['freshness'], 'current' if new_available else 'unavailable')
        self.assertEqual(results, [newest])
        self.assertEqual(self.room('standings'), newest)

    def test_standings_old_ready_cannot_replace_new_unavailable_health(self):
        self.standings_health_race(True, False)

    def test_standings_old_unavailable_cannot_replace_new_ready_health(self):
        self.standings_health_race(False, True)

    def test_shared_probe_order_prevents_stale_ready_begin(self):
        self.prepare()
        entered, release = threading.Event(), threading.Event()
        results, errors = [], []
        def capture_health():
            available = self.runner.available
            if threading.current_thread().name == 'older-begin':
                entered.set()
                self.assertTrue(release.wait(3))
            return dict(available=available, languages=broker.LANGUAGES)
        self.runner.ready = capture_health
        def older_request():
            try:
                results.append(self.begin())
            except broker.BrokerError as error:
                errors.append(error.code)
        thread = threading.Thread(target=older_request, name='older-begin')
        thread.start()
        try:
            self.assertTrue(entered.wait(3))
            self.runner.available = False
            self.assertEqual(self.room('standings')['freshness'], 'unavailable')
        finally:
            release.set()
            thread.join(3)
        self.assertFalse(thread.is_alive())
        self.assertEqual(results, [])
        self.assertEqual(errors, ['runtime_mismatch'])
        self.assertEqual(self.state()['serverStartUnixMilliseconds'], 0)


if __name__ == '__main__':
    unittest.main()
