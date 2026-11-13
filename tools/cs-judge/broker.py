"""Operator-only CS broker. Student programs execute only in Linux Isolate workers."""
import argparse
import base64
import hashlib
import hmac
import json
import os
import re
import sqlite3
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

from isolate_runner import BoundedThreads, IsolateRunner, RunnerBusy, config_identity, public_output

VERSION = 1
CONTEXT = {'packageId', 'packageDigestHex', 'clientIdHex', 'sessionIdHex', 'candidateId'}
BASE = CONTEXT | {'programmingVersion', 'operation', 'requestId'}
LANGUAGES = ['cpp20', 'python3']
MAX_SOURCE = 32768
MAX_BODY = 131072
MAX_PRIVATE_BODY = 4 * 1024 * 1024


class BrokerError(Exception):
    def __init__(self, code, status=400, response=None):
        super().__init__(code)
        self.code, self.status, self.response = code, status, response


def canonical_json(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False,
                      allow_nan=False).encode('utf-8')


def request_digest(body):
    # Server admission mode cannot change a candidate's immutable retry identity.
    return hashlib.sha256(canonical_json({k: v for k, v in body.items() if k != 'recoveryOnly'})).hexdigest()


def decode_json(raw):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise BrokerError('duplicate_field')
            result[key] = value
        return result
    try:
        return json.loads(raw.decode('utf-8'), object_pairs_hook=pairs,
                          parse_constant=lambda _: (_ for _ in ()).throw(BrokerError('invalid_number')))
    except (ValueError, UnicodeError, RecursionError):
        raise BrokerError('invalid_json') from None


def fields(value, allowed, required):
    if not isinstance(value, dict) or set(value) - set(allowed) or not set(required) <= set(value):
        raise BrokerError('invalid_fields')


def text(value, maximum, empty=False):
    if not isinstance(value, str):
        raise BrokerError('invalid_text')
    try:
        raw = value.encode('utf-8')
    except UnicodeError:
        raise BrokerError('invalid_utf8') from None
    if (not raw and not empty) or len(raw) > maximum or any(
            (ord(c) < 32 and c not in '\t\n\r') or ord(c) == 127 for c in value):
        raise BrokerError('text_bounds')
    return raw


def identifier(value):
    if not isinstance(value, str) or re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]{0,95}', value) is None:
        raise BrokerError('invalid_id')
    return value


def hex_id(value, length):
    if not isinstance(value, str) or re.fullmatch('[0-9a-f]{' + str(length) + '}', value) is None or not int(value, 16):
        raise BrokerError('invalid_digest')
    return value


def integer(value, minimum, maximum):
    if type(value) is not int or not minimum <= value <= maximum:
        raise BrokerError('integer_bounds')
    return value


def version(body):
    if type(body.get('programmingVersion')) is not int or body['programmingVersion'] != VERSION:
        raise BrokerError('unsupported_version')


def context(body):
    version(body)
    result = {key: body[key] for key in CONTEXT if key in body}
    if len(result) != len(CONTEXT):
        raise BrokerError('invalid_context')
    identifier(result['packageId'])
    hex_id(result['packageDigestHex'], 64)
    hex_id(result['clientIdHex'], 32)
    hex_id(result['sessionIdHex'], 32)
    text(result['candidateId'], 128)
    return result


def session_key(ctx):
    return hashlib.sha256(canonical_json(ctx)).hexdigest()


def validate_manifest(manifest):
    allowed = {'id', 'title', 'subject', 'candidate', 'durationSeconds', 'contentMode',
               'programmingVersion', 'sessionMode', 'languages', 'questions', 'allowedOrigins', 'documents'}
    fields(manifest, allowed, allowed - {'subject', 'candidate', 'allowedOrigins', 'documents'})
    if manifest.get('allowedOrigins', []) != [] or manifest.get('documents', []) != []:
        raise BrokerError('unsupported_origins')
    version(manifest)
    identifier(manifest['id'])
    text(manifest['title'], 240)
    for key, limit in [('subject', 120), ('candidate', 160)]:
        if key in manifest:
            text(manifest[key], limit, True)
    integer(manifest['durationSeconds'], 60, 86400)
    if manifest['contentMode'] != 'computer_science' or not isinstance(manifest['sessionMode'], str) or manifest['sessionMode'] not in {'exam', 'contest', 'practice'} or manifest['languages'] != LANGUAGES:
        raise BrokerError('unsupported_policy')
    questions = manifest['questions']
    if not isinstance(questions, list) or not 1 <= len(questions) <= 64:
        raise BrokerError('problem_bounds')
    seen = set()
    for problem in questions:
        allowed = {'id', 'type', 'revision', 'prompt', 'instruction', 'points', 'inputDescription',
                   'outputDescription', 'constraints', 'explanation', 'samples', 'timeLimitMs', 'memoryLimitMiB'}
        fields(problem, allowed, {'id', 'type', 'revision', 'prompt', 'samples', 'timeLimitMs', 'memoryLimitMiB'})
        problem_id = identifier(problem['id'])
        if problem_id in seen or problem['type'] != 'programming':
            raise BrokerError('invalid_problem')
        seen.add(problem_id)
        integer(problem['revision'], 1, 4294967295)
        text(problem['prompt'], 8000)
        for key in ['instruction', 'inputDescription', 'outputDescription', 'constraints', 'explanation']:
            if key in problem:
                text(problem[key], 4000, True)
        integer(problem.get('points', 100), 0, 1000)
        integer(problem['timeLimitMs'], 100, 10000)
        integer(problem['memoryLimitMiB'], 16, 512)
        if not isinstance(problem['samples'], list) or not 1 <= len(problem['samples']) <= 8:
            raise BrokerError('sample_bounds')
        samples = set()
        for sample in problem['samples']:
            fields(sample, {'id', 'input', 'output'}, {'id', 'input', 'output'})
            sample_id = identifier(sample['id'])
            if sample_id in samples:
                raise BrokerError('duplicate_sample')
            samples.add(sample_id)
            text(sample['input'], 4096, True)
            text(sample['output'], 4096, True)
    if len(canonical_json(manifest)) > MAX_BODY:
        raise BrokerError('manifest_bounds')


class Broker:
    def __init__(self, database, token, runner=None, clock=None):
        if not isinstance(token, str) or len(token.encode()) < 32:
            raise ValueError('operator secret must contain at least 32 bytes')
        self.token, self.runner = token, runner
        self.clock = clock or (lambda: time.time_ns() // 1000000)
        self.lock = threading.RLock()
        self.clock_lock = threading.Lock()
        self.clock_regressed = False
        self.runs = {}
        self.closed = False
        self.lease = open(str(database) + '.lock', 'a+b')
        try:
            if os.name == 'nt':
                import msvcrt
                if self.lease.seek(0, 2) == 0:
                    self.lease.write(b'\0')
                    self.lease.flush()
                self.lease.seek(0)
                msvcrt.locking(self.lease.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.lease.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.db = sqlite3.connect(str(database), check_same_thread=False)
        except (OSError, sqlite3.Error):
            self.lease.close()
            raise BrokerError('storage_unavailable', 503) from None
        try:
            self._initialize_database()
            self.last_observed_clock = self.db.execute('SELECT max(last_clock) FROM sessions').fetchone()[0] or 0
        except sqlite3.Error:
            self.db.close()
            self.lease.close()
            raise BrokerError('storage_unavailable', 503) from None

    def _initialize_database(self):
        self.db.row_factory = sqlite3.Row
        self.db.execute('PRAGMA journal_mode=WAL')
        self.db.execute('PRAGMA synchronous=FULL')
        self.db.execute('PRAGMA foreign_keys=ON')
        self.db.execute('PRAGMA max_page_count=524288')
        self.db.executescript('''
            CREATE TABLE IF NOT EXISTS packages(digest TEXT PRIMARY KEY, package_id TEXT, manifest BLOB);
            CREATE TABLE IF NOT EXISTS grading(digest TEXT PRIMARY KEY, package_digest TEXT, data BLOB);
            CREATE TABLE IF NOT EXISTS sessions(key TEXT PRIMARY KEY, context BLOB, grading_digest TEXT,
                mode TEXT, state TEXT, start INTEGER DEFAULT 0, deadline INTEGER DEFAULT 0,
                last_clock INTEGER DEFAULT 0, begin_hash TEXT, begin_response BLOB, stop_hash TEXT, stop_response BLOB,
                toolchain TEXT DEFAULT '');
            CREATE TABLE IF NOT EXISTS drafts(session TEXT, problem TEXT, language TEXT, revision INTEGER,
                source BLOB, digest TEXT, saved INTEGER, PRIMARY KEY(session,problem,language));
            CREATE TABLE IF NOT EXISTS submissions(receipt TEXT PRIMARY KEY, session TEXT, request TEXT,
                request_hash TEXT, problem TEXT, language TEXT, revision INTEGER, source BLOB, digest TEXT,
                received INTEGER, eligible INTEGER, response BLOB, judge_state TEXT, outcome TEXT,
                attempts INTEGER DEFAULT 0, UNIQUE(session,request));
            CREATE TABLE IF NOT EXISTS finalizations(session TEXT PRIMARY KEY, request_hash TEXT, response BLOB);
        ''')
        if 'toolchain' not in {row['name'] for row in self.db.execute('PRAGMA table_info(sessions)')}:
            self.db.execute("ALTER TABLE sessions ADD COLUMN toolchain TEXT DEFAULT ''")
        self.db.execute("UPDATE submissions SET judge_state='pending' WHERE judge_state='running'")
        self.db.commit()

    def close(self):
        with self.lock:
            if not self.closed:
                self.db.close()
                self.lease.close()
                self.closed = True

    def capture_admission(self):
        with self.clock_lock:
            now = self.clock()
            self.clock_regressed |= now < self.last_observed_clock
            self.last_observed_clock = max(self.last_observed_clock, now)
            return now, self.clock_regressed

    def dispatch(self, route, body, token, admission=None):
        if not isinstance(token, str) or not hmac.compare_digest(token.encode(), self.token.encode()):
            raise BrokerError('unauthorized', 401)
        if not isinstance(body, dict):
            raise BrokerError('invalid_fields')
        admission = self.capture_admission() if admission is None else admission
        try:
            if route == '/v1/validate-context':
                with self.lock:
                    return self._validate_context(body)
            if route == '/v1/programming' and 'recoveryOnly' in body:
                if type(body['recoveryOnly']) is not bool:
                    raise BrokerError('invalid_recovery_mode')
                operation = body.get('operation')
                if body['recoveryOnly'] and (not isinstance(operation, str) or operation not in {'draft', 'submit', 'finalize', 'recover'}):
                    raise BrokerError('recovery_only')
            if route == '/v1/programming' and body.get('operation') == 'run':
                return self._run(body)
            if route == '/v1/programming' and body.get('operation') == 'cancel':
                return self._cancel(body)
            with self.lock, self.db:
                if route == '/v1/packages':
                    return self._package(body)
                if route == '/v1/grading':
                    return self._grading(body)
                if route == '/v1/sessions':
                    return self._create_session(body)
                if route in {'/v1/begin', '/v1/stop'}:
                    return self._lifecycle(route, body)
                if route == '/v1/programming':
                    return self._operation(body, admission)
            raise BrokerError('unknown_route', 404)
        except sqlite3.Error:
            raise BrokerError('storage_unavailable', 503) from None

    def _validate_context(self, body):
        required = CONTEXT | {'programmingVersion'}
        fields(body, required, required)
        ctx = context(body)
        exists = self.db.execute('SELECT 1 FROM sessions WHERE key=?', (session_key(ctx),)).fetchone()
        response = dict(ctx, programmingVersion=1, status='known' if exists else 'unknown', serverTime=self.clock())
        if not exists:
            raise BrokerError('unknown_context', 404, response)
        return response

    def _package(self, body):
        fields(body, {'programmingVersion', 'packageId', 'packageDigestHex', 'manifest'},
               {'programmingVersion', 'packageId', 'packageDigestHex', 'manifest'})
        version(body)
        identifier(body['packageId'])
        digest = hex_id(body['packageDigestHex'], 64)
        validate_manifest(body['manifest'])
        if body['packageId'] != body['manifest']['id']:
            raise BrokerError('package_mismatch')
        wire = canonical_json(body['manifest'])
        old = self.db.execute('SELECT manifest FROM packages WHERE digest=?', (digest,)).fetchone()
        if old and old['manifest'] != wire:
            raise BrokerError('conflict', 409)
        self.db.execute('INSERT OR IGNORE INTO packages VALUES(?,?,?)', (digest, body['packageId'], wire))
        return dict(status='registered', packageId=body['packageId'], packageDigestHex=digest)

    def _manifest(self, ctx):
        row = self.db.execute('SELECT package_id,manifest FROM packages WHERE digest=?',
                              (ctx['packageDigestHex'],)).fetchone()
        if not row or row['package_id'] != ctx['packageId']:
            raise BrokerError('unknown_package', 404)
        return decode_json(row['manifest'])

    def _grading(self, body):
        required = {'programmingVersion', 'packageId', 'packageDigestHex', 'gradingDigestHex', 'problems'}
        fields(body, required, required)
        version(body)
        digest = hex_id(body['gradingDigestHex'], 64)
        public = self._manifest(body)
        payload = {key: value for key, value in body.items() if key != 'gradingDigestHex'}
        wire = canonical_json(payload)
        if len(wire) > MAX_PRIVATE_BODY or hashlib.sha256(wire).hexdigest() != digest:
            raise BrokerError('grading_digest_mismatch')
        expected = {p['id']: p for p in public['questions']}
        if not isinstance(body['problems'], list) or len(body['problems']) != len(expected):
            raise BrokerError('grading_problem_mismatch')
        seen = set()
        for problem in body['problems']:
            fields(problem, {'id', 'revision', 'cases'}, {'id', 'revision', 'cases'})
            identifier(problem['id'])
            integer(problem['revision'], 1, 4294967295)
            if problem['id'] not in expected or problem['id'] in seen or problem['revision'] != expected[problem['id']]['revision']:
                raise BrokerError('grading_problem_mismatch')
            seen.add(problem['id'])
            if not isinstance(problem['cases'], list) or not 1 <= len(problem['cases']) <= 64:
                raise BrokerError('private_case_bounds')
            for case in problem['cases']:
                fields(case, {'input', 'output'}, {'input', 'output'})
                text(case['input'], 65536, True)
                text(case['output'], 65536, True)
        old = self.db.execute('SELECT data FROM grading WHERE digest=?', (digest,)).fetchone()
        if old and old['data'] != wire:
            raise BrokerError('conflict', 409)
        self.db.execute('INSERT OR IGNORE INTO grading VALUES(?,?,?)', (digest, body['packageDigestHex'], wire))
        return dict(status='registered', gradingDigestHex=digest)

    def _create_session(self, body):
        fields(body, CONTEXT | {'programmingVersion', 'sessionMode', 'gradingDigestHex'},
               CONTEXT | {'programmingVersion', 'sessionMode', 'gradingDigestHex'})
        ctx = context(body)
        public = self._manifest(ctx)
        if body['sessionMode'] != public['sessionMode']:
            raise BrokerError('policy_mismatch')
        grading = self.db.execute('SELECT package_digest FROM grading WHERE digest=?',
                                  (hex_id(body['gradingDigestHex'], 64),)).fetchone()
        if not grading or grading['package_digest'] != ctx['packageDigestHex']:
            raise BrokerError('grading_unavailable', 409)
        key = session_key(ctx)
        toolchain = self._runtime_identity()
        old = self.db.execute('SELECT * FROM sessions WHERE key=?', (key,)).fetchone()
        if old and (old['grading_digest'] != body['gradingDigestHex'] or old['mode'] != body['sessionMode'] or old['toolchain'] != toolchain):
            raise BrokerError('conflict', 409)
        self.db.execute('INSERT OR IGNORE INTO sessions(key,context,grading_digest,mode,state,last_clock,toolchain) VALUES(?,?,?,?,?,?,?)',
                        (key, canonical_json(ctx), body['gradingDigestHex'], body['sessionMode'], 'waiting', self.clock(), toolchain))
        return dict(ctx, status='prepared', programmingVersion=1)

    def _session(self, body):
        ctx = context(body)
        key = session_key(ctx)
        row = self.db.execute('SELECT * FROM sessions WHERE key=?', (key,)).fetchone()
        if not row:
            raise BrokerError('unknown_context', 404)
        return ctx, key, row

    def _response(self, body, status, **extra):
        if 'problemRevision' in body:
            extra['problemRevision'] = body['problemRevision']
        return dict(context(body), type='exam_programming_response', programmingVersion=1,
                    operation=body['operation'], requestId=body['requestId'], status=status,
                    serverTime=self.clock(), **extra)

    def _runtime_identity(self):
        if self.runner is None:
            return ''
        config = getattr(self.runner, 'config', None)
        if config:
            return config_identity(config)
        return self.runner.ready().get('toolchainId', '')

    def _available(self, session=None):
        if self.runner is None:
            return False
        result = self.runner.ready()
        return (bool(result.get('available')) and result.get('languages') == LANGUAGES and
                (session is None or bool(session['toolchain']) and session['toolchain'] == self._runtime_identity()))

    def _lifecycle(self, route, body):
        allowed = CONTEXT | {'programmingVersion', 'requestId'}
        if route == '/v1/begin':
            allowed |= {'durationSeconds'}
        fields(body, allowed, allowed)
        identifier(body['requestId'])
        ctx, key, row = self._session(body)
        digest = hashlib.sha256(canonical_json(body)).hexdigest()
        operation = 'begin' if route == '/v1/begin' else 'stop'
        if row[operation + '_response']:
            if row[operation + '_hash'] != digest:
                raise BrokerError('conflict', 409)
            return decode_json(row[operation + '_response'])
        now = self.clock()
        if operation == 'begin':
            duration = integer(body['durationSeconds'], 60, 86400)
            if row['state'] != 'waiting' or now < row['last_clock']:
                raise BrokerError('attempt_closed', 409)
            if not self._available(row):
                raise BrokerError('runner_unavailable', 503)
            response = dict(ctx, programmingVersion=1, requestId=body['requestId'], status='begun',
                            serverStartUnixMilliseconds=now, deadlineUnixMilliseconds=now + duration * 1000, serverTime=now)
            self.db.execute("UPDATE sessions SET state='active',start=?,deadline=?,last_clock=?,begin_hash=?,begin_response=? WHERE key=?",
                            (now, response['deadlineUnixMilliseconds'], now, digest, canonical_json(response), key))
        else:
            response = dict(ctx, programmingVersion=1, requestId=body['requestId'], status='stopped', serverTime=now)
            self.db.execute("UPDATE sessions SET state='closed',stop_hash=?,stop_response=? WHERE key=?",
                            (digest, canonical_json(response), key))
            active = self.runs.get(key)
            if active:
                active['cancel'].set()
        return response

    def _problem(self, ctx, body):
        identifier(body['problemId'])
        if body['language'] not in LANGUAGES:
            raise BrokerError('unsupported_language')
        integer(body['problemRevision'], 1, 4294967295)
        for problem in self._manifest(ctx)['questions']:
            if problem['id'] == body['problemId']:
                if problem['revision'] != body['problemRevision']:
                    raise BrokerError('problem_revision_mismatch', 409)
                return problem
        raise BrokerError('unknown_problem', 404)

    def _solution(self, ctx, body):
        self._problem(ctx, body)
        source = text(body['source'], MAX_SOURCE, body['operation'] == 'draft')
        integer(body['draftRevision'], 1, 9007199254740991)
        return source, hashlib.sha256(source).hexdigest()

    def _operation(self, body, admission):
        operation = body.get('operation')
        if not isinstance(operation, str):
            raise BrokerError('unknown_operation')
        extra = {
            'draft': {'problemId', 'problemRevision', 'language', 'source', 'draftRevision'},
            'submit': {'problemId', 'problemRevision', 'language', 'source', 'draftRevision'},
            'recover': {'problemId', 'problemRevision', 'language'},
            'ready': set(), 'cancel': {'targetRunId', 'problemId', 'problemRevision', 'language'},
            'finalize': {'receiptIds', 'noSourceProblemIds'},
        }.get(operation)
        if extra is None:
            raise BrokerError('unknown_operation')
        fields(body, BASE | extra | {'recoveryOnly'}, BASE | extra)
        identifier(body['requestId'])
        ctx, key, session = self._session(body)
        if operation == 'ready':
            available = self._available(session)
            return self._response(body, 'ready' if available else 'unavailable',
                                  runnerReady=available, reason='' if available else 'runner_unavailable')
        if operation == 'recover':
            self._problem(ctx, body)
            draft = self.db.execute('SELECT * FROM drafts WHERE session=? AND problem=? AND language=?',
                                    (key, body['problemId'], body['language'])).fetchone()
            last = self.db.execute('SELECT response FROM submissions WHERE session=? AND problem=? AND language=? ORDER BY rowid DESC LIMIT 1',
                                   (key, body['problemId'], body['language'])).fetchone()
            submitted = self.db.execute('SELECT source,revision FROM submissions WHERE session=? AND problem=? AND language=? ORDER BY revision DESC,rowid DESC LIMIT 1',
                                        (key, body['problemId'], body['language'])).fetchone()
            receipt = decode_json(last['response']) if last else None
            selected = draft if draft and (not submitted or draft['revision'] >= submitted['revision']) else submitted
            source = selected['source'] if selected else b''
            return self._response(body, 'recovered', problemId=body['problemId'], language=body['language'],
                                  draftRevision=selected['revision'] if selected else 0,
                                  sourceDigest=hashlib.sha256(source).hexdigest(), sourceBase64=base64.b64encode(source).decode(),
                                  latestReceiptId=receipt['receiptId'] if receipt else '',
                                  receiptDigest=receipt['sourceDigest'] if receipt else '', finalized=session['state'] == 'closed',
                                  serverStartUnixMilliseconds=session['start'], deadlineUnixMilliseconds=session['deadline'])
        if operation == 'finalize':
            return self._finalize(body, ctx, key)
        source, digest = self._solution(ctx, body)
        now, regressed = admission if operation == 'submit' else (self.clock(), False)
        if operation == 'submit':
            fingerprint = request_digest(body)
            existing = self.db.execute('SELECT request_hash,response FROM submissions WHERE session=? AND request=?',
                                       (key, body['requestId'])).fetchone()
            if existing:
                if existing['request_hash'] != fingerprint:
                    raise BrokerError('conflict', 409)
                return decode_json(existing['response'])
            count = self.db.execute('SELECT count(*) FROM submissions WHERE session=?', (key,)).fetchone()[0]
            if count >= 1024:
                raise BrokerError('submission_quota', 503)
            eligible = (not body.get('recoveryOnly', False) and session['state'] == 'active' and
                        session['start'] <= now < session['deadline'] and not regressed)
            receipt = uuid.uuid4().hex
            response = self._response(body, 'received' if eligible else 'late', receiptId=receipt,
                                      problemId=body['problemId'], language=body['language'], draftRevision=body['draftRevision'], sourceDigest=digest)
            response['serverTime'] = now
            self.db.execute('INSERT INTO submissions(receipt,session,request,request_hash,problem,language,revision,source,digest,received,eligible,response,judge_state) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)',
                            (receipt, key, body['requestId'], fingerprint, body['problemId'], body['language'],
                             body['draftRevision'], source, digest, now, int(eligible), canonical_json(response), 'pending' if eligible else 'ineligible'))
            if regressed:
                self.db.execute("UPDATE sessions SET state='disrupted' WHERE key=?", (key,))
            self.db.execute('UPDATE sessions SET last_clock=max(last_clock,?) WHERE key=?', (now, key))
            return response
        if session['state'] == 'waiting' and not body.get('recoveryOnly', False):
            raise BrokerError('attempt_not_begun', 409)
        draft = self.db.execute('SELECT * FROM drafts WHERE session=? AND problem=? AND language=?',
                                (key, body['problemId'], body['language'])).fetchone()
        if draft and (body['draftRevision'] < draft['revision'] or
                      (body['draftRevision'] == draft['revision'] and digest != draft['digest'])):
            raise BrokerError('draft_conflict', 409)
        saved = draft['saved'] if draft and draft['revision'] == body['draftRevision'] else now
        self.db.execute('INSERT OR REPLACE INTO drafts VALUES(?,?,?,?,?,?,?)',
                        (key, body['problemId'], body['language'], body['draftRevision'], source, digest, saved))
        return dict(self._response(body, 'saved', problemId=body['problemId'], language=body['language'],
                                   draftRevision=body['draftRevision'], sourceDigest=digest), serverTime=saved)

    def _finalize(self, body, ctx, key):
        receipt_ids, empty = body['receiptIds'], body['noSourceProblemIds']
        if not isinstance(receipt_ids, list) or not isinstance(empty, list) or len(receipt_ids) > 64 or len(empty) > 64:
            raise BrokerError('inventory_bounds')
        for receipt in receipt_ids:
            hex_id(receipt, 32)
        for problem_id in empty:
            identifier(problem_id)
        if len(set(receipt_ids)) != len(receipt_ids) or len(set(empty)) != len(empty):
            raise BrokerError('inventory_conflict', 409)
        inventory = set()
        for receipt in receipt_ids:
            hex_id(receipt, 32)
            row = self.db.execute('SELECT problem FROM submissions WHERE session=? AND receipt=?', (key, receipt)).fetchone()
            if not row or row['problem'] in inventory:
                raise BrokerError('inventory_conflict', 409)
            inventory.add(row['problem'])
        for problem_id in empty:
            identifier(problem_id)
            if problem_id in inventory:
                raise BrokerError('inventory_conflict', 409)
            inventory.add(problem_id)
        if inventory != {p['id'] for p in self._manifest(ctx)['questions']}:
            raise BrokerError('inventory_incomplete', 409)
        fingerprint = request_digest(body)
        old = self.db.execute('SELECT * FROM finalizations WHERE session=?', (key,)).fetchone()
        if old:
            if old['request_hash'] != fingerprint:
                raise BrokerError('conflict', 409)
            return decode_json(old['response'])
        response = self._response(body, 'finalized', receiptId=uuid.uuid4().hex)
        self.db.execute('INSERT INTO finalizations VALUES(?,?,?)', (key, fingerprint, canonical_json(response)))
        self.db.execute("UPDATE sessions SET state='closed' WHERE key=?", (key,))
        active = self.runs.get(key)
        if active:
            active['cancel'].set()
        return response

    def _cancel(self, body):
        required = BASE | {'targetRunId', 'problemId', 'problemRevision', 'language'}
        fields(body, required | {'recoveryOnly'}, required)
        identifier(body['requestId'])
        identifier(body['targetRunId'])
        with self.lock:
            ctx, key, _ = self._session(body)
            self._problem(ctx, body)
            active = self.runs.get(key)
            identity = (body['targetRunId'], body['problemId'], body['problemRevision'], body['language'])
            if not active or active['identity'] != identity:
                return self._response(body, 'rejected', problemId=body['problemId'], language=body['language'])
            active['cancel'].set()
        # An acknowledgement means the owned worker returned after termination/cleanup.
        stopped = active['done'].wait(25) and active['status'] == 'cancelled'
        return self._response(body, 'cancelled' if stopped else 'unavailable',
                              problemId=body['problemId'], language=body['language'])

    def _run(self, body):
        required = BASE | {'problemId', 'problemRevision', 'language', 'source', 'draftRevision', 'sampleId'}
        fields(body, required | {'recoveryOnly'}, required)
        identifier(body['requestId'])
        with self.lock, self.db:
            ctx, key, session = self._session(body)
            source, digest = self._solution(ctx, body)
            problem = self._problem(ctx, body)
            sample = next((s for s in problem['samples'] if s['id'] == body['sampleId']), None)
            if sample is None:
                raise BrokerError('unknown_sample', 404)
            if session['state'] != 'active':
                raise BrokerError('attempt_closed', 409)
            if not self._available(session):
                raise BrokerError('runner_unavailable', 503)
            now, regressed = self.capture_admission()
            if regressed:
                self.db.execute("UPDATE sessions SET state='disrupted' WHERE key=?", (key,))
                self.db.commit()
                raise BrokerError('attempt_closed', 409)
            if not session['start'] <= now < session['deadline']:
                raise BrokerError('attempt_closed', 409)
            if key in self.runs or len(self.runs) >= 8:
                raise BrokerError('run_queue_full', 503)
            self.db.execute('UPDATE sessions SET last_clock=? WHERE key=?', (now, key))
            self.db.commit()
            cancel = threading.Event()
            active = dict(identity=(body['requestId'], body['problemId'], body['problemRevision'], body['language']),
                          cancel=cancel, done=threading.Event(), status='unavailable')
            self.runs[key] = active
            expiry = threading.Timer((session['deadline'] - now) / 1000, cancel.set)
            expiry.daemon = True
            expiry.start()
        try:
            result = self.runner.execute(source, body['language'], sample['input'].encode(), problem, 'public', cancel)
            status = {'time_limit': 'timeout', 'memory_limit': 'runtime_error', 'output_limit': 'runtime_error'}.get(result['status'], result['status'])
            if status not in {'completed', 'compilation_error', 'runtime_error', 'timeout', 'cancelled'}:
                raise RuntimeError('invalid public status')
            out, err, diagnostics, clipped = public_output(result['stdout'], result['stderr'], result['diagnostics'])
            truncated = bool(result.get('truncated')) or clipped
            active['status'] = status
            return self._response(body, status, problemId=body['problemId'], language=body['language'],
                                  draftRevision=body['draftRevision'], sourceDigest=digest, sampleId=body['sampleId'],
                                  stdout=out, stderr=err, diagnostics=diagnostics, truncated=truncated,
                                  sampleMatched=result['status'] == 'completed' and not truncated and result['stdout'] == sample['output'].encode())
        except (OSError, RuntimeError):
            raise BrokerError('runner_unavailable', 503) from None
        finally:
            expiry.cancel()
            with self.lock:
                self.runs.pop(key, None)
                active['done'].set()

    def judge_one(self):
        with self.lock, self.db:
            row = self.db.execute("SELECT submissions.* FROM submissions JOIN sessions ON sessions.key=submissions.session "
                                  "WHERE judge_state='pending' AND eligible=1 AND sessions.toolchain=? "
                                  "ORDER BY received,submissions.rowid LIMIT 1",
                                  (self._runtime_identity(),)).fetchone()
            if row is None:
                return False
            session = self.db.execute('SELECT * FROM sessions WHERE key=?', (row['session'],)).fetchone()
            if not self._available(session):
                return False
            self.db.execute("UPDATE submissions SET judge_state='running',attempts=attempts+1 WHERE receipt=?", (row['receipt'],))
            ctx = decode_json(session['context'])
            problem = next(p for p in self._manifest(ctx)['questions'] if p['id'] == row['problem'])
            grading = decode_json(self.db.execute('SELECT data FROM grading WHERE digest=?', (session['grading_digest'],)).fetchone()['data'])
            cases = next(p['cases'] for p in grading['problems'] if p['id'] == row['problem'])
        outcome = 'accepted'
        try:
            # shortcut: each private case compiles in a fresh box; batch only after measured capacity requires it.
            for case in cases:
                result = self.runner.execute(row['source'], row['language'], case['input'].encode(), problem, 'private', threading.Event())
                if result['status'] != 'completed' or result.get('truncated'):
                    if result['status'] == 'cancelled':
                        outcome = 'judge_error'
                    else:
                        outcome = 'output_limit' if result.get('truncated') else result['status']
                    break
                if result['stdout'] != case['output'].encode():
                    outcome = 'wrong_answer'
                    break
        except RunnerBusy:
            with self.lock, self.db:
                self.db.execute("UPDATE submissions SET judge_state='pending',attempts=attempts-1 WHERE receipt=?", (row['receipt'],))
            return False
        except (OSError, RuntimeError):
            outcome = 'judge_error'
        with self.lock, self.db:
            state = 'pending' if outcome == 'judge_error' and row['attempts'] < 2 else 'judged'
            self.db.execute('UPDATE submissions SET judge_state=?,outcome=? WHERE receipt=?', (state, outcome, row['receipt']))
        return True

    def private_score(self, ctx):
        """Trusted referee processing only; never exposed by the HTTP/candidate API."""
        with self.lock:
            _, key, session = self._session(dict(ctx, programmingVersion=1))
            public = self._manifest(ctx)
            rows = self.db.execute('SELECT * FROM submissions WHERE session=? AND eligible=1 ORDER BY received,rowid', (key,)).fetchall()
            score, solved, penalty = 0, 0, 0
            for problem in public['questions']:
                attempts = [r for r in rows if r['problem'] == problem['id']]
                if session['mode'] != 'contest':
                    if attempts and attempts[-1]['judge_state'] == 'judged' and attempts[-1]['outcome'] == 'accepted':
                        score += problem.get('points', 100)
                        solved += 1
                else:
                    wrong = 0
                    for attempt in attempts:
                        if attempt['judge_state'] != 'judged':
                            break
                        if attempt['outcome'] == 'accepted':
                            solved += 1
                            penalty += (attempt['received'] - session['start']) // 60000 + wrong * 20
                            break
                        if attempt['outcome'] in {'wrong_answer', 'runtime_error', 'time_limit', 'memory_limit', 'output_limit'}:
                            wrong += 1
            return dict(score=score, solved=solved, penaltyMinutes=penalty, eligibleSubmissions=len(rows))


def serve(app, port):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            try:
                authorization = self.headers.get('Authorization', '')
                token = authorization[7:] if authorization.startswith('Bearer ') else ''
                if not hmac.compare_digest(token.encode(), app.token.encode()):
                    raise BrokerError('unauthorized', 401)
                limit = MAX_PRIVATE_BODY if self.path == '/v1/grading' else MAX_BODY
                length = self.headers.get('Content-Length', '')
                if self.headers.get('Transfer-Encoding') or not re.fullmatch(r'[0-9]{1,8}', length) or not 0 < int(length) <= limit:
                    raise BrokerError('request_bounds')
                if self.headers.get('Content-Type') != 'application/json':
                    raise BrokerError('content_type')
                self.connection.settimeout(10)
                raw = self.rfile.read(int(length))
                if len(raw) != int(length):
                    raise BrokerError('truncated_request')
                admission = app.capture_admission()
                result = app.dispatch(self.path, decode_json(raw), token, admission)
                status = 200
            except BrokerError as error:
                status, result = error.status, error.response if error.response is not None else dict(error=error.code)
            except (OSError, ValueError, TypeError):
                status, result = 400, dict(error='invalid_request')
            wire = canonical_json(result)
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(wire)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.end_headers()
            self.wfile.write(wire)
    class Server(BoundedThreads, HTTPServer):
        request_queue_size = 16
    server = Server(('127.0.0.1', port), Handler)
    server.serve_forever()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--database', required=True)
    parser.add_argument('--config')
    parser.add_argument('--port', type=int, default=47831)
    args = parser.parse_args()
    os.umask(0o077)
    Path(args.database).parent.mkdir(parents=True, exist_ok=True)
    config = decode_json(Path(args.config).read_bytes()) if args.config else None
    app = Broker(args.database, os.environ.get('NSTU_CS_BROKER_TOKEN', ''), IsolateRunner(config))
    stop = threading.Event()
    def judge():
        while not stop.wait(0.2):
            try:
                app.judge_one()
            except (BrokerError, sqlite3.Error):
                stop.wait(1)
    thread = threading.Thread(target=judge, daemon=True)
    thread.start()
    try:
        serve(app, args.port)
    finally:
        stop.set()
        thread.join(timeout=45)
        app.close()


if __name__ == '__main__':
    main()
