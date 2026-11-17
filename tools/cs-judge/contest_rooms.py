"""Operator room ledger. Keys and candidate transport authority remain teacher-owned."""
import importlib


ROOM_BASE = {'programmingVersion', 'roomId', 'roundId'}
MAX_MEMBERS = 64
MAX_MEMBER_HISTORY = 256


class ContestRooms:
    def __init__(self, broker, owner_id):
        self.broker = broker
        self.api = importlib.import_module(type(broker).__module__)
        self.owner_id = owner_id
        if owner_id is not None:
            self.api.identifier(owner_id)
        broker.db.executescript('''
            CREATE TABLE IF NOT EXISTS contest_owner(singleton INTEGER PRIMARY KEY CHECK(singleton=1), owner TEXT);
            CREATE TABLE IF NOT EXISTS contest_clock(singleton INTEGER PRIMARY KEY CHECK(singleton=1), last_clock INTEGER);
            CREATE TABLE IF NOT EXISTS contest_rooms(room TEXT PRIMARY KEY, round TEXT UNIQUE, owner TEXT,
                package_id TEXT, package_digest TEXT, grading_digest TEXT, runtime TEXT, publication TEXT,
                duration INTEGER, state TEXT, revision INTEGER, start INTEGER DEFAULT 0, deadline INTEGER DEFAULT 0,
                last_clock INTEGER DEFAULT 0, create_hash TEXT, begin_hash TEXT, begin_response BLOB,
                stop_hash TEXT, stop_response BLOB);
            CREATE TABLE IF NOT EXISTS contest_members(session TEXT PRIMARY KEY REFERENCES sessions(key),
                room TEXT REFERENCES contest_rooms(room), display_name TEXT, prepared INTEGER,
                approved INTEGER, version INTEGER);
            CREATE TABLE IF NOT EXISTS contest_standings(room TEXT PRIMARY KEY REFERENCES contest_rooms(room),
                revision INTEGER, generated INTEGER, freshness TEXT, scores BLOB);
        ''')
        old = broker.db.execute('SELECT owner FROM contest_owner WHERE singleton=1').fetchone()
        if old and old['owner'] != owner_id:
            raise self.api.BrokerError('owner_mismatch', 409)
        if owner_id is not None:
            broker.db.execute('INSERT OR IGNORE INTO contest_owner VALUES(1,?)', (owner_id,))
            broker.db.execute('INSERT OR IGNORE INTO contest_clock VALUES(1,0)')
        broker.db.commit()

    def checkpoint(self, admission):
        if self.owner_id is None:
            return
        b = self.broker
        now, regressed = admission
        self.api.integer(now, 1, 9007199254740991 - 86400000)
        # Authenticated admission checkpoints time before the read-only projection.
        # capture_admission releases clock_lock before this database lock is taken.
        with b.lock, b.db:
            b.db.execute('UPDATE contest_clock SET last_clock=? WHERE singleton=1 AND last_clock<?', (now, now))
            b.db.execute("UPDATE contest_rooms SET state='Closed',revision=revision+1 WHERE state<>'Closed' "
                         "AND ((deadline>0 AND deadline<=?) OR ?)", (now, int(regressed)))
            if regressed:
                b.db.execute("UPDATE sessions SET state='closed' WHERE state<>'closed' AND key IN "
                             "(SELECT m.session FROM contest_members m JOIN contest_rooms r ON r.room=m.room WHERE r.state='Closed')")
            # Keep predeadline admission eligibility; Stop/Finalize/disruption close sessions.
            for member in b.db.execute("SELECT m.session FROM contest_members m JOIN contest_rooms r ON r.room=m.room WHERE r.state='Closed'"):
                active = b.runs.get(member['session'])
                if active:
                    active['cancel'].set()

    def _room(self, body):
        row = self.broker.db.execute('SELECT * FROM contest_rooms WHERE room=? AND round=?',
                                     (body['roomId'], body['roundId'])).fetchone()
        if not row or row['owner'] != self.owner_id:
            raise self.api.BrokerError('unknown_room', 404)
        return row

    def _closed(self, row, now):
        return (row['state'] == 'Closed' or self.broker.clock_regressed or
                bool(row['deadline']) and now >= row['deadline'])

    def _state(self, row, now):
        members = self.broker.db.execute('SELECT m.*,s.context FROM contest_members m '
                    'JOIN sessions s ON s.key=m.session WHERE m.room=? AND m.approved=1 ORDER BY m.session', (row['room'],)).fetchall()
        return dict(programmingVersion=1, roomId=row['room'], roundId=row['round'], ownerId=row['owner'],
                    packageId=row['package_id'], packageDigestHex=row['package_digest'],
                    gradingDigestHex=row['grading_digest'], runtimeIdentity=row['runtime'],
                    publicationPolicy=row['publication'], durationSeconds=row['duration'],
                    state='Closed' if self._closed(row, now) else row['state'], revision=row['revision'],
                    serverStartUnixMilliseconds=row['start'], deadlineUnixMilliseconds=row['deadline'], serverTime=now,
                    members=[dict(context=self.api.decode_json(m['context']), displayName=m['display_name'],
                                  prepared=bool(m['prepared']), approved=bool(m['approved']),
                                  memberVersion=m['version']) for m in members])

    def _revision(self, row, body):
        self.api.integer(body['expectedRevision'], 1, 9007199254740991)
        if row['revision'] != body['expectedRevision']:
            raise self.api.BrokerError('stale_revision', 409)

    def _replay(self, row, operation, body):
        if row[operation + '_response']:
            if row[operation + '_hash'] != self.api.request_digest(body):
                raise self.api.BrokerError('conflict', 409)
            return self.api.decode_json(row[operation + '_response'])

    def dispatch(self, route, body, admission):
        a, b = self.api, self.broker
        if self.owner_id is None:
            raise a.BrokerError('room_owner_unconfigured', 503)
        operation = route.removeprefix('/v1/contest-rooms/')
        extras = {
            'create': {'packageId', 'packageDigestHex', 'gradingDigestHex', 'publicationPolicy'},
            'members': {'action', 'expectedRevision', 'context', 'displayName', 'memberVersion', 'packageDigestHex'},
            'begin': {'requestId', 'expectedRevision'}, 'stop': {'requestId', 'expectedRevision'},
            'state': set(), 'standings': set(),
        }.get(operation)
        if extras is None:
            raise a.BrokerError('unknown_route', 404)
        required = ROOM_BASE | (extras if operation != 'members' else {'action', 'expectedRevision', 'context'})
        if operation == 'create':
            required -= {'publicationPolicy'}
        a.fields(body, ROOM_BASE | extras, required)
        a.version(body)
        a.hex_id(body['roomId'], 32)
        a.hex_id(body['roundId'], 32)
        now, regressed = admission
        a.integer(now, 1, 9007199254740991 - 86400000)
        if operation in {'begin', 'stop'}:
            a.identifier(body['requestId'])
            a.integer(body['expectedRevision'], 1, 9007199254740991)
        # Replay and read-only state do not depend on current network readiness.
        with b.lock:
            if operation != 'create':
                row = self._room(body)
                if operation in {'begin', 'stop'}:
                    replay = self._replay(row, operation, body)
                    if replay is not None:
                        return replay
                if operation == 'state':
                    return self._state(row, now)
        if operation == 'standings':
            b._runner_snapshot()
            with b.lock, b.db:
                return self._standings(self._room(body), now, b.latest_runner_probe)
        probe = None
        if operation == 'create' or operation == 'begin' or operation == 'members' and body.get('action') in ('prepare', 'remove'):
            probe = b._runner_snapshot()
        with b.lock, b.db:
            if operation == 'create':
                return self._create(body, now, probe)
            row = self._room(body)
            if operation in {'begin', 'stop'}:
                replay = self._replay(row, operation, body)
                if replay is not None:
                    return replay
            self._revision(row, body)
            if operation == 'members':
                return self._members(row, body, now, probe)
            if operation == 'begin':
                return self._begin(row, body, now, regressed, probe)
            return self._stop(row, body, now)

    def _standings(self, row, now, probe):
        a, b = self.api, self.broker
        members = b.db.execute('SELECT m.session,m.display_name,s.context FROM contest_members m '
                    'JOIN sessions s ON s.key=m.session WHERE m.room=? AND m.approved=1 '
                    'ORDER BY m.session LIMIT ?', (row['room'], MAX_MEMBERS + 1)).fetchall()
        if len(members) > MAX_MEMBERS:
            raise a.BrokerError('member_bounds')
        old = b.db.execute('SELECT * FROM contest_standings WHERE room=?', (row['room'],)).fetchone()
        previous = {s['session']: s for s in a.decode_json(old['scores'])} if old else {}
        available = b._runtime_matches(probe) and probe[2] and bool(row['runtime']) and probe[1] == row['runtime']
        freshness = 'current' if available else 'unavailable'
        scores = []
        if available:
            for member in members:
                score = b.private_score(a.decode_json(member['context']))
                a.integer(score['solved'], 0, 64)
                a.integer(score['penaltyMinutes'], 0, 9007199254740991)
                scores.append(dict(session=member['session'], displayName=member['display_name'],
                                   solved=score['solved'], penaltyMinutes=score['penaltyMinutes']))
                if score['freshness'] == 'unavailable':
                    freshness = 'unavailable'
                elif score['freshness'] == 'pending' and freshness == 'current':
                    freshness = 'pending'
        if freshness == 'unavailable':
            # Keep only current members; cached scores never grant a removed tuple publication.
            scores = [dict(session=m['session'], displayName=m['display_name'],
                      solved=previous.get(m['session'], {}).get('solved', 0),
                      penaltyMinutes=previous.get(m['session'], {}).get('penaltyMinutes', 0)) for m in members]
        encoded = a.canonical_json(scores)
        changed = not old or encoded != old['scores'] or freshness != old['freshness']
        revision = old['revision'] if old else 1
        generated = old['generated'] if old else now
        if changed:
            if old:
                if revision >= 9007199254740991:
                    raise a.BrokerError('snapshot_revision_exhausted', 503)
                revision += 1
            if freshness != 'unavailable':
                generated = max(now, generated)
            b.db.execute('INSERT OR REPLACE INTO contest_standings VALUES(?,?,?,?,?)',
                         (row['room'], revision, generated, freshness, encoded))
        a.integer(revision, 1, 9007199254740991)
        ranked = sorted(scores, key=lambda s: (-s['solved'], s['penaltyMinutes'], s['session']))
        rows, last, rank = [], None, 0
        for position, score in enumerate(ranked, 1):
            sporting = (score['solved'], score['penaltyMinutes'])
            if sporting != last:
                rank = position
                last = sporting
            rows.append(dict(rank=rank, displayName=score['displayName'], solved=score['solved'],
                             penaltyMinutes=score['penaltyMinutes']))
        return dict(roomId=row['room'], roundId=row['round'], revision=revision,
                    generatedTime=generated, freshness=freshness, rows=rows)

    def _create(self, body, now, probe):
        a, b = self.api, self.broker
        a.identifier(body['packageId'])
        a.hex_id(body['packageDigestHex'], 64)
        a.hex_id(body['gradingDigestHex'], 64)
        publication = body.get('publicationPolicy', 'teacher_only')
        if not isinstance(publication, str) or publication not in {'teacher_only', 'live'}:
            raise a.BrokerError('unsupported_policy')
        fingerprint = a.request_digest(dict(body, publicationPolicy=publication))
        old = b.db.execute('SELECT * FROM contest_rooms WHERE room=? OR round=?',
                            (body['roomId'], body['roundId'])).fetchone()
        if old:
            if old['owner'] != self.owner_id or old['create_hash'] != fingerprint:
                raise a.BrokerError('conflict', 409)
            return self._state(old, now)
        public = b._manifest(body)
        if public['sessionMode'] != 'contest':
            raise a.BrokerError('policy_mismatch')
        grading = b.db.execute('SELECT package_digest FROM grading WHERE digest=?', (body['gradingDigestHex'],)).fetchone()
        if not grading or grading['package_digest'] != body['packageDigestHex']:
            raise a.BrokerError('grading_unavailable', 409)
        if not b._runtime_matches(probe):
            raise a.BrokerError('runtime_mismatch', 409)
        # Deadline reconciliation is a lifecycle write, never a side effect of /state.
        b.db.execute("UPDATE contest_rooms SET state='Closed',revision=revision+1 WHERE state<>'Closed' AND deadline>0 AND deadline<=?", (now,))
        if b.db.execute("SELECT 1 FROM contest_rooms WHERE state<>'Closed'").fetchone():
            raise a.BrokerError('room_already_open', 409)
        if b.db.execute('SELECT count(*) FROM contest_rooms').fetchone()[0] >= 1024:
            raise a.BrokerError('room_quota', 503)
        b.db.execute('INSERT INTO contest_rooms(room,round,owner,package_id,package_digest,grading_digest,runtime,publication,duration,state,revision,last_clock,create_hash) '
                     'VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)', (body['roomId'], body['roundId'], self.owner_id,
                     body['packageId'], body['packageDigestHex'], body['gradingDigestHex'], probe[1],
                     publication, public['durationSeconds'], 'Draft', 1, now, fingerprint))
        return self._state(self._room(body), now)

    def _members(self, row, body, now, probe):
        a, b = self.api, self.broker
        if self._closed(row, now) or row['state'] == 'Running':
            raise a.BrokerError('room_closed', 409)
        action = body['action']
        extra = {'approve': {'displayName'}, 'remove': {'memberVersion'},
                 'prepare': {'memberVersion', 'packageDigestHex'}}.get(action) if isinstance(action, str) else None
        if extra is None:
            raise a.BrokerError('unknown_operation')
        allowed = ROOM_BASE | {'action', 'expectedRevision', 'context'} | extra
        a.fields(body, allowed, allowed)
        a.fields(body['context'], a.CONTEXT, a.CONTEXT)
        ctx = a.context(dict(body['context'], programmingVersion=1))
        _, key, session = b._session(dict(ctx, programmingVersion=1))
        member = b.db.execute('SELECT * FROM contest_members WHERE session=?', (key,)).fetchone()
        if (ctx['packageId'] != row['package_id'] or ctx['packageDigestHex'] != row['package_digest'] or
                session['grading_digest'] != row['grading_digest'] or session['mode'] != 'contest' or
                session['toolchain'] != row['runtime']):
            raise a.BrokerError('member_pin_mismatch', 409)
        if action == 'approve':
            a.text(body['displayName'], 64)
            if member or session['state'] != 'waiting':
                raise a.BrokerError('member_conflict', 409)
            all_members = b.db.execute('SELECT s.context,m.approved FROM contest_members m JOIN sessions s ON s.key=m.session WHERE m.room=?', (row['room'],)).fetchall()
            if sum(m['approved'] for m in all_members) >= MAX_MEMBERS:
                raise a.BrokerError('member_bounds')
            if len(all_members) >= MAX_MEMBER_HISTORY:
                raise a.BrokerError('member_history_bounds', 409)
            for m in all_members:
                old = a.decode_json(m['context'])
                if any(old[field] == ctx[field] for field in ['candidateId', 'clientIdHex', 'sessionIdHex']):
                    raise a.BrokerError('member_conflict', 409)
            b.db.execute('INSERT INTO contest_members VALUES(?,?,?,?,?,?)',
                         (key, row['room'], body['displayName'], 0, 1, 1))
        else:
            a.integer(body['memberVersion'], 1, 9007199254740991)
            if not member or member['room'] != row['room'] or not member['approved']:
                raise a.BrokerError('unknown_member', 404)
            if member['version'] != body['memberVersion']:
                raise a.BrokerError('stale_member', 409)
            self._runtime(row, probe, require_ready=False)
            if action == 'remove':
                # Keep the link tombstone so removed tuples cannot use generic Begin.
                b.db.execute('UPDATE contest_members SET approved=0,prepared=0,version=version+1 WHERE session=?', (key,))
                b.db.execute("UPDATE sessions SET state='closed' WHERE key=?", (key,))
            else:
                a.hex_id(body['packageDigestHex'], 64)
                if body['packageDigestHex'] != row['package_digest']:
                    raise a.BrokerError('package_mismatch', 409)
                b.db.execute('UPDATE contest_members SET prepared=1,version=version+1 WHERE session=?', (key,))
        counts = b.db.execute('SELECT count(*),sum(prepared) FROM contest_members WHERE room=? AND approved=1', (row['room'],)).fetchone()
        state = 'Prepared' if counts[0] and counts[0] == counts[1] and probe and probe[2] else 'Joining'
        b.db.execute('UPDATE contest_rooms SET state=?,revision=revision+1,last_clock=max(last_clock,?) WHERE room=?',
                     (state, now, row['room']))
        return self._state(self._room(body), now)

    def _runtime(self, row, probe, require_ready=True):
        if not self.broker._runtime_matches(probe) or probe[1] != row['runtime']:
            raise self.api.BrokerError('runtime_mismatch', 409)
        if require_ready and (not probe[1] or not probe[2]):
            raise self.api.BrokerError('runner_unavailable', 503)

    def _begin(self, row, body, now, regressed, probe):
        a, b = self.api, self.broker
        if self._closed(row, now) or row['state'] == 'Running' or regressed or now < row['last_clock']:
            raise a.BrokerError('room_closed', 409)
        members = b.db.execute('SELECT m.*,s.* FROM contest_members m JOIN sessions s ON s.key=m.session '
                               'WHERE m.room=? AND m.approved=1', (row['room'],)).fetchall()
        if not members or any(not m['prepared'] or m['state'] != 'waiting' or
                              m['toolchain'] != row['runtime'] or m['grading_digest'] != row['grading_digest'] for m in members):
            raise a.BrokerError('members_unprepared', 409)
        self._runtime(row, probe)
        deadline = now + row['duration'] * 1000
        b.db.execute("UPDATE contest_rooms SET state='Running',revision=revision+1,start=?,deadline=?,last_clock=? WHERE room=?",
                     (now, deadline, now, row['room']))
        for member in members:
            b.db.execute("UPDATE sessions SET state='active',start=?,deadline=?,last_clock=? WHERE key=?",
                         (now, deadline, now, member['session']))
        response = dict(self._state(self._room(body), now), requestId=body['requestId'])
        b.db.execute('UPDATE contest_rooms SET begin_hash=?,begin_response=? WHERE room=?',
                     (a.request_digest(body), a.canonical_json(response), row['room']))
        return response

    def _stop(self, row, body, now):
        a, b = self.api, self.broker
        b.db.execute("UPDATE contest_rooms SET state='Closed',revision=revision+1,last_clock=max(last_clock,?) WHERE room=?",
                     (now, row['room']))
        b.db.execute("UPDATE sessions SET state='closed' WHERE key IN (SELECT session FROM contest_members WHERE room=?)", (row['room'],))
        response = dict(self._state(self._room(body), now), requestId=body['requestId'])
        b.db.execute('UPDATE contest_rooms SET stop_hash=?,stop_response=? WHERE room=?',
                     (a.request_digest(body), a.canonical_json(response), row['room']))
        for member in b.db.execute('SELECT session FROM contest_members WHERE room=?', (row['room'],)):
            active = b.runs.get(member['session'])
            if active:
                active['cancel'].set()
        return response

    def linked(self, key):
        return self.broker.db.execute('SELECT 1 FROM contest_members WHERE session=?', (key,)).fetchone() is not None

    def authorize(self, key, operation, now):
        row = self.broker.db.execute('SELECT r.*,m.approved,s.state AS session_state FROM contest_members m '
                    'JOIN contest_rooms r ON r.room=m.room JOIN sessions s ON s.key=m.session WHERE m.session=?', (key,)).fetchone()
        if row is None:
            return
        if not row['approved']:
            raise self.api.BrokerError('member_revoked', 403)
        closed = self._closed(row, now) or row['session_state'] in {'closed', 'disrupted'}
        if closed and operation not in {'draft', 'submit', 'recover', 'finalize'}:
            raise self.api.BrokerError('recovery_only', 409)
        if not closed and row['state'] != 'Running' and operation != 'ready':
            raise self.api.BrokerError('attempt_not_begun', 409)

    def finalized(self, key):
        b = self.broker
        row = b.db.execute('SELECT room FROM contest_members WHERE session=?', (key,)).fetchone()
        if row and not b.db.execute("SELECT 1 FROM contest_members m JOIN sessions s ON s.key=m.session "
                                   "WHERE m.room=? AND m.approved=1 AND s.state<>'closed'", (row['room'],)).fetchone():
            b.db.execute("UPDATE contest_rooms SET state='Closed',revision=revision+1 WHERE room=? AND state<>'Closed'", (row['room'],))
