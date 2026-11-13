"""Trusted operator setup/lifecycle client; credentials travel over verified TLS."""
import argparse
import http.client
import os
import ssl
import sys
from pathlib import Path
from urllib.parse import urlsplit

from broker import BrokerError, canonical_json, decode_json

ROUTES = {'packages', 'grading', 'sessions', 'begin', 'stop', 'programming', 'validate-context'}


def post(url, route, body, token):
    origin = urlsplit(url)
    if origin.scheme != 'https' or not origin.hostname or origin.username or origin.password or origin.path not in {'', '/'} or origin.query or origin.fragment or route not in ROUTES:
        raise ValueError('HTTPS operator origin/allowlisted route required')
    if not isinstance(token, str) or len(token.encode()) < 32:
        raise ValueError('operator credential required')
    payload = canonical_json(body)
    if len(payload) > (4194304 if route == 'grading' else 131072):
        raise ValueError('operator request too large')
    connection = http.client.HTTPSConnection(origin.hostname, origin.port or 443, timeout=120,
                                             context=ssl.create_default_context())
    try:
        connection.request('POST', '/v1/' + route, payload,
                           {'Content-Type': 'application/json', 'Authorization': 'Bearer ' + token})
        response = connection.getresponse()
        raw = response.read(131073)
        if len(raw) > 131072:
            raise ValueError('operator response too large')
        result = decode_json(raw)
        if response.status != 200:
            raise ValueError('operator request refused')
        return result
    finally:
        connection.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--url', default=os.environ.get('NSTU_CS_BROKER_URL'))
    parser.add_argument('--route', choices=sorted(ROUTES), required=True)
    parser.add_argument('--body', required=True)
    args = parser.parse_args()
    try:
        with Path(args.body).open('rb') as stream:
            raw = stream.read(4194305)
        if len(raw) > 4194304:
            raise ValueError('operator file too large')
        print(canonical_json(post(args.url or '', args.route, decode_json(raw),
                                  os.environ.get('NSTU_CS_BROKER_TOKEN', ''))).decode())
    except (BrokerError, ValueError, OSError, http.client.HTTPException):
        print('Operator request failed. Check reviewed JSON, HTTPS trust, route and protected credential.', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
