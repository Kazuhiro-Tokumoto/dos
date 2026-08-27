#!/usr/bin/env python3
"""1.44MB の FAT12 フロッピーイメージを組み立てる。

レイアウトは boot/stage1.asm の BPB がすべての権威で、このスクリプトは
そこに書かれている値を読み取って従う。BPB を書き換えたらイメージも自動で
追随するので、二重管理にならない。

  LBA 0        Stage1 (BPB を含むブートセクタ)
  LBA 1-17     Stage2                          ┐ reserved_sectors
  LBA 18-26    FAT1
  LBA 27-35    FAT2
  LBA 36-49    ルートディレクトリ
  LBA 50-      データ領域

ファイルの投入はこのスクリプトではなく mtools の mcopy に任せる。
BPB が正しく書けていれば mcopy はこのイメージをそのまま扱えるので、
当時の .COM / .EXE を入れるのに専用ツールを書かなくて済む。
"""
import argparse
import struct
import sys

SECTOR = 512


class BPB:
    """ブートセクタの先頭 62 バイトから BPB を読み出す。"""

    def __init__(self, boot: bytes):
        if len(boot) != SECTOR:
            raise ValueError(f"ブートセクタが {len(boot)} バイト (512 でなければならない)")
        if boot[510:512] != b"\x55\xAA":
            raise ValueError("ブートシグネチャ 0xAA55 がない")
        if boot[0] != 0xEB or boot[2] != 0x90:
            raise ValueError("先頭が EB xx 90 でない (BPB を飛び越す形になっていない)")

        self.bytes_per_sec = struct.unpack_from("<H", boot, 0x0B)[0]
        self.sec_per_clus = boot[0x0D]
        self.reserved_secs = struct.unpack_from("<H", boot, 0x0E)[0]
        self.num_fats = boot[0x10]
        self.root_entries = struct.unpack_from("<H", boot, 0x11)[0]
        self.total_secs = struct.unpack_from("<H", boot, 0x13)[0]
        self.media = boot[0x15]
        self.secs_per_fat = struct.unpack_from("<H", boot, 0x16)[0]
        self.secs_per_track = struct.unpack_from("<H", boot, 0x18)[0]
        self.num_heads = struct.unpack_from("<H", boot, 0x1A)[0]

        if self.bytes_per_sec != SECTOR:
            raise ValueError(f"1 セクタ {self.bytes_per_sec} バイトには対応していない")
        if self.total_secs == 0:
            raise ValueError("total_secs16 が 0 (FAT32 用の 32bit 側は未対応)")

    @property
    def fat1_lba(self):
        return self.reserved_secs

    @property
    def root_lba(self):
        return self.fat1_lba + self.num_fats * self.secs_per_fat

    @property
    def root_secs(self):
        return (self.root_entries * 32 + SECTOR - 1) // SECTOR

    @property
    def data_lba(self):
        return self.root_lba + self.root_secs

    @property
    def cluster_count(self):
        return (self.total_secs - self.data_lba) // self.sec_per_clus

    def describe(self):
        return "\n".join(
            [
                f"  1 セクタ            : {self.bytes_per_sec} バイト",
                f"  1 クラスタ          : {self.sec_per_clus} セクタ",
                f"  予約セクタ          : {self.reserved_secs}  (LBA 0-{self.reserved_secs - 1})",
                f"  FAT                 : {self.num_fats} 個 x {self.secs_per_fat} セクタ  "
                f"(LBA {self.fat1_lba}-{self.root_lba - 1})",
                f"  ルートディレクトリ  : {self.root_entries} エントリ = {self.root_secs} セクタ  "
                f"(LBA {self.root_lba}-{self.data_lba - 1})",
                f"  データ領域          : LBA {self.data_lba}-{self.total_secs - 1}  "
                f"({self.cluster_count} クラスタ)",
                f"  ジオメトリ          : {self.secs_per_track} セクタ/トラック, "
                f"{self.num_heads} ヘッド",
            ]
        )


def build(stage1_path, stage2_path, out_path, quiet=False):
    with open(stage1_path, "rb") as f:
        stage1 = f.read()
    bpb = BPB(stage1)

    with open(stage2_path, "rb") as f:
        stage2 = f.read()

    # Stage2 は予約領域の 2 セクタ目以降にそのまま置く。
    # ここを超えると Stage1 が読みきれず、症状が「なぜか途中で暴走する」と
    # いう最悪の形で出るので、ビルド時に止める。
    stage2_capacity = (bpb.reserved_secs - 1) * SECTOR
    if len(stage2) > stage2_capacity:
        raise SystemExit(
            f"stage2 が大きすぎる: {len(stage2)} バイト > 予約領域 {stage2_capacity} バイト\n"
            f"  boot/stage1.asm の bpb_reserved_secs を増やし、STAGE2_SECTORS も合わせること"
        )

    img = bytearray(b"\x00" * (bpb.total_secs * SECTOR))

    # --- LBA 0: ブートセクタ ---
    img[0:SECTOR] = stage1

    # --- LBA 1..: Stage2 ---
    img[SECTOR : SECTOR + len(stage2)] = stage2

    # --- FAT の先頭 3 バイト ---
    # クラスタ 0 にメディアディスクリプタ、クラスタ 1 に EOC を置くのが FAT12 の約束。
    # F0 FF FF は「クラスタ0 = 0xFF0(メディア), クラスタ1 = 0xFFF」を 12bit で
    # パックしたもの。
    for i in range(bpb.num_fats):
        off = (bpb.fat1_lba + i * bpb.secs_per_fat) * SECTOR
        img[off : off + 3] = bytes([bpb.media, 0xFF, 0xFF])

    # --- ルートディレクトリにボリュームラベルを置く ---
    # 属性 0x08 (ボリュームラベル)。DIR や mdir がラベルを拾えるようになる。
    label = stage1[0x2B:0x36]  # 拡張 BPB のラベルをそのまま使う
    entry = bytearray(32)
    entry[0:11] = label
    entry[11] = 0x08
    img[bpb.root_lba * SECTOR : bpb.root_lba * SECTOR + 32] = entry

    with open(out_path, "wb") as f:
        f.write(img)

    if not quiet:
        print(f"{out_path} を作成 ({len(img)} バイト)")
        print(bpb.describe())
        print(
            f"  Stage2              : {len(stage2)} / {stage2_capacity} バイト "
            f"({len(stage2) * 100 // stage2_capacity}% 使用)"
        )


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--stage1", required=True)
    ap.add_argument("--stage2", required=True)
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()
    try:
        build(args.stage1, args.stage2, args.out, args.quiet)
    except ValueError as e:
        sys.exit(f"エラー: {e}")


if __name__ == "__main__":
    main()
