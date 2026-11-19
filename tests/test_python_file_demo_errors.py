#!/usr/bin/env python3
"""Run python/examples/tts_file_demo.py against a fake spacemit_tts.

Checks that the demo exits non-zero, without the success banner, when any
synthesis or any save fails, and that it writes every file when nothing
fails. Needs no models and no compiled bindings.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

DEMO = Path(__file__).resolve().parents[1] / "python" / "examples" / "tts_file_demo.py"
SUCCESS_BANNER = "Demo completed successfully"

# Mirrors the parts of spacemit_tts the demo uses. A failed synthesis carries
# no audio, so bool(result) is False and save() returns False, as with the
# real bindings. Every synthesis appends a line to FAKE_TTS_CALLS.
FAKE_MODULE = '''
import os
from pathlib import Path

_calls = 0


class Config:
    @staticmethod
    def preset(name):
        return Config()

    def with_provider(self, name):
        return self


class Result:
    def __init__(self, index):
        self.index = index
        self.is_success = index != int(os.environ.get("FAKE_TTS_FAIL_SYNTH", "0"))
        self.message = "" if self.is_success else "injected synthesis failure"
        self.audio_float = [0.1] * 160 if self.is_success else []
        self.duration_ms = 10 if self.is_success else 0
        self.rtf = 0.1

    def __bool__(self):
        return bool(self.audio_float)

    def save(self, file_path):
        if not self.audio_float or self.index == int(os.environ.get("FAKE_TTS_FAIL_SAVE", "0")):
            return False
        Path(file_path).write_bytes(b"RIFF")
        return True


class Engine:
    engine_name = "fake"
    sample_rate = 16000

    def __init__(self, config=None):
        pass

    def set_speed(self, speed):
        pass

    def synthesize(self, text):
        global _calls
        _calls += 1
        with open(os.environ["FAKE_TTS_CALLS"], "a") as calls:
            calls.write(text + "\\n")
        return Result(_calls)


def synthesize_to_file(text, file_path):
    return Engine().synthesize(text).save(file_path)
'''


def run_demo(fail_synth=0, fail_save=0):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        (root / "fake").mkdir()
        (root / "fake" / "spacemit_tts.py").write_text(FAKE_MODULE)
        calls = root / "calls.txt"
        env = dict(os.environ,
                   PYTHONPATH=str(root / "fake"),
                   PYTHONDONTWRITEBYTECODE="1",
                   FAKE_TTS_CALLS=str(calls),
                   FAKE_TTS_FAIL_SYNTH=str(fail_synth),
                   FAKE_TTS_FAIL_SAVE=str(fail_save))
        out_dir = root / "out"
        out_dir.mkdir()
        proc = subprocess.run([sys.executable, str(DEMO)], cwd=out_dir, env=env,
                              capture_output=True, text=True, timeout=60)
        synth_calls = len(calls.read_text().splitlines()) if calls.exists() else 0
        wav_files = sorted(p.name for p in out_dir.glob("*.wav"))
        return proc, synth_calls, wav_files


def main():
    failures = []

    proc, total, wav_files = run_demo()
    if proc.returncode != 0 or SUCCESS_BANNER not in proc.stdout:
        failures.append(f"success run: exit {proc.returncode}\n{proc.stdout}{proc.stderr}")
    if total == 0 or len(wav_files) != total:
        failures.append(f"success run: {total} syntheses but files {wav_files}")
    print(f"success run: exit {proc.returncode}, {total} syntheses, {len(wav_files)} files")

    for kind, index in [("synthesis", i) for i in range(1, total + 1)] + \
                       [("save", i) for i in range(1, total + 1)]:
        args = {"fail_synth": index} if kind == "synthesis" else {"fail_save": index}
        proc, _, _ = run_demo(**args)
        ok = (proc.returncode != 0 and SUCCESS_BANNER not in proc.stdout
              and "Traceback" not in proc.stderr and proc.stderr.strip().startswith("✗"))
        print(f"{kind} failure #{index}: exit {proc.returncode} -> {'ok' if ok else 'WRONG'}")
        if not ok:
            failures.append(f"{kind} failure #{index}: exit {proc.returncode}\n{proc.stdout}{proc.stderr}")

    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("PASS --python-file-demo-errors")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
