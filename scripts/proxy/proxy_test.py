#!/usr/bin/env python3
"""
Simple RCLI proxy test client (no OpenCode required).

Examples:
  python3 scripts/proxy/proxy_test.py listen --duration 20
  python3 scripts/proxy/proxy_test.py speak --text "Hello from proxy test"
  python3 scripts/proxy/proxy_test.py repl
"""

from __future__ import annotations

import argparse
import json
import os
import re
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


def run_cmd(cmd: list[str]) -> tuple[int, str, str]:
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def parse_duration_seconds(path: str) -> float:
    rc, out, err = run_cmd([
        "ffprobe",
        "-v",
        "error",
        "-show_entries",
        "format=duration",
        "-of",
        "default=nokey=1:noprint_wrappers=1",
        path,
    ])
    if rc != 0:
        raise RuntimeError(f"ffprobe failed: {err.strip()}")
    return float(out.strip())


def detect_non_silent_intervals(path_wav: str, silence_db: float, silence_dur: float, min_segment: float) -> list[tuple[float, float]]:
    rc, _out, err = run_cmd([
        "ffmpeg",
        "-i",
        path_wav,
        "-af",
        f"silencedetect=noise={silence_db}dB:d={silence_dur}",
        "-f",
        "null",
        "-",
    ])
    if rc != 0:
        # ffmpeg writes silencedetect to stderr and often returns 0, but on some builds
        # it may still return non-zero for null muxing edge cases. Continue if stderr has data.
        if not err:
            raise RuntimeError("ffmpeg silencedetect failed")

    duration = parse_duration_seconds(path_wav)
    silences: list[tuple[float, float]] = []
    pending_start: float | None = None

    for line in err.splitlines():
        m_start = re.search(r"silence_start:\s*([0-9.]+)", line)
        if m_start:
            pending_start = float(m_start.group(1))
            continue
        m_end = re.search(r"silence_end:\s*([0-9.]+)", line)
        if m_end and pending_start is not None:
            end = float(m_end.group(1))
            silences.append((pending_start, end))
            pending_start = None

    if pending_start is not None:
        silences.append((pending_start, duration))

    intervals: list[tuple[float, float]] = []
    cursor = 0.0
    for s_start, s_end in silences:
        if s_start - cursor >= min_segment:
            intervals.append((cursor, s_start))
        cursor = max(cursor, s_end)

    if duration - cursor >= min_segment:
        intervals.append((cursor, duration))

    # If no silences were detected, treat full file as one phrase.
    if not intervals and duration >= min_segment:
        intervals.append((0.0, duration))

    return intervals


def cmd_segment_file(args: argparse.Namespace) -> int:
    audio = os.path.expanduser(args.audio)
    if not audio or not os.path.exists(audio):
        print(f"[proxy-test] audio file not found: {audio}")
        return 1

    rcli_bin = os.path.expanduser(args.rcli_bin)
    if not os.path.exists(rcli_bin):
        print(f"[proxy-test] rcli binary not found: {rcli_bin}")
        return 1

    tmp_base = f"/tmp/rcli-seg-{int(time.time())}-{os.getpid()}"
    src_wav = tmp_base + "-src.wav"

    rc, _out, err = run_cmd(["ffmpeg", "-y", "-i", audio, "-ac", "1", "-ar", "16000", src_wav])
    if rc != 0:
        print(f"[proxy-test] failed to convert audio to WAV: {err.strip()}")
        return 1

    try:
        intervals = detect_non_silent_intervals(src_wav, args.silence_db, args.silence_dur, args.min_segment)
    except Exception as e:
        print(f"[proxy-test] silence detection failed: {e}")
        return 1

    print(f"[proxy-test] detected {len(intervals)} phrase segment(s)")
    phrases: list[str] = []

    for idx, (start, end) in enumerate(intervals, start=1):
        seg_wav = f"{tmp_base}-seg{idx}.wav"
        seg_out = f"{tmp_base}-seg{idx}-tts.wav"

        rc, _o, err = run_cmd([
            "ffmpeg",
            "-y",
            "-i",
            src_wav,
            "-ss",
            f"{start:.3f}",
            "-to",
            f"{end:.3f}",
            "-ac",
            "1",
            "-ar",
            "16000",
            seg_wav,
        ])
        if rc != 0:
            print(f"  {idx}. [skip] segment extract failed: {err.strip()}")
            continue

        rc, out, err = run_cmd([rcli_bin, "process-wav", seg_wav, seg_out])
        stt_match = re.search(r'STT result:\s*"(.*?)"', err)
        phrase = stt_match.group(1).strip() if stt_match else ""
        if not phrase and out.strip():
            phrase = out.strip().splitlines()[-1].strip()

        print(f"  {idx}. [{start:.2f}s - {end:.2f}s] {phrase if phrase else '(no transcript)'}")
        if phrase:
            phrases.append(phrase)

    if phrases:
        print("\n[proxy-test] phrases:")
        for i, p in enumerate(phrases, start=1):
            print(f"  {i}. {p}")
    else:
        print("\n[proxy-test] no phrases recognized")

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
    p.add_argument("mode", choices=["listen", "listen-file", "segment-file", "speak", "repl"], help="Test mode")
    p.add_argument("--socket", default="~/.opencode/rcli-voice.sock", help="Unix socket path")
    p.add_argument("--stt-model", default="zipformer", help="STT model in config message")
    p.add_argument("--vad-threshold", type=float, default=0.5, help="VAD threshold in config message")
    p.add_argument("--tts-voice", default="", help="Optional TTS voice override")
    p.add_argument("--duration", type=int, default=20, help="Listen duration (seconds)")
    p.add_argument("--audio", default="", help="Audio file for listen-file mode (wav/m4a/aiff)")
    p.add_argument("--post-wait", type=float, default=1.8, help="Seconds to wait after playback")
    p.add_argument("--show-phrases", action="store_true", help="Print final transcript phrases after listen-file run")
    p.add_argument("--rcli-bin", default=os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "build", "rcli")), help="Path to rcli binary (for segment-file mode)")
    p.add_argument("--silence-db", type=float, default=-35.0, help="Silence threshold in dB for segment-file mode")
    p.add_argument("--silence-dur", type=float, default=0.5, help="Minimum silence duration (seconds) used to split phrases")
    p.add_argument("--min-segment", type=float, default=0.25, help="Minimum non-silent segment length (seconds)")
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

    if args.mode == "segment-file":
        return cmd_segment_file(args)

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
