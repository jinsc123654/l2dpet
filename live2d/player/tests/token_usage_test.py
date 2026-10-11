"""Run a compiled token_usage_test against a local-only fake provider."""
import http.server
import json
import os
import subprocess
import sys
import threading

requests = []

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        requests.append(body)
        truncated = len(requests) == 2
        response = json.dumps({
            'choices': [{'message': {'content': '{"steps":[]}'},
                         'finish_reason': 'length' if truncated else 'stop'}],
            'usage': {'prompt_tokens': 150 if truncated else 100,
                      'completion_tokens': 30 if truncated else 20},
        }).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(response)))
        self.end_headers()
        self.wfile.write(response)

with http.server.HTTPServer(('127.0.0.1', 0), Handler) as server:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    env = dict(os.environ, LLM_ALLOW_PRIVATE='1', NO_PROXY='127.0.0.1', no_proxy='127.0.0.1')
    try:
        subprocess.run([sys.argv[1], f'http://127.0.0.1:{server.server_port}'],
                       env=env, check=True, timeout=20)
        assert len(requests) == 2
        assert requests[0]['max_tokens'] == 768
        assert 'max_tokens' not in requests[1]
        print('PASS actual HTTP request cap and maintenance budget isolation')
    finally:
        server.shutdown()
        thread.join()
