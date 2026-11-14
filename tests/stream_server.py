#!/usr/bin/env python3
# Streams an endless 1kHz tone as WAV over HTTP the way a radio server would:
# barely any burst up front (so the socket buffers can't hide a stall), then
# real time. Once a stream reaches 3s of audio it stalls, like a connection
# that drops out, for ?stall=SECONDS (0.8 by default).
#
# FFmpeg probes a WAV stream for a few seconds of audio before it plays it,
# which works as a cushion of its own, so a stall only shows past that.
#
#   stream_server.py <port>

import math
import struct
import urllib.parse
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

RATE = 44100
CHUNK = 2048
BURST_SECONDS = 0.1
STALL_AT_SECONDS = 3.0
STALL_SECONDS = 0.8


def tone_chunk(start):
    out = bytearray()
    for i in range(start, start + CHUNK):
        v = int(0.5 * 32767 * math.sin(2 * math.pi * 1000 * i / RATE))
        out += struct.pack("<hh", v, v)
    return bytes(out)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        query = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        stall_seconds = float(query.get("stall", [STALL_SECONDS])[0])
        self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.end_headers()
        # Data size as large as it goes: an endless stream
        header = b"RIFF" + struct.pack("<I", 0xFFFFFFFF) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, RATE, RATE * 4, 4, 16)
        header += b"data" + struct.pack("<I", 0xFFFFFFF0)
        try:
            self.wfile.write(header)
            frame = 0
            start = time.monotonic()
            stalled = False
            while frame < RATE * 60:
                self.wfile.write(tone_chunk(frame))
                frame += CHUNK
                if not stalled and frame >= RATE * STALL_AT_SECONDS:
                    stalled = True
                    time.sleep(stall_seconds)
                    start += stall_seconds
                ahead = frame / RATE - BURST_SECONDS - (time.monotonic() - start)
                if ahead > 0:
                    time.sleep(ahead)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *args):
        pass


ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
