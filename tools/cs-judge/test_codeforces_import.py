import unittest
from unittest.mock import patch

try:
    import codeforces_import
except ModuleNotFoundError:
    codeforces_import = None


HTML = '''<div class="problem-statement"><div class="header"><div class="title">A. Addition</div>
<div class="time-limit">time limit per test1 second</div>
<div class="memory-limit">memory limit per test64 megabytes</div>
<div class="input-file">inputstandard input</div><div class="output-file">outputstandard output</div></div>
<p>Add the numbers.</p><div class="sample-tests"><div class="input"><pre><div class="test-example-line">1 2</div><div class="test-example-line">3 &amp; 4  </div></pre></div>
<div class="output"><pre>3<br/>7  <br/></pre></div></div><script>SECRET_BAD_SCRIPT()</script></div>'''


class ImportTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(codeforces_import, 'Codeforces preparation adapter is missing')

    def test_only_supported_public_codeforces_routes_are_accepted(self):
        self.assertEqual(codeforces_import.problem_url('1', 'A'), 'https://codeforces.com/problemset/problem/1/A')
        for value in ['https://localhost/1/A', 'https://codeforces.com.evil.test/problemset/problem/1/A',
                      'https://user:password@codeforces.com/problemset/problem/1/A',
                      'https://codeforces.com/gym/123/problem/A']:
            with self.subTest(value=value), self.assertRaises(codeforces_import.ImportError):
                codeforces_import.parse_url(value)

    def test_exact_public_samples_are_extracted_without_trim(self):
        result = codeforces_import.prepare('1', 'A', HTML)
        samples = result['manifest']['questions'][0]['samples']
        self.assertEqual(samples[0]['input'], '1 2\n3 & 4  \n')
        self.assertEqual(samples[0]['output'], '3\n7  \n')
        self.assertNotIn('SECRET_BAD_SCRIPT', result['manifest']['questions'][0]['prompt'])
        self.assertNotIn('solvedCount', str(result['manifest']))
        self.assertIn('private_grading_not_attached', result['blockers'])
        self.assertTrue(result['requiresReview'])

    def test_missing_sample_and_unsupported_interactive_task_never_export_as_runnable(self):
        for html in [HTML.replace('<div class="output"><pre>3<br/>7  <br/></pre></div>', ''),
                     HTML.replace('Add the numbers.', 'This is an interactive problem.')]:
            with self.assertRaises(codeforces_import.ImportError):
                codeforces_import.prepare('1', 'A', html)

    def test_empty_expected_output_is_preserved(self):
        html = HTML.replace('3<br/>7  <br/>', '')
        result = codeforces_import.prepare('1', 'A', html)
        self.assertEqual(result['manifest']['questions'][0]['samples'][0]['output'], '')

    def test_current_stdin_stdout_labels_are_supported(self):
        source = HTML.replace('inputstandard input', 'inputstdin').replace('outputstandard output', 'outputstdout')
        result = codeforces_import.prepare('1', 'A', source)
        self.assertEqual(result['manifest']['questions'][0]['samples'][0]['input'], '1 2\n3 & 4  \n')

    def test_similar_named_file_labels_and_ambiguous_samples_are_rejected(self):
        for source in [HTML.replace('inputstandard input', 'inputsecret-stdin'),
                       HTML.replace('outputstandard output', 'outputanswer-stdout'),
                       HTML.replace('<div class="input"><pre>', '<div class="input output"><pre>')]:
            with self.subTest(source=source), self.assertRaises(codeforces_import.ImportError):
                codeforces_import.prepare('1', 'A', source)

    def test_any_private_dns_address_is_rejected_before_connection(self):
        addresses = [(2, 1, 6, '', ('8.8.8.8', 443)), (2, 1, 6, '', ('127.0.0.1', 443))]
        with patch('socket.getaddrinfo', return_value=addresses), patch('socket.create_connection') as connect:
            with self.assertRaises(codeforces_import.ImportError):
                codeforces_import.fetch_public('1', 'A')
            connect.assert_not_called()


if __name__ == '__main__':
    unittest.main()
