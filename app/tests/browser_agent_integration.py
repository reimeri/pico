#!/usr/bin/env python3
"""Real Pico -> browser tools -> CEF -> image-bearing provider request.
Uses a loopback OpenAI Responses fixture, not credentials or a paid/live model.
Run under nix develop .#browser-tests and isolated hardware-backed X11, like the UI test.
"""
import base64
import http.server
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import re

FAILURE = "--failure" in sys.argv
DEVELOPER = "--developer" in sys.argv or FAILURE
COMPLETION = "Failure loop verified." if FAILURE else "Developer loop verified." if DEVELOPER else "Visual loop verified."
from PIL import Image


class Fixture(http.server.BaseHTTPRequestHandler):
    round = 0
    done = threading.Event()
    failure = None
    first_observation = None
    stage = 0
    root = None
    output_file = None
    process = None

    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == '/kill-helper' and FAILURE:
            from browser_profile import process_tree
            helpers = []
            for pid,_ in process_tree(Fixture.process.pid):
                try:
                    command = Path(f'/proc/{pid}/cmdline').read_bytes()
                    if b'pico_browser_helper' in command and b'--type=' not in command:
                        helpers.append(pid)
                except FileNotFoundError:
                    pass
            assert len(helpers) == 1, helpers
            self.send_response(204); self.end_headers()
            os.kill(helpers[0],signal.SIGKILL)
            return
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.end_headers()
        self.wfile.write(b'''<!doctype html><style>body{margin:0;background:rgb(12,34,56);height:2400px}
input{position:fixed;top:10px;left:10px;width:300px;height:40px}</style>
<input id="field" aria-label="Developer field" oninput="document.body.style.background='rgb(23,45,67)'">
<script>console.log('developer-fixture-console'); onscroll=()=>document.body.style.background='rgb(65,43,21)'</script>''')

    def failure_call(self, body):
        results = [i for i in body['input'] if i['type'] == 'function_call_output']
        text = results[-1]['output'] if results else ''
        stage = Fixture.stage; Fixture.stage += 1
        url = f'http://127.0.0.1:{self.server.server_port}/app'
        if stage == 0:
            return 'browser_open', {'url':url}
        if stage == 1:
            assert 'queued' in text.lower(), text
            return 'browser_evaluate', {'expression':"new Promise(resolve=>{fetch('/kill-helper');setTimeout(()=>resolve('late'),20000)})"}
        assert 'signal 9' in text and 'Close and reopen' in text, text
        if stage == 2:
            assert 'uncertain' in text, text
            return 'browser_diagnostics', {'cursor':None}
        if stage == 3:
            return 'browser_open', {'url':url}
        if stage == 4:
            return None, None
        raise AssertionError(f'unexpected failure stage {stage}')

    def developer_call(self, body):
        catalog = {t['name'] for t in body.get('tools', [])}
        assert {'browser_diagnostics','browser_inspect','browser_evaluate','browser_viewport'} <= catalog
        results = [i for i in body['input'] if i['type'] == 'function_call_output']
        text = results[-1]['output'] if results else ''
        stage = Fixture.stage
        Fixture.stage += 1
        if stage == 0:
            return 'browser_diagnostics', {}
        if stage == 1:
            assert 'No open browser' in text, text
            assert not (Fixture.root/'config/pico/browser').exists(), 'inspection created Chromium'
            return 'browser_open', {'url':f'http://127.0.0.1:{self.server.server_port}/app'}
        if stage == 2:
            assert 'queued' in text.lower()
            return 'browser_diagnostics', {'cursor':None}
        if stage == 3:
            records = [json.loads(line) for line in text.splitlines()]
            assert records[0]['complete'], records
            if 'developer-fixture-console' not in text:
                Fixture.stage = stage
                return 'browser_diagnostics', {}
            return 'browser_inspect', {'mode':'dom','selector':'#field'}
        if stage == 4:
            assert 'Developer field' in json.loads(text)['data']['outerHTML'], text
            return 'browser_inspect', {'mode':'accessibility','selector':None}
        if stage == 5:
            assert any(n.get('name',{}).get('value') == 'Developer field' for n in json.loads(text)['data']['nodes'])
            return 'browser_evaluate', {'expression':"document.getElementById('field').value='changed'; 'A'.repeat(200000)+'tail'"}
        if stage == 6:
            path = Path(re.search(r'Full output path: (.+)', text)[1])
            size = int(re.search(r'Full output size: (\d+)', text)[1])
            assert path.stat().st_size == size
            assert json.loads(path.read_text())['data']['result']['value'] == 'A'*200000+'tail'
            Fixture.output_file = path
            return 'browser_viewport', {'preset':'mobile','width':None,'height':None}
        if stage == 7:
            metadata = json.loads(text)
            assert (metadata['width'],metadata['height']) == (390,844) and metadata['mode'] == 'fixed'
            return 'browser_evaluate', {'expression':"[innerWidth,innerHeight,document.getElementById('field').value]"}
        if stage == 8:
            assert json.loads(text)['data']['result']['value'] == [390,844,'changed']
            return 'browser_viewport', {'preset':'desktop','width':960,'height':640}
        if stage == 9:
            assert (json.loads(text)['width'],json.loads(text)['height']) == (960,640), text
            return 'browser_viewport', {'preset':'unknown','width':1000,'height':700}
        if stage == 10:
            assert (json.loads(text)['width'],json.loads(text)['height']) == (1000,700), text
            return 'browser_viewport', {'preset':None,'width':1024,'height':768}
        if stage == 11:
            assert (json.loads(text)['width'],json.loads(text)['height']) == (1024,768), text
            return 'browser_viewport', {'preset':'tablet','width':1000,'height':None}
        if stage == 12:
            assert (json.loads(text)['width'],json.loads(text)['height']) == (768,1024), text
            # Viewport acknowledgement precedes renderer layout. Establish the
            # actual size before checking that an invalid call cannot change it.
            return 'browser_evaluate', {'expression':"new Promise(resolve => { const ready = () => { if (innerWidth === 768 && innerHeight === 1024) { removeEventListener('resize', ready); resolve([innerWidth, innerHeight]); } }; addEventListener('resize', ready); ready(); })"}
        if stage == 13:
            assert json.loads(text)['data']['result']['value'] == [768,1024], text
            return 'browser_viewport', {'preset':'mobile','width':0,'height':600}
        if stage == 14:
            assert 'width' in text and 'integer' in text and 'No viewport change' in text, text
            return 'browser_evaluate', {'expression':'[innerWidth,innerHeight]'}
        if stage == 15:
            assert json.loads(text)['data']['result']['value'] == [768,1024], text
            return 'browser_viewport', {'preset':None,'width':640,'height':None}
        if stage == 16:
            assert 'both dimensions' in text and '{"preset":"desktop"}' in text, text
            return 'browser_viewport', {'width':960,'height':600}
        if stage == 17:
            assert (json.loads(text)['width'],json.loads(text)['height']) == (960,600), text
            return 'browser_viewport', {'preset':'unknown'}
        if stage == 18:
            assert 'preset must be' in text, text
            return 'browser_inspect', {'mode':'dom','selector':None}
        if stage == 19:
            assert 'non-empty selector' in text, text
            return 'browser_evaluate', {'expression':None}
        if stage == 20:
            assert 'expression is required' in text, text
            return 'browser_viewport', {'preset':'dock'}
        if stage == 21:
            assert json.loads(text)['mode'] == 'dock'
            return 'browser_observe', {}
        if stage == 22:
            assert 'does not support image' in text, text
            return None, None
        raise AssertionError(f'unexpected developer stage {stage}')

    def do_POST(self):
        try:
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            assert self.path == '/v1/responses'
            assert all(tool.get('strict') is False for tool in body.get('tools', [])), 'implicit strict policy'
            Fixture.round += 1
            current = Fixture.round
            if FAILURE:
                name, args = self.failure_call(body)
            elif DEVELOPER:
                name, args = self.developer_call(body)
            else:
                catalog = {t['name'] for t in body.get('tools', [])}
                assert {'browser_open', 'browser_observe', 'browser_act'} <= catalog
                results = [i for i in body['input'] if i['type'] == 'function_call_output']
                if current >= 3:
                    assert len(results) == current - 1
                    if current < 6: assert results[-1]['output'].startswith('{'), results[-1]
                    meta = json.loads(results[-1]['output']) if current < 6 else None
                    images = [p['image_url'] for i in body['input'] if i.get('type') == 'message'
                              for p in i.get('content', []) if p.get('type') == 'input_image']
                    assert images, 'provider received no actual image parts'
                    data = base64.b64decode(images[-1].split(',', 1)[1], validate=True)
                    with Image.open(io.BytesIO(data)) as image:
                        image = image.convert('RGB')
                        expected = {3: (12,34,56), 4: (23,45,67), 5: (65,43,21), 6: (65,43,21), 7: (65,43,21)}[current]
                        assert image.getpixel((400,400)) == expected, (current, image.getpixel((400,400)))
                        if meta:
                            assert image.size == (meta['width'], meta['height'])
                            assert 'error' not in meta, meta
                    if current == 3:
                        Fixture.first_observation = meta['observation_id']
                    if current in (4,5):
                        assert meta['completed_actions'] == (2 if current == 4 else 1), meta
                if current == 1:
                    name, args = 'browser_open', {'url': f'http://127.0.0.1:{self.server.server_port}/app'}
                elif current == 2:
                    assert 'queued' in results[-1]['output'].lower()
                    name, args = 'browser_observe', {}
                elif current == 3:
                    name, args = 'browser_act', {'observation_id': meta['observation_id'], 'actions': [
                        {'type':'click','x':40,'y':30,'button':None,'count':None,'modifiers':None,'points':None},
                        {'type':'text','text':'Agent input é 🙂','modifiers':None,'x':None,'key':None}]}
                elif current == 4:
                    # Old observation remains usable after typing, until geometry/navigation changes.
                    name, args = 'browser_act', {'observation_id': Fixture.first_observation, 'actions': [
                        {'type':'scroll','x':400,'y':400,'delta_x':0,'delta_y':450}]}
                elif current == 5:
                    name, args = 'browser_act', {'observation_id': meta['observation_id'], 'actions': [
                        {'type':'click','x':None,'y':30}]}
                elif current == 6:
                    assert 'Invalid browser_act' in results[-1]['output'], results[-1]
                    name, args = 'browser_act', {'observation_id': Fixture.first_observation, 'actions': [
                        {'type':'key','key':'V','modifiers':['Control']}]}
                elif current == 7:
                    assert 'clipboard' in results[-1]['output'].lower()
                    name, args = None, None
                else:
                    raise AssertionError(f'unexpected request {current}')
            output = ([{'type':'function_call','id':f'fc_{current}','call_id':f'call_{current}',
                        'name':name,'arguments':json.dumps(args)}] if name else
                      [{'type':'message','role':'assistant','content':[{'type':'output_text','text':COMPLETION}]}])
            response = {'type':'response.completed','response':{'status':'completed','output':output}}
            payload = ('data: '+json.dumps(response)+'\n\n').encode()
            self.send_response(200)
            self.send_header('Content-Type','text/event-stream')
            self.send_header('Content-Length',str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            if not name: Fixture.done.set()
        except BaseException:
            Fixture.failure = traceback.format_exc()
            Fixture.done.set()
            self.send_error(500)


def main():
    if os.environ.get('WAYLAND_DISPLAY') or not os.environ.get('DISPLAY'):
        raise RuntimeError('Use an isolated hardware-backed X11 display, not the user desktop.')
    binary = str(Path(sys.argv[1]).resolve())
    def interrupted(signum, frame):
        raise RuntimeError(f'agent integration watchdog at provider round {Fixture.round}')
    signal.signal(signal.SIGTERM, interrupted)
    server = http.server.ThreadingHTTPServer(('127.0.0.1',0), Fixture)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='pico-browser-agent-') as directory:
        root = Path(directory)
        Fixture.root = root
        settings = root / '.pico/settings.json'
        settings.parent.mkdir()
        settings.write_text(json.dumps({'model':'fixture','models':[{'id':'fixture','provider':'openai',
            'vision':not DEVELOPER,'base_url':f'http://127.0.0.1:{server.server_port}/v1'}],
            'compact_at':None, 'resume_last':False}))
        environment = {k:v for k,v in os.environ.items() if not k.startswith(('PICO_', 'OPENAI_', 'HYPER_', 'XAI_'))}
        environment.update(XDG_CONFIG_HOME=str(root/'config'), XDG_SESSION_TYPE='x11',
                           OPENAI_API_KEY='loopback-fixture-only')
        with (root/'pico.log').open('w') as log:
            process = subprocess.Popen([binary,'--safe'],cwd=root,env=environment,stdout=log,stderr=log)
            Fixture.process = process
            def x(*args):
                return subprocess.check_output(['xdotool',*map(str,args)],text=True).strip()
            try:
                window = x('search','--sync','--pid',process.pid).splitlines()[0]
                x('windowsize',window,1100,800); x('windowfocus','--sync',window)
                # A successful F12 capture establishes that the UI is pumping.
                screenshot = root/'pico_screenshot.png'
                while not screenshot.exists():
                    assert process.poll() is None
                    x('key','F12'); time.sleep(.02)
                # Raylib polls input once per frame; a complete synthetic press
                # between frames can disappear. Hold each submission event.
                x('mousemove','--window',window,350,720); x('mousedown',1)
                time.sleep(.08); x('mouseup',1)
                x('type','--clearmodifiers','--delay',15,'Exercise the embedded browser.')
                x('keydown','Return'); time.sleep(.08); x('keyup','Return')
                while not Fixture.done.wait(.05):
                    assert process.poll() is None, 'Pico exited before provider observation'
                assert not Fixture.failure, Fixture.failure
                # Wait for ordinary asynchronous persistence to adopt the terminal record.
                while True:
                    sessions = list((root/'config/pico/sessions').glob('*/*.jsonl'))
                    records = []
                    for session in sessions:
                        records = [json.loads(line) for line in session.read_text().splitlines()]
                        if any(COMPLETION in json.dumps(r) for r in records): break
                    if records and any(COMPLETION in json.dumps(r) for r in records): break
                    assert process.poll() is None; time.sleep(.02)
                images = [p for record in records if record.get('type') == 'tool_result' for p in record.get('parts',[])]
                assert len(images) == (0 if DEVELOPER else 3), images
                sid = records[0]['id']
                for image in images:
                    assert Path(image['path']).parent == root/'.pico/media/browser'/sid
                    assert Path(image['path']).is_file()
                assert not any('base64,' in json.dumps(r) for r in records), 'image bytes leaked into JSONL'
                print('PASS: registered tools preserve helper signal cause, uncertain evaluation and explicit no-restart recovery' if FAILURE else 'PASS: non-vision developer tools, lazy lifecycle, complete large file outputs and viewport control' if DEVELOPER else 'PASS: real browser_open/observe/act, native click/type/scroll, model-visible PNG payloads, clipboard refusal, and owned history assets')
            except BaseException:
                log.flush(); print((root/'pico.log').read_text(),file=sys.stderr); raise
            finally:
                process.terminate()
                try: process.wait(timeout=10)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
                owner = root/'config/pico/browser/.pico-owner.lock'
                if owner.exists():
                    import fcntl
                    with owner.open() as lock: fcntl.flock(lock, fcntl.LOCK_EX)
    if DEVELOPER and not FAILURE:
        assert Fixture.output_file and Fixture.output_file.exists(), 'returned output must survive browser/process close'
        Fixture.output_file.unlink()
    server.shutdown()

if __name__ == '__main__':
    main()
