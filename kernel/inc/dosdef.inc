; ============================================================================
; dosdef.inc  -  MYDOS カーネル全体で共有する定数と構造体のオフセット
;
; ここに書かれているレイアウトは当時のプログラムが直接覗きにくる場所なので、
; 本物の MS-DOS と一致していないと互換性が壊れる。値の出典は
; Ralf Brown's Interrupt List と MS-DOS 3.x/5.0 のプログラマ向け資料。
; ============================================================================

; --- メモリ配置 -------------------------------------------------------------
;
;   0x00000 - 0x003FF   割り込みベクタテーブル
;   0x00400 - 0x004FF   BIOS データエリア
;   0x00500 - 0x005FF   DOS の作業領域 (List of Lists など)
;   0x00600 - ......    IO.SYS (このカーネル。code + data + バッファ)
;   ........ - 0x9FFFF  MCB アリーナ (プログラムに配られる領域)
;   0xA0000 -           ビデオ RAM
;
; カーネルは Stage2 に 0x1000:0000 へ読み込まれたあと、自分自身を
; 0x0060:0000 へ降ろしてから動き出す。低位に置くほどプログラムに渡せる
; 連続領域が広くなる。
KERNEL_SEG      equ 0x0060      ; カーネルの最終的な居場所
LOAD_SEG        equ 0x1000      ; Stage2 が最初に置く場所 (再配置元)
DOSDATA_SEG     equ 0x0050      ; 0x500 - 0x5FF, List of Lists などの置き場
MEM_TOP_SEG     equ 0xA000      ; コンベンショナルメモリの上限 (640KB)

; --- 割り込み ---------------------------------------------------------------
INT_TERMINATE   equ 0x20
INT_DOS         equ 0x21
INT_TERM_ADDR   equ 0x22        ; プログラム終了時の戻り先
INT_CTRLC       equ 0x23
INT_CRITERR     equ 0x24
INT_ABSREAD     equ 0x25
INT_ABSWRITE    equ 0x26
INT_TSR         equ 0x27
INT_IDLE        equ 0x28
INT_FASTCON     equ 0x29
INT_MULTIPLEX   equ 0x2F

; --- PSP (Program Segment Prefix) -------------------------------------------
;
; 256 バイト。プログラムの直前に置かれ、DS と ES がここを指した状態で
; プログラムが始まる。当時のプログラムは以下を直接読みにくる:
;   +2   使えるメモリの上限 (「あと何 KB 使える」の判定に使う)
;   +2C  環境変数ブロックのセグメント (getenv がここを辿る)
;   +80  コマンドライン (argv の取得元)
;   +5C  FCB (古い形式のファイル指定)
PSP_INT20       equ 0x00        ; CD 20 (INT 20h) — CS:0 への jmp で終了できる
PSP_MEMTOP      equ 0x02        ; word: 使用可能な最終セグメント+1
PSP_RESERVED1   equ 0x04
PSP_CPM_CALL    equ 0x05        ; 9A xx xx xx xx : CP/M 互換の far call
PSP_INT22       equ 0x0A        ; dword: 終了アドレス (親の INT 22h)
PSP_INT23       equ 0x0E        ; dword: Ctrl-C ハンドラ
PSP_INT24       equ 0x12        ; dword: クリティカルエラーハンドラ
PSP_PARENT      equ 0x16        ; word: 親プロセスの PSP セグメント
PSP_JFT         equ 0x18        ; 20 バイト: ハンドル → SFT 番号の変換表
PSP_ENVSEG      equ 0x2C        ; word: 環境ブロックのセグメント
PSP_STACK       equ 0x2E        ; dword: DOS 呼び出し中の SS:SP 退避先
PSP_JFT_SIZE    equ 0x32        ; word: JFT のエントリ数 (既定 20)
PSP_JFT_PTR     equ 0x34        ; dword: JFT へのポインタ (既定 PSP:0018)
PSP_PREV_PSP    equ 0x38        ; dword: 一つ前の PSP (SHARE 用)
PSP_DISPATCH    equ 0x50        ; CD 21 CB : INT 21h + RETF
PSP_FCB1        equ 0x5C        ; 16 バイト
PSP_FCB2        equ 0x6C        ; 20 バイト
PSP_CMDTAIL_LEN equ 0x80        ; byte: コマンドテイルの長さ
PSP_CMDTAIL     equ 0x81        ; 127 バイト, 0x0D 終端
PSP_SIZE        equ 0x100

JFT_ENTRIES     equ 20
JFT_FREE        equ 0xFF

; --- MCB (Memory Control Block) ---------------------------------------------
;
; 16 バイト。確保された各ブロックの直前に置かれ、連鎖して全メモリを覆う。
; AH=48h で「空きを全部よこせ」と言ってくる .EXE がこの連鎖を前提にしている。
MCB_SIG         equ 0x00        ; byte: 'M' = 続きがある / 'Z' = 最後
MCB_OWNER       equ 0x01        ; word: 所有者の PSP (0 = 空き, 8 = DOS 自身)
MCB_SIZE        equ 0x03        ; word: ブロックの大きさ (パラグラフ, MCB 自身は含まない)
MCB_NAME        equ 0x08        ; 8 バイト: プログラム名 (DOS 4.0+)
MCB_HDRSIZE     equ 16

MCB_SIG_MID     equ 'M'
MCB_SIG_LAST    equ 'Z'
MCB_OWNER_FREE  equ 0x0000
MCB_OWNER_DOS   equ 0x0008

; メモリ確保の戦略 (AH=58h)
FIT_FIRST       equ 0
FIT_BEST        equ 1
FIT_LAST        equ 2

; --- ディレクトリエントリ (32 バイト) ---------------------------------------
DIR_NAME        equ 0x00        ; 11 バイト: 8.3 を空白で埋めたもの
DIR_ATTR        equ 0x0B
DIR_NTRES       equ 0x0C
DIR_CRTIME      equ 0x0E
DIR_CRDATE      equ 0x10
DIR_ACCDATE     equ 0x12
DIR_CLUSHI      equ 0x14        ; FAT12/16 では未使用
DIR_TIME        equ 0x16
DIR_DATE        equ 0x18
DIR_CLUSTER     equ 0x1A
DIR_SIZE        equ 0x1C
DIR_ENTSIZE     equ 32

DIR_FREE        equ 0xE5        ; 削除済み
DIR_END         equ 0x00        ; ここから先は未使用

; ファイル属性
ATTR_READONLY   equ 0x01
ATTR_HIDDEN     equ 0x02
ATTR_SYSTEM     equ 0x04
ATTR_VOLUME     equ 0x08
ATTR_DIRECTORY  equ 0x10
ATTR_ARCHIVE    equ 0x20
ATTR_LFN        equ 0x0F        ; 長いファイル名の断片 (無視する)

; --- ファイルハンドル -------------------------------------------------------
SFT_ENTRIES     equ 40          ; システム全体で開けるファイル数
SFT_NAME        equ 0x00        ; 11 バイト: 8.3 名
SFT_MODE        equ 0x0B        ; byte: オープンモード
SFT_ATTR        equ 0x0C        ; byte: 属性
SFT_CLUSTER     equ 0x0D        ; word: 先頭クラスタ
SFT_SIZE        equ 0x0F        ; dword: ファイルサイズ
SFT_POS         equ 0x13        ; dword: 現在位置
SFT_REFCNT      equ 0x17        ; byte: この SFT を指しているハンドルの数
SFT_DEVICE      equ 0x18        ; byte: 0=ファイル, 1=CON, 2=NUL, 3=AUX, 4=PRN
SFT_DIRSEC      equ 0x19        ; word: ディレクトリエントリのある LBA
SFT_DIRENT      equ 0x1B        ; byte: そのセクタ内のエントリ番号
SFT_FLAGS       equ 0x1C        ; byte: bit0 = 書き換えられた
SFT_CIDX        equ 0x1D        ; word: 直前に辿ったクラスタの番号 (0 起算)
SFT_CCLUS       equ 0x1F        ; word: そのときのクラスタ番号
SFT_DIRCLUS     equ 0x21        ; word: 親ディレクトリのクラスタ (0 = ルート)
SFT_ENTSIZE     equ 48

SFT_DEV_FILE    equ 0
SFT_DEV_CON     equ 1
SFT_DEV_NUL     equ 2
SFT_DEV_AUX     equ 3
SFT_DEV_PRN     equ 4

SFT_FLAG_DIRTY  equ 0x01

OPEN_READ       equ 0
OPEN_WRITE      equ 1
OPEN_RDWR       equ 2

; --- DOS のエラーコード (AX に入れて CF=1 で返す) ---------------------------
ERR_NONE        equ 0
ERR_FUNC        equ 1           ; 無効な機能番号
ERR_FILE_NOT_FOUND equ 2
ERR_PATH_NOT_FOUND equ 3
ERR_TOO_MANY_FILES equ 4
ERR_ACCESS      equ 5
ERR_BAD_HANDLE  equ 6
ERR_MCB_DESTROYED equ 7
ERR_NOT_ENOUGH_MEM equ 8
ERR_BAD_MCB     equ 9
ERR_BAD_ENV     equ 10
ERR_BAD_FORMAT  equ 11
ERR_BAD_ACCESS  equ 12
ERR_BAD_DATA    equ 13
ERR_BAD_DRIVE   equ 15
ERR_NO_MORE_FILES equ 18
ERR_WRITE_PROTECT equ 19
ERR_DISK_FULL   equ 39
ERR_FILE_EXISTS equ 80

; --- DTA (Disk Transfer Area) の FindFirst/FindNext 形式 --------------------
;
; AH=4Eh/4Fh は結果をここに書く。先頭 21 バイトは DOS の作業用で、
; プログラムが読むのは +0x15 以降。FindNext を続けるために検索状態を
; DTA 自身に埋め込むのが本物の作法 (プログラムが DTA を切り替えて
; 複数の検索を並行させられる)。
FIND_DRIVE      equ 0x00        ; byte: ドライブ番号
FIND_PATTERN    equ 0x01        ; 11 バイト: 検索パターン (8.3)
FIND_ATTR       equ 0x0C        ; byte: 検索属性
FIND_ENTRY      equ 0x0D        ; word: 次に見るエントリ番号
FIND_DIRSEC     equ 0x0F        ; word: 検索中のディレクトリの先頭 LBA
FIND_DIRCNT     equ 0x11        ; word: そのディレクトリのセクタ数
FIND_RESERVED   equ 0x13        ; word
FIND_ATTR_FOUND equ 0x15        ; byte: 見つかったファイルの属性
FIND_TIME       equ 0x16        ; word
FIND_DATE       equ 0x18        ; word
FIND_SIZE       equ 0x1A        ; dword
FIND_NAME       equ 0x1E        ; 13 バイト: "NAME.EXT" + 0 終端

; --- その他 -----------------------------------------------------------------
SECTOR_SIZE     equ 512
DOS_VER_MAJOR   equ 5
DOS_VER_MINOR   equ 0

SERIAL_BASE     equ 0x3F8       ; COM1
