import unittest
import http.client
from types import SimpleNamespace
from unittest.mock import patch

try:
    import operator_client
except ModuleNotFoundError:
    operator_client = None


class OperatorTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(operator_client, 'operator setup client is missing')

    def test_secret_is_never_sent_to_http_or_credentialed_url(self):
        for url in ['http://judge.example', 'https://user:pass@judge.example', 'https://judge.example/path']:
            with self.subTest(url=url), self.assertRaises(ValueError):
                operator_client.post(url, 'packages', {}, 'x' * 32)

    def test_unallowlisted_route_and_missing_secret_fail_before_network(self):
        for route, token in [('private-results', 'x' * 32), ('packages', '')]:
            with self.subTest(route=route), self.assertRaises(ValueError):
                operator_client.post('https://judge.example', route, {}, token)

    def test_valid_operator_request_returns_bounded_server_receipt(self):
        response = SimpleNamespace(status=200, read=lambda maximum: b'{"status":"registered"}')
        with patch.object(http.client.HTTPSConnection, 'request'), patch.object(
                http.client.HTTPSConnection, 'getresponse', return_value=response):
            result = operator_client.post('https://judge.example', 'packages', {}, 'x' * 32)
        self.assertEqual(result, {'status': 'registered'})

    def test_context_validation_uses_authenticated_fixed_post_route(self):
        response = SimpleNamespace(status=200, read=lambda maximum: b'{"status":"known"}')
        with patch.object(http.client.HTTPSConnection, 'request') as request, patch.object(
                http.client.HTTPSConnection, 'getresponse', return_value=response):
            self.assertEqual(operator_client.post('https://judge.example', 'validate-context', {}, 'x' * 32), {'status': 'known'})
        self.assertEqual(request.call_args.args[:2], ('POST', '/v1/validate-context'))
        self.assertEqual(request.call_args.args[3]['Authorization'], 'Bearer ' + 'x' * 32)


if __name__ == '__main__':
    unittest.main()
