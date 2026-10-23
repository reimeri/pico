#!/usr/bin/env python3
"""Descriptive Linux browser benchmark; no performance pass/fail thresholds.

Helper matrix (sandbox enabled): nix develop -c python3 app/tests/browser_profile.py HELPER
Actual host import path: nix develop .#browser-tests -c env -u WAYLAND_DISPLAY \
  python3 app/tests/browser_profile.py HELPER --pico PICO
WARNING: --pico sends X11 mouse/keyboard input; use an isolated hardware-backed X11 display,
never your desktop DISPLAY. The script cannot certify arbitrary display isolation.
Use --output NEW_FILE to retain private JSON results. See docs/browser-profiling.md.
"""
import argparse
import contextlib
import http.server
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

from browser_integration import Browser

PAGE = r'''<!doctype html><style>html,body{margin:0;background:rgb(12,34,56);overflow:hidden}canvas{display:block}</style>
<canvas id="canvas"></canvas><script>
const query=new URLSearchParams(location.search), name=query.get('name'), kind=query.get('workload');
const canvas=document.getElementById('canvas'); canvas.width=Number(query.get('width')); canvas.height=Number(query.get('height'));
let draws=0, error=null, renderer=null, pixel=null;
try {
  let draw;
  if(kind==='animated') {
    const ctx=canvas.getContext('2d');
    draw=()=>{ctx.fillStyle='rgb(12,34,56)';ctx.fillRect(0,0,canvas.width,canvas.height);
      for(let i=0;i<160;i++){ctx.fillStyle=`hsl(${(i+draws)%360},70%,50%)`;
        ctx.fillRect((i*37+draws*3)%canvas.width,(i*61)%canvas.height,32,32);}};
  } else if(kind==='webgl') {
    const gl=canvas.getContext('webgl'); if(!gl) throw Error('WebGL unavailable');
    const debug=gl.getExtension('WEBGL_debug_renderer_info');
    renderer=debug?gl.getParameter(debug.UNMASKED_RENDERER_WEBGL):gl.getParameter(gl.RENDERER);
    function shader(type,source){const s=gl.createShader(type);gl.shaderSource(s,source);gl.compileShader(s);
      if(!gl.getShaderParameter(s,gl.COMPILE_STATUS))throw Error(gl.getShaderInfoLog(s));return s;}
    const p=gl.createProgram();
    gl.attachShader(p,shader(gl.VERTEX_SHADER,'attribute vec2 pos;void main(){gl_Position=vec4(pos,0.,1.);}'));
    gl.attachShader(p,shader(gl.FRAGMENT_SHADER,'precision mediump float;uniform float tick;void main(){vec2 p=gl_FragCoord.xy*.003;float v=tick;for(int i=0;i<16;i++){v=sin(v+p.x)*cos(v+p.y);}gl_FragColor=vec4(.1+abs(v),.3,.5,1.);}'));
    gl.linkProgram(p);if(!gl.getProgramParameter(p,gl.LINK_STATUS))throw Error(gl.getProgramInfoLog(p));
    gl.useProgram(p); gl.viewport(0,0,canvas.width,canvas.height); const b=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,b);
    gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([-1,-1,3,-1,-1,3]),gl.STATIC_DRAW);
    const a=gl.getAttribLocation(p,'pos');gl.enableVertexAttribArray(a);gl.vertexAttribPointer(a,2,gl.FLOAT,false,0,0);
    const t=gl.getUniformLocation(p,'tick');
    draw=()=>{gl.uniform1f(t,draws*.01);gl.drawArrays(gl.TRIANGLES,0,3);
      if(!pixel){const bytes=new Uint8Array(4);gl.readPixels(0,0,1,1,gl.RGBA,gl.UNSIGNED_BYTE,bytes);pixel=Array.from(bytes);
        if(gl.getError()!==gl.NO_ERROR)throw Error('WebGL draw/readback error');}};
    canvas.addEventListener('webglcontextlost',()=>{error='WebGL context lost';report();});
  }
  if(draw) {function frame(){try{draw();draws++;requestAnimationFrame(frame);}catch(e){error=String(e);report();}};frame();}
} catch(e){error=String(e);}
function report(){fetch('/status?'+new URLSearchParams({name,data:JSON.stringify({kind,draws,error,renderer,pixel,width:innerWidth,height:innerHeight,canvas_width:canvas.width,canvas_height:canvas.height})}));}
report();setInterval(report,1000);
</script>'''.encode()


class Fixture(http.server.BaseHTTPRequestHandler):
    reports = {}
    lock = threading.Lock()

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        if url.path == '/status':
            query = urllib.parse.parse_qs(url.query)
            with self.lock:
                self.reports[query['name'][0]] = (time.monotonic(), json.loads(query['data'][0]))
            self.send_response(204); self.end_headers()
        else:
            self.send_response(200); self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Cache-Control', 'no-store'); self.end_headers(); self.wfile.write(PAGE)

    def log_message(self, *args):
        pass


def process_tree(root):
    """PPid discovery includes CEF zygotes/renderers/GPU; key by pid/starttime."""
    processes = {}
    for path in Path('/proc').glob('[0-9]*/stat'):
        try:
            text = path.read_text(); fields = text[text.rfind(')')+2:].split()
            if fields[0] == 'Z':
                continue
            processes[int(path.parent.name)] = (int(fields[1]), int(fields[19]),
                                                int(fields[11])+int(fields[12]), int(fields[21]))
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            pass
    selected = {root}
    while True:
        added = {pid for pid, (parent, *_) in processes.items() if parent in selected} - selected
        if not added:
            break
        selected |= added
    return {(pid, processes[pid][1]): processes[pid][2:] for pid in selected if pid in processes}


def sample(process, duration):
    previous = process_tree(process.pid); cpu_ticks = 0; rss = []; counts = []; disappeared = 0; appeared = 0
    started = time.monotonic()
    while time.monotonic() - started < duration:
        if process.poll() is not None:
            raise RuntimeError(f'benchmark process exited: {process.returncode}')
        time.sleep(min(.2, max(0, duration - (time.monotonic()-started))))
        current = process_tree(process.pid)
        disappeared += len(previous.keys()-current.keys()); appeared += len(current.keys()-previous.keys())
        cpu_ticks += sum(max(0, ticks - previous.get(key, (0, 0))[0]) for key, (ticks, _) in current.items())
        rss.append(sum(pages for _, pages in current.values()) * os.sysconf('SC_PAGE_SIZE'))
        counts.append(len(current)); previous = current
    elapsed = time.monotonic() - started
    return {'seconds': elapsed, 'cpu_seconds': cpu_ticks/os.sysconf('SC_CLK_TCK'),
            'cpu_percent_one_core': 100*cpu_ticks/os.sysconf('SC_CLK_TCK')/elapsed,
            'cpu_accounting':'sampled-live-processes', 'processes_disappeared':disappeared, 'processes_appeared':appeared,
            'rss_mean_bytes': sum(rss)/len(rss), 'rss_peak_bytes': max(rss), 'processes_peak': max(counts)}


def wait_reports(names, process):
    while True:
        if process.poll() is not None:
            raise RuntimeError(f'process exited before page readiness: {process.returncode}')
        with Fixture.lock:
            if all(name in Fixture.reports for name in names):
                data = {name: Fixture.reports[name][1] for name in names}
                return data
        time.sleep(.02)


def fresh_reports(names, process, since):
    while True:
        wait_reports(names, process)
        with Fixture.lock:
            entries = {name: Fixture.reports[name] for name in names}
        if all(timestamp >= since for timestamp, _ in entries.values()):
            data = {name: dict(record) for name, (_,record) in entries.items()}
            for name, record in data.items():
                if record['error']:
                    raise RuntimeError(f'{name}: {record["error"]}')
            return data
        time.sleep(.02)


def check_progress(before, after):
    for name, record in after.items():
        if record['kind'] != 'static' and record['draws'] <= before[name]['draws']:
            raise RuntimeError(f'{name}: workload did not progress (no FPS threshold)')


@contextlib.contextmanager
def watchdog(label, seconds):
    def expired(signum, frame):
        raise TimeoutError(f'{label}: functional watchdog expired after {seconds:g}s')
    previous = signal.signal(signal.SIGALRM,expired)
    signal.setitimer(signal.ITIMER_REAL,seconds)
    try:
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL,0); signal.signal(signal.SIGALRM,previous)


def terminate_tree(known):
    # Identify by pid/starttime, never signal an unrelated reused PID. This is
    # only cleanup after a benchmark process, not production browser policy.
    def alive(key):
        pid,start=key
        try:
            text=Path(f'/proc/{pid}/stat').read_text(); fields=text[text.rfind(')')+2:].split()
            return fields[0] != 'Z' and int(fields[19]) == start
        except (FileNotFoundError,ProcessLookupError):
            return False
    for key in known:
        if alive(key):
            try: os.kill(key[0],signal.SIGTERM)
            except ProcessLookupError: pass
    deadline=time.monotonic()+2
    while any(alive(key) for key in known) and time.monotonic()<deadline:
        time.sleep(.02)
    for key in known:
        if alive(key):
            try: os.kill(key[0],signal.SIGKILL)
            except ProcessLookupError: pass


def helper_trial(args, url, workload, count, visibility):
    traffic = {'frames': 0, 'frame_bytes': 0}
    lock = threading.Lock()

    def frame(token, length):
        with lock:
            traffic['frames'] += 1; traffic['frame_bytes'] += length

    with tempfile.TemporaryDirectory(prefix='pico-browser-profile-') as root:
        browser = Browser(args.helper, root, frame_callback=frame)
        try:
            names = [f'helper-{workload}-{count}-{visibility}-{i}' for i in range(count)]
            for token, name in enumerate(names, 1):
                browser.send(token, 1, profile=f'{token:012x}', text=f'{url}/?workload={workload}&name={name}&width={args.width}&height={args.height}')
                browser.wait(token, 1)
                browser.send(token, 3, x=args.width, y=args.height)
                browser.send(token, 18, a=1)
                browser.send(token, 4, a=int(visibility == 'visible' or visibility == 'mixed' and token == 1))
            pages = wait_reports(names, browser.process)
            if any(page['error'] for page in pages.values()):
                return dict(path='helper-only', workload=workload, sessions=count, visibility=visibility,
                            viewport=[args.width,args.height], status='unavailable', pages=pages)
            # An evaluation barrier verifies the requested logical dimensions
            # without introducing screenshot captures into the measured period.
            for token in range(1, count+1):
                error, result = browser.inspect(token, 21, {'expression':'[innerWidth,innerHeight,canvas.width,canvas.height]'})
                if error or json.loads(result)['data']['result']['value'] != [args.width, args.height,args.width,args.height]:
                    raise RuntimeError(f'viewport not applied: {result}')
            time.sleep(args.warmup)
            before_pages = fresh_reports(names,browser.process,time.monotonic())
            with lock:
                before = dict(traffic)
            result = sample(browser.process, args.seconds)
            measurement_end = time.monotonic()
            with lock:
                result.update({key: traffic[key] - before[key] for key in before})
            result.update(status='measured', path='helper-only', workload=workload, sessions=count, visibility=visibility,
                          viewport=[args.width,args.height], pages_before=before_pages, pages_after=fresh_reports(names,browser.process,measurement_end))
            check_progress(result['pages_before'],result['pages_after'])
            return result
        finally:
            known = process_tree(browser.process.pid)
            try: browser.close()
            finally: terminate_tree(known)


def host_trial(args, url, workload, visibility):
    """One actually presented browser; concurrent workloads measured above."""
    if not os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY'):
        raise RuntimeError('--pico uses isolated X11 UI driving; use an isolated hardware-backed X11 display with WAYLAND_DISPLAY unset')
    with tempfile.TemporaryDirectory(prefix='pico-browser-host-profile-') as directory:
        root = Path(directory); metrics = root/'metrics.jsonl'; session = root/'session.jsonl'
        session.write_text(json.dumps({'type':'session','version':5,'id':'abcdef123456','kind':'normal','cwd':directory})+'\n')
        environment = dict(os.environ, XDG_CONFIG_HOME=str(root/'config'), XDG_SESSION_TYPE='x11', PICO_BROWSER_PROFILE=str(metrics))
        with (root/'pico.log').open('w') as log:
            process = subprocess.Popen([args.pico,'--safe','--session',str(session)], cwd=root, env=environment, stdout=log, stderr=log)

            def x(*parts):
                if process.poll() is not None:
                    raise RuntimeError((root/'pico.log').read_text())
                return subprocess.check_output(['xdotool', *map(str,parts)], text=True).strip()

            def click(xpos, ypos):
                x('mousemove','--window',window,xpos,ypos); x('mousedown',1,'sleep','.05','mouseup',1)

            def records():
                if not metrics.exists():
                    return []
                # The writer may be appending a record; only complete lines.
                return [json.loads(line) for line in metrics.read_text().splitlines(keepends=True) if line.endswith('\n')]

            try:
                window = x('search','--sync','--pid',process.pid).splitlines()[0]
                x('windowsize',window,1100,800); x('windowfocus','--sync',window)
                while not (root/'config/pico/browser').exists():
                    click(1075,25); time.sleep(.05)
                click(825,28); x('key','ctrl+a'); x('type','--clearmodifiers',f'{args.width}x{args.height}'); x('key','Return')
                name = f'host-{workload}-{visibility}'
                click(900,60); x('key','ctrl+a'); x('type','--clearmodifiers',f'{url}/?workload={workload}&name={name}&width={args.width}&height={args.height}'); x('key','Return')
                pages = wait_reports([name],process)
                if any(page['error'] for page in pages.values()):
                    return dict(path='pico-presented',workload=workload,sessions=1,visibility=visibility,
                                viewport=[args.width,args.height],status='unavailable',pages=pages)
                # Observe real page pixels for the static fixture, not just IPC.
                # Animated/WebGL presentation has the same host path; counters
                # establish imports, not visual/GPU correctness certification.
                while not records() or records()[-1]['imports'] == 0:
                    time.sleep(.05); x('getwindowname',window)
                if workload != 'static':
                    before_import = records()[-1]['imports']
                    while records()[-1]['imports'] <= before_import:
                        time.sleep(.05); x('getwindowname',window)
                if workload == 'static':
                    from PIL import Image
                    click(350,200)
                    screenshot = root/'pico_screenshot.png'
                    while True:
                        screenshot.unlink(missing_ok=True)
                        x('key','F12')
                        for _ in range(20):
                            try:
                                with Image.open(screenshot) as image:
                                    pixels = image.convert('RGB').crop((600,90,1100,760))
                                    presented = sum(pixel == (12,34,56) for pixel in pixels.get_flattened_data()) > 1000
                                break
                            except (FileNotFoundError,OSError):
                                time.sleep(.02)
                        else:
                            continue
                        if presented:
                            break
                if visibility == 'hidden':
                    click(1075,28)
                time.sleep(args.warmup)
                while not records() or time.monotonic()-records()[-1]['monotonic_seconds'] > 1.5:
                    x('getwindowname',window); time.sleep(.05)
                before_pages = fresh_reports([name],process,time.monotonic())
                if [before_pages[name]['width'],before_pages[name]['height'],before_pages[name]['canvas_width'],before_pages[name]['canvas_height']] != [args.width,args.height,args.width,args.height]:
                    raise RuntimeError(f'host fixture viewport/buffer mismatch: {before_pages}')
                before = records()[-1]
                result = sample(process,args.seconds)
                # Wait for a boundary record, then report its exact counter span.
                boundary = time.monotonic()
                while records()[-1]['monotonic_seconds'] < boundary:
                    x('getwindowname',window); time.sleep(.05)
                after = records()[-1]
                result['host_counter_seconds'] = after['monotonic_seconds']-before['monotonic_seconds']
                result['host_counters'] = {key: after[key]-before[key] for key in
                    ('frames','frame_bytes','replaced_frames','imports','png_bytes','import_seconds')}
                result['queue_peak_lifetime'] = after['queue_peak']
                result.update(status='measured',path='pico-presented',workload=workload,sessions=1,visibility=visibility,
                              viewport=[args.width,args.height],pages_before=before_pages,pages_after=fresh_reports([name],process,boundary))
                check_progress(result['pages_before'],result['pages_after'])
                return result
            finally:
                known = process_tree(process.pid)
                process.terminate()
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()
                finally:
                    terminate_tree(known)
                owner = root/'config/pico/browser/.pico-owner.lock'
                if owner.exists():
                    import fcntl
                    deadline=time.monotonic()+2
                    with owner.open() as file:
                        while True:
                            try: fcntl.flock(file,fcntl.LOCK_EX|fcntl.LOCK_NB); break
                            except BlockingIOError:
                                if time.monotonic()>=deadline: raise RuntimeError('browser profile lock retained after bounded cleanup')
                                time.sleep(.02)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('helper',type=lambda p:str(Path(p).resolve()))
    parser.add_argument('--pico',type=lambda p:str(Path(p).resolve()))
    parser.add_argument('--seconds',type=float,default=10)
    parser.add_argument('--warmup',type=float,default=3)
    parser.add_argument('--sessions',type=int,nargs='+',default=[1,3])
    parser.add_argument('--workloads',nargs='+',choices=['static','animated','webgl'],default=['static','animated','webgl'])
    parser.add_argument('--width',type=int,default=1280); parser.add_argument('--height',type=int,default=720)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    if args.seconds<=0 or args.warmup<0 or any(n<1 for n in args.sessions) or not (1<=args.width<=4096 and 1<=args.height<=4096):
        parser.error('positive duration/session counts, nonnegative warmup, and 1–4096 viewport dimensions required')
    def interrupted(signum,frame):
        raise RuntimeError('benchmark interrupted; cleaning up browser processes')
    signal.signal(signal.SIGTERM,interrupted)
    report={'environment':{'utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'platform':platform.platform(),
        'cpu':platform.processor(),'cpus':os.cpu_count(),'helper':args.helper,'pico':args.pico,
        'graphics_env':{key:os.environ.get(key) for key in ('DISPLAY','WAYLAND_DISPLAY','LIBGL_ALWAYS_SOFTWARE','GALLIUM_DRIVER')},
        'warmup_seconds':args.warmup,'requested_seconds':args.seconds},'trials':[]}
    # Reserve the output before doing work; never overwrite an existing report.
    output = os.fdopen(os.open(args.output,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600),'w') if args.output else sys.stdout
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Fixture)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    label = 'initialization'
    report['complete'] = False
    try:
        url=f'http://127.0.0.1:{server.server_port}'
        for workload in args.workloads:
            for count in args.sessions:
                for visibility in (['visible','hidden'] if count==1 else ['visible','hidden','mixed']):
                    label=f'helper {workload} {count} {visibility}'
                    print(f'profile: {label}',file=sys.stderr,flush=True)
                    with watchdog(label,args.warmup+args.seconds+60):
                        report['trials'].append(helper_trial(args,url,workload,count,visibility))
            if args.pico:
                for visibility in ('visible','hidden'):
                    label=f'host {workload} {visibility}'
                    print(f'profile: {label}',file=sys.stderr,flush=True)
                    with watchdog(label,args.warmup+args.seconds+60):
                        report['trials'].append(host_trial(args,url,workload,visibility))
        report['complete'] = True
    except BaseException as error:
        report['failure'] = {'trial':label,'type':type(error).__name__,'message':str(error)}
        raise
    finally:
        server.shutdown(); server.server_close()
        try: json.dump(report,output,indent=2); output.write('\n'); output.flush()
        finally:
            if args.output: output.close()


if __name__=='__main__':
    main()
