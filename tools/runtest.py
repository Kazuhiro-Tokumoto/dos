#!/usr/bin/env python3
"""MYDOS をエミュレータで起動して、シリアルに出た結果を判定する。

カーネルを -DSERIAL_CONSOLE でビルドすると、画面に出す文字が COM1 にも
流れる。QEMU の -serial file:... でそれをホスト側のファイルに落とし、
ここで読んで合否を見る。DOS の API から見える挙動は何も変わらないので、
テストのためにカーネルの動きを変えているわけではない。

起動 → AUTOEXEC.BAT → DOSTEST.COM の順に自動で進み、DOSTEST が
'###TEST-END###' を出したところで終わりと判断する。
"""
import argparse
import os
import re
import subprocess
import sys
import time

END_MARKER = "###TEST-END###"
RESULT_RE = re.compile(r"### RESULT pass=(\d+) fail=(\d+)")

# DOSTEST の集計だけでは見られない経路 (プログラムの起動、リロケーション、
# シェルの内部コマンド) を、出力に現れる文字列で確かめる。
REQUIRED = [
    ("Hello from HELLO.COM", ".COM プログラムの起動"),
    ("HELLO.EXE started", ".EXE プログラムの起動"),
    ("EXE-RELOC: PASS", ".EXE のリロケーション適用"),
    ("MYDOS - an MS-DOS compatible", "TYPE によるファイル表示"),
    ("1 file(s) copied", "COPY"),
    ("bytes in largest free block", "MEM (MCB 連鎖の走査)"),
]


def run(image, timeout, qemu, keep_log):
    log_path = os.path.join(os.path.dirname(image) or ".", "serial.log")
    if os.path.exists(log_path):
        os.remove(log_path)

    cmd = [
        qemu,
        "-fda", image,
        "-boot", "a",
        "-display", "none",
        "-no-reboot",
        "-serial", f"file:{log_path}",
    ]
    print("$ " + " ".join(cmd))
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    deadline = time.time() + timeout
    text = ""
    try:
        while time.time() < deadline:
            if proc.poll() is not None:
                break
            if os.path.exists(log_path):
                with open(log_path, "rb") as f:
                    text = f.read().decode("latin-1")
                if END_MARKER in text:
                    break
            time.sleep(0.25)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

    if os.path.exists(log_path):
        with open(log_path, "rb") as f:
            text = f.read().decode("latin-1")

    print("=" * 70)
    print(text.replace("\r\n", "\n").rstrip())
    print("=" * 70)

    if not keep_log and os.path.exists(log_path):
        os.remove(log_path)

    if END_MARKER not in text:
        print(f"\n判定: 失敗 — {timeout} 秒以内にテストが終わらなかった")
        if not text.strip():
            print("      シリアルに何も出ていない。ブートの段階で止まっている可能性がある。")
        return 1

    m = RESULT_RE.search(text)
    if not m:
        print("\n判定: 失敗 — 集計行が見つからない")
        return 1

    npass, nfail = int(m.group(1)), int(m.group(2))

    missing = [(needle, label) for needle, label in REQUIRED if needle not in text]
    for needle, label in missing:
        print(f"  [FAIL] {label}  (出力に '{needle}' がない)")
    for needle, label in REQUIRED:
        if (needle, label) not in missing:
            print(f"  [PASS] {label}")

    if nfail or missing:
        print(f"\n判定: 失敗 — DOSTEST {npass}/{npass + nfail} 件、"
              f"出力の確認 {len(REQUIRED) - len(missing)}/{len(REQUIRED)} 件")
        return 1

    print(f"\n判定: 成功 — DOSTEST {npass} 件 + 出力の確認 {len(REQUIRED)} 件、すべて成功")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image")
    ap.add_argument("-t", "--timeout", type=int, default=60)
    ap.add_argument("--qemu", default="qemu-system-i386")
    ap.add_argument("--keep-log", action="store_true")
    args = ap.parse_args()
    sys.exit(run(args.image, args.timeout, args.qemu, args.keep_log))


if __name__ == "__main__":
    main()
