# MYDOS

**MS-DOS 6.22 互換**を目標にしたディスクオペレーティングシステム。
16bit x86 リアルモードで動く本物の OS で、ブートセクタからカーネル、
コマンドインタプリタまで NASM で書いてある。当時の `.COM` / `.EXE` が
そのまま動くことを目標にしている。

**INT 21h は 6.22 の 103 機能をすべて実装済み**（FCB 系、常駐終了、
オーバーレイ、拡張オープンを含む）。DOS の内部データ構造も、当時のツールが
辿る本物の形でメモリ上に実在する。FAT12 のフロッピーと、パーティションを
切った FAT16 のハードディスクを同時に扱える。起動時には `CONFIG.SYS` を
読み、`DEVICE=` で書かれた `.SYS` ドライバを組み込む。A20 と HMA を扱い、
`HIMEM.SYS` 相当の XMS ドライバを内蔵している
（残りは[まだやっていないこと](#まだやっていないこと)）。

```
$ make            # イメージを作る
$ make run        # QEMU で起動する (フロッピー + ハードディスク)
$ make test       # 自動テストを走らせる (66 項目)
$ make check      # ホスト側のツールでイメージを検証する
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
- **ファイルシステム**: FAT12 と FAT16 の読み書き。サブディレクトリ、
  `.` / `..`、ディレクトリの自動拡張に対応
- **ドライブ**: フロッピー (A: / B:) とハードディスク。MBR のパーティション
  テーブルを読んで C: 以降に割り当てる。LBA は 32bit、転送は CHS と
  INT 13h 拡張の両対応で 32MB の壁が無い
- **メモリ管理**: メモリ上に実在する MCB 連鎖 (`M`/`Z`、所有者 PSP、サイズ)
- **プログラム実行**: `.COM` と `.EXE` (MZ ヘッダ解析、リロケーション適用、
  `min_alloc` / `max_alloc` に従うメモリ確保)、オーバーレイ (`AH=4Bh AL=3`)
- **プロセス**: 完全な PSP、`AH=4Bh` EXEC、`AH=4Ch` 終了、`AH=31h` 常駐終了、
  親子関係、終了時のメモリ・ファイルの自動回収
- **ファイル入出力**: ハンドル系 (SFT / JFT の 2 段構え) と **FCB 系の両方**
- **ドライブ情報**: ドライブごとの DPB を連鎖させた本物の形 (`AH=32h`)、
  List of Lists (`AH=52h`)、CDS 配列、デバイスドライバ連鎖、
  ディスクバッファ連鎖
- **起動時の構成**: `CONFIG.SYS` の `FILES=` `BUFFERS=` `LASTDRIVE=` `SHELL=`
  `DEVICE=` `DEVICEHIGH=` `INSTALL=` `FCBS=` `STACKS=` `BREAK=` `DOS=`
- **インストール可能デバイスドライバ**: `.SYS` を読み込んで `INIT` を呼び、
  連鎖に差し込む。文字デバイスは名前で開けるようになり、ブロック
  デバイスは新しいドライブ文字として生える
- **1MB の壁の向こう**: A20 ゲートの開閉、HMA の貸し出し、`HIMEM.SYS` 相当の
  XMS ドライバ (`INT 2Fh AX=4300h/4310h` の窓口と 3.0 の機能一式)。
  拡張メモリの転送はアンリアルモードで行う
- **シェル**: `COMMAND.COM` (カーネルの一部ではなく、ただの `.COM`)、
  `AUTOEXEC.BAT` と `.BAT` の実行、`C:` によるドライブ切り替え

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
| `config.inc` | CONFIG.SYS の解析と反映、`.SYS` ドライバの読み込み |
| `xms.inc` | A20 ゲート、HMA、XMS (HIMEM.SYS 相当) |
| `con.inc` | CON デバイス (INT 10h / 16h)、行編集 |
| `disk.inc` | セクタ入出力。CHS (`AH=02h`/`03h`) と INT 13h 拡張 (`AH=42h`/`43h`) |
| `drive.inc` | ドライブ表、パーティションの検出、BPB の取り込み、DPB の組み立て |
| `buffer.inc` | ディスクバッファ (`BUFFERS=` の実体、LRU で入れ替え) |
| `cds.inc` | ドライブごとのカレントディレクトリ (CDS 配列) |
| `device.inc` | デバイスドライバのヘッダ連鎖と List of Lists |
| `fat.inc` | FAT12 / FAT16 の読み書き、クラスタ確保、ディレクトリ走査、パス解決 |
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

### ハードディスク

`make hd` (または `make test` / `make run`) が `build/hd.img` を作る。
MBR にパーティションを 2 つ置いた 60MB の FAT16 ディスクで、MYDOS からは
C: と D: に見える。

```
LBA 0            MBR (パーティションテーブル)
LBA 63-81982     パーティション 1  40MB FAT16  → C:
LBA 82656-...    パーティション 2  20MB FAT16  → D:
```

ドライブ文字の割り当ては当時の DOS と同じで、フロッピーが A: / B:、
ハードディスクの基本パーティションが C: から順に。拡張パーティションの
中までは辿らない。

パーティションの先頭 LBA は `disk_xfer` が転送のたびに足す。上の層は
「そのドライブの何番目のセクタか」しか知らない。LBA が 65536 を超えると
CHS では届かなくなるので、起動時に `INT 13h AH=41h` で拡張の有無を調べ、
使えるドライブでは `AH=42h`/`43h` に切り替える。

FAT12 と FAT16 の使い分けはクラスタ数で決める (4085 個未満なら FAT12)。
パーティションの種別バイトではなく実際のクラスタ数で決めるのが本来の判定。
FAT12 は最大でも 12 セクタなので起動時に丸ごとメモリへ載せ、FAT16 は
大きすぎるのでディスクバッファ越しに 1 セクタずつ引く。

```
$ mdir -i build/hd.img@@32256 ::      # C: の中身 (mtools はオフセット指定で読める)
$ make check                          # 両方のイメージを fsck.fat にかける
```

## CONFIG.SYS とデバイスドライバ

DOS は起動の途中でルートディレクトリの `CONFIG.SYS` を読み、自分の形を
決める。バッファの数もファイルハンドルの数もドライブの数も、そこで
決まってから確定するので、順序が要る。

```
1. 既定値で一通り組み立てる   ← これが無いと CONFIG.SYS 自体が読めない
2. CONFIG.SYS を読んで控える
3. 数の指定を反映して組み直す (FILES= / BUFFERS= / LASTDRIVE=)
4. DEVICE= を上から順に読み込む
5. INSTALL= を実行する
6. SHELL= のコマンドインタプリタを起動する
```

3 で組み直せるのは、この時点でまだ何も開いていないから。

`.SYS` は実行ファイルではなく、デバイスヘッダが先頭に置かれただけの
バイナリ。読み込んだあと `INIT` (コマンド 0) を一度呼び、ドライバが返した
「常駐部分の末尾」までメモリを切り詰めてから、ヘッダを連鎖の先頭 (NUL の
直後) に差し込む。名前で探すときは先頭から順に見るので、あとから入れた
ドライバが組み込みより優先される。ANSI.SYS が CON を乗っ取れるのは
この仕組みによる。

`tests/mydev.asm` と `tests/ramdisk.asm` が、その両方の形の実物になっている。

```
DEVICE=\MYDEV.SYS hello-from-config   文字デバイス。"MYDEV" として開ける
DEVICE=\RAMDISK.SYS                   ブロックデバイス。64KB の FAT12 が E: に
```

### 入れ子の INT 21h

デバイスドライバの `INIT` は画面に何か出す。つまり `CONFIG.SYS` の処理中に
INT 21h が呼ばれる。作業領域を 1 組しか持っていないと、内側の呼び出しが
外側の状態を上書きして帰ってこられない。

DOS はこのために作業用のスタックを何本か持っている (I/O スタック、
ディスクスタック、補助スタック)。MYDOS も同じことをしていて、入れ子の
深さでスタックを選び、外側のレジスタ退避領域は積んで避ける。起動処理は
さらに別のスタックで動く。

ただし本物と同じ制限も残る。ファイルシステム側の作業用変数は 1 組しか
無いので、入れ子で安全に呼べるのは画面・キーボード・ベクタ操作
(`AH=01h`〜`0Ch`, `25h`, `30h`, `35h`) まで。当時のデバイスドライバの
`INIT` に許されていた範囲と同じで、`InDOS` フラグ (`AH=34h`) はそのための
目印。

## 本物の DOS バイナリで試す

自作のテストは「自分で作ったものを自分で読む」閉じた輪になりやすい。
実際に当時の道具で作られたバイナリを食わせてみると、その輪の外にある
間違いが出てくる。Mpxplay (DOS/32A エクステンダ付きの 16bit MZ スタブ)
を動かしてみて、下の 3 つが見つかった。

* **外部ブロックドライバのドライブへの書き込みが BIOS に流れていた**
  `disk_xfer` が `DRVF_EXTERNAL` を見ていなかったので、`DEVICE=` で
  入れたドライバのドライブに書くと、そのドライバのユニット番号が
  そのまま BIOS のドライブ番号として使われた。ユニット 0 のラムディスク
  なら「BIOS の 0 番」= 起動フロッピーになる。ラムディスクの予約セクタは
  1 なので、その FAT が起動フロッピーのセクタ 1 — つまり Stage2 の
  置き場所 — に書かれ、次に起動したとき止まる。使ったあとのイメージが
  壊れるので、同じイメージで 2 回起動して初めて分かる種類の壊れ方だった。

* **子プロセスの環境ブロックを作っていなかった**
  親のものをそのまま渡していた。DOS は EXEC のたびに写しを作り、
  文字列の並びのあとに `word 1` と「起動したプログラムのフルパス」を
  置く。C の `argv[0]` はここから来るし、DOS エクステンダは 32bit の
  本体を読むために自分の EXE をここから探す。親のパスが入っていると
  COMMAND.COM を自分だと思って開き、`DOS/32A fatal (2002): error in
  exec file "A:\COMMAND.COM"` で落ちる。
  なお環境ブロックはプログラム本体より**先に**確保しなければならない。
  `.COM` も `maxalloc=FFFF` の `.EXE` も空きを全部持っていくので、
  あとからでは場所が残らない。持ち主 (MCB +1) に 0 を入れるのも駄目で、
  0 は「空き」の意味なので本体の確保に食われる。

* **`truename` が常に `A:` を返していた**
  複数ドライブになったのに追随していなかった。C: に置いたプログラムの
  フルパスが A: のものとして子に渡る。

いまのところ Mpxplay は、自分の EXE を見つけて 700KB を読み込み、
保護モードへの移行まで進んだところで止まる。MYDOS に DPMI が無いため
で、16bit DOS 側の仕事はそこで終わっている。`tests/envtest.asm` は
このとき見つけた環境ブロックまわりを回帰テストにしたもの。

## 長いファイル名 (VFAT の LFN)

FAT のディレクトリエントリは名前を 11 バイト固定で持つ。「8 文字 +
拡張子 3 文字」という制限はここから来ていて、FAT という形式そのものの
制限ではなくエントリの形の制限でしかない。

VFAT は、本体のエントリの手前に **属性 0x0F のエントリ** を並べ、そこに
長い名前を 13 文字ずつ分けて入れる。属性 0x0F は
読み取り専用+隠し+システム+ボリュームラベル の組み合わせで、昔の DOS は
これを読み飛ばすので、知らないシステムから見ても壊れて見えない。

    断片エントリ (32 バイト)
      +00  byte   並び番号 (1..20)。bit6 が立っていれば連鎖の先頭
      +01  10     文字 1..5   (UTF-16LE)
      +0B  byte   属性 = 0x0F
      +0D  byte   短い名前のチェックサム
      +0E  12     文字 6..11
      +1C  4      文字 12..13

MYDOS はこれを読み書きする。

    A:\>COPY README.TXT "my long notes.text"
            1 file(s) copied

    A:\>DIR
     Directory of A:\
    README.TXT                           471  08-28-2026 11:30
    My Long File Name.txt                153  08-28-2026 11:30
    my long notes.text                   471  08-28-2026 11:30

* 長い名前でも短い名前 (`MYLONG~1.TXT`) でも開ける。短い名前は
  作るときに自動で付ける。長い名前を知らないプログラムからも触れる
  ようにするためで、本物と同じ作法。
* `AH=4Eh/4Fh` は 8.3 のまま。長い名前は `AX=714Eh/714Fh` で返す。
  `AX=71A0h` が「長い名前が使える」と名乗り、対応していない下位機能は
  `CF=1 / AX=7100h` を返す — 呼ぶ側が古いやり方に落とせる約束。
* 消したり名前を変えたりしたあと、宙に浮いた断片は片付ける。放って
  おくと次にそこへ作られたファイルに前の名前が付いて見える。
* シェルの引数は二重引用符で囲める。長い名前には空白が入りうるため。

`tests/lfntest.asm` が 7 件。断片の並び、短い名前の生成、長い名前での
開き直し、`714Eh` の検索、長い名前のディレクトリ、消したあとの後始末。

### この作業で踏んだもの

* **カーネルが 64KB を超えた。** MYDOS は `0x0060:0000` に置いた 1 つの
  セグメントの中で動く。長い名前のための表を足したところで 63 バイト
  超え、オフセットが回り込んで即座に壊れた。大きな表を「場所だけ確保
  する」領域 (`absolute` + `resb`) に移してファイルから外した。
  NASM の `-f bin` では `resb` をそのまま並べても 0 が書き出されるので、
  `absolute` を使う必要がある。
* **DMA は 64KB の物理境界をまたげない。** その「場所だけ確保する」領域は
  セグメントの終わりにあるので、そこにセクタバッファを置いたら物理
  `0x10000` をまたいでしまい、BIOS のディスク転送がまるごと失敗した。
  ディレクトリが 1 つも読めなくなり、「COMMAND.COM が無い」という形で
  表に出た。BIOS に渡すバッファはカーネルの前のほうに置かなければ
  ならない。
* **`mov ds, [u_ds]` を `mov si, [u_dx]` より先に書いた。** `u_ds` も
  `u_dx` もカーネル側の変数なので、DS を付け替えたあとに読むと呼び出し
  側のセグメントの同じオフセットを読むことになる。たいてい 0 が返り、
  PSP の先頭 (`CD 20 ...`) をパス名として写していた。

## FreeDOS のユーティリティで試す

「DOS をよく使うフリーソフト」で叩くと、自作のテストでは出ない間違いが
出てくる。FreeDOS 1.3 のユーティリティ (GPL) を C: に置いて走らせた。
どれも当時のコンパイラで作られた実物で、`SS≠0` の本物の EXE。

    C:\>MEM
    Segment      Size  Owner
    -------  --------  ---------------
    1009           64  DOS
    102A        66224  DOS
    2264       514496  free
    75104 bytes used
    514496 bytes free

MEM は List of Lists から MCB 連鎖をたどって表を作る。Windows が
やることとほぼ同じで、内部構造が本物の形になっていないと通らない。
ATTRIB / TREE / FC / XCOPY / FIND / SORT / MORE も動く。

### ここで見つかったバグ

* **ルートディレクトリの属性が引けなかった。** `AH=43h` は
  `path_resolve` の結果を `dir_find` に渡していたが、`"C:\"` のような
  パスは要素が残らないので探しても見つからない。ルートにはディレクトリ
  エントリが無いのだから当然で、そこを拾っていなかった。XCOPY は複写元が
  ディレクトリかどうかをこれで確かめるので、「Source path not found」と
  言って止まっていた。

* **EXEC が子プロセスの DTA を設定していなかった。** DOS は EXEC のたびに
  DTA を新しい PSP:0080 に向け直す。プログラムが `AH=1Ah` を呼ばずに
  `AH=4Eh` や FCB の検索を使うと、そこへ結果が書かれる。MYDOS は
  `start_shell` でしか DTA を設定しておらず、子は親のものを引き継いで
  いた。つまり子の検索結果が COMMAND.COM の PSP に書き込まれ、子は自分の
  PSP を読んで「何も見つからなかった」と判断する。当時のユーティリティは
  だいたい既定の DTA をそのまま使うので、これは広く効く。
  子が終わったら親の DTA に戻すようにもした。

* **ボリュームラベルが引けなかった。** `dir_find` がラベルのエントリを
  無条件に飛ばしていた。LABEL のようなプログラムは属性 8 を立てた
  拡張 FCB を `AH=0Fh` / `AH=11h` に渡してラベルを探すので、
  明示的に求められたときは返すようにした。

* **`AH=65h` が表そのものを書いていた。** AL=02h/04h/05h/06h/07h は
  「表への far ポインタ」を返す約束で、国別情報を書く AL=01h とは形が
  違う。呼び出し側はその中身をアドレスとして読むので、SORT は
  でたらめな場所を照合順序の表だと思って読み、並びが崩れていた。
  照合順序・大文字変換・ファイル名の区切り文字・DBCS の各表を用意して
  far ポインタを返すようにした。小文字を大文字と同じ重みにしてあるので、
  SORT の並びが DOS と同じ (大小を区別しない) になる。

        直す前:  zebra mango apple banana
        直した後: apple banana mango zebra

## リダイレクトとパイプ

COMMAND.COM が `>` `>>` `<` `|` を扱うようになった。

    C:\>DIR > DIRLIST.TXT
    C:\>ECHO tail-line >> DIRLIST.TXT
    C:\>SORT < UN.TXT
    C:\>TYPE UN.TXT | SORT > SORTED.TXT

DOS のプログラムはハンドル 0 と 1 をそのまま使うだけで、自分が
リダイレクトされていることを知らない。シェルが起動前に `AH=45h` で
元を控え、`AH=46h` で差し替え、終わったら戻す。

パイプは、左側の出力をいったん一時ファイルに落として、それを右側の
入力に回す。当時の DOS も同じことをしていた。プロセスが同時に 2 つ
動かないので、本当の意味で管をつなぐことはできない。入れ子
(`a | b | c`) にも対応していて、一時ファイルの名前は深さで変えている。

`tests/pipetest.asm` が相手役。標準入力を読んで 1 行目を返すだけの
フィルタで、`TYPE README.TXT | PIPETEST` と `PIPETEST < README.TXT` の
両方を自動テストで確かめている。

## DPMI ホストを載せる

保護モードのプログラム (DOS エクステンダを被せたもの) を動かすには DPMI
ホストが要る。MYDOS はこれを自前では持たないので、フリーの **CWSDPMI**
(DJGPP のもの、Charles W Sandmann 作) を C: に置いて使う。

    C:\>CWSDPMI -p -s-
    CWSDPMI V0.90+ (r7) Copyright (C) 2010 CW Sandmann

    C:\>DJECHO hello from a real DJGPP program
    hello from a real DJGPP program

    C:\>GO32
    go32/v2 version 2.0 built Oct 18 2015
    DPMI memory available: 130375 Kb

**DJGPP で作られた 32bit 保護モードのプログラムが動く。** CWSDPMI は
本物のリンカが吐いた 16bit EXE (512 バイトヘッダ、リロケーション 25 個、
SS≠0) で、MYDOS の上で起動し、常駐し、要求ページングでメモリを配り、
保護モードのクライアントから DOS を呼び返すところまで通る。

### ここで見つかったバグ

最初は動かなかった。CWSDPMI が保護モードに入ったあと、MYDOS のカーネル
自身が 32bit のコードで書き潰されていた。ページテーブルを直接読むと
原因がはっきりした。

    linear 0x00401000 -> 物理 0x00000000   ← カーネルの中
    linear 0x00402000 -> 物理 0x00001000   ← カーネルの中
    ...
    linear 0x00410000 -> 物理 0x0000F000   ← カーネルの中

CWSDPMI は物理 0 番地から順にクライアントへ配っていた。つまり拡張メモリ
を 1 バイトも掴めていない。XMS の呼び出しを記録すると、呼んでいたのは

    AH=88h  空きを 32bit で問い合わせる
    AH=89h  32bit で確保する      ← MYDOS が実装していなかった
    AH=0Ch  ロックして物理アドレスを得る
    AH=05h  A20 を開ける

**MYDOS は XMS 3.0 の 32bit 系 (89h/8Eh/8Fh) を実装していなかった。**
16bit の 09h は DX が KB 数なので 64MB までしか頼めず、いまの DPMI ホスト
はどれも 89h しか呼ばない。実装が無いと「確保できなかった」ではなく
「そんな機能はない」が返り、CWSDPMI はそれを見落として、掴んでもいない
メモリを自分のものとして配り始めていた。89h/8Eh/8Fh を実装したら通った。

`tests/xmstest.asm` に回帰テストを 1 件足してある。88h の空き問い合わせ、
89h の確保、8Eh の大きさ照会、8Fh の縮小、そして **0Ch が返す物理アドレス
が 1MB より上を指していること** を確かめる。最後の 1 行がこのバグを
捕まえる。

同じ調査で、INT 2Fh のハンドラが「知らない機能に対して AL=0 を書いて
返す」間違いも見つかった。INT 2Fh は呼ぶ側が AL=0 を入れて呼び、常駐した
ものがあれば AL=FFh を返す導入確認の場所なので、DOS が勝手に AL を触って
はいけない。DPMI の導入確認 (AX=1687h) が AX=1600h を返していた。これも
直してテストを足した。

`tests/dpmichk.asm` (ホストの有無と諸元) と `tests/dpmirun.asm`
(最小の 32bit クライアント) を道具として置いてある。イメージに DPMI
ホストを同梱していないので自動テストには入れていない。

**まだ動かないもの**: Mpxplay (DOS/32A) は保護モードに入って走り出す
ところまでは行くが、UI を出すところまで届かない。サウンドカードの
無い環境で止まっているように見えるが、確かめていない。

## FreeDOS のユーティリティを走らせて出てきたもの

当時のものではないが、DOS の細かいところを本気で使うプログラムとして
FreeDOS の外部コマンドを一式かけた。ここで見つかったのは全部 MYDOS 側の
バグで、しかもどれも「症状と原因が全く違う場所にある」たちの悪いものだった。

| 症状 | 本当の原因 |
| --- | --- |
| CHKDSK がバナーの後で黙る | INT 25h が FLAGS を捨てて返っていた |
| CHKDSK が C: (40MB) で固まる | INT 25h の 32MB 超え用の形が未実装 |
| CHKDSK が「invalid last write date」 | イメージのラベルの日時欄が 0 だった |
| LABEL が「Not a valid drive」 | FCB で CON を開けなかった |
| **LABEL がディスクの中身を全部消す** | AH=13h が拡張 FCB の属性を見ていなかった |
| DELTREE が「DOS error 3」 | RMDIR が 2 回目の検索で親のクラスタを渡していなかった |
| FIND が「Cannot change to directory」 | AX=7160h (長い名前の正規化) が未実装 |

**LABEL の件がいちばん怖かった。** ラベルの張り替えは「属性 8 を立てた
拡張 FCB に `???????????` を入れて AH=13h」という手順で行う。属性を見ずに
名前だけで消すと、`???????????` がすべてのファイルに一致する。実際、
`LABEL A: TESTVOL` を一度走らせただけで、IO.SYS を含む 26 ファイルが
まとめて削除マークを付けられた。DOS のテストは「動くか」だけでなく
「壊さないか」を見ないといけない、という見本のような不具合だった。

回帰テストは `tests/fcbtest.asm` に 3 件 (FCB のドライブ欄、FCB による
デバイスのオープン、拡張 FCB の属性)、`tests/hdtest.asm` に 1 件
(INT 25h の従来形式と 32MB 超え形式)、`tests/dostest.asm` に 1 件
(ルート以外のディレクトリを消す) を足してある。最後のものは、それまでの
テストがルート直下でしか RMDIR を試していなかったために見逃されていた。
親がルートだとクラスタ番号が 0 で、取り違えた値もたまたま 0 になり、
壊れているのに通ってしまう。

残っているのは CHKDSK が FAT12 のフロッピーに対して出す
「Suspicious descriptor in boot」の 1 行。`fsck.fat` も `mtools` も
Linux の vfat も同じイメージを問題なしと見るので、`reserved_sectors` が
18 であることに対する CHKDSK 0.9.2 側の判定と思われるが、確証はない。

## 1MB の壁と XMS

8086 のアドレス線は 20 本しかないので、セグメント:オフセットが 1MB を
超えると 0 番地へ回り込んだ。286 以降はアドレス線が増えたが、その回り込みを
当てにしたプログラムが山ほどあったため、20 本目より上を「A20 ゲート」で
殺しておく互換動作が残った。

A20 を開けると、リアルモードのままでも `FFFF:0010` から `FFFF:FFFF` までの
64KB−16 バイトに手が届く。これが HMA。その上 (1MB+64KB 以降) が拡張メモリで、
リアルモードからは直接触れないので XMS という約束ごしにやり取りする。
窓口の見つけ方まで決まっている。

```
INT 2Fh AX=4300h  → AL=80h なら XMS が居る
INT 2Fh AX=4310h  → ES:BX に入口の far アドレス
以降は far call で AH に機能番号を入れて呼ぶ
```

実装しているのは XMS 3.0 の機能一式 —
バージョン (`00h`)、HMA の貸し借り (`01h`/`02h`)、A20 の開閉
(`03h`〜`07h`)、空き容量 (`08h`/`88h`)、拡張メモリの確保・解放
(`09h`/`0Ah`)、ブロック転送 (`0Bh`)、ロックと情報 (`0Ch`〜`0Fh`)、
UMB (`10h`/`11h`)。

### 拡張メモリへの転送

1MB より上へ届かせる方法は 2 つある。

1. BIOS の `INT 15h AH=87h` に頼む。286 の頃の `HIMEM.SYS` はこれだった
2. 自分でいったんプロテクトモードに入り、4GB のリミットをセグメント
   レジスタの隠し部分に焼き付けてからリアルモードへ戻る。戻ったあとも
   その隠しリミットは残るので、リアルモードのまま 32bit のオフセットで
   1MB より上に手が届く (俗にアンリアルモード)。386 以降の `HIMEM.SYS` は
   こちら

MYDOS は 2 を使う。焼き付ける先を FS と GS にしてあるのは、DS/ES/SS を
触らずに済ませるため。リアルモードでセグメントレジスタに値を入れ直すと
隠しリミットが 64KB に戻ってしまうので、転送の間はこの 2 本以外を触らない。
同じ理由で割り込みも止めておく (割り込みハンドラが FS/GS を作り直すと
台無しになる)。

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
| ハンドル | `44` (`AL=00h`/`01h`/`08h`/`09h`/`0Eh`/`0Fh`) `45` `46` |
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
- `DOSINT.COM` — `AH=52h` の List of Lists から DPB 連鎖・SFT 連鎖・
  デバイス連鎖・CDS 配列・バッファ連鎖・MCB 連鎖を、当時のツールと
  同じやり方で辿る 7 項目
- `HDTEST.COM` — ハードディスクと FAT16 の 7 項目 (下記)
- `CFGTEST.COM` — CONFIG.SYS と `.SYS` ドライバの 6 項目 (下記)
- `XMSTEST.COM` — A20 / HMA / XMS の 8 項目 (下記)
- `C:` / `DIR` / `A:` — シェルによるドライブ切り替え
- `FCBTEST.COM ALPHA BETA` — FCB 系と 6.22 追加分の 15 項目
- `DOSTEST.COM` — ハンドル系 INT 21h の 10 項目

QEMU にはフロッピー (`-fda`) と一緒に、パーティションを切った FAT16 の
ハードディスク (`-hda build/hd.img`) を繋いである。`HDTEST.COM` は
そちらを相手に、パーティションの先頭 LBA が足されているか、FAT16 の
エントリを正しく引けているか、クラスタ→LBA の計算が 32bit になっているかを
確かめる。最後のものは 32MB を超えた位置にデータを書いて読み返す形で見る。
16bit で計算していると、そこへの書き込みがパーティションの先頭付近を
壊しにいくので、読み返した内容が食い違う。

起動時には `tests/config.sys` が読まれ、`CFGTEST.COM` がその結果を
List of Lists から確かめる。数の指定 (`FILES=` / `BUFFERS=` /
`LASTDRIVE=`) が実際の表の大きさになっていること、`DEVICE=` で入れた
2 つのドライバが連鎖の組み込みより前にいること、文字デバイスが名前で
開けて読み書きと IOCTL が通ること、ブロックデバイスが生やしたドライブに
本当にファイルが作れることを見る。

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

  [PASS] AH=52h  List of Lists fields are populated
  [PASS] LoL-2   MCB chain walks to a Z block
  [PASS] LoL+00  DPB chain matches AH=32h and the BPB
  [PASS] LoL+22  device chain NUL-CON-AUX-PRN-CLOCK$-block
  [PASS] LoL+04  SFT chain shows an open file by name
  [PASS] LoL+16  CDS array follows CHDIR
  [PASS] LoL+12  disk buffer chain is linked

  [PASS] C: and D: appeared from the partition table
  [PASS] AH=0Eh/19h  switch the current drive to C:
  [PASS] AH=36h  C: is FAT16 (more than 4085 clusters)
  [PASS] file round trip on C: (partition offset applied)
  [PASS] write past the 32MB mark (32-bit cluster to LBA)
  [PASS] C: and D: are usable at the same time
  [PASS] AH=32h  DPB of C: matches its BPB

  [PASS] FILES=30    the SFT table really holds 30 entries
  [PASS] BUFFERS=12  the disk buffer chain is 12 long
  [PASS] LASTDRIVE=H is reported in the List of Lists
  [PASS] DEVICE=     both drivers sit ahead of CON in the chain
  [PASS] MYDEV.SYS   opens by name, reads, writes, IOCTL
  [PASS] RAMDISK.SYS gave a usable drive with a real FAT12

  [PASS] INT 2Fh 4300h/4310h  the XMS entry point is there
  [PASS] XMS 00h  version 3.00 and the HMA exists
  [PASS] XMS 05h/06h/07h  the A20 gate really opens
  [PASS] XMS 01h/02h  the HMA can be borrowed and given back
  [PASS] XMS 08h/09h  allocate 64K of extended memory
  [PASS] XMS 0Bh  copy up past 1MB and back, byte for byte
  [PASS] XMS 0Ch/0Dh/0Eh/0Fh  lock, query and shrink
  [PASS] XMS 0Ah  the handle is gone after freeing it

  [PASS] .COM / .EXE の起動と .EXE のリロケーション適用
  [PASS] AH=31h 常駐終了が本当に残っている
  [PASS] AH=4Bh AL=3 オーバーレイのリロケーション
```

テストが書いたファイルは実際のディスクイメージに残るので、走らせたあとに
`make check` でホスト側から検証できる。フロッピーもハードディスクの
FAT16 パーティションも、`mdir` で中身が読め、`fsck.fat` がエラーなしを
返すことを確認済み。

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

INT 21h の面はすべて埋まり、内部データ構造 (A) とストレージ (D) も
本物の形になった。「6.22 と完全互換」と言うにはまだ以下が要る。
効いてくる順に並べてある。

### A. 内部データ構造を本物の形にする

実装済み。SFT (DOS 4 以降の 59 バイト形式)、CDS 配列、デバイスドライバの
ヘッダ連鎖 (NUL → CON → AUX → PRN → CLOCK$ → ブロックデバイス)、
ディスクバッファ連鎖、ドライブごとの DPB 連鎖が、すべて List of Lists から
辿れる本物の形でメモリ上に実在する。`tests/dosint.asm` が当時のツールと
同じやり方でそれを検証している。

### B. 起動時の構成

CONFIG.SYS とインストール可能デバイスドライバは実装済み。残っているのは
以下。

- **`STACKS=`** — 数は受け取って控えるが、割り込みごとにスタックを
  切り替えるところまではやっていない
- **`COUNTRY=`** — 国別情報の実体がまだ形だけ
- **`MENUITEM=` / `MENUDEFAULT=` / `INCLUDE=`** — 6.0 で入った起動メニュー
- **`DEVICEHIGH=`** — いまは `DEVICE=` と同じ扱い (UMB がまだ無い)

### C. メモリ

A20・HMA・XMS は実装済み。残っているのは以下。

- **`DOS=HIGH`** — CONFIG.SYS の指定は読むが、カーネル自身を HMA へ移す
  ところまではやっていない。移すこと自体は動く (先頭 16 バイトを余白に
  してあるのはそのため) が、移したあとが問題で、BIOS のディスク転送も
  インストール可能デバイスドライバも 1MB より上のバッファに届かない。
  低位メモリを中継させる形も試したが安定せず、いまは低位に居座らせている。
  HMA は XMS 経由でプログラムに貸し出す
- **UMB** (`DOS=UMB`) — 640KB と 1MB の間の「穴」に RAM を貼るには V86
  モードとページングが要る。それをやるのが EMM386 で、それがまだ無いので
  XMS の `10h` は正直に「無い」と答える (EMM386 を入れずに `DOS=UMB` と
  だけ書いたときの本物の振る舞いと同じ)
- **EMS** (LIM 4.0) — 上と同じ理由でまだ

**未解決の不具合**: XMS のブロック転送 (`0Bh`) を呼んだあと、別ドライブの
ディレクトリ一覧が壊れることがある (C: の DIR に A: の内容が繰り返し出る)。
BIOS の `AH=87h` を使う実装でもアンリアルモードの実装でも起き、1MB より上に
一切触れない転送でも起きるので、転送そのものではなく `xms_block_move` の
前後で機械の状態が変わることが原因と思われる。原因は未特定。
そのため自動テストでは XMS の試験を最後に回してある。

### D. ストレージ

FAT16、ハードディスク、パーティション、複数ドライブ、INT 13h 拡張は
実装済み。残っているのは以下。

- **FAT32** — 6.22 には無いので互換性の観点では不要だが、大きなディスクを
  そのまま使えるようにするなら要る
- **拡張パーティション** — いまは基本パーティション 4 つまで
- **メディア交換の検出** — フロッピーを入れ替えたときにバッファを捨てる
  (`DEVC_MEDIACHECK` は常に「変わっていない」と答えている)
- **`FORMAT` / `FDISK`** — 作る側。読み書きはできるがフォーマットはできない

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
