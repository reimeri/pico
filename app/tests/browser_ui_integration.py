#!/usr/bin/env python3
"""Real Pico/Clay/Raylib browser presentation, driven through the UI.
Run on an isolated X11 display (not the user's desktop):
  nix develop .#browser-tests -c xvfb-run -a -s '-screen 0 1600x1200x24' \
    timeout 90s python3 app/tests/browser_ui_integration.py app/build/debug/pico
The outer timeout is the overall hang watchdog, not a page-load speed assertion.
"""
import http.server
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import time
from PIL import Image

PAGE_COLORS = [(12, 34, 56), (65, 43, 21)]


class Page(http.server.BaseHTTPRequestHandler):
    visits = 0

    def do_GET(self):
        if self.path != '/':
            self.send_response(204)
            self.end_headers()
            return
        color = PAGE_COLORS[min(Page.visits, len(PAGE_COLORS) - 1)]
        Page.visits += 1
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write((f'<!doctype html><body style="margin:0;background:rgb{color}">' +
                         '<h1>Browser rendering fixture</h1>').encode())

    def log_message(self, *args):
        pass


def main():
    if os.environ.get('WAYLAND_DISPLAY') or not os.environ.get('DISPLAY'):
        raise RuntimeError('Run this test under xvfb-run, with WAYLAND_DISPLAY unset.')
    binary = str(Path(sys.argv[1]).resolve())
    phase = 'startup'

    def interrupted(signum, frame):
        raise RuntimeError(f'UI test interrupted during {phase}')

    signal.signal(signal.SIGTERM, interrupted)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Page)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='pico-browser-ui-') as directory:
        root = Path(directory)
        session = root / 'session.jsonl'
        session.write_text(json.dumps({'type': 'session', 'version': 5,
                                      'id': '123456abcdef', 'kind': 'normal', 'cwd': directory}) + '\n')
        environment = dict(os.environ, XDG_CONFIG_HOME=str(root / 'config'),
                           XDG_SESSION_TYPE='x11', LIBGL_ALWAYS_SOFTWARE='1')
        with (root / 'pico.log').open('w') as log:
            process = subprocess.Popen([binary, '--safe', '--session', str(session)],
                                       cwd=root, env=environment, stdout=log, stderr=log)

            def x(*args):
                return subprocess.check_output(['xdotool', *map(str, args)], text=True).strip()

            def alive():
                if process.poll() is not None:
                    raise RuntimeError(f'Pico exited during {phase}')

            def click(xpos, ypos):
                x('mousemove', '--window', window, xpos, ypos)
                x('click', 1)

            def screenshot():
                file = root / 'pico_screenshot.png'
                file.unlink(missing_ok=True)
                while True:
                    alive()
                    x('key', '--delay', 60, 'F12')
                    # Poll for a complete new capture rather than assuming a
                    # fixed rendering/IPC delay. F12 is Pico's normal UI action.
                    for _ in range(10):
                        try:
                            with Image.open(file) as image:
                                return image.convert('RGB')
                        except (FileNotFoundError, OSError):
                            time.sleep(.02)

            def wait_page(color):
                while True:
                    image = screenshot()
                    # Check actual presented website pixels, not just receipt
                    # of a frame or HTTP request. An opaque Clay overlay used
                    # to leave this region white despite successful navigation.
                    if image.getpixel((900, 300)) == color:
                        return

            try:
                window = x('search', '--sync', '--pid', process.pid).splitlines()[0]
                x('windowsize', window, 1100, 800)
                x('windowfocus', '--sync', window)
                phase = 'opening the browser'
                # Explicit fixture viewport and ordinary user hit locations.
                # Repeat until first browser initialization, not for a fixed
                # startup delay (the resumed session may still be loading).
                while not (root / 'config/pico/browser').exists():
                    alive()
                    click(1075, 25)
                    time.sleep(.05)
                phase = 'address-bar navigation'
                click(900, 60)
                x('key', '--delay', 60, 'ctrl+a')
                x('type', '--clearmodifiers', '--delay', 20, f'http://127.0.0.1:{server.server_port}/')
                x('key', '--delay', 60, 'Return')
                wait_page(PAGE_COLORS[0])
                phase = 'toolbar reload'
                click(680, 28)
                wait_page(PAGE_COLORS[1])
                print('PASS: address-bar navigation and toolbar reload visibly render page content')
            except BaseException:
                log.flush()
                print((root / 'pico.log').read_text(), file=sys.stderr)
                raise
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                # The helper sees the closed IPC sockets and flushes/closes.
                # Wait for its lifetime lock before deleting the profile tree.
                owner = root / 'config/pico/browser/.pico-owner.lock'
                if owner.exists():
                    import fcntl
                    with owner.open() as lock:
                        fcntl.flock(lock, fcntl.LOCK_EX)
    server.shutdown()


if __name__ == '__main__':
    # xvfb-run supplies DISPLAY; do not let a desktop Wayland socket leak in.
    os.environ.pop('WAYLAND_DISPLAY', None)
    main()
