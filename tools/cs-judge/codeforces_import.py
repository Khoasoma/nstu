"""Teacher preparation only: public Codeforces statement and canonical samples."""
import argparse
import datetime
import html
import http.client
import ipaddress
import json
import queue
import re
import socket
import ssl
import sys
import time
import threading
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urlsplit

MAX_HTML = 2 * 1024 * 1024


class ImportError(ValueError):
    pass


def problem_url(contest, index):
    if not re.fullmatch(r'[1-9][0-9]{0,6}', str(contest)) or not re.fullmatch(r'[A-Z][A-Z0-9]{0,3}', index):
        raise ImportError('unsupported public problem identity')
    return f'https://codeforces.com/problemset/problem/{contest}/{index}'


def parse_url(value):
    parsed = urlsplit(value)
    if parsed.scheme != 'https' or parsed.hostname != 'codeforces.com' or parsed.username or parsed.password or parsed.port not in {None, 443} or parsed.query or parsed.fragment:
        raise ImportError('only public HTTPS codeforces.com problem routes are supported')
    match = re.fullmatch(r'/(?:problemset/problem/([1-9][0-9]{0,6})/([A-Z][A-Z0-9]{0,3})|contest/([1-9][0-9]{0,6})/problem/([A-Z][A-Z0-9]{0,3}))', parsed.path)
    if not match:
        raise ImportError('unsupported public problem route; private/gym routes are not imported')
    return (match[1], match[2]) if match[1] else (match[3], match[4])


def fetch_public(contest, index):
    url = problem_url(contest, index)
    answer = queue.Queue(maxsize=1)
    def resolve():
        try:
            answer.put(socket.getaddrinfo('codeforces.com', 443, type=socket.SOCK_STREAM))
        except OSError:
            answer.put(None)
    threading.Thread(target=resolve, daemon=True).start()
    try:
        addresses = answer.get(timeout=5)
    except queue.Empty:
        raise ImportError('provider DNS exceeded time budget') from None
    if not addresses or any(not ipaddress.ip_address(item[4][0]).is_global for item in addresses):
        raise ImportError('provider resolved to a nonpublic address')
    # Connect to the already verified address; TLS still verifies the canonical hostname.
    tls = ssl.create_default_context()
    connection = http.client.HTTPSConnection('codeforces.com', timeout=10, context=tls)
    address = addresses[0][4][0]
    raw_socket = socket.create_connection((address, 443), timeout=10)
    try:
        connection.sock = tls.wrap_socket(raw_socket, server_hostname='codeforces.com')
        connection.request('GET', urlsplit(url).path, headers={'Host': 'codeforces.com',
                           'User-Agent': 'NSTU teacher preparation', 'Accept-Encoding': 'identity'})
        response = connection.getresponse()
        if response.status != 200:
            raise ImportError(f'provider returned HTTP {response.status}; use authorized manual import or retry later')
        if response.getheader('Content-Encoding', 'identity').lower() != 'identity' or not response.getheader('Content-Type', '').lower().startswith('text/html'):
            raise ImportError('unsupported provider content encoding/type')
        length = response.getheader('Content-Length')
        if length and (not length.isdecimal() or int(length) > MAX_HTML):
            raise ImportError('provider statement exceeds size limit')
        started, data = time.monotonic(), bytearray()
        while True:
            chunk = response.read(4096)
            if not chunk:
                break
            if len(data) + len(chunk) > MAX_HTML or time.monotonic() - started > 30:
                raise ImportError('provider statement exceeds byte/time budget')
            data.extend(chunk)
        return bytes(data).decode('utf-8')
    except (OSError, UnicodeError, http.client.HTTPException) as error:
        raise ImportError('provider retrieval unavailable; retain draft and use authorized manual HTML import') from error
    finally:
        connection.close()
        raw_socket.close()


class StatementParser(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=False)
        self.stack = []
        self.statement = []
        self.fields = {}
        self.samples = {'input': [], 'output': []}
        self.pre = None
        self.skipped = 0
        self.found = False

    def handle_starttag(self, tag, attrs):
        classes = dict(attrs).get('class', '').split()
        in_statement = any('problem-statement' in c for _, c in self.stack) or 'problem-statement' in classes
        if tag in {'script', 'style', 'iframe', 'form', 'object', 'svg'}:
            self.skipped += 1
        self.stack.append((tag, classes))
        if 'problem-statement' in classes:
            self.found = True
        if not in_statement or self.skipped:
            return
        if tag == 'pre':
            sides = [side for _, cls in self.stack for side in ['input', 'output'] if side in cls]
            if sides:
                if self.pre is not None or len(sides) != 1:
                    raise ImportError('ambiguous nested public samples')
                self.pre = [sides[-1], []]
        if tag == 'br':
            self._data('\n')
        elif tag in {'p', 'li'} and self.pre is None:
            self.statement.append('\n')
        if tag in {'br', 'img', 'hr', 'input', 'meta', 'link'}:
            self.stack.pop()

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        if tag not in {'br', 'img', 'hr', 'input', 'meta', 'link'}:
            self.handle_endtag(tag)

    def handle_endtag(self, tag):
        if tag in {'script', 'style', 'iframe', 'form', 'object', 'svg'}:
            self.skipped = max(0, self.skipped - 1)
        if not self.stack:
            return
        position = next((i for i in range(len(self.stack) - 1, -1, -1) if self.stack[i][0] == tag), None)
        if position is None:
            return
        _, classes = self.stack[position]
        if self.pre and tag == 'div' and 'test-example-line' in classes:
            self.pre[1].append('\n')
        if self.pre and tag == 'pre':
            self.samples[self.pre[0]].append(''.join(self.pre[1]))
            self.pre = None
        if tag in {'p', 'li'} and self.pre is None:
            self.statement.append('\n')
        del self.stack[position:]

    def _data(self, data):
        if self.skipped or not any('problem-statement' in cls for _, cls in self.stack):
            return
        if self.pre:
            self.pre[1].append(data)
            return
        for _, classes in self.stack:
            for key in ['title', 'time-limit', 'memory-limit', 'input-file', 'output-file']:
                if key in classes:
                    self.fields[key] = self.fields.get(key, '') + data
                    return
        if not any('sample-tests' in cls for _, cls in self.stack):
            self.statement.append(data)

    def handle_data(self, data):
        self._data(data)

    def handle_entityref(self, name):
        self._data(html.unescape('&' + name + ';'))

    def handle_charref(self, name):
        self._data(html.unescape('&#' + name + ';'))


def prepare(contest, index, source):
    url = problem_url(contest, index)
    if not isinstance(source, str) or len(source.encode()) > MAX_HTML:
        raise ImportError('statement exceeds UTF-8 input limit')
    parser = StatementParser()
    parser.feed(source)
    parser.close()
    if parser.pre is not None:
        raise ImportError('unfinished public sample markup requires teacher correction')
    statement = ''.join(parser.statement).strip()
    title = parser.fields.get('title', '').strip()
    times = re.search(r'([0-9]+(?:\.[0-9]+)?)\s*seconds?', parser.fields.get('time-limit', ''))
    memory = re.search(r'([0-9]+)\s*megabytes?', parser.fields.get('memory-limit', ''))
    if not parser.found or not statement or not title or not times or not memory:
        raise ImportError('supported statement/title/limits were not found; review an authorized manual draft')
    if re.search(r'\binteractive problem\b|\bthis is an interactive\b', statement, re.I):
        raise ImportError('interactive judging is unsupported')
    for key, label, shorthand in [('input-file', 'standard input', 'stdin'), ('output-file', 'standard output', 'stdout')]:
        declared = parser.fields.get(key, '').lower().strip()
        heading = 'input' if key == 'input-file' else 'output'
        if declared not in {label, shorthand, heading + label, heading + shorthand}:
            raise ImportError('only standard input/output batch tasks are supported')
    inputs, outputs = parser.samples['input'], parser.samples['output']
    if not 1 <= len(inputs) == len(outputs) <= 8:
        raise ImportError('public input/output samples are missing or unpaired; teacher correction is required')
    problem = dict(id=f'cf-{contest}-{index}', type='programming', revision=1, prompt=statement,
                   points=100, timeLimitMs=round(float(times[1]) * 1000), memoryLimitMiB=int(memory[1]),
                   samples=[dict(id=f'sample-{i+1}', input=a, output=b) for i, (a, b) in enumerate(zip(inputs, outputs))])
    manifest = dict(id=f'cf-{contest}-{index}', title=title, durationSeconds=3600, contentMode='computer_science',
                    programmingVersion=1, sessionMode='exam', languages=['cpp20', 'python3'], questions=[problem])
    from broker import BrokerError, validate_manifest
    try:
        validate_manifest(manifest)
    except BrokerError as error:
        raise ImportError('imported content/limits exceed supported package bounds: ' + error.code) from None
    return dict(manifest=manifest, provenance=dict(provider='Codeforces', contestId=str(contest), index=index,
                originalUrl=url, importedUtc=datetime.datetime.now(datetime.timezone.utc).isoformat()),
                requiresReview=True, blockers=['private_grading_not_attached', 'teacher_sample_review_required'])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--url', required=True)
    parser.add_argument('--html', help='authorized instructor-supplied HTML instead of provider retrieval')
    parser.add_argument('--output', required=True)
    parser.add_argument('--acknowledge-content-rights', action='store_true')
    args = parser.parse_args()
    try:
        if not args.acknowledge_content_rights:
            raise ImportError('instructor must confirm access/distribution rights before preparing this content')
        contest, index = parse_url(args.url)
        if args.html:
            with Path(args.html).open('rb') as stream:
                raw = stream.read(MAX_HTML + 1)
            if len(raw) > MAX_HTML:
                raise ImportError('manual HTML exceeds size limit')
            source = raw.decode('utf-8')
        else:
            source = fetch_public(contest, index)
        result = prepare(contest, index, source)
        path = Path(args.output)
        # Never overwrite a teacher-corrected review/draft on a failed or repeated import.
        with path.open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(result, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
    except (ImportError, OSError, UnicodeError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
