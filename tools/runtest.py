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
    ("TSR-RESIDENT: PASS", "AH=31h 常駐終了が本当に残っている"),
    ("OVERLAY-RELOC: PASS", "AH=4Bh AL=3 オーバーレイのリロケーション"),
    ("C:\\>", "COMMAND.COM のドライブ切り替え (C:)"),
    ("Directory of C:\\", "DIR がハードディスク側を読む"),
    ("MYDEV.SYS installed, args=hello-from-config",
     "CONFIG.SYS の DEVICE= が引数付きで文字デバイスを組み込む"),
    ("RAMDISK.SYS installed as drive",
     "CONFIG.SYS の DEVICE= がブロックデバイスを組み込む"),
    ("INSTALL: instest ran with args: alpha beta",
     "CONFIG.SYS の INSTALL= がプログラムを引数付きで実行する"),
    ("My Long File Name.txt", "DIR が長いファイル名をそのまま表示する"),
    ("tail-line", "COMMAND.COM の >> による追記"),
    ("PIPE-GOT: MYDOS", "COMMAND.COM の | と < でハンドルが差し替わる"),
    ("after XMS block move", "XMS のブロック転送のあとでもハードディスクを触れる"),
    ("EMM386.SYS installed", "CONFIG.SYS の DEVICE= が EMS ドライバを組み込む"),
    ("Format complete.", "FORMAT が別のフロッピーを FAT12 で作り直す"),
    ("System transferred", "SYS がブートローダとシステムファイルを移す"),
    ("Directory of B:\\", "DIR がドライブ指定を見る"),
    ("Master boot record written.", "FDISK がマスターブートレコードを書く"),
]

# XMSTEST / CFGTEST / HDTEST / DOSINT / FCBTEST / DOSTEST / ENVTEST の
# LFNTEST を加えた 8 本が、それぞれ集計行と終了の目印を出す。
# ENVTEST は A: と C: の両方から走らせるので、集計行は 9 本ぶん出る。
EXPECTED_SUITES = 10

# 作ったディスクから起動できたと判断する目印
BOOT_MARKER = "MYDOS Command Interpreter"


def run(image, timeout, qemu, keep_log, hd, fdb=None,
        verify_fd=None, verify_hd=None):
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
    if hd:
        # パーティションを切った FAT16 のハードディスク。
        # MYDOS はフロッピーから起動し、こちらは C: / D: として見えるはず。
        cmd += ["-hda", hd]
    if fdb:
        # 2 台目のフロッピー。MYDOS 以外の道具で作った普通の FAT12 で、
        # テストの中で FORMAT と SYS を掛けて起動できるようにする。
        cmd += ["-fdb", fdb]
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
                if text.count(END_MARKER) >= EXPECTED_SUITES:
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

    if text.count(END_MARKER) < EXPECTED_SUITES:
        print(f"\n判定: 失敗 — {timeout} 秒以内に "
              f"{EXPECTED_SUITES} 本のテストが終わらなかった "
              f"(終了したのは {text.count(END_MARKER)} 本)")
        if not text.strip():
            print("      シリアルに何も出ていない。ブートの段階で止まっている可能性がある。")
        return 1

    results = RESULT_RE.findall(text)
    if len(results) < EXPECTED_SUITES:
        print(f"\n判定: 失敗 — 集計行が {len(results)} 本ぶんしか見つからない")
        return 1

    npass = sum(int(a) for a, _ in results)
    nfail = sum(int(b) for _, b in results)

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

    if verify_fd:
        rc = boot_check(verify_fd, qemu, timeout, "-fda", "a",
                        "FORMAT + SYS で作ったフロッピー")
        if rc:
            return rc
    if verify_hd:
        rc = boot_check(verify_hd, qemu, timeout, "-hda", "c",
                        "FDISK + FORMAT + SYS で入れたハードディスク")
        if rc:
            return rc

    print(f"\n判定: 成功 — DOSTEST {npass} 件 + 出力の確認 {len(REQUIRED)} 件、すべて成功")
    return 0


def boot_check(image, qemu, timeout, media, boot_order, label):
    """MYDOS 自身が用意したディスクから、本当に起動できるか。

    テストの中で作られたディスクを、今度は起動ディスクとして立ち上げる。
    ブートセクタ・Stage2・IO.SYS・COMMAND.COM が揃って初めてここまで来る。
    ハードディスクの場合はさらに MBR も要る。
    """
    log_path = os.path.join(os.path.dirname(image) or ".", "bootcheck.log")
    if os.path.exists(log_path):
        os.remove(log_path)
    cmd = [qemu, media, image, "-boot", boot_order, "-display", "none",
           "-no-reboot", "-serial", f"file:{log_path}"]
    print(f"\n--- {label}から起動してみる ---")
    print("$ " + " ".join(cmd))
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    deadline = time.time() + min(timeout, 60)
    text = ""
    try:
        while time.time() < deadline:
            if proc.poll() is not None:
                break
            if os.path.exists(log_path):
                with open(log_path, "rb") as f:
                    text = f.read().decode("latin-1")
                if BOOT_MARKER in text:
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
        os.remove(log_path)
    print(text.replace("\r\n", "\n").rstrip())
    if BOOT_MARKER not in text:
        print(f"\n判定: 失敗 — {label}から起動できなかった")
        return 1
    print(f"  [PASS] {label}が起動する")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image")
    ap.add_argument("-t", "--timeout", type=int, default=150)
    ap.add_argument("--qemu", default="qemu-system-i386")
    ap.add_argument("--keep-log", action="store_true")
    ap.add_argument("--hd", help="ハードディスクのイメージ (C: / D: になる)")
    ap.add_argument("--fdb", help="2 台目のフロッピー (B: になる)")
    ap.add_argument("--verify-boot-fd",
                    help="テストのあと、このフロッピーから起動できるか確かめる")
    ap.add_argument("--verify-boot-hd",
                    help="テストのあと、このハードディスクから起動できるか確かめる")
    args = ap.parse_args()
    sys.exit(run(args.image, args.timeout, args.qemu, args.keep_log, args.hd,
                 args.fdb, args.verify_boot_fd, args.verify_boot_hd))


if __name__ == "__main__":
    main()
