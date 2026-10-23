#!/usr/bin/env python3
"""Real renderer kill: precise failure, pending evaluation, isolation, reopen.
Run inside nix develop, guarded by an outer timeout. Sandboxing remains enabled.
"""
import http.server
import json
import os
from pathlib import Path
import signal
import sys
import tempfile
import threading

from browser_integration import Browser
from browser_profile import process_tree


class Fixture(http.server.BaseHTTPRequestHandler):
    pending = threading.Event()

    def do_GET(self):
        if self.path == '/pending':
            self.pending.set(); self.send_response(204); self.end_headers()
        else:
            self.send_response(200); self.send_header('Content-Type','text/html'); self.end_headers()
            self.wfile.write(b'<body style="background:rgb(12,34,56)">Renderer isolation fixture</body>')

    def log_message(self,*args):
        pass


def main():
    def interrupted(signum, frame):
        raise RuntimeError('renderer failure test interrupted')
    signal.signal(signal.SIGTERM,interrupted)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Fixture)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='pico-browser-failure-') as root:
        browser=Browser(str(Path(sys.argv[1]).resolve()),root)
        try:
            url=f'http://127.0.0.1:{server.server_port}/'
            browser.send(1,1,profile='abcdef123456',text=url); browser.wait(1,1)
            browser.send(1,18,a=1); browser.observe(1)
            renderers=[]
            for pid,_ in process_tree(browser.process.pid):
                try:
                    if b'--type=renderer ' in Path(f'/proc/{pid}/cmdline').read_bytes():
                        renderers.append(pid)
                except FileNotFoundError:
                    pass
            assert renderers, 'no renderer subprocess'
            # Chromium may retain a spare renderer. Identify the page renderer
            # through real evaluation CPU work, not launch order or client IDs.
            before = process_tree(browser.process.pid)
            error,result = browser.inspect(1,21,{'expression':"(()=>{const until=performance.now()+500;while(performance.now()<until){};return true})()"})
            assert not error, result
            after = process_tree(browser.process.pid)
            activity = {pid: ticks-before.get((pid,start),(ticks,0))[0] for (pid,start),(ticks,_) in after.items() if pid in renderers}
            renderer = max(activity,key=activity.get)
            assert activity[renderer] > 0, activity
            browser.send(2,1,profile='abcdef654321',text=url); browser.wait(2,1)
            browser.send(2,18,a=1); browser.observe(2)
            browser.sequence+=1; request=browser.sequence
            with tempfile.NamedTemporaryFile(prefix='pico-browser-arguments-',mode='w') as argument:
                json.dump({'expression':"new Promise(resolve=>{fetch('/pending');setTimeout(()=>resolve('late'),20000)})"},argument)
                argument.flush(); browser.send(1,21,request=request,text=argument.name)
                while not Fixture.pending.wait(.1):
                    assert browser.process.poll() is None, 'helper exited before pending evaluation'
                os.kill(renderer,signal.SIGKILL)
                stopped=browser.wait(1,8)
                assert b'renderer killed' in stopped[-1], stopped
                error,result=browser.text_result(1,request)
                assert error and 'renderer killed' in result and 'uncertain' in result, result
            browser.wait(1,5)
            assert browser.process.poll() is None, 'renderer kill stopped shared helper'
            browser.observe(2)
            error,result=browser.inspect(2,21,{'expression':"document.body.textContent"})
            assert not error and 'Renderer isolation fixture' in result, result
            browser.send(3,1,profile='abcdef123456',text=url); browser.wait(3,1)
            browser.observe(3)
            # Malformed command packets fail explicitly, not a mysterious exit0.
            browser.commands.send(b'malformed command')
            malformed=browser.wait(0,4)
            assert b'command protocol error' in malformed[-1], malformed
            print('PASS: renderer kill cause, pending evaluation failure, independent page progress, explicit reopen and command protocol errors')
        finally:
            browser.close()
    server.shutdown(); server.server_close()


if __name__=='__main__':
    main()
