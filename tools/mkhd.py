#!/usr/bin/env python3
# ============================================================================
# mkhd.py  -  テスト用のハードディスクイメージを作る
#
# MYDOS の phase D (複数ドライブ / ハードディスク / FAT16) を実機に近い形で
# 試すためのイメージ。中身は
#   LBA 0        MBR (パーティションテーブル 1 個目に FAT16 を登録)
#   LBA 63 以降  FAT16 のパーティション
# という、当時の DOS でフォーマットしたディスクと同じ並びにしてある。
#
# パーティションを 1 個目だけでなく 2 個目にも置いてあるので、
# C: と D: の 2 台が生えることまで確かめられる。
# ============================================================================
import os
import subprocess
import sys

SECTOR = 512

# ジオメトリ。当時の BIOS が返していた値に合わせる。
SPT = 63
HEADS = 16

def chs(lba):
    """LBA を CHS の 3 バイト表現に直す (届かないところは最大値で頭打ち)"""
    cyl, rem = divmod(lba, SPT * HEADS)
    head, sec = divmod(rem, SPT)
    sec += 1
    if cyl > 1023:
        cyl, head, sec = 1023, HEADS - 1, SPT
    return bytes([head, ((cyl >> 2) & 0xC0) | sec, cyl & 0xFF])

def part_entry(boot, start, count, ptype):
    return (bytes([0x80 if boot else 0x00])
            + chs(start)
            + bytes([ptype])
            + chs(start + count - 1)
            + start.to_bytes(4, 'little')
            + count.to_bytes(4, 'little'))

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'build/hd.img'

    # パーティションを 2 つ置く。1 つ目が C:、2 つ目が D: になる。
    p1_start, p1_secs = SPT, 40 * 1024 * 2          # 40MB → FAT16
    p2_start = p1_start + p1_secs
    p2_start += (-p2_start) % (SPT * HEADS)          # シリンダ境界に揃える
    p2_secs = 20 * 1024 * 2                          # 20MB → FAT16
    total = p2_start + p2_secs
    total += (-total) % (SPT * HEADS)

    with open(out, 'wb') as f:
        f.truncate(total * SECTOR)

    mbr = bytearray(SECTOR)
    # 起動コードは置かない。MYDOS はフロッピーから起動し、このディスクは
    # データ用として認識されるだけなので、あっても使われない。
    mbr[0x1BE:0x1CE] = part_entry(True,  p1_start, p1_secs, 0x06)
    mbr[0x1CE:0x1DE] = part_entry(False, p2_start, p2_secs, 0x06)
    mbr[0x1FE:0x200] = b'\x55\xAA'
    with open(out, 'r+b') as f:
        f.write(mbr)

    for start, secs, label in ((p1_start, p1_secs, 'MYDOSC'),
                               (p2_start, p2_secs, 'MYDOSD')):
        subprocess.run([
            'mformat',
            '-i', f'{out}@@{start * SECTOR}',
            '-h', str(HEADS), '-s', str(SPT), '-H', str(start),
            '-T', str(secs),
            '-c', '4',                  # 1 クラスタ 4 セクタ = 2KB
            '-v', label,
            '::',
        ], check=True)

    print(f'{out}: {total} セクタ ({total * SECTOR // 1024 // 1024}MB)')
    print(f'  パーティション 1 (C:): LBA {p1_start}, {p1_secs} セクタ, FAT16')
    print(f'  パーティション 2 (D:): LBA {p2_start}, {p2_secs} セクタ, FAT16')

if __name__ == '__main__':
    main()
