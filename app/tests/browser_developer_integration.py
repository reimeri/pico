#!/usr/bin/env python3
"""Real embedded CEF inspection, complete file output and developer/HMR loop.
Run inside nix develop with a 120-second outer watchdog and sandbox enabled.
"""
import http.server
import json
import resource
import signal
import socket
import sys
import tempfile
import threading
from pathlib import Path
from browser_integration import Browser, png_pixel

DIAGNOSTICS, INSPECT, EVALUATE, VIEWPORT = 19, 20, 21, 22

class Fixture(http.server.BaseHTTPRequestHandler):
    script = None
    updated = threading.Event()
    watching = threading.Event()
    def log_message(self, *args): pass
    def do_GET(self):
        if self.path == '/offline':
            self.connection.shutdown(socket.SHUT_RDWR); self.connection.close(); return
        if self.path.startswith('/app.js'):
            content = self.script.read_bytes(); kind = 'text/javascript'
        elif self.path == '/failure':
            self.send_response(503); self.end_headers(); return
        elif self.path == '/hmr':
            self.send_response(200); self.send_header('Content-Type', 'text/event-stream'); self.end_headers()
            self.wfile.write(b': connected\n\n'); self.wfile.flush(); self.watching.set()
            if self.updated.wait(60):
                try: self.wfile.write(b'data: update\n\n'); self.wfile.flush()
                except OSError: pass
            return
        else:
            kind = 'text/html; charset=utf-8'
            content = b'''<!doctype html><style>body{margin:0;background:rgb(12,34,56)}</style>
<button id="bug" aria-label="Bug fixture">Loading</button><div id="large"></div>
<script>console.log('earliest-page-console');
window.hotReloaded = new Promise(resolve => { window.hotResolve = resolve; });
new EventSource('/hmr').onmessage = async()=>{await import('/app.js?hot'); hotResolve();};
</script><script type="module" src="/app.js"></script>'''
        self.send_response(200); self.send_header('Content-Type', kind)
        self.send_header('Cache-Control', 'no-store'); self.end_headers(); self.wfile.write(content)


def main():
    phase = 'startup'
    def interrupted(signum, frame): raise RuntimeError(f'developer fixture watchdog during {phase}')
    signal.signal(signal.SIGTERM, interrupted)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='pico-browser-developer-') as root:
        Fixture.script = Path(root) / 'app.js'
        Fixture.script.write_text("document.querySelector('#bug').textContent='Broken'; console.error('bug: incorrect label');")
        browser = Browser(str(Path(sys.argv[1]).resolve()), root)
        def call(kind, args=None, token=1, **fields):
            error, output = browser.inspect(token, kind, args, **fields)
            assert not error, output
            return output
        def evaluate(expression, token=1):
            return json.loads(call(EVALUATE, {'expression':expression}, token))['data']['result'].get('value')
        def diagnostics(cursor=None, token=1):
            return [json.loads(line) for line in call(DIAGNOSTICS, {'cursor':cursor} if cursor else {}, token).splitlines()]
        journal = None
        try:
            url = f'http://127.0.0.1:{server.server_port}'
            browser.send(1, 1, text=url, profile='aaa1'); browser.wait(1, 1)
            original = browser.observe(1)
            phase = 'read-only User control and creation-time diagnostics'
            tree = json.loads(call(INSPECT, {'mode':'accessibility'}))
            assert any(n.get('name', {}).get('value') == 'Bug fixture' for n in tree['data']['nodes'])
            dom = json.loads(call(INSPECT, {'mode':'dom','selector':'#bug'}))
            assert 'Broken' in dom['data']['outerHTML']
            records = diagnostics(); journal = Path(records[0]['source_path'])
            assert records[0]['complete'] and any('earliest-page-console' in json.dumps(r) for r in records[1:])
            assert any('bug: incorrect label' in json.dumps(r) for r in records[1:])
            error, text = browser.inspect(1, EVALUATE, {'expression':"document.body.innerHTML='unauthorized'"})
            assert error and 'User control' in text
            error, text = browser.inspect(1, VIEWPORT, x=390, y=844, a=1)
            assert error and 'User control' in text
            browser.send(1, 18, a=1)
            phase = 'network failures and uncaught exceptions'
            assert evaluate("Promise.all([fetch('/failure'), fetch('/offline').catch(()=>null)]).then(()=>true)")
            evaluate("new Promise(resolve=>{addEventListener('error',()=>resolve(true),{once:true});setTimeout(()=>{throw new Error('uncaught-fixture')},0)})")
            while True:
                records = diagnostics()
                errors = [r for r in records[1:] if r['kind'] == 'network_failure']
                if any(r['data']['status'] == 503 and r['data']['url'].endswith('/failure') for r in errors) and \
                   any(r['data']['outcome'] == 'failed' and r['data']['url'].endswith('/offline') for r in errors) and \
                   any(r['kind'] == 'exception' and 'uncaught-fixture' in json.dumps(r) for r in records[1:]): break
            assert all('headers' not in r['data'] and 'body' not in r['data'] for r in errors)
            cursor = records[0]['cursor']
            phase = 'complete large output, cursor and browser isolation'
            large = 'START-' + 'x' * 200000 + '-END-🙂'
            assert evaluate("'START-'+'x'.repeat(200000)+'-END-🙂'") == large
            evaluate("console.log('START-'+'x'.repeat(200000)+'-END-🙂'); document.querySelector('#large').textContent='START-'+'x'.repeat(200000)+'-END-🙂'; true")
            delta = diagnostics(cursor)
            assert any(a.get('value') == large for r in delta[1:] if r['kind'] == 'console' for a in r['data'].get('args', []))
            assert not any('earliest-page-console' in json.dumps(r) for r in delta[1:])
            assert large in json.loads(call(INSPECT, {'mode':'dom', 'selector':'#large'}))['data']['outerHTML']
            assert diagnostics(cursor) == delta, 'reads must not consume diagnostic history'
            browser.send(2, 1, text='about:blank', profile='bbb2'); browser.wait(2, 1)
            assert not any('START-' in json.dumps(r) for r in diagnostics(token=2))
            error, text = browser.inspect(2, DIAGNOSTICS, {'cursor':cursor})
            assert error and 'different browser' in text
            phase = 'fixed viewport and stale identity semantics'
            call(VIEWPORT, x=390, y=844, a=1)
            assert evaluate('[innerWidth,innerHeight,devicePixelRatio]') == [390,844,1]
            mobile = browser.observe(1); assert mobile[2] == (390,844)
            call(VIEWPORT, x=390, y=844, a=1)
            same = browser.act(1, mobile, [(6,dict(x=350,y=800,a=-1,end_action=1))])
            assert not same[0].error
            stale = browser.act(1, original, [(6,dict(x=20,y=20,a=-1,end_action=1))])
            assert b'Stale' in stale[0].error
            phase = 'edit app and hot reload in the same browser'
            assert Fixture.watching.wait(5)
            identity = mobile[0].navigation
            Fixture.script.write_text("document.querySelector('#bug').textContent='Fixed'; document.body.style.background='rgb(65,43,21)'; console.info('bug fixed by hot reload');")
            Fixture.updated.set()
            assert evaluate("hotReloaded.then(()=>document.querySelector('#bug').textContent)") == 'Fixed'
            fixed = browser.observe(1)
            assert fixed[0].navigation == identity and png_pixel(fixed[1],300,500) == (65,43,21)
            assert 'Fixed' in json.loads(call(INSPECT, {'mode':'dom','selector':'#bug'}))['data']['outerHTML']
            phase = 'history across navigation and interrupted evaluation'
            browser.send(1, 1, text='about:blank', profile='aaa1'); browser.observe(1)
            assert any('earliest-page-console' in json.dumps(r) for r in diagnostics()[1:])
            phase = 'explicit incomplete collection on storage failure'
            limit = resource.prlimit(browser.process.pid, resource.RLIMIT_FSIZE)
            try:
                resource.prlimit(browser.process.pid, resource.RLIMIT_FSIZE, (journal.stat().st_size, limit[1]))
                evaluate("console.log('cannot-append-to-full-journal'); true")
            finally:
                resource.prlimit(browser.process.pid, resource.RLIMIT_FSIZE, limit)
            error, text = browser.inspect(1, DIAGNOSTICS)
            partial = [json.loads(line) for line in text.splitlines()]
            assert error and not partial[0]['complete'] and partial[0]['collection_error']
            assert any('earliest-page-console' in json.dumps(r) for r in partial[1:]), 'retain previously collected data'
            phase = 'interrupted evaluation'
            # An unresolved evaluation must not survive takeover and later mutate
            # a page the human now controls.
            error, text = browser.inspect(1, EVALUATE, {'expression':'new Promise(()=>{})'}, interrupt=18)
            assert error and 'page closed' in text
            browser.wait(1, 5)
            assert journal.exists() and 'earliest-page-console' in journal.read_text()
            assert diagnostics(token=2)[0]['complete']
            print('PASS: diagnostics, accessibility/DOM, complete files, evaluation control, fixed viewport, and same-page HMR')
        finally:
            Fixture.updated.set(); browser.close()
    server.shutdown()

if __name__ == '__main__': main()
