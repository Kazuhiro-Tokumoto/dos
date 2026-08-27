; ============================================================================
; int21.inc  -  INT 21h ディスパッチャと各機能の実装
;
; 呼び出し元のレジスタは io.asm の入口ですべて u_?? に退避されている。
; 各ハンドラは u_?? を読み書きし、最後に int21_ok / int21_err へ飛ぶ。
; 直接レジスタを触らないのは、DOS を呼んだプログラムから見て
; 「変更したレジスタだけが変わっている」状態を作るため。
;
; 分岐は 256 エントリのジャンプテーブルで行う。比較の連鎖にすると
; 機能番号が大きいものほど遅くなり、AH=3Fh/40h のような
; 何度も呼ばれる機能が不利になる。
; ============================================================================

int21_dispatch:
        movzx   bx, byte [u_ax + 1]     ; AH = 機能番号
        shl     bx, 1
        jmp     [int21_table + bx]

; ---------------------------------------------------------------------------
; 共通の出口
; ---------------------------------------------------------------------------
int21_ok:
        mov     byte [ret_cf], 0
        jmp     int21_exit

int21_err:                              ; AX = エラーコード
        mov     [u_ax], ax
        mov     [last_error], ax
        mov     byte [ret_cf], 1
        jmp     int21_exit

f_bad:
        ; 実装していない機能。DOS は「そんな機能はない」を CF=1 で返す
        mov     ax, ERR_FUNC
        jmp     int21_err

; ---------------------------------------------------------------------------
; copy_path - 呼び出し元の DS:DX が指す ASCIIZ をカーネル側へ写す
;   出力: SI = path_buf
;
; パス文字列を扱う関数はすべて DS = カーネル を前提にしている。
; 呼び出し元のセグメントのまま使い回すと、パスを解決している途中で
; カーネルの変数が読めなくなる。
; ---------------------------------------------------------------------------
copy_path:
        push    ax
        push    cx
        push    di
        push    es

        push    ds
        pop     es
        mov     di, path_buf

        push    ds
        mov     ds, [cs:u_ds]
        mov     si, [cs:u_dx]
        mov     cx, 127
.loop:
        lodsb
        stosb
        test    al, al
        jz      .done
        loop    .loop
        xor     al, al
        stosb
.done:
        pop     ds
        mov     si, path_buf

        pop     es
        pop     di
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; copy_path2 - 呼び出し元の ES:DI が指す ASCIIZ を写す (AH=56h の第 2 引数)
;   出力: DI = path_buf2
; ---------------------------------------------------------------------------
copy_path2:
        push    ax
        push    cx
        push    si
        push    es

        push    ds
        pop     es
        mov     di, path_buf2

        push    ds
        mov     ds, [cs:u_es]
        mov     si, [cs:u_di]
        mov     cx, 127
.loop:
        lodsb
        stosb
        test    al, al
        jz      .done
        loop    .loop
        xor     al, al
        stosb
.done:
        pop     ds
        mov     di, path_buf2

        pop     es
        pop     si
        pop     cx
        pop     ax
        ret

; ===========================================================================
; 00h / 4Ch - プログラム終了
; ===========================================================================
f_00:
        mov     ax, 0x4C00
        call    do_terminate            ; ここへは戻らない

f_4C:
        mov     ax, [u_ax]
        call    do_terminate            ; ここへは戻らない

f_31:   ; TSR は常駐部分を残さず、ただの終了として扱う
        mov     ax, [u_ax]
        mov     ah, 0x03
        call    do_terminate

; ===========================================================================
; 01h - キー入力 (エコーあり)
; ===========================================================================
f_01:
        call    con_in
        mov     [u_ax], al
        test    al, al
        jz      .no_echo
        call    con_out
.no_echo:
        jmp     int21_ok

; ===========================================================================
; 02h - 1 文字表示 (DL)
; ===========================================================================
f_02:
        mov     al, [u_dx]
        call    con_out
        mov     [u_ax], al              ; DOS は出した文字を AL に残す
        jmp     int21_ok

; ===========================================================================
; 03h / 04h / 05h - AUX と PRN (実機がないので受け流す)
; ===========================================================================
f_03:
        mov     byte [u_ax], 0x1A       ; EOF を返しておく
        jmp     int21_ok
f_04:
f_05:
        jmp     int21_ok

; ===========================================================================
; 06h - 直接コンソール入出力
;   DL = 0FFh なら入力、それ以外なら出力
; ===========================================================================
f_06:
        mov     al, [u_dx]
        cmp     al, 0xFF
        je      .input
        call    con_out
        mov     [u_ax], al
        jmp     int21_ok
.input:
        ; 入力があれば ZF=0 で文字を返し、なければ ZF=1 で返す。
        ; ret_zf: 1 = 呼び出し元の ZF を立てる, 2 = 倒す
        call    con_status
        jz      .none
        call    con_in
        mov     [u_ax], al
        mov     byte [ret_zf], 2
        jmp     int21_ok
.none:
        mov     byte [u_ax], 0
        mov     byte [ret_zf], 1
        jmp     int21_ok

; ===========================================================================
; 07h / 08h - キー入力 (エコーなし)
; ===========================================================================
f_07:
f_08:
        call    con_in
        mov     [u_ax], al
        jmp     int21_ok

; ===========================================================================
; 09h - '$' で終わる文字列を表示
; ===========================================================================
f_09:
        push    ds
        mov     ds, [cs:u_ds]
        mov     si, [cs:u_dx]
.loop:
        lodsb
        cmp     al, '$'
        je      .done
        call    con_out
        jmp     .loop
.done:
        pop     ds
        mov     byte [u_ax], '$'
        jmp     int21_ok

; ===========================================================================
; 0Ah - 行入力 (DS:DX にバッファ)
; ===========================================================================
f_0A:
        mov     es, [u_ds]
        mov     di, [u_dx]
        call    con_readline
        jmp     int21_ok

; ===========================================================================
; 0Bh - キーが押されているか
; ===========================================================================
f_0B:
        call    con_status
        jz      .none
        mov     byte [u_ax], 0xFF
        jmp     int21_ok
.none:
        mov     byte [u_ax], 0
        jmp     int21_ok

; ===========================================================================
; 0Ch - 入力バッファを捨ててから AL の機能を実行
; ===========================================================================
f_0C:
        ; 溜まっているキーを全部読み捨てる
.flush:
        call    con_status
        jz      .flushed
        call    con_in
        jmp     .flush
.flushed:
        mov     al, [u_ax]              ; AL = 続けて実行する機能番号
        cmp     al, 0x01
        je      f_01
        cmp     al, 0x06
        je      f_06
        cmp     al, 0x07
        je      f_07
        cmp     al, 0x08
        je      f_08
        cmp     al, 0x0A
        je      f_0A
        jmp     int21_ok

; ===========================================================================
; 0Dh - ディスクリセット (書きかけを吐き出す)
; ===========================================================================
f_0D:
        call    disk_flush_fat
        jmp     int21_ok

; ===========================================================================
; 0Eh - ドライブ選択 → ドライブ数を返す
; ===========================================================================
f_0E:
        mov     byte [u_ax], 1
        jmp     int21_ok

; ===========================================================================
; 19h - カレントドライブ (A: 固定)
; ===========================================================================
f_19:
        mov     byte [u_ax], 0
        jmp     int21_ok

; ===========================================================================
; 1Ah - DTA を設定
; ===========================================================================
f_1A:
        mov     ax, [u_ds]
        mov     [dta_seg], ax
        mov     ax, [u_dx]
        mov     [dta_off], ax
        jmp     int21_ok

; ===========================================================================
; 2Fh - DTA を取得 (ES:BX)
; ===========================================================================
f_2F:
        mov     ax, [dta_seg]
        mov     [u_es], ax
        mov     ax, [dta_off]
        mov     [u_bx], ax
        jmp     int21_ok

; ===========================================================================
; 25h - 割り込みベクタを設定 (AL = 番号, DS:DX = ハンドラ)
; ===========================================================================
f_25:
        movzx   bx, byte [u_ax]
        shl     bx, 2
        push    es
        xor     ax, ax
        mov     es, ax
        mov     ax, [u_dx]
        mov     [es:bx], ax
        mov     ax, [u_ds]
        mov     [es:bx + 2], ax
        pop     es
        jmp     int21_ok

; ===========================================================================
; 35h - 割り込みベクタを取得 (AL = 番号 → ES:BX)
; ===========================================================================
f_35:
        movzx   bx, byte [u_ax]
        shl     bx, 2
        push    es
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:bx]
        mov     [u_bx], ax
        mov     ax, [es:bx + 2]
        mov     [u_es], ax
        pop     es
        jmp     int21_ok

; ===========================================================================
; 2Ah - 日付の取得 (CX=年 DH=月 DL=日 AL=曜日)
; ===========================================================================
f_2A:
        call    rtc_read
        mov     ax, [rtc_year]
        mov     [u_cx], ax
        mov     al, [rtc_day]
        mov     [u_dx], al
        mov     al, [rtc_month]
        mov     [u_dx + 1], al
        call    day_of_week
        mov     [u_ax], al
        jmp     int21_ok

; ===========================================================================
; 2Bh - 日付の設定 (RTC は触らないので「できない」を返す)
; ===========================================================================
f_2B:
        mov     byte [u_ax], 0xFF
        jmp     int21_ok

; ===========================================================================
; 2Ch - 時刻の取得 (CH=時 CL=分 DH=秒 DL=1/100秒)
; ===========================================================================
f_2C:
        call    rtc_read
        mov     al, [rtc_min]
        mov     [u_cx], al
        mov     al, [rtc_hour]
        mov     [u_cx + 1], al
        mov     byte [u_dx], 0
        mov     al, [rtc_sec]
        mov     [u_dx + 1], al
        jmp     int21_ok

f_2D:
        mov     byte [u_ax], 0xFF
        jmp     int21_ok

; ===========================================================================
; 2Eh / 54h - ベリファイフラグ
; ===========================================================================
f_2E:
        mov     al, [u_ax]
        mov     [verify_flag], al
        jmp     int21_ok
f_54:
        mov     al, [verify_flag]
        mov     [u_ax], al
        jmp     int21_ok

; ===========================================================================
; 30h - DOS のバージョン
; ===========================================================================
f_30:
        mov     byte [u_ax], DOS_VER_MAJOR
        mov     byte [u_ax + 1], DOS_VER_MINOR
        mov     word [u_bx], 0xFF00     ; BH = OEM 番号 (FFh = 汎用)
        mov     word [u_cx], 0
        jmp     int21_ok

; ===========================================================================
; 33h - Ctrl-Break のチェック有無
; ===========================================================================
f_33:
        mov     al, [u_ax]
        test    al, al
        jz      .get
        cmp     al, 1
        je      .set
        cmp     al, 5
        je      .bootdrv
        jmp     int21_ok
.get:
        mov     al, [break_flag]
        mov     [u_dx], al
        jmp     int21_ok
.set:
        mov     al, [u_dx]
        mov     [break_flag], al
        jmp     int21_ok
.bootdrv:
        mov     byte [u_dx], 1          ; 起動ドライブ = A:
        jmp     int21_ok

; ===========================================================================
; 34h - InDOS フラグの位置 (ES:BX)
;
; 常駐ソフトが「いま DOS の中か」を判定するために読む。値そのものより
; 「読める場所がある」ことが大事。
; ===========================================================================
f_34:
        mov     ax, cs
        mov     [u_es], ax
        mov     word [u_bx], indos_flag
        jmp     int21_ok

; ===========================================================================
; 36h - 空きディスク容量
;   出力: AX = クラスタあたりセクタ数, BX = 空きクラスタ数,
;         CX = セクタあたりバイト数, DX = 総クラスタ数
; ===========================================================================
f_36:
        call    fat_count_free
        mov     [u_bx], ax
        mov     ax, [sec_per_clus]
        mov     [u_ax], ax
        mov     word [u_cx], SECTOR_SIZE
        mov     ax, [max_cluster]
        sub     ax, 2
        mov     [u_dx], ax
        jmp     int21_ok

; ===========================================================================
; 38h - 国別情報 (日本を名乗るが中身は最小限)
; ===========================================================================
f_38:
        mov     word [u_bx], 81         ; 日本の国番号
        jmp     int21_ok

; ===========================================================================
; 39h / 3Ah / 3Bh - ディレクトリの作成・削除・移動
; ===========================================================================
f_39:
        call    copy_path
        call    dir_mkdir
        jc      int21_err
        jmp     int21_ok

f_3A:
        call    copy_path
        call    dir_rmdir
        jc      int21_err
        jmp     int21_ok

f_3B:
        call    copy_path
        call    dir_chdir
        jc      int21_err
        jmp     int21_ok

; ===========================================================================
; 3Ch - ファイル作成 / 3Dh - ファイルを開く
; ===========================================================================
f_3C:
        call    copy_path
        mov     cx, [u_cx]
        call    file_create
        jc      int21_err
        mov     [u_ax], ax
        jmp     int21_ok

f_3D:
        call    copy_path
        mov     al, [u_ax]
        and     al, 0x07                ; 上位ビットは共有モード。今は無視する
        call    file_open
        jc      int21_err
        mov     [u_ax], ax
        jmp     int21_ok

; ===========================================================================
; 3Eh - 閉じる / 3Fh - 読む / 40h - 書く
; ===========================================================================
f_3E:
        mov     bx, [u_bx]
        call    file_close
        jc      int21_err
        jmp     int21_ok

f_3F:
        mov     bx, [u_bx]
        mov     cx, [u_cx]
        mov     es, [u_ds]
        mov     di, [u_dx]
        call    file_read
        jc      int21_err
        mov     [u_ax], ax
        jmp     int21_ok

f_40:
        mov     bx, [u_bx]
        mov     cx, [u_cx]
        mov     es, [u_ds]
        mov     di, [u_dx]
        call    file_write
        jc      int21_err
        mov     [u_ax], ax
        jmp     int21_ok

; ===========================================================================
; 41h - ファイルを消す
; ===========================================================================
f_41:
        call    copy_path
        call    file_delete
        jc      int21_err
        jmp     int21_ok

; ===========================================================================
; 42h - 読み書き位置の移動
; ===========================================================================
f_42:
        mov     bx, [u_bx]
        mov     al, [u_ax]
        mov     cx, [u_cx]
        mov     dx, [u_dx]
        call    file_seek
        jc      int21_err
        mov     [u_ax], ax
        mov     [u_dx], dx
        jmp     int21_ok

; ===========================================================================
; 43h - 属性の取得・設定
; ===========================================================================
f_43:
        call    copy_path
        mov     al, [u_ax]
        mov     cx, [u_cx]
        call    get_set_attr
        jc      int21_err
        mov     [u_cx], cx
        jmp     int21_ok

; ===========================================================================
; 44h - IOCTL
;
; AL=00h の「デバイス情報の取得」だけは実装しておく必要がある。
; 当時のプログラムは、これで標準出力が画面かファイルかを判断して
; 「リダイレクトされているなら色を付けない」といった分岐をする。
; ===========================================================================
f_44:
        mov     al, [u_ax]
        test    al, al
        jz      .get_info
        cmp     al, 1
        je      .set_info
        mov     ax, ERR_FUNC
        jmp     int21_err

.get_info:
        mov     bx, [u_bx]
        call    jft_get
        jc      .bad_handle
        cmp     byte [si + SFT_DEVICE], SFT_DEV_FILE
        je      .is_file

        ; 文字デバイス: bit7 = デバイス, bit0/1 = 標準入出力, bit4 = 特殊
        mov     word [u_dx], 0x80D3
        jmp     int21_ok
.is_file:
        ; ファイル: bit7=0、下位 6bit がドライブ番号 (0 = A:)
        mov     word [u_dx], 0x0000
        jmp     int21_ok

.set_info:
        jmp     int21_ok
.bad_handle:
        mov     ax, ERR_BAD_HANDLE
        jmp     int21_err

; ===========================================================================
; 45h - ハンドルの複製 / 46h - ハンドルの付け替え
;
; リダイレクトはこの 2 つで実現される:
;   AH=45h で標準出力を控え、AH=46h で標準出力をファイルに向け、
;   子プロセスを起動し、終わったら控えを AH=46h で書き戻す。
; ===========================================================================
f_45:
        mov     bx, [u_bx]
        call    jft_get
        jc      .bad
        mov     [.sftno], al
        push    ax
        call    jft_alloc
        pop     ax
        jc      .too_many
        call    jft_set
        inc     byte [si + SFT_REFCNT]
        mov     [u_ax], bx
        jmp     int21_ok
.bad:
        mov     ax, ERR_BAD_HANDLE
        jmp     int21_err
.too_many:
        mov     ax, ERR_TOO_MANY_FILES
        jmp     int21_err
.sftno: db 0

f_46:
        ; BX の指す先を CX にも向ける (CX が開いていたら先に閉じる)
        mov     bx, [u_bx]
        call    jft_get
        jc      f_45.bad
        mov     [.srcsft], al

        mov     bx, [u_cx]
        cmp     bx, JFT_ENTRIES
        jae     f_45.bad
        call    jft_get
        jc      .not_open
        push    bx
        call    file_close              ; 使い回す前に閉じる
        pop     bx
.not_open:
        mov     al, [.srcsft]
        call    jft_set
        call    sft_addr
        inc     byte [si + SFT_REFCNT]
        jmp     int21_ok
.srcsft: db 0

; ===========================================================================
; 47h - カレントディレクトリの取得 (DS:SI に書く)
; ===========================================================================
f_47:
        mov     es, [u_ds]
        mov     di, [u_si]
        call    get_cwd
        mov     byte [u_ax], 0
        jmp     int21_ok

; ===========================================================================
; 48h / 49h / 4Ah - メモリの確保・解放・サイズ変更
; ===========================================================================
f_48:
        mov     bx, [u_bx]
        call    mem_alloc
        jc      .fail
        ; 確保したブロックの持ち主を今のプロセスにする
        push    es
        mov     dx, ax
        dec     dx
        mov     es, dx
        mov     dx, [cur_psp]
        mov     [es:MCB_OWNER], dx
        pop     es
        mov     [u_ax], ax
        jmp     int21_ok
.fail:
        mov     [u_bx], bx              ; 確保できる最大サイズを教える
        jmp     int21_err

f_49:
        mov     es, [u_es]
        call    mem_free
        jc      int21_err
        jmp     int21_ok

f_4A:
        mov     es, [u_es]
        mov     bx, [u_bx]
        call    mem_resize
        jc      .fail
        jmp     int21_ok
.fail:
        mov     [u_bx], bx
        jmp     int21_err

; ===========================================================================
; 4Bh - プログラムの起動 (EXEC)
; ===========================================================================
f_4B:
        mov     al, [u_ax]
        test    al, al
        jnz     .not_supported          ; AL=1 (ロードのみ) と AL=3 (オーバーレイ) は未対応

        call    copy_path
        mov     es, [u_es]
        mov     bx, [u_bx]
        call    do_exec                 ; 成功したらここへは戻らない
        jmp     int21_err

.not_supported:
        mov     ax, ERR_FUNC
        jmp     int21_err

; ===========================================================================
; 4Dh - 直前のプロセスの終了コード
; ===========================================================================
f_4D:
        mov     ax, [last_exit_code]
        mov     [u_ax], ax
        mov     word [last_exit_code], 0        ; 読んだら消えるのが本物の挙動
        jmp     int21_ok

; ===========================================================================
; 4Eh / 4Fh - ファイル検索
; ===========================================================================
f_4E:
        call    copy_path
        mov     cx, [u_cx]
        call    find_first
        jc      int21_err
        jmp     int21_ok

f_4F:
        call    find_next
        jc      int21_err
        jmp     int21_ok

; ===========================================================================
; 50h / 51h / 62h - カレント PSP
; ===========================================================================
f_50:
        mov     ax, [u_bx]
        mov     [cur_psp], ax
        jmp     int21_ok

f_51:
f_62:
        mov     ax, [cur_psp]
        mov     [u_bx], ax
        jmp     int21_ok

; ===========================================================================
; 52h - DOS の内部データ (List of Lists)
;
; ES:BX の 2 バイト手前に最初の MCB のセグメントが入っている、というのが
; 昔からの約束。MEM やメモリマネージャの類がここから連鎖を辿る。
; ===========================================================================
f_52:
        mov     ax, cs
        mov     [u_es], ax
        mov     word [u_bx], dos_lol
        jmp     int21_ok

; ===========================================================================
; 56h - 名前の変更
; ===========================================================================
f_56:
        call    copy_path
        call    copy_path2
        mov     si, path_buf
        mov     di, path_buf2
        call    dir_rename
        jc      int21_err
        jmp     int21_ok

; ===========================================================================
; 57h - ファイルの日時
; ===========================================================================
f_57:
        mov     bx, [u_bx]
        call    jft_get
        jc      .bad
        mov     al, [u_ax]
        test    al, al
        jnz     .set
        ; 取得: SFT にはディレクトリエントリの日時を持っていないので読み直す
        push    es
        push    ds
        pop     es
        mov     ax, [si + SFT_DIRSEC]
        mov     [dir_cur_lba], ax
        push    bx
        mov     bx, dir_buf
        mov     cx, 1
        call    disk_read
        pop     bx
        pop     es
        jc      .bad

        movzx   ax, byte [si + SFT_DIRENT]
        mov     cl, DIR_ENTSIZE
        mul     cl
        mov     di, dir_buf
        add     di, ax
        mov     ax, [di + DIR_TIME]
        mov     [u_cx], ax
        mov     ax, [di + DIR_DATE]
        mov     [u_dx], ax
        jmp     int21_ok
.set:
        jmp     int21_ok                ; 設定は受け付けたことにする
.bad:
        mov     ax, ERR_BAD_HANDLE
        jmp     int21_err

; ===========================================================================
; 58h - メモリ確保の戦略
; ===========================================================================
f_58:
        mov     al, [u_ax]
        test    al, al
        jnz     .set
        movzx   ax, byte [alloc_strategy]
        mov     [u_ax], ax
        jmp     int21_ok
.set:
        mov     al, [u_bx]
        cmp     al, 2
        ja      .bad
        mov     [alloc_strategy], al
        jmp     int21_ok
.bad:
        mov     ax, ERR_FUNC
        jmp     int21_err

; ===========================================================================
; 59h - 直前のエラーの詳細
; ===========================================================================
f_59:
        mov     ax, [last_error]
        mov     [u_ax], ax
        mov     word [u_bx], 0x0D01     ; BH = 分類 (不明), BL = 対処 (再試行)
        mov     byte [u_cx + 1], 0x01   ; CH = 発生場所 (不明)
        jmp     int21_ok

; ===========================================================================
; 実装していないが、呼ばれても困らないように成功を返しておくもの
; ===========================================================================
f_nop_ok:
        jmp     int21_ok

; ---------------------------------------------------------------------------
; ジャンプテーブル (256 エントリ)
; ---------------------------------------------------------------------------
int21_table:
        dw f_00, f_01, f_02, f_03, f_04, f_05, f_06, f_07       ; 00-07
        dw f_08, f_09, f_0A, f_0B, f_0C, f_0D, f_0E, f_bad      ; 08-0F
        dw f_bad, f_bad, f_bad, f_bad, f_bad, f_bad, f_bad, f_bad ; 10-17
        dw f_nop_ok, f_19, f_1A, f_bad, f_bad, f_bad, f_bad, f_bad ; 18-1F
        dw f_bad, f_bad, f_bad, f_bad, f_bad, f_25, f_bad, f_bad ; 20-27
        dw f_bad, f_bad, f_2A, f_2B, f_2C, f_2D, f_2E, f_2F     ; 28-2F
        dw f_30, f_31, f_bad, f_33, f_34, f_35, f_36, f_bad     ; 30-37
        dw f_38, f_39, f_3A, f_3B, f_3C, f_3D, f_3E, f_3F       ; 38-3F
        dw f_40, f_41, f_42, f_43, f_44, f_45, f_46, f_47       ; 40-47
        dw f_48, f_49, f_4A, f_4B, f_4C, f_4D, f_4E, f_4F       ; 48-4F
        dw f_50, f_51, f_52, f_bad, f_54, f_bad, f_56, f_57     ; 50-57
        dw f_58, f_59, f_bad, f_bad, f_bad, f_bad, f_bad, f_bad ; 58-5F
        dw f_bad, f_bad, f_62, f_nop_ok, f_bad, f_bad, f_bad, f_nop_ok ; 60-67
        dw f_nop_ok, f_bad, f_bad, f_bad, f_bad, f_bad, f_bad, f_bad ; 68-6F
        times (256 - 0x70) dw f_bad

; --- 変数 ------------------------------------------------------------------
verify_flag:    db 0
break_flag:     db 0
indos_flag:     db 0
last_error:     dw 0
path_buf:       times 128 db 0
path_buf2:      times 128 db 0

; --- List of Lists (AH=52h が返す構造体) -----------------------------------
; 手前 2 バイトに最初の MCB のセグメントを置くのが約束。
                dw 0                    ; -4: 予約
lol_first_mcb:  dw 0                    ; -2: 最初の MCB のセグメント
dos_lol:
                dd 0                    ; +0: 最初の DPB
                dd 0                    ; +4: 最初の SFT
                dd 0                    ; +8: CLOCK$ デバイス
                dd 0                    ; +C: CON デバイス
                dw SECTOR_SIZE          ; +10: 最大セクタサイズ
                times 16 db 0
