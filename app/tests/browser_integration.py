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
import zlib

# Mirror the private same-build IPC layout, using C alignment rather than a
# hand-maintained wire size. All interactions exercise the production helper.
class Command(ctypes.Structure):
    _fields_ = [("token", ctypes.c_uint64), ("type", ctypes.c_uint32)] + [
        (n, ctypes.c_int32) for n in ("x", "y", "a", "b", "modifiers")
    ] + [("text", ctypes.c_char * 2048), ("profile", ctypes.c_char * 80)] + [(n, ctypes.c_uint64) for n in ("request", "navigation", "viewport")] + [("end_action", ctypes.c_uint32)]

class Observation(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint64) for n in ("request", "navigation", "viewport")] + [
        (n, ctypes.c_uint32) for n in ("completed", "png_size", "delivered_events", "partial_action", "uncertain")
    ] + [("url", ctypes.c_char * 2048), ("error", ctypes.c_char * 512)]

def png_pixel(png, x, y):
    """Read one pixel from CEF's non-interlaced RGB(A) PNG, using only stdlib."""
    assert png.startswith(b'\x89PNG\r\n\x1a\n')
    offset, compressed = 8, b''
    while offset < len(png):
        length, kind = struct.unpack('>I4s', png[offset:offset+8])
        data = png[offset+8:offset+8+length]; offset += 12 + length
        if kind == b'IHDR':
            w, h, depth, color, _, _, interlace = struct.unpack('>IIBBBBB', data)
            assert depth == 8 and color in (2, 6) and interlace == 0
        if kind == b'IDAT': compressed += data
    raw = zlib.decompress(compressed); channels = 3 if color == 2 else 4; stride = w * channels
    previous = bytearray(stride)
    for row in range(y+1):
        base = row * (stride+1); filtering = raw[base]; current = bytearray(raw[base+1:base+1+stride])
        for i in range(stride):
            a = current[i-channels] if i >= channels else 0
            b = previous[i]; c = previous[i-channels] if i >= channels else 0
            prediction = a + b - c
            pa, pb, pc = abs(prediction-a), abs(prediction-b), abs(prediction-c)
            paeth = a if pa <= pb and pa <= pc else b if pb <= pc else c
            current[i] = (current[i] + (0, a, b, (a+b)//2, paeth)[filtering]) & 255
        previous = current
    return tuple(previous[x*channels:x*channels+3])

reports = queue.Queue()
HTML = b'''<!doctype html><style>body{margin:0;background:rgb(12,34,56)}input{position:absolute;top:10px;left:10px;width:300px;height:40px}</style>
<input id="field" oninput="document.body.style.background='rgb(23,45,67)';fetch('/input?value='+encodeURIComponent(this.value))">
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
        self.observations = {}
        self.sequence = 0
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
                    self.events.put((token, kind, width, height, data if kind == 6 else data[:100] if kind != 2 else b''))
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
    def observation(self, token, request):
        while (token, request) not in self.observations:
            event = self.events.get()
            if event is None: raise RuntimeError('helper exited before observation')
            if event[1] == 4: raise RuntimeError(event[-1].decode(errors='replace'))
            if event[1] != 6: continue
            info = Observation.from_buffer_copy(event[-1])
            png = event[-1][ctypes.sizeof(Observation):]
            assert len(png) == info.png_size
            self.observations[(event[0], info.request)] = (info, png, event[2:4])
        return self.observations.pop((token, request))
    def observe(self, token):
        self.sequence += 1
        self.send(token, 14, request=self.sequence)
        result = self.observation(token, self.sequence)
        assert not result[0].error, result[0].error
        return result
    def act(self, token, observation, steps, interrupt=None):
        self.sequence += 1; request = self.sequence
        info, _, (width, height) = observation
        self.send(token, 15, request=request, navigation=info.navigation, viewport=info.viewport, x=width, y=height)
        for kind, fields in steps: self.send(token, kind, request=request, **fields)
        self.send(token, 16, request=request)
        if interrupt: self.send(token, interrupt, request=request, a=0)
        return self.observation(token, request)
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
            # Hidden captures are requested, not display textures. Observe two
            # independent pages before giving the helper control of one.
            hidden = b.observe(1)
            other = b.observe(2)
            assert b'name=hidden' in hidden[0].url and b'name=b' in other[0].url
            assert png_pixel(hidden[1], 400, 400) == (12, 34, 56)
            assert hidden[2] == (640, 480) and other[2] == (800, 600)
            b.send(1,18,a=1)
            changed = b.act(1, hidden, [
                (6,dict(x=40,y=30,a=0,b=0)),
                (6,dict(x=40,y=30,a=0,b=1,end_action=1)),
                (9,dict(text='Agent é',end_action=1)),
            ])
            assert not changed[0].error and changed[0].completed == 2, changed[0].error
            assert png_pixel(changed[1], 400, 400) == (23, 45, 67)
            while report('/input')['value'] != ['Agent é']: pass
            assert png_pixel(b.observe(2)[1], 400, 400) == (12, 34, 56)
            # Older observation remains usable after input, without a resize/navigation.
            spaced = b.act(1, hidden, [
                (8,dict(a=32,text=' ',profile='Space')),
                (8,dict(a=32,b=1,text=' ',profile='Space',end_action=1)),
            ])
            assert not spaced[0].error and spaced[0].completed == 1, spaced[0].error
            while report('/input')['value'] != ['Agent é ']: pass
            # Resize invalidates identity even if dimensions later return.
            b.send(1,3,x=600,y=450)
            stale = b.act(1, hidden, [(9,dict(text='must-not-insert',end_action=1))])
            assert b'Stale' in stale[0].error and stale[0].completed == 0
            resized = b.observe(1)
            # Takeover rejects input but still permits fresh observation.
            b.send(1,18,a=0)
            denied = b.act(1, resized, [(9,dict(text='must-not-insert',end_action=1))])
            assert b'User control' in denied[0].error
            user_view = b.observe(1)
            b.send(1,18,a=1)
            # A batch interrupted with a held button must settle releases and
            # expose a completed/partial prefix, not continue its queued tail.
            drag = [(6,dict(x=40,y=30,a=0))] + [(6,dict(x=50+i,y=30,a=-1)) for i in range(30)] + [
                (6,dict(x=80,y=30,a=0,b=1,end_action=1)),
                (9,dict(text='cancelled-tail',end_action=1))]
            cancelled = b.act(1, user_view, drag, interrupt=17)
            assert b'cancelled' in cancelled[0].error and cancelled[0].completed < 2
            assert not cancelled[0].uncertain
            after_cancel = b.observe(1)
            # Next click/text works: cancellation left no stuck button.
            again = b.act(1, after_cancel, [
                (6,dict(x=40,y=30,a=0)), (6,dict(x=40,y=30,a=0,b=1,end_action=1)),
                (9,dict(text='ok',end_action=1))])
            assert not again[0].error and again[0].completed == 2
            taken = b.act(1, after_cancel, drag, interrupt=18)
            assert b'taken' in taken[0].error and taken[0].completed < 2 and not taken[0].uncertain
            # Observation is still allowed after interrupting takeover.
            b.observe(1)
            # Two sessions' requests can be outstanding together without
            # sharing focus, data, or observation delivery.
            second_view = b.observe(2)
            b.send(2,18,a=1)
            b.sequence += 1; second_request = b.sequence
            info, _, (w,h) = second_view
            b.send(2,15,request=second_request,navigation=info.navigation,viewport=info.viewport,x=w,y=h)
            b.send(2,6,request=second_request,x=40,y=30,a=0)
            b.send(2,6,request=second_request,x=40,y=30,a=0,b=1,end_action=1)
            b.send(2,9,request=second_request,text='second session',end_action=1)
            b.send(2,16,request=second_request)
            independent = b.observe(1)
            second_result = b.observation(2,second_request)
            assert b'name=hidden' in independent[0].url and b'name=b' in second_result[0].url
            assert not second_result[0].error and second_result[0].completed == 2
            assert png_pixel(second_result[1],400,400) == (23,45,67)

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
    print('PASS: embedded frames, mouse/text, hidden progress, session isolation, resize, singleton rejection, persistent profiles, delayed SPA navigation, hidden PNG observations, acknowledged input, stale identity, and cancellation')
if __name__ == '__main__': main()
