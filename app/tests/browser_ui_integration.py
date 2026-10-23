#!/usr/bin/env python3
"""Real Pico/Clay/Raylib browser presentation, driven through the UI.
Run on an isolated hardware-backed X11 display (not the user's desktop):
  nix develop .#browser-tests -c env -u WAYLAND_DISPLAY \
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
import urllib.parse
from PIL import Image

PAGE_COLORS = [(12, 34, 56), (65, 43, 21)]
TARGET_COLOR = (220, 110, 40)


class Page(http.server.BaseHTTPRequestHandler):
    visits = 0
    clicks = []

    def do_GET(self):
        if self.path.startswith('/click?'):
            Page.clicks.append(urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query))
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
                         '<h1>Browser rendering fixture</h1>' +
                         '<button id="target" style="position:absolute;left:150px;top:300px;width:80px;height:60px;'
                         f'border:0;background:rgb{TARGET_COLOR}">Target</button>' +
                         '<script>document.onclick=e=>fetch("/click?"+new URLSearchParams({'
                         'target:e.target.id,width:innerWidth,height:innerHeight}));</script>').encode())

    def log_message(self, *args):
        pass


def main():
    if os.environ.get('WAYLAND_DISPLAY') or not os.environ.get('DISPLAY'):
        raise RuntimeError('Use an isolated hardware-backed X11 display, with WAYLAND_DISPLAY unset.')
    binary = str(Path(sys.argv[1]).resolve())
    phase = 'startup'

    def interrupted(signum, frame):
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
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
                           XDG_SESSION_TYPE='x11')
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
                # Hold across a frame; XTest's zero-duration down/up pair can
                # both arrive in one Raylib poll and never become a press. Let
                # the release span a frame too, before a consecutive click.
                x('mousedown', 1, 'sleep', '.05', 'mouseup', 1, 'sleep', '.05')

            def screenshot():
                # F12 belongs to the focused page when native browser input
                # owns focus. Return focus to inert chat space before asking
                # Pico for its normal window screenshot.
                click(350, 200)
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
                phase = 'mobile-sized viewport and mapped pointer'
                # Keep a real composer draft while applying viewport choices.
                click(420, 706)
                x('type', '--clearmodifiers', '--delay', 25, 'keep this draft')
                click(825, 28)
                click(795, 65)  # Mobile, first dropdown row.

                def colored_bounds(image, color):
                    mask = Image.new('1', image.size)
                    mask.putdata([p == color for p in image.get_flattened_data()])
                    return mask.getbbox()

                def mobile_image():
                    while True:
                        image = screenshot()
                        bounds = colored_bounds(image, PAGE_COLORS[1])
                        if bounds:
                            left, top, right, bottom = bounds
                            # A tall, responsive page must be fitted, not stretched
                            # across the wide dock or rendered at the dock's size.
                            if abs((right-left)/(bottom-top) - 390/844) < .01:
                                return image, bounds

                image, bounds = mobile_image()
                left, top, right, bottom = bounds
                target = colored_bounds(image, TARGET_COLOR)
                assert target
                click((target[0]+target[2])//2, (target[1]+target[3])//2)
                while not Page.clicks:
                    alive(); time.sleep(.02)
                assert Page.clicks[-1] == {'target':['target'], 'width':['390'], 'height':['844']}, Page.clicks
                # The margins must not become page coordinates. A subsequent
                # real click is an observable input barrier for this assertion.
                click(left-10, (top+bottom)//2)
                click((target[0]+target[2])//2, (target[1]+target[3])//2)
                while len(Page.clicks) < 2:
                    alive(); time.sleep(.02)
                assert all(c['target'] == ['target'] for c in Page.clicks), Page.clicks

                phase = 'control slider selection and drag takeover'
                target_x = (target[0]+target[2])//2
                target_y = (target[1]+target[3])//2
                # Agent is the left side. Selecting it twice must not toggle
                # back to User or let either page click through.
                click(720, 28)
                click(target_x, target_y)
                click(720, 28)
                click(target_x, target_y)
                # Drag from Agent to User, releasing outside the track. The
                # gesture belongs to the selector, not to the embedded page.
                x('mousemove', '--window', window, 720, 28)
                x('mousedown', 1, 'sleep', '.05')
                x('mousemove', '--window', window, 780, 28)
                x('sleep', '.05', 'mouseup', 1, 'sleep', '.05')
                click(target_x, target_y)
                while len(Page.clicks) < 3:
                    alive(); time.sleep(.02)
                assert len(Page.clicks) == 3, Page.clicks
                # Collapse/reopen preserves the page and selected control mode.
                phase = 'web icon collapse and reopen'
                click(1070, 28)
                collapsed = screenshot()
                assert not colored_bounds(collapsed, PAGE_COLORS[1])
                click(1070, 28)
                while not colored_bounds(screenshot(), PAGE_COLORS[1]):
                    alive()
                assert Page.visits == 2, Page.visits

                phase = 'custom viewport popover and Enter ownership'
                click(825, 28)
                x('key', '--delay', 60, 'Up', 'Return')  # Mobile wraps to Custom.
                click(684, 87)   # Width.
                x('key', '--delay', 60, 'ctrl+a')
                x('type', '--clearmodifiers', '--delay', 30, '640')
                click(808, 87)   # Height.
                x('key', '--delay', 60, 'ctrl+a')
                x('type', '--clearmodifiers', '--delay', 30, '480')
                x('key', '--delay', 60, 'Return')
                while True:
                    image = screenshot()
                    custom_bounds = colored_bounds(image, PAGE_COLORS[1])
                    if custom_bounds and abs((custom_bounds[2]-custom_bounds[0])/(custom_bounds[3]-custom_bounds[1]) - 640/480) < .01:
                        break
                target = colored_bounds(image, TARGET_COLOR)
                click((target[0]+target[2])//2, (target[1]+target[3])//2)
                while len(Page.clicks) < 4:
                    alive(); time.sleep(.02)
                assert Page.clicks[-1]['width'] == ['640'] and Page.clicks[-1]['height'] == ['480'], Page.clicks
                # Applying Custom must not submit the populated composer draft.
                assert not any(json.loads(line).get('role') == 'user' for line in session.read_text().splitlines())
                # Verify the preset has actually been presented before testing
                # the separate transition back to remembered Custom dimensions.
                phase = 'remembered custom viewport dimensions'
                click(825, 28); click(795, 65)
                mobile_image()
                click(825, 28)
                x('key', '--delay', 60, 'Up', 'Return')  # Mobile wraps to Custom.
                click(826, 163)  # Apply.
                while True:
                    image = screenshot()
                    bounds_now = colored_bounds(image, PAGE_COLORS[1])
                    if bounds_now and abs((bounds_now[2]-bounds_now[0])/(bounds_now[3]-bounds_now[1]) - 640/480) < .01:
                        break
                click(825, 28); click(795, 65)  # Restore Mobile for letterboxing checks.
                mobile_image()

                phase = 'dock resize and stable letterboxing'
                x('windowsize', window, 1300, 900)
                image, bounds = mobile_image()
                for _ in range(8):
                    assert colored_bounds(screenshot(), PAGE_COLORS[1]) == bounds
                target = colored_bounds(image, TARGET_COLOR)
                click((target[0]+target[2])//2, (target[1]+target[3])//2)
                while len(Page.clicks) < 5:
                    alive(); time.sleep(.02)
                assert len(Page.clicks) == 5 and Page.clicks[-1]['width'] == ['390'] and Page.clicks[-1]['height'] == ['844']
                print('PASS: navigation/reload presentation, control slider click/drag, web icon collapse/reopen, viewport dropdown/custom and composer Enter ownership, mapped clicks and stable resized dock')
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
    # The isolated X11 display supplies DISPLAY; do not mix in a Wayland socket.
    os.environ.pop('WAYLAND_DISPLAY', None)
    main()
