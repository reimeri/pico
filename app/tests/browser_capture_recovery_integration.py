#!/usr/bin/env python3
"""Real CEF static-frame recovery after resize, without changing capture policy.
Run inside nix develop with a 180-second outer watchdog and sandbox enabled.
"""
import http.server
import json
from pathlib import Path
import signal
import sys
import tempfile
import threading

from browser_integration import Browser, png_pixel


class Fixture(http.server.BaseHTTPRequestHandler):
    pending = threading.Event()
    release = threading.Event()
    def log_message(self, *args): pass

    def do_GET(self):
        kind = 'text/html; charset=utf-8'
        if self.path == '/pending.js':
            self.pending.set(); self.release.wait()
            content = b'void 0;'; kind = 'text/javascript'
        elif self.path == '/loading':
            content = b'<!doctype html><body style="margin:0;background:rgb(23,45,67)"><script src="/pending.js"></script>'
        else:
            content = b'''<!doctype html><style>
body{margin:0;background:rgb(12,34,56)}button{position:absolute;left:10px;top:10px;width:120px;height:50px}
</style><button onclick="window.clicks=(window.clicks||0)+1;document.body.style.background='rgb(23,45,67)'">Change</button>'''
        self.send_response(200); self.send_header('Content-Type', kind)
        self.send_header('Cache-Control', 'no-store'); self.end_headers(); self.wfile.write(content)


def main():
    phase = 'startup'
    def interrupted(signum, frame): raise RuntimeError(f'capture recovery watchdog during {phase}')
    signal.signal(signal.SIGTERM, interrupted)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='pico-browser-capture-recovery-') as root:
        browser = Browser(str(Path(sys.argv[1]).resolve()), root)
        def inspect(token, kind, args=None, **fields):
            error, output = browser.inspect(token, kind, args, **fields)
            assert not error, output
            return output
        def diagnostics(token):
            return [json.loads(line) for line in inspect(token, 19, {}).splitlines()]
        def viewport(token, size): inspect(token, 22, {}, x=size[0], y=size[1], a=1)
        try:
            url = f'http://127.0.0.1:{server.server_port}/'
            # Retain the helper/page history of the real failure: a warm helper,
            # a new static page, then a smaller viewport without new DOM damage.
            browser.send(1, 1, text='about:blank', profile='aa1'); browser.wait(1, 1)
            browser.observe(1); browser.send(1, 2); browser.wait(1, 5)
            retries = 0
            for token, visible in ((2, False), (3, True)):
                phase = f'static resize, visible={visible}'
                browser.send(token, 1, text=url, profile=format(token, 'x')); browser.wait(token, 1)
                browser.send(token, 18, a=1); browser.send(token, 4, a=int(visible))
                viewport(token, (1280, 720)); browser.observe(token)
                for size in ((390, 844), (1080, 1500), (1280, 720)):
                    viewport(token, size)
                    # A renderer round-trip verifies the actual viewport and
                    # settles the resize without a readiness sleep or repaint.
                    assert json.loads(inspect(token, 21, {'expression':'[innerWidth,innerHeight]'}))['data']['result']['value'] == list(size)
                    observation = browser.observe(token)
                    assert observation[2] == size and png_pixel(observation[1], 200, 300) == (12, 34, 56)
                changed = browser.act(token, observation, [
                    (6, dict(x=40, y=30, a=0)), (6, dict(x=40, y=30, a=0, b=1, end_action=1)),
                ])
                assert not changed[0].error and png_pixel(changed[1], 200, 300) == (23, 45, 67), changed[0].error
                assert json.loads(inspect(token, 21, {'expression':'window.clicks'}))['data']['result']['value'] == 1, 'paint recovery replayed the click'
                records = diagnostics(token)
                retries += sum(r['kind'] == 'capture' and r['data']['event'] == 'paint_retry' for r in records[1:])
                for r in records[1:]:
                    if r['kind'] == 'capture' and r['data']['event'] == 'paint_retry':
                        d = r['data']
                        assert (d['priming_capture'] or d['capturing']) and not d['loading'] and not d['compositor_pending'] and not d['input_pending'], d
                        assert d['priming_capture'] or not d['fresh_view'] or (d['popup_shown'] and not d['fresh_popup']), d
                if not visible:
                    phase = 'cancel pending observation and replace its capture'
                    # Hold a load-blocking resource so completion cannot win a
                    # cross-process race against cancellation. Paint-wait timing
                    # itself is intentionally not controlled by a test hook.
                    browser.send(token, 1, text=url+'loading', profile=format(token, 'x'))
                    assert Fixture.pending.wait(10), 'load barrier was not reached'
                    browser.sequence += 1; request = browser.sequence
                    browser.send(token, 14, request=request)
                    browser.send(token, 17, request=request)
                    cancelled = browser.observation(token, request)
                    assert b'cancelled' in cancelled[0].error and not cancelled[0].uncertain, cancelled[0].error
                    Fixture.release.set()
                    viewport(token, (600, 900))
                    replacement = browser.observe(token)
                    assert replacement[2] == (600, 900) and png_pixel(replacement[1], 200, 300) == (23, 45, 67)
                    trace = [r['data'] for r in diagnostics(token)[1:] if r['kind'] == 'capture' and r['data']['request_id'] == format(request, 'x')]
                    assert trace[-1]['event'] == 'finish' and trace[-1]['outcome'] == 'error', trace
                browser.send(token, 2); browser.wait(token, 5)
            # Whether retries were needed depends on Chromium's transient resize
            # readback race. Success must not depend on reproducing that race.
            print(f'PASS: static hidden/visible resize, fresh pixels, no input replay, cancellation; observed {retries} paint retries')
        finally:
            Fixture.release.set(); browser.close()
    server.shutdown(); server.server_close()


if __name__ == '__main__': main()
