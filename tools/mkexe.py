#!/usr/bin/env python3
"""フラットバイナリに MZ ヘッダを被せて .EXE を作る。

NASM の -f bin は MZ 形式を出せないので、本体だけをアセンブルしておき、
ヘッダとリロケーションテーブルをここで組み立てる。

本体の先頭 8 バイトは、この道具に渡すための小さな表になっている:

    +0  word  初期 IP     (ロードセグメントからのオフセット)
    +2  word  初期 SP
    +4  word  リロケーションの数
    +6  word  リロケーションテーブルの位置

リロケーションテーブルは word の並びで、それぞれが「直すべき word の
イメージ内オフセット」。イメージは 64KB 未満を前提にしているので、
MZ 側のセグメント欄はすべて 0 でよい。

MZ ヘッダの pages / last_page は「ヘッダを含むファイル全体」を表す。
DOS のローダは

    イメージ長 = pages * 512 - (last_page ? 512 - last_page : 0) - ヘッダ長

で本体の長さを求めるので、ここを取り違えると末尾が切れたり、
ゴミが余分に読み込まれたりする。
"""
import argparse
import struct
import sys


def build(body: bytes, min_alloc: int, max_alloc: int) -> bytes:
    if len(body) < 8:
        raise ValueError("本体が短すぎる (先頭 8 バイトの表がない)")

    init_ip, init_sp, nrelocs, rtab = struct.unpack_from("<HHHH", body, 0)

    relocs = []
    for i in range(nrelocs):
        off = struct.unpack_from("<H", body, rtab + i * 2)[0]
        relocs.append((off, 0))

    # ヘッダ = 28 バイトの固定部 + リロケーションテーブル。
    # パラグラフ境界に切り上げる (イメージ本体が 16 の倍数から始まる必要がある)
    header_bytes = 28 + 4 * nrelocs
    header_paras = (header_bytes + 15) // 16
    header_size = header_paras * 16

    total = header_size + len(body)
    pages = (total + 511) // 512
    last_page = total % 512

    hdr = bytearray(header_size)
    struct.pack_into(
        "<HHHHHHHHHHHHHH",
        hdr,
        0,
        0x5A4D,             # 'MZ'
        last_page,          # 最終ページの使用バイト数 (0 なら 512)
        pages,              # 512 バイト単位のページ数
        nrelocs,            # リロケーションの数
        header_paras,       # ヘッダの大きさ (パラグラフ)
        min_alloc,          # 最低限必要な追加メモリ
        max_alloc,          # 欲しい追加メモリ
        0,                  # 初期 SS (ロードセグメントからの相対)
        init_sp,            # 初期 SP
        0,                  # チェックサム
        init_ip,            # 初期 IP
        0,                  # 初期 CS (ロードセグメントからの相対)
        28,                 # リロケーションテーブルの位置
        0,                  # オーバーレイ番号
    )
    for i, (off, seg) in enumerate(relocs):
        struct.pack_into("<HH", hdr, 28 + i * 4, off, seg)

    return bytes(hdr) + body


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("body", help="nasm -f bin で作った本体")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--min-alloc", type=lambda s: int(s, 0), default=0x0100)
    ap.add_argument("--max-alloc", type=lambda s: int(s, 0), default=0xFFFF)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    with open(args.body, "rb") as f:
        body = f.read()

    try:
        exe = build(body, args.min_alloc, args.max_alloc)
    except ValueError as e:
        sys.exit(f"エラー: {e}")

    with open(args.out, "wb") as f:
        f.write(exe)

    if args.verbose:
        ip, sp, n, _ = struct.unpack_from("<HHHH", body, 0)
        print(
            f"{args.out}: {len(exe)} バイト "
            f"(ヘッダ {len(exe) - len(body)} + 本体 {len(body)}), "
            f"IP={ip:#06x} SP={sp:#06x} リロケーション {n} 個"
        )


if __name__ == "__main__":
    main()
