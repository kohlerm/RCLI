#!/usr/bin/env python3
"""
Simple RCLI proxy test client (no OpenCode required).

Examples:
  python3 scripts/proxy_test.py listen --duration 20
  python3 scripts/proxy_test.py speak --text "Hello from proxy test"
  python3 scripts/proxy_test.py repl
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import threading
import time
from typing import Any


def expand_socket_path(path: str) -> str:
    return os.path.expanduser(path)


class ProxyClient:
    def __init__(self, socket_path: str):
        self.socket_path = socket_path
        self.sock: socket.socket | None = None
        self._reader: threading.Thread | None = None
        self._running = False
        self._messages: list[dict[str, Any]] = []
        self._lock = threading.Lock()

    def connect(self, timeout: float = 5.0) -> None:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(self.socket_path)
        s.settimeout(None)
        self.sock = s
        self._running = True
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def close(self) -> None:
        self._running = False
        if self.sock is not None:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None

    def send(self, payload: dict[str, Any]) -> None:
        if self.sock is None:
            raise RuntimeError("Not connected")
        line = json.dumps(payload, separators=(",", ":")) + "\n"
        self.sock.sendall(line.encode("utf-8"))

    def _read_loop(self) -> None:
        assert self.sock is not None
        buf = ""
        while self._running:
            try:
                data = self.sock.recv(4096)
                if not data:
                    print("[proxy-test] disconnected")
                    return
                buf += data.decode("utf-8", errors="replace")
                lines = buf.split("\n")
                buf = lines.pop() if lines else ""
                for line in lines:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        msg = json.loads(line)
                    except json.JSONDecodeError:
                        print(f"[proxy-test] raw: {line}")
                        continue
                    with self._lock:
                        self._messages.append(msg)
                    ts = time.strftime("%H:%M:%S")
                    print(f"[{ts}] {json.dumps(msg, ensure_ascii=False)}")
            except OSError as e:
                if self._running:
                    print(f"[proxy-test] socket error: {e}")
                return

    def snapshot_messages(self) -> list[dict[str, Any]]:
        with self._lock:
            return list(self._messages)


def send_default_config(client: ProxyClient, args: argparse.Namespace) -> None:
    client.send(
        {
            "type": "config",
            "sttModel": args.stt_model,
            "vadThreshold": args.vad_threshold,
            **({"ttsVoice": args.tts_voice} if args.tts_voice else {}),
        }
    )


def cmd_listen(client: ProxyClient, args: argparse.Namespace) -> int:
    print("[proxy-test] enabling listening; speak into mic...")
    send_default_config(client, args)
    client.send({"type": "toggle", "enabled": True})
    end_at = time.time() + args.duration
    try:
        while time.time() < end_at:
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    print("[proxy-test] disabling listening")
    client.send({"type": "toggle", "enabled": False})
    time.sleep(0.4)
    return 0


def cmd_speak(client: ProxyClient, args: argparse.Namespace) -> int:
    send_default_config(client, args)
    print(f"[proxy-test] speak: {args.text}")
    client.send({"type": "speak", "text": args.text})
    time.sleep(args.wait)
    return 0


def cmd_listen_file(client: ProxyClient, args: argparse.Namespace) -> int:
    audio = os.path.expanduser(args.audio)
    if not os.path.exists(audio):
        print(f"[proxy-test] audio file not found: {audio}")
        return 1

    player = "afplay"
    if not shutil_which(player):
        print("[proxy-test] afplay not found (macOS tool required for listen-file mode)")
        return 1

    start_idx = len(client.snapshot_messages())

    print(f"[proxy-test] enabling listening; replaying file: {audio}")
    send_default_config(client, args)
    client.send({"type": "toggle", "enabled": True})
    time.sleep(0.4)

    try:
        subprocess.run([player, audio], check=True)
    except subprocess.CalledProcessError as e:
        print(f"[proxy-test] failed to play audio: {e}")
        client.send({"type": "toggle", "enabled": False})
        return 1

    time.sleep(args.post_wait)
    print("[proxy-test] disabling listening")
    client.send({"type": "toggle", "enabled": False})
    time.sleep(0.4)

    if args.show_phrases:
        msgs = client.snapshot_messages()[start_idx:]
        finals: list[str] = []
        for msg in msgs:
            if msg.get("type") == "transcript" and msg.get("isFinal"):
                text = str(msg.get("text", "")).strip()
                if text and (not finals or finals[-1] != text):
                    finals.append(text)

        print("\n[proxy-test] final phrases:")
        if not finals:
            print("  (none)")
        else:
            for i, phrase in enumerate(finals, start=1):
                print(f"  {i}. {phrase}")

    return 0


def cmd_repl(client: ProxyClient, args: argparse.Namespace) -> int:
    send_default_config(client, args)
    print("[proxy-test] REPL started. Commands: on, off, speak <text>, config, quit")
    while True:
        try:
            raw = input("proxy> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return 0
        if not raw:
            continue
        if raw in {"quit", "exit", "q"}:
            return 0
        if raw == "on":
            client.send({"type": "toggle", "enabled": True})
            continue
        if raw == "off":
            client.send({"type": "toggle", "enabled": False})
            continue
        if raw == "config":
            send_default_config(client, args)
            continue
        if raw.startswith("speak "):
            text = raw[6:].strip()
            if text:
                client.send({"type": "speak", "text": text})
            continue
        print("unknown command")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Test RCLI proxy without OpenCode")
    p.add_argument("mode", choices=["listen", "listen-file", "speak", "repl"], help="Test mode")
    p.add_argument("--socket", default="~/.opencode/rcli-voice.sock", help="Unix socket path")
    p.add_argument("--stt-model", default="zipformer", help="STT model in config message")
    p.add_argument("--vad-threshold", type=float, default=0.5, help="VAD threshold in config message")
    p.add_argument("--tts-voice", default="", help="Optional TTS voice override")
    p.add_argument("--duration", type=int, default=20, help="Listen duration (seconds)")
    p.add_argument("--audio", default="", help="Audio file for listen-file mode (wav/m4a/aiff)")
    p.add_argument("--post-wait", type=float, default=1.8, help="Seconds to wait after playback")
    p.add_argument("--show-phrases", action="store_true", help="Print final transcript phrases after listen-file run")
    p.add_argument("--text", default="Hello from proxy test", help="Text for speak mode")
    p.add_argument("--wait", type=float, default=3.0, help="How long to wait after speak")
    return p


def shutil_which(name: str) -> str | None:
    path = os.environ.get("PATH", "")
    for base in path.split(":"):
        candidate = os.path.join(base, name)
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None


def main() -> int:
    args = build_parser().parse_args()
    socket_path = expand_socket_path(args.socket)

    if not os.path.exists(socket_path):
        print(f"[proxy-test] socket not found: {socket_path}")
        print("[proxy-test] start proxy first:")
        print("  rcli proxy --socket ~/.opencode/rcli-voice.sock --models ~/Library/RCLI/models")
        return 1

    client = ProxyClient(socket_path)
    try:
        client.connect()
        print(f"[proxy-test] connected: {socket_path}")

        if args.mode == "listen":
            return cmd_listen(client, args)
        if args.mode == "listen-file":
            return cmd_listen_file(client, args)
        if args.mode == "speak":
            return cmd_speak(client, args)
        if args.mode == "repl":
            return cmd_repl(client, args)
        return 2
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())
