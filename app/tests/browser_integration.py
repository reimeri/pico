#!/usr/bin/env python3
"""Real CEF OSR test (sandbox/user namespaces required, no Pico window).
Run inside nix develop:
  python3 app/tests/browser_integration.py app/build/debug/browser/runtime/pico_browser_helper
A CTest/manual outer timeout should guard a hung browser, not page-speed guesses.
"""
import ctypes
import http.server
from pathlib import Path
import queue
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import urllib.parse

# Mirror the private same-build IPC layout, using C alignment rather than a
# hand-maintained wire size. All interactions exercise the production helper.
class Command(ctypes.Structure):
    _fields_ = [("token", ctypes.c_uint64), ("type", ctypes.c_uint32)] + [
        (n, ctypes.c_int32) for n in ("x", "y", "a", "b", "modifiers")
    ] + [("text", ctypes.c_char * 2048), ("profile", ctypes.c_char * 80)]

reports = queue.Queue()
HTML = b'''<!doctype html><style>body{margin:0;background:rgb(12,34,56)}input{position:absolute;top:10px;left:10px;width:300px;height:40px}</style>
<input id="field" oninput="fetch('/input?value='+encodeURIComponent(this.value))">
<a href="/terms" style="position:absolute;left:10px;top:100px;width:200px;height:50px;background:white" onclick="event.preventDefault();navigate()">Terms</a>
<script>
const q = new URLSearchParams(location.search), name = q.get('name');
fetch('/report?'+new URLSearchParams({name,local:localStorage.getItem('value')||'',cookie:document.cookie}));
if(q.has('set')) { localStorage.setItem('value',q.get('set')); document.cookie='value='+q.get('set')+';max-age=3600;path=/'; }
setInterval(()=>fetch('/tick?name='+name),200);
function navigate() {
  history.pushState({}, '', '/terms');
  document.body.innerHTML = '<main style="color:white"><h1>Terms of service</h1><p>' +
    'New client-side page content. '.repeat(500) + '</p></main>';
  fetch('/report?name=navigated');
  setTimeout(() => {
    document.body.style.background = 'rgb(65,43,21)';
    fetch('/report?name=delayed');
  }, 12000);
}
</script>'''
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        if url.path in ('/report', '/input', '/tick'):
            reports.put((url.path, urllib.parse.parse_qs(url.query, keep_blank_values=True)))
            self.send_response(204); self.end_headers()
        else:
            self.send_response(200); self.send_header('Content-Type','text/html; charset=utf-8')
            self.end_headers(); self.wfile.write(HTML)
    def log_message(self, *args): pass

class Browser:
    def __init__(self, helper, root):
        self.commands, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        events, child_events = socket.socketpair()
        self.events = queue.Queue()
        self.process = subprocess.Popen([helper,str(child.fileno()),str(child_events.fileno()),root],
                                        pass_fds=(child.fileno(),child_events.fileno()))
        child.close(); child_events.close()
        def receive():
            try:
                while True:
                    header = events.recv(24, socket.MSG_WAITALL)
                    if not header: break
                    assert len(header) == 24
                    token, kind, length, width, height = struct.unpack('=QIIII',header)
                    data = events.recv(length, socket.MSG_WAITALL) if length else b''
                    assert len(data) == length
                    # Retain only metadata: do not enqueue unbounded video pixels.
                    self.events.put((token, kind, width, height, data[:100] if kind != 2 else b''))
            finally:
                events.close(); self.events.put(None)
        self.reader = threading.Thread(target=receive, daemon=True); self.reader.start()
    def send(self, token, kind, **kwargs):
        c = Command(token=token, type=kind)
        for k,v in kwargs.items(): setattr(c,k,v.encode() if isinstance(v,str) else v)
        self.commands.send(bytes(c))
    def wait(self, token, kind):
        while True:
            event = self.events.get()
            if event is None: raise RuntimeError(f'helper exited before event {kind}; inspect stderr/cef.log')
            if event[0] == token and event[1] == kind: return event
            if event[1] == 4: raise RuntimeError(event[-1].decode(errors='replace'))
    def close(self):
        if self.process.poll() is None:
            try: self.send(0,13)
            except OSError: pass
        try: self.process.wait(timeout=10)
        except subprocess.TimeoutExpired: self.process.kill(); self.process.wait(); raise
        self.commands.close(); self.reader.join()
        assert self.process.returncode == 0, self.process.returncode

def report(path, name=None, browser=None):
    while True:
        try:
            p, values = reports.get(timeout=0.1)
        except queue.Empty:
            # A crashed helper cannot produce another report. Do not turn a
            # useful crash into an apparent page-load hang until the watchdog.
            if browser and browser.process.poll() is not None:
                raise RuntimeError(f'helper exited ({browser.process.returncode}) waiting for {path}: {name}')
            continue
        if p == path and (name is None or values.get('name') == [name]): return values

def main():
    helper = str(Path(sys.argv[1]).resolve())
    # Let finally blocks close Chromium even when an outer test watchdog fires.
    import signal
    def interrupted(signum, frame): raise RuntimeError('browser test interrupted')
    signal.signal(signal.SIGTERM, interrupted)
    server = http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    url = f'http://127.0.0.1:{server.server_port}'
    with tempfile.TemporaryDirectory(prefix='pico-browser-test-') as root:
        b = Browser(helper,root)
        try:
            b.send(1,1,text=url+'/?name=a&set=alpha',profile='aaaa')
            b.send(1,4,a=1)
            b.wait(1,1)
            first = report('/report','a'); assert first['local'] == [''] and first['cookie'] == ['']
            b.wait(1,2)
            # The same root must fail closed in another Pico process, not
            # launch CEF's default Chrome window via singleton relaunch.
            second_owner = Browser(helper,root)
            error = second_owner.events.get()
            assert error and error[1] == 4 and b'in use' in error[-1], error
            assert second_owner.process.wait(timeout=10) != 0
            second_owner.commands.close(); second_owner.reader.join()
            b.send(2,1,text=url+'/?name=b&set=beta',profile='bbbb')
            second = report('/report','b'); assert second['local'] == [''] and second['cookie'] == ['']
            b.send(1,5,a=1) # focus
            b.send(1,6,x=40,y=30,a=0,b=0); b.send(1,6,x=40,y=30,a=0,b=1)
            b.send(1,9,text='Hello é')
            while report('/input')['value'] != ['Hello é']: pass
            b.send(1,6,x=40,y=120,a=0,b=0); b.send(1,6,x=40,y=120,a=0,b=1)
            report('/report','navigated',browser=b)
            # Soft-navigation metrics are dispatched asynchronously, after the
            # click/URL change. A delayed application update must still paint;
            # merely receiving the new URL would miss CEF #4234's later crash.
            report('/report','delayed',browser=b)
            b.send(1,3,x=640,y=480)
            while b.wait(1,2)[2:4] != (640,480): pass
            b.send(1,4,a=0)
            # Navigate while hidden, rather than accidentally accepting a tick
            # that was already buffered before the hide command.
            b.send(1,1,text=url+'/?name=hidden',profile='aaaa')
            report('/report','hidden')
            report('/tick','hidden') # hidden pages still execute application work
            b.send(1,2); b.wait(1,5)
            b.send(3,1,text=url+'/?name=restored',profile='aaaa')
            restored = report('/report','restored')
            assert restored['local'] == ['alpha'] and 'value=alpha' in restored['cookie'][0], restored
            b.send(3,4,a=1); b.send(3,3,x=420,y=280)
            while b.wait(3,2)[2:4] != (420,280): pass
        finally: b.close()
        # Reopening the helper must not reopen pages; explicit navigation uses
        # the same persisted session profile, without sharing it with session B.
        b = Browser(helper,root)
        try:
            b.send(4,1,text=url+'/?name=restarted',profile='bbbb')
            persisted = report('/report','restarted')
            assert persisted['local'] == ['beta'] and 'value=beta' in persisted['cookie'][0], persisted
        finally: b.close()
    server.shutdown()
    print('PASS: embedded frames, mouse/text, hidden progress, session isolation, resize, singleton rejection, persistent profiles, and delayed SPA navigation')
if __name__ == '__main__': main()
