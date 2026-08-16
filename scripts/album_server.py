#!/usr/bin/env python3
"""Static file server for the QO-100 DATV screenshot album, plus one
dynamic route: POST /snap runs screenshot.sh (grim capture of the whole
display + album_update.sh reindex) then redirects back to the gallery.

This lets the "Take Screenshot" button on the web page trigger a capture
without any IPC into the running qo100sdl process - grim grabs the whole
physical display regardless of which app currently has focus, so the web
page doesn't need to talk to the app at all.

No authentication - LAN-only hobby use, matches the rest of the album.
"""
import http.server
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.dirname(SCRIPT_DIR)
ALBUM_DIR = os.path.join(REPO_DIR, "screenshots", "album")
SCREENSHOT_SCRIPT = os.path.join(SCRIPT_DIR, "screenshot.sh")


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ALBUM_DIR, **kwargs)

    def do_POST(self):
        if self.path == "/snap":
            result = subprocess.run(
                [SCREENSHOT_SCRIPT], capture_output=True, text=True, check=False)
            if result.returncode != 0:
                sys.stderr.write(
                    "[ALBUM] screenshot.sh failed: %s\n" % result.stderr.strip())
            self.send_response(303)
            self.send_header("Location", "/")
            self.end_headers()
        else:
            self.send_error(404)

    def log_message(self, format_str, *args):
        sys.stderr.write("[ALBUM] %s\n" % (format_str % args))


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8090
    server = http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler)
    sys.stderr.write("[ALBUM] serving %s on port %d\n" % (ALBUM_DIR, port))
    server.serve_forever()
