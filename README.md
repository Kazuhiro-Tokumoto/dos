# MYDOS

**MS-DOS 6.22 互換**を目標にしたディスクオペレーティングシステム。
16bit x86 リアルモードで動く本物の OS で、ブートセクタからカーネル、
コマンドインタプリタまで NASM で書いてある。当時の `.COM` / `.EXE` が
そのまま動くことを目標にしている。

**INT 21h は 6.22 の 103 機能をすべて実装済み**（FCB 系、常駐終了、
オーバーレイ、拡張オープンを含む）。ただし「完全互換」にはまだ
CONFIG.SYS・デバイスドライバ・HMA/UMB・FAT16 が要る（[まだやっていないこと](#まだやっていないこと)）。

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
  `min_alloc` / `max_alloc` に従うメモリ確保)、オーバーレイ (`AH=4Bh AL=3`)
- **プロセス**: 完全な PSP、`AH=4Bh` EXEC、`AH=4Ch` 終了、`AH=31h` 常駐終了、
  親子関係、終了時のメモリ・ファイルの自動回収
- **ファイル入出力**: ハンドル系 (SFT / JFT の 2 段構え) と **FCB 系の両方**
- **ドライブ情報**: 本物の形の DPB (`AH=32h`)、List of Lists (`AH=52h`)
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
| `fcb.inc` | FCB 系 16 機能 (レコード単位の入出力、`AH=29h` の名前解析) |
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

**MS-DOS 6.22 の 103 機能をすべて実装済み。**

| | 機能 |
|---|---|
| 文字入出力 | `01` `02` `03` `04` `05` `06` `07` `08` `09` `0A` `0B` `0C` |
| **FCB 系** | `0F` `10` `11` `12` `13` `14` `15` `16` `17` `21` `22` `23` `24` `27` `28` `29` |
| ディスク | `0D` `0E` `19` `1B` `1C` `32` `36` `53` `69` |
| DTA | `1A` `2F` |
| ベクタ | `25` `35` |
| 日時 | `2A` `2B` `2C` `2D` |
| システム | `18` `2E` `30` `33` `34` `37` `38` `52` `54` `58` `59` `63` `64` `65` `66` `67` `6B` |
| ディレクトリ | `39` `3A` `3B` `47` `60` |
| ファイル | `3C` `3D` `3E` `3F` `40` `41` `42` `43` `56` `57` `5A` `5B` `5C` `68` `6A` `6C` |
| ハンドル | `44` `45` `46` |
| メモリ | `48` `49` `4A` |
| プロセス | `00` `26` `31` `4B` `4C` `4D` `50` `51` `55` `62` |
| 検索 | `4E` `4F` |
| ネットワーク | `5D` `5E` `5F` (ネットワークが無いので機能なしを返す) |

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
- `TSRTEST.COM` を 2 回 — 1 回目に `INT 60h` を横取りして常駐し、
  2 回目に**そのハンドラがまだ生きているか**を確かめる
- `OVLTEST.COM` — オーバーレイを読み込み、指定した係数でリロケーションが
  当たっているかを確かめてから far call する
- `FCBTEST.COM ALPHA BETA` — FCB 系と 6.22 追加分の 15 項目
- `DOSTEST.COM` — ハンドル系 INT 21h の 10 項目

```
  [PASS] PSP:5C / PSP:6C  command tail parsed into FCBs by EXEC
  [PASS] AH=29h  parse filename
  [PASS] AH=16h/15h/10h  FCB create and sequential write
  [PASS] AH=0Fh/14h  FCB open and sequential read
  [PASS] AH=23h  FCB file size in records
  [PASS] AH=21h/22h/24h  FCB random read, write, set record
  [PASS] AH=27h  FCB random block read
  [PASS] AH=11h/12h  FCB find first and next
  [PASS] AH=17h  FCB rename
  [PASS] AH=13h  FCB delete
  [PASS] AH=32h  drive parameter block matches the BPB
  [PASS] AH=1Bh  allocation info
  [PASS] AH=5Bh  create new file, fail if it exists
  [PASS] AH=6Ch  extended open and create
  [PASS] AH=60h  truename canonicalises paths

  [PASS] AH=30h  DOS version is 6.22
  [PASS] AH=3Ch/40h/3Dh/3Fh  small file round trip
  [PASS] AH=40h/3Fh  3000-byte file across clusters
  [PASS] AH=42h  seek from end and from start
  [PASS] AH=4Eh/4Fh  FindFirst / FindNext
  [PASS] AH=39h/3Bh/47h/3Ah  directory create, enter, remove
  [PASS] AH=48h/49h/4Ah  memory allocate, resize, free
  [PASS] AH=56h  rename / AH=41h  delete / AH=36h  free disk space

  [PASS] .COM / .EXE の起動と .EXE のリロケーション適用
  [PASS] AH=31h 常駐終了が本当に残っている
  [PASS] AH=4Bh AL=3 オーバーレイのリロケーション
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

INT 21h の面はすべて埋まったが、「6.22 と完全互換」と言うにはまだ
以下が要る。効いてくる順に並べてある。

### A. 内部データ構造を本物の形にする

当時のツール (メモリマネージャ、常駐ソフト、ファイラ、デバッガ) は
DOS の内部構造を直接辿る。いまは `AH=52h` から DPB と MCB 連鎖には
届くが、以下がまだ本物の形になっていない。

- **SFT** — いまは独自の 48 バイト形式。DOS 4.0 以降の 59 バイト形式にし、
  List of Lists から連鎖で辿れるようにする必要がある
- **CDS** (Current Directory Structure) — ドライブごとのカレントディレクトリ
  の配列。`LASTDRIVE` と複数ドライブの土台でもある
- **デバイスドライバのヘッダ連鎖** — NUL → CON → AUX → PRN → CLOCK$ →
  ブロックデバイスの並び。`AH=52h` の +0Ch が指す先
- **ディスクバッファ連鎖** — `BUFFERS=` の実体

### B. 起動時の構成

- **CONFIG.SYS** — `DEVICE=` `FILES=` `BUFFERS=` `LASTDRIVE=` `SHELL=`
  `STACKS=` `DOS=`
- **インストール可能デバイスドライバ** (`.SYS` の読み込みとヘッダ登録)

### C. メモリ

- **HMA** (`DOS=HIGH`) — A20 を開けて `0xFFFF:0x0010` 以降の 64KB を使う。
  6.22 でカーネル本体が常駐メモリを食わない理由がこれ
- **UMB** (`DOS=UMB`) — EMM386 相当が要るので V86 モードの実装が前提
- **XMS** (HIMEM.SYS 相当)

### D. ストレージ

- **FAT16 とハードディスク** — パーティションテーブル、複数ドライブ、
  32MB 超のボリューム
- **INT 13h 拡張** (LBA)

### E. COMMAND.COM

- **環境変数** — `SET` / `PATH` / `PROMPT` と `%VAR%` の展開
- **リダイレクトとパイプ** — `>` `>>` `<` `|`
  (`AH=45h`/`46h` は実装済みなので土台はある)
- **バッチの制御構造** — `FOR` `IF` `GOTO` `CALL` `SHIFT` `CHOICE`、入れ子

### F. 外部コマンド

`FORMAT` `FDISK` `SYS` `XCOPY` `CHKDSK` `SCANDISK` `MORE` `SORT` `FIND`
`EDIT` `DOSKEY` `LABEL` `TREE` `ATTRIB` `DEBUG` など。6.22 の配布物に
入っていたもの一式。

### G. その他

- **`INT 24h` の対話** — いまは常に「失敗」を返す。本物は
  「中止/再試行/無視/失敗」を聞いてくる
- **SHARE とファイルロック** — `AH=5Ch` はいま常に成功を返すだけ
- **ネットワークリダイレクタ** (`INT 2Fh AH=11h`)
- **国別情報とコードページの実体** — `AH=38h`/`65h`/`66h` は形だけ返している

## 対象 CPU

386 以降のリアルモード。ファイル位置とサイズが 32bit なので、16bit レジスタ
2 本で持ち回るより 32bit レジスタを使うほうが桁上がりの取りこぼしが起きにくい。
8086 専用にはしていない。
