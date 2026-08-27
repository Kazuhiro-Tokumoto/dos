# MYDOS

MS-DOS 互換のディスクオペレーティングシステム。16bit x86 リアルモードで動く
本物の OS で、ブートセクタからカーネル、コマンドインタプリタまで NASM で
書いてある。当時の `.COM` / `.EXE` がそのまま動くことを目標にしている。

```
$ make            # イメージを作る
$ make run        # QEMU で起動する
$ make test       # 自動テストを走らせる
```

## いま動くもの

```
MYDOS Stage2
Loading IO.SYS.......................................... OK

MYDOS Version 1.0
Copyright (C) 2026

MYDOS Command Interpreter
Type a command. Internal: DIR CD MD RD TYPE COPY DEL REN
                          CLS VER ECHO DATE TIME MEM EXIT

A:\>MEM

Segment      Size  Owner
-------  --------  ---------------
05A1        64  DOS
05A6      7568  program
0780    624640  free

7632 bytes used
624640 bytes free
624640 bytes in largest free block
```

- **ブート**: BPB 付きブートセクタ → Stage2 (FAT12 パーサ) → `IO.SYS`
- **ファイルシステム**: FAT12 の読み書き。サブディレクトリ、`.` / `..`、
  ディレクトリの自動拡張に対応
- **メモリ管理**: メモリ上に実在する MCB 連鎖 (`M`/`Z`、所有者 PSP、サイズ)
- **プログラム実行**: `.COM` と `.EXE` (MZ ヘッダ解析、リロケーション適用、
  `min_alloc` / `max_alloc` に従うメモリ確保)
- **プロセス**: 完全な PSP、`AH=4Bh` EXEC、`AH=4Ch` 終了、親子関係、
  終了時のメモリ・ファイルの自動回収
- **シェル**: `COMMAND.COM` (カーネルの一部ではなく、ただの `.COM`)、
  `AUTOEXEC.BAT` と `.BAT` の実行

## 構成

```
boot/stage1.asm     512B ブートセクタ。BPB を持ち、予約領域から Stage2 を読む
boot/stage2.asm     FAT12 を辿って IO.SYS を名前で探し、0x1000:0 に載せる
kernel/io.asm       カーネル本体。再配置、割り込みの設置、INT 21h の入口と出口
kernel/inc/         カーネルの各部品 (下表)
shell/command.asm   COMMAND.COM
tests/              テストプログラムと AUTOEXEC.BAT
tools/              イメージ生成、MZ ヘッダ生成、自動テスト
js/                 引き継ぎ元のブラウザ実装 (仕様の参照用。バグは修正済み)
```

| ファイル | 中身 |
|---|---|
| `dosdef.inc` | PSP / MCB / SFT / ディレクトリエントリのレイアウト、エラーコード |
| `con.inc` | CON デバイス (INT 10h / 16h)、行編集 |
| `disk.inc` | BPB の取り込み、LBA→CHS 変換、セクタ入出力、FAT の書き戻し |
| `fat12.inc` | FAT の読み書き、クラスタ確保、ディレクトリ走査、パス解決 |
| `mem.inc` | MCB の確保・分割・結合・解放、確保戦略 |
| `file.inc` | SFT / JFT、ハンドルによるファイル入出力、文字デバイス |
| `dirops.inc` | MKDIR / RMDIR / CHDIR / 検索 / 名前変更 / 属性 |
| `exec.inc` | PSP 構築、環境ブロック、`.COM` / `.EXE` ローダ、EXEC と終了 |
| `int21.inc` | INT 21h のジャンプテーブルと各機能 |
| `time.inc` | RTC の読み取りと DOS 形式への変換 |
| `util.inc` | 8.3 名の変換、ワイルドカード照合 |

## ディスクの配置

```
LBA 0        Stage1 (BPB を含むブートセクタ)
LBA 1-17     Stage2                          ┐ reserved_sectors = 18
LBA 18-26    FAT1 (9 セクタ)
LBA 27-35    FAT2 (9 セクタ)
LBA 36-49    ルートディレクトリ (224 エントリ)
LBA 50-2879  データ領域 (2830 クラスタ)
```

`reserved_sectors` を 1 ではなく 18 にしているのがこの設計の要。Stage1 は
LBA 1 から 17 セクタを生読みして Stage2 をロードするが、`reserved` が 1 の
ままだと LBA 1 は FAT1 の先頭にあたって衝突する。18 に増やすことでその
17 セクタが正当な予約領域になり、512 バイトのブートセクタに FAT12 パーサを
詰め込まずに済む。失う容量は 8.5KB (1.44MB の 0.6%)。

その代わり BPB は本物の値で埋めてあるので、ホスト側のツールがそのまま使える。

```
$ mdir -i build/dos.img ::        # 中身を見る
$ mcopy -i build/dos.img FOO.COM ::/   # ファイルを入れる
$ fsck.fat -n build/dos.img       # 整合性を検査する
```

当時の `.COM` / `.EXE` をイメージに入れるのに専用ツールが要らない、という
のがこの選択の実利。

## メモリの配置

```
0x00000 - 0x003FF   割り込みベクタテーブル
0x00400 - 0x004FF   BIOS データエリア
0x00600 - 0x05AFF   IO.SYS (カーネル。code + data + バッファ)
0x05B00 - 0x9FFFF   MCB アリーナ (プログラムに配られる ~620KB)
0xA0000 -           ビデオ RAM
```

カーネルは Stage2 に 0x1000:0000 (64KB の位置) へ読み込まれたあと、自分自身を
0x0060:0000 へ降ろしてから動き出す。低いところに居座るほどプログラムに渡せる
連続領域が広くなる。

## INT 21h の実装状況

| | 機能 |
|---|---|
| 文字入出力 | `01` `02` `03` `04` `05` `06` `07` `08` `09` `0A` `0B` `0C` |
| ディスク | `0D` `0E` `19` `36` |
| DTA | `1A` `2F` |
| ベクタ | `25` `35` |
| 日時 | `2A` `2B` `2C` `2D` |
| システム | `30` `31` `33` `34` `38` `52` `54` `58` `59` `2E` |
| ディレクトリ | `39` `3A` `3B` `47` |
| ファイル | `3C` `3D` `3E` `3F` `40` `41` `42` `43` `56` `57` |
| ハンドル | `44` `45` `46` |
| メモリ | `48` `49` `4A` |
| プロセス | `00` `4B` `4C` `4D` `50` `51` `62` |
| 検索 | `4E` `4F` |

その他の割り込み: `INT 20h` (終了)、`INT 22h` (終了アドレス)、
`INT 23h` (Ctrl-C)、`INT 24h` (クリティカルエラー)、`INT 25h` / `26h`
(絶対セクタ読み書き)、`INT 27h` (TSR)、`INT 2Fh` (マルチプレクサ)。

## テスト

```
$ make test
```

`-DSERIAL_CONSOLE` を付けてビルドしたカーネルは、画面に出す文字を COM1 にも
流す。QEMU をヘッドレスで起動してホスト側にログを落とし、
`tools/runtest.py` が中身を判定する。DOS の API から見える挙動は変わらない
ので、テストのためにカーネルの動きを変えているわけではない。

起動すると `AUTOEXEC.BAT` が走り、以下が自動で流れる。

- `HELLO.COM` — `.COM` の起動
- `HELLO.EXE` — `.EXE` の起動と、**自分のリロケーションが当たっているかの自己検査**
- `TYPE` / `COPY` / `DEL` / `DIR` / `MEM` — シェルの内部コマンド
- `DOSTEST.COM` — INT 21h の 10 項目

```
  [PASS] AH=30h  DOS version
  [PASS] AH=3Ch/40h/3Dh/3Fh  small file round trip
  [PASS] AH=40h/3Fh  3000-byte file across clusters
  [PASS] AH=42h  seek from end and from start
  [PASS] AH=4Eh/4Fh  FindFirst / FindNext
  [PASS] AH=39h/3Bh/47h/3Ah  directory create, enter, remove
  [PASS] AH=48h/49h/4Ah  memory allocate, resize, free
  [PASS] AH=56h  rename
  [PASS] AH=41h  delete
  [PASS] AH=36h  free disk space
```

テストが書いたファイルは実際のディスクイメージに残るので、走らせたあとに
`fsck.fat` でホスト側から検証できる (エラーなしを確認済み)。

### 参照実装 (js/)

引き継ぎ元のブラウザ実装は、INT 21h の関数表・FAT12 のレイアウト・PSP・
`.EXE` ローダの前例として価値が高いので `js/` に残してある。読んだ際に
見つかった不具合は修正した。

```
$ make js-test
```

- **`FAT12Disk.writeFile()`**: クラスタを確保した直後に FAT へ印を付けて
  いなかったため、次の `allocCluster()` が同じクラスタを返し、512 バイトを
  超えるファイルが 1 クラスタおきに失われていた
  (1024 バイト書いて 512 バイトしか読めない)
- **`INT 21h` の重複 `case`**: `0x40` `0x44` `0x48` `0x49` `0x4A` が同じ
  `switch` に二重に書かれており、JS は最初に一致したものだけを実行するため、
  後ろにある正しい実装が到達不能だった。とくに `AH=40h` は
  `dosFileIO` を通らない側が生きていたので、ファイルを開けても
  書き込みが常に `AX=6` (無効なハンドル) を返していた

`js/test-fat12.js` は修正前のコードに対しては失敗する (確認済み) ので、
同じ欠陥が戻ってくれば検出できる。

## まだやっていないこと

- **FCB 系** (`0F`-`18`, `21`-`24`, `27`-`28`)。ハンドル系より古い
  ファイル指定方法で、DOS 1.x 時代のプログラムが使う
- **本物の TSR**。`INT 27h` と `AH=31h` は受け付けるが常駐部分を残さない
- **`AH=4Bh` の AL=1 / AL=3** (ロードのみ / オーバーレイ)
- **`CONFIG.SYS`** とデバイスドライバの読み込み
- **複数ドライブ**。いまは A: のみ
- **`INT 24h` の対話** (中止/再試行/無視)。常に「失敗」を返す
- **バッチの入れ子**、`FOR` / `IF` / `GOTO` / `SET`

## 対象 CPU

386 以降のリアルモード。ファイル位置とサイズが 32bit なので、16bit レジスタ
2 本で持ち回るより 32bit レジスタを使うほうが桁上がりの取りこぼしが起きにくい。
8086 専用にはしていない。
