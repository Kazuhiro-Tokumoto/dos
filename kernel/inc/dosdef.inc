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

; --- SFT (System File Table) -----------------------------------------------
;
; DOS 4.0 以降の 59 バイト形式。当時のツール (ファイラ、デバッガ、
; ハンドルを覗く常駐ソフト) は List of Lists から SFT 連鎖を辿って
; ここを直接読むので、独自の形にしていると軒並み誤動作する。
;
; SFT はシステム全体で 1 つのテーブル。PSP の JFT (ハンドル → SFT 番号) が
; そこを指すという 2 段構えで、AH=45h の複製や AH=46h のリダイレクトが
; 「JFT の項目を同じ SFT に向け直す」だけで済むようになっている。
SFT_REFCNT      equ 0x00        ; word: この SFT を指しているハンドルの数
SFT_MODE        equ 0x02        ; word: オープンモード
SFT_ATTR        equ 0x04        ; byte: ファイル属性
SFT_DEVINFO     equ 0x05        ; word: デバイス情報 (下記)
SFT_DEVPTR      equ 0x07        ; dword: デバイスヘッダ (文字) / DPB (ブロック)
SFT_CLUSTER     equ 0x0B        ; word: 先頭クラスタ
SFT_TIME        equ 0x0D        ; word
SFT_DATE        equ 0x0F        ; word
SFT_SIZE        equ 0x11        ; dword: ファイルサイズ
SFT_POS         equ 0x15        ; dword: 現在位置
SFT_CIDX        equ 0x19        ; word: 直前に辿った相対クラスタ番号
SFT_DIRSEC      equ 0x1B        ; dword: ディレクトリエントリのあるセクタ
SFT_DIRENT      equ 0x1F        ; byte: そのセクタ内のエントリ番号
SFT_NAME        equ 0x20        ; 11 バイト: 8.3 名
SFT_SHARE_PREV  equ 0x2B        ; dword: SHARE 用
SFT_MACHINE     equ 0x2F        ; word: ネットワークのマシン番号
SFT_OWNER       equ 0x31        ; word: 所有プロセスの PSP
SFT_SHAREREC    equ 0x33        ; word: SHARE のレコード
SFT_CCLUS       equ 0x35        ; word: 直前に辿った絶対クラスタ番号
SFT_IFS         equ 0x37        ; dword: IFS ドライバ (0 = 通常)
SFT_ENTSIZE     equ 0x3B        ; 59 バイト

; SFT テーブルのヘッダ (List of Lists の +04 が指す先)
SFTH_NEXT       equ 0x00        ; dword: 次のテーブル (FFFF:FFFF = 最後)
SFTH_COUNT      equ 0x04        ; word: このテーブルのエントリ数
SFTH_ENTRIES    equ 0x06        ; ここから SFT が並ぶ
SFT_ENTRIES     equ 40          ; システム全体で開けるファイル数の既定値
MAX_SFT_ENTRIES equ 100         ; FILES= で増やせる上限

; SFT_DEVINFO のビット
SFTI_CHAR       equ 0x8000      ; 1 = 文字デバイス, 0 = ブロック (ディスク上のファイル)
SFTI_NETWORK    equ 0x0800
SFTI_IOCTL      equ 0x4000      ; IOCTL に対応している
SFTI_CLEAN      equ 0x0040      ; 1 = まだ書き込まれていない (0 = 書き換え済み)
SFTI_EOF        equ 0x0040
SFTI_BINARY     equ 0x0020      ; 1 = バイナリ (raw) モード
SFTI_CLOCK      equ 0x0008
SFTI_NUL        equ 0x0004
SFTI_STDOUT     equ 0x0002
SFTI_STDIN      equ 0x0001
SFTI_DRIVE      equ 0x003F      ; ブロックデバイスのときはドライブ番号

; 我々が扱う文字デバイスの種類 (SFT_DEVPTR の指す先で判別する)
SFT_DEV_FILE    equ 0
SFT_DEV_CON     equ 1
SFT_DEV_NUL     equ 2
SFT_DEV_AUX     equ 3
SFT_DEV_PRN     equ 4
SFT_DEV_CLOCK   equ 5
SFT_DEV_OTHER   equ 6            ; DEVICE= で入れた文字デバイス

OPEN_READ       equ 0
OPEN_WRITE      equ 1
OPEN_RDWR       equ 2

; --- CDS (Current Directory Structure) --------------------------------------
;
; ドライブごとのカレントディレクトリ。DOS 4.0 以降は 88 バイト。
; List of Lists の +16h がこの配列を指し、LASTDRIVE の数だけ並ぶ。
; 「A: のカレントは \DOS、C: のカレントは \TOOLS」を同時に覚えていられるのは
; この配列があるから。複数ドライブと SUBST/JOIN の土台でもある。
CDS_PATH        equ 0x00        ; 67 バイト: "X:\PATH" 形式の ASCIZ
CDS_FLAGS       equ 0x43        ; word: 属性 (下記)
CDS_DPB         equ 0x45        ; dword: このドライブの DPB
CDS_CLUSTER     equ 0x49        ; word: カレントディレクトリの先頭クラスタ
CDS_UNKNOWN1    equ 0x4B        ; word
CDS_UNKNOWN2    equ 0x4D        ; dword
CDS_ROOTLEN     equ 0x51        ; word: パス中でルートの直後にあたる位置
CDS_UNKNOWN3    equ 0x53        ; byte
CDS_IFS         equ 0x54        ; dword: IFS ドライバ
CDS_UNKNOWN4    equ 0x58
CDS_ENTSIZE     equ 0x58        ; 88 バイト

CDSF_NETWORK    equ 0x8000
CDSF_PHYSICAL   equ 0x4000      ; 実在するドライブ
CDSF_JOIN       equ 0x2000
CDSF_SUBST      equ 0x1000

MAX_DRIVES      equ 26          ; A: - Z:
DEFAULT_LASTDRIVE equ 5         ; 既定は E: まで (MS-DOS と同じ)

; --- デバイスドライバのヘッダ (18 バイト) ------------------------------------
;
; NUL → CON → AUX → PRN → CLOCK$ → ブロックデバイス という順に連鎖する。
; CONFIG.SYS の DEVICE= で読み込んだドライバは NUL の直後に差し込まれ、
; 先に見つかったほうが優先される (だから ANSI.SYS が CON を乗っ取れる)。
DEV_NEXT        equ 0x00        ; dword: 次のデバイス (FFFF:FFFF = 最後)
DEV_ATTR        equ 0x04        ; word: 属性
DEV_STRATEGY    equ 0x06        ; word: 要求を受け取る入口のオフセット
DEV_INTERRUPT   equ 0x08        ; word: 実際に処理する入口のオフセット
DEV_NAME        equ 0x0A        ; 8 バイト: 文字デバイスの名前
                                ;   ブロックデバイスは 1 バイト目がユニット数
DEV_HDRSIZE     equ 18

; デバイス属性
DEVA_CHAR       equ 0x8000      ; 1 = 文字デバイス
DEVA_IOCTL      equ 0x4000      ; IOCTL に対応
DEVA_OUTBUSY    equ 0x2000      ; 「話中まで出力」に対応 (文字)
DEVA_NONIBM     equ 0x2000      ; IBM 以外の形式 (ブロック)
DEVA_OPENCLOSE  equ 0x0800      ; オープン/クローズ/リムーバブルに対応
DEVA_GENIOCTL   equ 0x0040      ; 汎用 IOCTL に対応
DEVA_CLOCK      equ 0x0008      ; CLOCK$ である
DEVA_NUL        equ 0x0004      ; NUL である
DEVA_STDOUT     equ 0x0002      ; 標準出力である
DEVA_STDIN      equ 0x0001      ; 標準入力である

; --- デバイスドライバの要求ヘッダ -------------------------------------------
;
; DOS はドライバを「STRATEGY で要求の場所を渡し、INTERRUPT で実行させる」
; という 2 段階で呼ぶ。大昔のマルチタスク構想の名残で、実際には
; STRATEGY が場所を覚えて INTERRUPT がすぐ処理する。
REQ_LENGTH      equ 0x00        ; byte: この要求ヘッダの長さ
REQ_UNIT        equ 0x01        ; byte: ユニット番号 (ブロックデバイス)
REQ_CMD         equ 0x02        ; byte: コマンド
REQ_STATUS      equ 0x03        ; word: 結果
REQ_RESERVED    equ 0x05        ; 8 バイト
; コマンドごとの続き (読み書きの場合)
REQ_MEDIA       equ 0x0D        ; byte: メディアディスクリプタ
REQ_BUFFER      equ 0x0E        ; dword: 転送先/元
REQ_COUNT       equ 0x12        ; word: セクタ数 / バイト数
REQ_START       equ 0x14        ; word: 開始セクタ
REQ_VOLID       equ 0x16        ; dword
REQ_STARTL      equ 0x1A        ; dword: 32bit の開始セクタ (65535 超のとき)
; INIT (コマンド 0) の場合
REQ_INIT_UNITS  equ 0x0D        ; byte: 対応したユニット数
REQ_INIT_END    equ 0x0E        ; dword: 常駐部分の末尾
REQ_INIT_BPB    equ 0x12        ; dword: BPB 配列へのポインタ
REQ_INIT_DRIVE  equ 0x16        ; byte: 最初のドライブ番号

; デバイスドライバのコマンド
DEVC_INIT       equ 0
DEVC_MEDIACHECK equ 1
DEVC_BUILDBPB   equ 2
DEVC_IOCTLREAD  equ 3
DEVC_READ       equ 4
DEVC_PEEK       equ 5           ; 破壊しない読み取り
DEVC_INSTATUS   equ 6
DEVC_INFLUSH    equ 7
DEVC_WRITE      equ 8
DEVC_WRITEVFY   equ 9
DEVC_OUTSTATUS  equ 10
DEVC_OUTFLUSH   equ 11
DEVC_IOCTLWRITE equ 12
DEVC_OPEN       equ 13
DEVC_CLOSE      equ 14
DEVC_REMOVABLE  equ 15
DEVC_OUTBUSY    equ 16

; 要求の結果 (REQ_STATUS)
DEVS_DONE       equ 0x0100      ; 完了
DEVS_BUSY       equ 0x0200
DEVS_ERROR      equ 0x8000
DEVE_WRITEPROT  equ 0x00
DEVE_UNKNOWNUNIT equ 0x01
DEVE_NOTREADY   equ 0x02
DEVE_UNKNOWNCMD equ 0x03
DEVE_CRC        equ 0x04
DEVE_SEEK       equ 0x06
DEVE_UNKNOWNMEDIA equ 0x07
DEVE_SECTORNOTFOUND equ 0x08
DEVE_WRITEFAULT equ 0x0A
DEVE_READFAULT  equ 0x0B
DEVE_GENERAL    equ 0x0C

; --- List of Lists (AH=52h が ES:BX で返す構造体) ---------------------------
;
; DOS の内部構造への入口。ここから DPB 連鎖・SFT 連鎖・デバイス連鎖・
; CDS 配列に辿り着ける。手前 2 バイトに最初の MCB があるのは昔からの約束。
LOL_FIRST_MCB   equ -2          ; word: 最初の MCB のセグメント
LOL_FIRST_DPB   equ 0x00        ; dword
LOL_FIRST_SFT   equ 0x04        ; dword
LOL_CLOCK       equ 0x08        ; dword: CLOCK$ デバイス
LOL_CON         equ 0x0C        ; dword: CON デバイス
LOL_MAXSECTOR   equ 0x10        ; word: 最大セクタサイズ
LOL_BUFFERS     equ 0x12        ; dword: ディスクバッファ連鎖
LOL_CDS         equ 0x16        ; dword: CDS 配列
LOL_FCBTABLE    equ 0x1A        ; dword: FCB テーブル
LOL_FCBKEEP     equ 0x1E        ; word: 保護される FCB の数
LOL_NUMBLOCK    equ 0x20        ; byte: ブロックデバイスの数
LOL_LASTDRIVE   equ 0x21        ; byte: LASTDRIVE
LOL_NULDEV      equ 0x22        ; 18 バイト: NUL デバイスのヘッダそのもの
LOL_NUMJOIN     equ 0x34        ; byte: JOIN されたドライブ数
LOL_SIZE        equ 0x35

; --- ディスクバッファ (BUFFERS= の実体) -------------------------------------
BUF_NEXT        equ 0x00        ; dword: 次のバッファ
BUF_DRIVE       equ 0x04        ; byte: ドライブ番号 (FFh = 未使用)
BUF_FLAGS       equ 0x05        ; byte
BUF_SECTOR      equ 0x06        ; dword: このバッファに載っているセクタ
BUF_COPIES      equ 0x0A        ; byte: 書き戻す FAT のコピー数
BUF_OFFSET      equ 0x0B        ; word: FAT のコピー間のセクタ数
BUF_DPB         equ 0x0D        ; dword
BUF_USE         equ 0x11        ; word
BUF_DATA        equ 0x14        ; ここから 512 バイト
BUF_HDRSIZE     equ 0x14
DEFAULT_BUFFERS equ 8

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

; --- DPB (Drive Parameter Block) --------------------------------------------
;
; DOS がドライブごとに持つ構造体。AH=32h でそのまま渡され、
; List of Lists からも連鎖で辿れる。当時のディスクツールはここを読む。
DPB_DRIVE       equ 0x00        ; byte: ドライブ番号 (0 = A:)
DPB_UNIT        equ 0x01        ; byte: ドライバ内のユニット番号
DPB_SECSIZE     equ 0x02        ; word: セクタあたりバイト数
DPB_CLUSMASK    equ 0x04        ; byte: クラスタ内の最大セクタ番号 (spc - 1)
DPB_CLUSSHIFT   equ 0x05        ; byte: クラスタ→セクタのシフト量
DPB_RESERVED    equ 0x06        ; word: 予約セクタ数
DPB_NUMFATS     equ 0x08        ; byte: FAT の数
DPB_ROOTENT     equ 0x09        ; word: ルートディレクトリのエントリ数
DPB_DATASEC     equ 0x0B        ; word: データ領域の開始セクタ
DPB_MAXCLUS     equ 0x0D        ; word: 最大クラスタ番号 (データクラスタ数 + 1)
DPB_SECPERFAT   equ 0x0F        ; word: FAT 1 個のセクタ数
DPB_DIRSEC      equ 0x11        ; word: ルートディレクトリの開始セクタ
DPB_DRIVER      equ 0x13        ; dword: デバイスドライバのヘッダ
DPB_MEDIA       equ 0x17        ; byte: メディアディスクリプタ
DPB_ACCESS      equ 0x18        ; byte: 0 = アクセス済み, FFh = 未
DPB_NEXT        equ 0x19        ; dword: 次の DPB
DPB_FREESRCH    equ 0x1D        ; word: 空き探索の開始クラスタ
DPB_FREECNT     equ 0x1F        ; word: 空きクラスタ数 (FFFFh = 不明)
DPB_SIZE        equ 0x21

; --- ドライブ表 (drive.inc) -------------------------------------------------
;
; DOS の DPB は「そのドライブのファイルシステムの形」しか持っていない。
; どの物理ドライブの、どのパーティションの、どの位置から始まるのかは
; 別に覚えておく必要がある。DPB を含む一回り大きい表をドライブごとに置き、
; DPB はその中に埋め込む (List of Lists から辿れるのは DPB の側)。
DRV_FLAGS       equ 0x00        ; byte: 下の DRVF_*
DRV_PHYS        equ 0x01        ; byte: BIOS のドライブ番号 (00h/01h/80h/81h)
DRV_USEEXT      equ 0x02        ; byte: 0 = CHS, 1 = INT 13h 拡張
DRV_FATTYPE     equ 0x03        ; byte: 12 か 16
DRV_PARTLBA     equ 0x04        ; dword: パーティションの先頭 LBA (絶対)
DRV_SECPERTRK   equ 0x08        ; word
DRV_HEADS       equ 0x0A        ; word
DRV_SECPERCLUS  equ 0x0C        ; word
DRV_RESERVED    equ 0x0E        ; word: 予約セクタ数
DRV_NUMFATS     equ 0x10        ; word
DRV_ROOTENT     equ 0x12        ; word
DRV_SECPERFAT   equ 0x14        ; word
DRV_TOTALSEC    equ 0x16        ; dword: 総セクタ数 (小さい版と大きい版の統合)
DRV_MEDIA       equ 0x1A        ; byte
DRV_ROOTLBA     equ 0x1C        ; word: ルートディレクトリ (パーティション相対)
DRV_ROOTSECS    equ 0x1E        ; word
DRV_DATALBA     equ 0x20        ; word: データ領域の先頭 (パーティション相対)
DRV_MAXCLUS     equ 0x22        ; word: 存在する最大クラスタ番号 + 1
DRV_CLUSBYTES   equ 0x24        ; word: 1 クラスタのバイト数
DRV_SERIAL      equ 0x26        ; dword: ボリュームシリアル
DRV_LABEL       equ 0x2A        ; 11 バイト: ボリュームラベル
DRV_DRIVER      equ 0x35        ; dword: このドライブを扱うデバイスドライバ
DRV_UNIT        equ 0x39        ; byte: そのドライバの中でのユニット番号
DRV_DPB         equ 0x3A        ; DPB_SIZE バイト: この中に DPB を埋め込む
DRV_ENTSIZE     equ 0x5B

DRVF_PRESENT    equ 0x01        ; 実在する
DRVF_REMOVABLE  equ 0x02        ; メディアを入れ替えられる (フロッピー)
DRVF_EXTERNAL   equ 0x04        ; DEVICE= で入れたドライバが扱う (INT 13h ではない)

; --- パーティションテーブル (MBR の +1BEh から 16 バイト x 4) ---------------
PART_BOOT       equ 0x00        ; byte: 80h なら起動可能
PART_TYPE       equ 0x04        ; byte: システム ID (01h/04h/06h/0Eh など)
PART_LBA        equ 0x08        ; dword: 先頭 LBA
PART_SECS       equ 0x0C        ; dword: セクタ数
PART_ENTSIZE    equ 16
PART_TABLE      equ 0x1BE       ; MBR 内での位置

; --- その他 -----------------------------------------------------------------
SECTOR_SIZE     equ 512
FAT_RESIDENT_SECS equ 12       ; 常駐させる FAT の最大セクタ数 (FAT12 用)
DOS_VER_MAJOR   equ 6
DOS_VER_MINOR   equ 22

SERIAL_BASE     equ 0x3F8       ; COM1
