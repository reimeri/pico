#!/usr/bin/env python3
"""Real Pico -> browser tools -> CEF -> image-bearing provider request.
Uses a loopback OpenAI Responses fixture, not credentials or a paid/live model.
Run under nix develop .#browser-tests and isolated xvfb-run, like the UI test.
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
from PIL import Image


class Fixture(http.server.BaseHTTPRequestHandler):
    round = 0
    done = threading.Event()
    failure = None
    first_observation = None

    def log_message(self, *args):
        pass

    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.end_headers()
        self.wfile.write(b'''<!doctype html><style>body{margin:0;background:rgb(12,34,56);height:2400px}
input{position:fixed;top:10px;left:10px;width:300px;height:40px}</style>
<input oninput="document.body.style.background='rgb(23,45,67)'">
<script>onscroll=()=>document.body.style.background='rgb(65,43,21)'</script>''')

    def do_POST(self):
        try:
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            assert self.path == '/v1/responses'
            Fixture.round += 1
            current = Fixture.round
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
                    expected = {3: (12,34,56), 4: (23,45,67), 5: (65,43,21), 6: (65,43,21)}[current]
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
                    {'type':'click','x':40,'y':30}, {'type':'text','text':'Agent input é 🙂'}]}
            elif current == 4:
                # Old observation remains usable after typing, until geometry/navigation changes.
                name, args = 'browser_act', {'observation_id': Fixture.first_observation, 'actions': [
                    {'type':'scroll','x':400,'y':400,'delta_x':0,'delta_y':450}]}
            elif current == 5:
                name, args = 'browser_act', {'observation_id': meta['observation_id'], 'actions': [
                    {'type':'key','key':'V','modifiers':['Control']}]}
            elif current == 6:
                assert 'clipboard' in results[-1]['output'].lower()
                name, args = None, None
            else:
                raise AssertionError(f'unexpected request {current}')
            output = ([{'type':'function_call','id':f'fc_{current}','call_id':f'call_{current}',
                        'name':name,'arguments':json.dumps(args)}] if name else
                      [{'type':'message','role':'assistant','content':[{'type':'output_text','text':'Visual loop verified.'}]}])
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
        raise RuntimeError('Use an isolated xvfb-run display, not the user desktop.')
    binary = str(Path(sys.argv[1]).resolve())
    def interrupted(signum, frame):
        raise RuntimeError(f'agent integration watchdog at provider round {Fixture.round}')
    signal.signal(signal.SIGTERM, interrupted)
    server = http.server.ThreadingHTTPServer(('127.0.0.1',0), Fixture)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='pico-browser-agent-') as directory:
        root = Path(directory)
        settings = root / '.pico/settings.json'
        settings.parent.mkdir()
        settings.write_text(json.dumps({'model':'fixture','models':[{'id':'fixture','provider':'openai',
            'vision':True,'base_url':f'http://127.0.0.1:{server.server_port}/v1'}],
            'compact_at':None, 'resume_last':False}))
        environment = {k:v for k,v in os.environ.items() if not k.startswith(('PICO_', 'OPENAI_', 'HYPER_', 'XAI_'))}
        environment.update(XDG_CONFIG_HOME=str(root/'config'), XDG_SESSION_TYPE='x11',
                           LIBGL_ALWAYS_SOFTWARE='1', OPENAI_API_KEY='loopback-fixture-only')
        with (root/'pico.log').open('w') as log:
            process = subprocess.Popen([binary,'--safe'],cwd=root,env=environment,stdout=log,stderr=log)
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
                x('mousemove','--window',window,350,720); x('click',1)
                x('type','--clearmodifiers','--delay',15,'Exercise the embedded browser.'); x('key','Return')
                while not Fixture.done.wait(.05):
                    assert process.poll() is None, 'Pico exited before provider observation'
                assert not Fixture.failure, Fixture.failure
                # Wait for ordinary asynchronous persistence to adopt the terminal record.
                while True:
                    sessions = list((root/'config/pico/sessions').glob('*/*.jsonl'))
                    records = []
                    for session in sessions:
                        records = [json.loads(line) for line in session.read_text().splitlines()]
                        if any('Visual loop verified.' in json.dumps(r) for r in records): break
                    if records and any('Visual loop verified.' in json.dumps(r) for r in records): break
                    assert process.poll() is None; time.sleep(.02)
                images = [p for record in records if record.get('type') == 'tool_result' for p in record.get('parts',[])]
                assert len(images) == 3, images
                sid = records[0]['id']
                for image in images:
                    assert Path(image['path']).parent == root/'.pico/media/browser'/sid
                    assert Path(image['path']).is_file()
                assert not any('base64,' in json.dumps(r) for r in records), 'image bytes leaked into JSONL'
                print('PASS: real browser_open/observe/act, native click/type/scroll, model-visible PNG payloads, clipboard refusal, and owned history assets')
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
    server.shutdown()

if __name__ == '__main__':
    main()
