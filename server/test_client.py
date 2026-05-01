"""
Smoke test: post 3 seconds of silence to the server from your laptop.
Run while uvicorn is up:
    python test_client.py
    python test_client.py http://192.168.1.42:8000/donaldify  # specific URL
"""

import sys

import urllib.request

URL = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8000/donaldify"
SAMPLE_RATE = 16000
SECONDS = 3
silence = b"\x00\x00" * (SAMPLE_RATE * SECONDS)

req = urllib.request.Request(
    URL,
    data=silence,
    headers={"Content-Type": "application/octet-stream"},
    method="POST",
)
with urllib.request.urlopen(req, timeout=10) as resp:
    body = resp.read()

print(f"status: {resp.status}")
print(f"sent:   {len(silence)} bytes")
print(f"got:    {len(body)} bytes  (echo intact: {len(body) == len(silence)})")
