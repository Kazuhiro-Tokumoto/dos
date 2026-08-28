; ============================================================================
; sda.inc  -  SDA (Swappable Data Area / 入れ替え可能データ領域)
;
; ※ このファイルは kernel_bss の absolute 領域の中で %include される。
;    命令は書けない。場所の確保だけを並べる。
;
; DOS はリエントラントではない。INT 21h の途中で別の処理が INT 21h を呼ぶと、
; 作業用の変数を上書きして壊す。そこで DOS 3.0 以降は「途中で壊れると困る
; 変数」を一箇所に集め、その場所を AH=5D06h で外に教えるようにした。
; これが SDA で、外側のプログラムはここを丸ごと退避・復元することで、
; DOS の呼び出しの途中から別のタスクへ切り替えられる。
;
; 使うのは主にこの 3 種類:
;
;   * Windows のリアルモード (2.x/3.0) と DOS マルチタスカ
;     DOS 呼び出しの最中にタスクを切り替えるため、ここを入れ替える。
;   * ネットワークリダイレクタ (INT 2Fh AH=11h)
;     横取りした呼び出しの結果を、DOS 自身の変数として書き戻す。
;   * TSR
;     InDOS とクリティカルエラーフラグを見て「いま割り込んでいいか」を決める。
;
; いちばん大事なのは、ここに並ぶのが写しではなく本体でなければならない、
; ということ。別の場所に本物を置いて SDA にはコピーを置く作りにすると、
; 外から書き戻された値がカーネルに届かず、退避・復元しているつもりで
; 何も起きない。だから InDOS もカレント PSP も DTA も、カーネル中では
; この中の番地を直接読み書きしている。
;
; --- 先頭の並び (DOS 4.0 以降) ---------------------------------------------
;   +00 BYTE   クリティカルエラーフラグ (INT 24h の中で 1)
;   +01 BYTE   InDOS フラグ (INT 21h の入れ子の深さ)
;   +02 BYTE   クリティカルエラーが起きたドライブ (無ければ FFh)
;   +03 BYTE   直前のエラーの発生場所 (locus)
;   +04 WORD   直前の拡張エラーコード
;   +06 BYTE   推奨される対処 (action)
;   +07 BYTE   エラーの分類 (class)
;   +08 DWORD  エラーに関係する ES:DI
;   +0C DWORD  いまの DTA
;   +10 WORD   いまの PSP
;   +12 WORD   INT 23h をまたぐ SP の控え
;   +14 WORD   直前に終了したプロセスの戻り値
;   +16 BYTE   カレントドライブ
;   +17 BYTE   拡張 BREAK フラグ
;   +18 以降   DOS 内部の作業領域
;
; +18h より先の中身は DOS のバージョンごとに違い、公開もされていない。
; だからここには MYDOS 自身の作業用バッファを置いてある。パス組み立て用の
; バッファも、長い名前を組み立てる途中の状態も、INT 21h 1 回ぶんの
; 使い捨ての値なので、まさに「入れ替えられるべきもの」に当たる。
;
; 大きさは AH=5D06h が CX / DX で返す。本物の MS-DOS 6.0 は CX=05F0h、
; DX=037Ch を返すが、その数字は DOS 自身の作業領域の大きさであって、
; 意味のある定数ではない。呼び出す側は返ってきた値ぶんだけ退避する決まりに
; なっているので、こちらの実際の大きさをそのまま返す。
; ============================================================================

sda_start:
sda_errflag:    resb 1          ; +00
indos_flag:     resb 1          ; +01
sda_errdrive:   resb 1          ; +02
sda_locus:      resb 1          ; +03
last_error:     resw 1          ; +04
sda_action:     resb 1          ; +06
sda_class:      resb 1          ; +07
sda_errptr:     resd 1          ; +08
dta_off:        resw 1          ; +0C
dta_seg:        resw 1          ; +0E
cur_psp:        resw 1          ; +10  いま動いているプロセスの PSP
sda_int23sp:    resw 1          ; +12
sda_retcode:    resw 1          ; +14
cur_drive:      resb 1          ; +16  カレントドライブ (0 = A:)
break_flag:     resb 1          ; +17
sda_work:                       ; ここから先が「DOS 内部の作業領域」
                resb 8          ; +18..+1F  本物が使っている枠に合わせた予備

; --- ここから MYDOS の作業領域 ---------------------------------------------
name83:         resb 11         ; 8.3 に畳んだ作業用の名前
                resb 1          ; 整列
lfn_name:       resb LFN_MAXENT * LFN_CHARS + 2      ; 組み立て中の長い名前
name_long:      resb LFN_MAXLEN + 1                  ; 照合したい名前
find_longname:  resb LFN_MAXLEN + 1                  ; 直前に見つけたものの名前
lfn_states:     resb LFN_SLOTS * LFN_STATE_SIZE      ; 長い名前の検索の状態
find_pattern_long: resb LFN_MAXLEN + 1               ; 検索パターン (畳む前)
path_buf:       resb LFN_PATHMAX
path_buf2:      resb LFN_PATHMAX
sda_end:

SDA_SIZE        equ sda_end - sda_start         ; DOS の中にいる間だけ入れ替える
SDA_ALWAYS      equ sda_work - sda_start        ; 常に入れ替える (先頭 18h)
