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
%ifdef INT21_TRACE_ALL
        ; -DINT21_TRACE_ALL で、呼ばれた機能をすべてシリアルに出す。
        ; 実物のプログラムがどこで詰まっているかを追うための道具。
        pusha
        push    ds
        push    cs
        pop     ds
        mov     bx, [u_ax]
        mov     al, bh
        shr     al, 4
        call    trace_hexd
        mov     al, bh
        call    trace_hexd
        mov     al, '/'
        call    trace_putc
        mov     al, bl
        shr     al, 4
        call    trace_hexd
        mov     al, bl
        call    trace_hexd
        mov     al, ' '
        call    trace_putc
        pop     ds
        popa
%endif
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
%ifdef INT21_TRACE
        ; -DINT21_TRACE でビルドすると、実装していない機能が呼ばれるたびに
        ; シリアルへ "!21:AX " を出す。実物のプログラムを走らせて、
        ; 何が足りないのかを洗い出すための道具。
        pusha
        push    ds
        push    cs
        pop     ds
        mov     bx, [u_ax]
        mov     al, '!'
        call    trace_putc
        mov     al, '2'
        call    trace_putc
        mov     al, '1'
        call    trace_putc
        mov     al, ':'
        call    trace_putc
        mov     al, bh
        shr     al, 4
        call    trace_hexd
        mov     al, bh
        call    trace_hexd
        mov     al, bl
        shr     al, 4
        call    trace_hexd
        mov     al, bl
        call    trace_hexd
        mov     al, ' '
        call    trace_putc
        pop     ds
        popa
%endif
        mov     ax, ERR_FUNC
        jmp     int21_err

%if 1
trace_putc:
        push    ax
        push    dx
        mov     ah, al
.wait:
        mov     dx, 0x3FD
        in      al, dx
        test    al, 0x20
        jz      .wait
        mov     dx, 0x3F8
        mov     al, ah
        out     dx, al
        pop     dx
        pop     ax
        ret
trace_hexd:
        push    ax
        and     al, 0x0F
        cmp     al, 10
        jb      .d
        add     al, 'A' - 10 - '0'
.d:     add     al, '0'
        call    trace_putc
        pop     ax
        ret
%endif

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

; --- 31h: 常駐終了 (Terminate and Stay Resident) ---
;   AL = 終了コード, DX = 残すパラグラフ数
f_31:
        mov     al, [u_ax]
        mov     ah, 0x03                ; 終了の種類: 常駐
        mov     dx, [u_dx]
        call    do_tsr                  ; ここへは戻らない

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
;   DL = ドライブ番号 (0 = A:)
;   出力: AL = 論理ドライブの数
;
; 存在しないドライブを指定されてもエラーは返さない (この呼び出しには
; エラーの返し方が無い)。実際に使えるかどうかは、その後の呼び出しで
; 「無効なドライブ」として返る。DOS もそうしている。
; ===========================================================================
f_0E:
        mov     al, [u_dx]
        call    cds_set_drive
        mov     al, [lol_lastdrive]
        mov     [u_ax], al
        jmp     int21_ok

; ===========================================================================
; 19h - カレントドライブ
;   出力: AL = ドライブ番号 (0 = A:)
; ===========================================================================
f_19:
        mov     al, [cur_drive]
        mov     [u_ax], al
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
        ; DL = ドライブ (0 = カレント, 1 = A:)
        mov     al, [u_dx]
        test    al, al
        jnz     .named
        mov     al, [cur_drive]
        inc     al
.named:
        dec     al
        call    drive_select
        jc      .bad
        call    fat_count_free
        mov     [u_bx], ax
        mov     ax, [sec_per_clus]
        mov     [u_ax], ax
        mov     word [u_cx], SECTOR_SIZE
        mov     ax, [max_cluster]
        sub     ax, 2
        mov     [u_dx], ax
        jmp     int21_ok
.bad:
        mov     word [u_ax], 0xFFFF     ; 無効なドライブ
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
        cmp     al, 0x02
        je      .ioctl_read
        cmp     al, 0x03
        je      .ioctl_write
        cmp     al, 0x08
        je      .removable
        cmp     al, 0x09
        je      .is_remote
        cmp     al, 0x0E
        je      .map_get
        cmp     al, 0x0F
        je      .map_set
        mov     ax, ERR_FUNC
        jmp     int21_err

.get_info:
        mov     bx, [u_bx]
        call    jft_get
        jc      .bad_handle
        call    sft_devkind
        cmp     al, SFT_DEV_FILE
        je      .is_file

        ; 文字デバイス: bit7 = デバイス, bit0/1 = 標準入出力, bit4 = 特殊
        mov     word [u_dx], 0x80D3
        jmp     int21_ok
.is_file:
        ; ファイル: bit7=0、下位 6bit がドライブ番号 (0 = A:)
        mov     ax, [si + SFT_DEVINFO]
        and     ax, SFTI_DRIVE
        mov     [u_dx], ax
        jmp     int21_ok

.set_info:
        jmp     int21_ok

.ioctl_read:
        ; AL=02h: 文字デバイスから制御情報を受け取る
        ;   BX = ハンドル, CX = バイト数, DS:DX = バッファ
        ; ドライバの IOCTL 読み取り (コマンド 3) をそのまま呼ぶ。
        ; 当時のドライバは、これで設定値や状態を返していた。
        mov     al, DEVC_IOCTLREAD
        jmp     .ioctl_common
.ioctl_write:
        ; AL=03h: 文字デバイスへ制御情報を送る (コマンド 12)
        mov     al, DEVC_IOCTLWRITE
.ioctl_common:
        push    ax
        mov     bx, [u_bx]
        call    jft_get
        pop     ax
        jc      .bad_handle
        test    word [si + SFT_DEVINFO], SFTI_CHAR
        jz      .not_char

        push    ax
        mov     ax, [si + SFT_DEVPTR + 2]
        mov     [.devseg], ax
        mov     ax, [si + SFT_DEVPTR]
        mov     [.devoff], ax
        pop     ax

        push    es
        mov     es, [.devseg]
        mov     si, [.devoff]
        mov     cx, [u_cx]
        mov     dx, [u_ds]
        mov     di, [u_dx]
        call    dev_char_io
        pop     es
        jc      .ioctl_fail
        mov     [u_ax], ax
        jmp     int21_ok
.ioctl_fail:
        mov     ax, ERR_BAD_DATA
        jmp     int21_err
.not_char:
        mov     ax, ERR_FUNC
        jmp     int21_err
.devseg: dw 0
.devoff: dw 0

.removable:
        ; AL=08h: そのドライブのメディアは交換できるか
        ;   BL = ドライブ (0 = カレント, 1 = A:)
        ;   出力: AX = 0 なら交換可能 (フロッピー), 1 なら固定 (ハードディスク)
        ; INSTALL やセットアッププログラムが「次のディスクを入れてください」と
        ; 言うかどうかをこれで決めていた。
        mov     al, [u_bx]
        test    al, al
        jnz     .rm_named
        mov     al, [cur_drive]
        inc     al
.rm_named:
        dec     al
        push    di
        call    drive_entry
        jc      .rm_bad
        mov     al, [di + DRV_FLAGS]
        pop     di
        test    al, DRVF_REMOVABLE
        jz      .fixed
        mov     word [u_ax], 0
        jmp     int21_ok
.fixed:
        mov     word [u_ax], 1
        jmp     int21_ok
.rm_bad:
        pop     di
        jmp     .bad_drive

.is_remote:
        ; AL=09h: ネットワークドライブか。すべて手元のドライブなので 0。
        mov     word [u_dx], 0
        jmp     int21_ok

.map_get:
        ; AL=0Eh: 論理ドライブの別名を聞く。SUBST も JOIN もしていないので 0。
        mov     word [u_ax], 0
        jmp     int21_ok
.map_set:
        mov     word [u_ax], 0
        jmp     int21_ok

.bad_drive:
        mov     ax, ERR_BAD_DRIVE
        jmp     int21_err
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
        inc     word [si + SFT_REFCNT]
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
        inc     word [si + SFT_REFCNT]
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
        cmp     al, 0
        je      .execute
        cmp     al, 1
        je      .load_only
        cmp     al, 3
        je      .overlay
        mov     ax, ERR_FUNC
        jmp     int21_err

; --- AL=0: 読み込んで実行する ---
.execute:
        call    copy_path
        mov     es, [u_es]
        mov     bx, [u_bx]
        call    do_exec                 ; 成功したらここへは戻らない
        jmp     int21_err

; --- AL=1: 読み込むだけで実行しない ---
;
; PSP まで作ってメモリも確保するが、制御は渡さない。入口とスタックの
; 位置をパラメータブロックに書き戻すので、呼び出し側が好きなときに
; 自分でジャンプできる。デバッガがこれを使う。
.load_only:
        call    copy_path
        mov     es, [u_es]
        mov     bx, [u_bx]
        mov     ax, [es:bx + EPB_ENVSEG]
        mov     [exec_envseg], ax
        mov     eax, [es:bx + EPB_CMDTAIL]
        mov     [exec_psp_cmdsrc], eax
        call    prog_load
        jc      int21_err

        mov     es, [u_es]
        mov     bx, [u_bx]
        mov     ax, [exec_stack]
        mov     [es:bx + EPB_INITSP], ax
        mov     ax, [exec_stack + 2]
        mov     [es:bx + EPB_INITSS], ax
        mov     ax, [exec_entry]
        mov     [es:bx + EPB_INITIP], ax
        mov     ax, [exec_entry + 2]
        mov     [es:bx + EPB_INITCS], ax
        jmp     int21_ok

; --- AL=3: オーバーレイとして読み込む ---
.overlay:
        call    copy_path
        mov     es, [u_es]
        mov     bx, [u_bx]
        mov     ax, [es:bx + EPB_OVL_SEG]
        mov     dx, [es:bx + EPB_OVL_FACTOR]
        call    prog_load_overlay
        jc      int21_err
        jmp     int21_ok

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
        mov     al, [si + SFT_DEVINFO]
        and     al, SFTI_DRIVE
        call    drive_select
        mov     eax, [si + SFT_DIRSEC]
        call    dir_load_lba
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
; 1Bh / 1Ch - ディスクの割り当て情報
;   出力: AL = クラスタあたりセクタ数, CX = セクタあたりバイト数,
;         DX = 総クラスタ数, DS:BX = メディアディスクリプタの位置
;
; AH=36h より古い形式。FAT の識別バイトへのポインタを返すのが特徴で、
; 当時のツールはこれでフロッピーの種類を見分けていた。
; ===========================================================================
f_1B:
        mov     al, [cur_drive]
        jmp     f_1C.select
f_1C:
        ; DL = ドライブ (0 = カレント, 1 = A:)
        mov     al, [u_dx]
        test    al, al
        jnz     .named
        mov     al, [cur_drive]
        inc     al
.named:
        dec     al
.select:
        call    drive_select
        jc      .bad
        mov     ax, [sec_per_clus]
        mov     [u_ax], al
        mov     word [u_cx], SECTOR_SIZE
        mov     ax, [max_cluster]
        sub     ax, 2
        mov     [u_dx], ax
        mov     ax, cs
        mov     [u_ds], ax
        mov     word [u_bx], media_desc
        jmp     int21_ok
.bad:
        mov     byte [u_ax], 0xFF
        jmp     int21_ok

; ===========================================================================
; 26h - PSP を作る
;   DX = 新しい PSP を置くセグメント
;
; 今の PSP を丸ごと写して、親を自分にする。EXEC が使えなかった時代の
; 子プロセス起動の作法で、いまでも自前でプロセスを作るコードが使う。
; ===========================================================================
f_26:
        push    es
        push    ds

        mov     ax, [u_dx]
        mov     es, ax
        mov     ds, [cs:cur_psp]
        xor     si, si
        xor     di, di
        mov     cx, PSP_SIZE
        rep     movsb                   ; 今の PSP をそのまま写す

        push    cs
        pop     ds

        ; 親を今のプロセスにする
        mov     ax, [cur_psp]
        mov     [es:PSP_PARENT], ax

        ; 使えるメモリの上限を MCB から取り直す
        mov     bx, [u_dx]
        dec     bx
        push    es
        mov     es, bx
        mov     cx, [es:MCB_SIZE]
        pop     es
        mov     ax, [u_dx]
        add     ax, cx
        mov     [es:PSP_MEMTOP], ax

        pop     ds
        pop     es
        jmp     int21_ok

; ===========================================================================
; 32h - DPB を取り出す
;   DL = ドライブ (0 = カレント, 1 = A:)
;   出力: AL = 0 なら DS:BX = DPB、AL = FFh なら無効なドライブ
; ===========================================================================
f_32:
        mov     al, [u_dx]
        test    al, al
        jnz     .named
        mov     al, [cur_drive]         ; 0 = カレントドライブ
        inc     al
.named:
        dec     al                      ; 1 = A: なので 0 起算に直す
        call    drive_entry
        jc      .bad
        call    dpb_for_drive
        mov     ax, cs
        mov     [u_ds], ax
        mov     [u_bx], bx
        mov     byte [u_ax], 0
        jmp     int21_ok
.bad:
        mov     byte [u_ax], 0xFF
        jmp     int21_ok

; ===========================================================================
; 37h - スイッチ文字 (オプションの前に付ける記号)
;   AL=0 取得 → DL、AL=1 設定
;
; DOS は内部的に '/' 以外も選べるようになっていた。ここを読んで
; オプションの区切りを決めるプログラムがある。
; ===========================================================================
f_37:
        mov     al, [u_ax]
        test    al, al
        jnz     .set
        mov     al, [switch_char]
        mov     [u_dx], al
        mov     byte [u_ax], 0
        jmp     int21_ok
.set:
        cmp     al, 1
        jne     .bad
        mov     al, [u_dx]
        mov     [switch_char], al
        mov     byte [u_ax], 0
        jmp     int21_ok
.bad:
        mov     byte [u_ax], 0xFF
        jmp     int21_ok

; ===========================================================================
; 5Ah - 一時ファイルを作る
;   DS:DX = '\\' で終わるパス (13 バイトの空きが必要)
;   出力: AX = ハンドル、DS:DX には作られたファイル名が付け足される
; ===========================================================================
f_5A:
        call    copy_path

        ; 名前を作る。時刻を 16 進に開いて重ならないようにする。
        push    si
        mov     di, si
        call    .append_name
        pop     si

        mov     cx, [u_cx]
        call    file_create
        jc      int21_err
        mov     [u_ax], ax

        ; 呼び出し元のバッファにも同じ名前を書き戻す
        push    es
        mov     es, [u_ds]
        mov     di, [u_dx]
        mov     si, path_buf
.copy_back:
        mov     al, [si]
        mov     [es:di], al
        inc     si
        inc     di
        test    al, al
        jnz     .copy_back
        pop     es
        jmp     int21_ok

; path_buf の末尾に一意なファイル名を足す
.append_name:
        push    ax
        push    cx
        push    di
        mov     di, path_buf
        call    .strend
        call    get_dos_time
        mov     dx, ax
        call    get_dos_date
        xor     dx, ax                  ; 日付と時刻を混ぜる
        add     dx, [temp_counter]
        inc     word [temp_counter]
        mov     cx, 4
.hex:
        rol     dx, 4
        mov     al, dl
        and     al, 0x0F
        add     al, '0'
        cmp     al, '9'
        jbe     .digit
        add     al, 'A' - '0' - 10
.digit:
        mov     [di], al
        inc     di
        loop    .hex
        mov     dword [di], '.TMP'
        mov     byte [di + 4], 0
        pop     di
        pop     cx
        pop     ax
        ret

.strend:
        mov     al, [di]
        test    al, al
        jz      .strend_done
        inc     di
        jmp     .strend
.strend_done:
        ret

; ===========================================================================
; 5Bh - 新規ファイルを作る (すでにあれば失敗する)
;
; AH=3Ch との違いは「同名があったら切り詰めずにエラーにする」こと。
; ロックファイルを作るときのように、先に誰かが作っていたかどうかを
; 知りたい場面で使う。
; ===========================================================================
f_5B:
        call    copy_path

        ; 先に存在を確かめる
        push    si
        call    path_resolve
        jc      .no_path
        push    si
        mov     si, name83
        mov     cl, 0
        call    dir_find
        pop     si
        pop     si
        jnc     .exists

        mov     cx, [u_cx]
        call    file_create
        jc      int21_err
        mov     [u_ax], ax
        jmp     int21_ok
.exists:
        mov     ax, ERR_FILE_EXISTS
        jmp     int21_err
.no_path:
        pop     si
        mov     ax, ERR_PATH_NOT_FOUND
        jmp     int21_err

; ===========================================================================
; 5Ch - ファイルの一部をロック / 解除
;
; 同時に動くプロセスが 1 つしかないので、常に成功でよい。呼ばれても
; 失敗しないことが大事で、ここでエラーを返すと「他が使用中」と
; 判断して止まるプログラムがある。
; ===========================================================================
f_5C:
        mov     bx, [u_bx]
        call    jft_get
        jc      .bad
        jmp     int21_ok
.bad:
        mov     ax, ERR_BAD_HANDLE
        jmp     int21_err

; ===========================================================================
; 5Dh / 5Eh / 5Fh - ネットワーク関連 (ネットワークはないので断る)
; ===========================================================================
; ===========================================================================
; 5Dh - DOS 内部サービス
;
; もともとはネットワークリダイレクタ (MS-NET / LAN Manager) のためのもので、
; サーバ側が「クライアントの代わりに DOS を呼ぶ」ための入口が並んでいる。
; その中に 1 つだけ、ネットワークと関係なく広く使われる機能がある。
; ===========================================================================
f_5D:
        mov     al, [u_ax]
        cmp     al, 0x06
        je      .get_sda
        cmp     al, 0x0B
        je      .get_sda_list
        cmp     al, 0x0A
        je      .set_exterr
        mov     ax, ERR_FUNC
        jmp     int21_err

; --- 5D06h: SDA の場所と大きさを返す ---------------------------------------
;   出力: DS:SI = SDA, CX = 中にいる間だけ入れ替える大きさ,
;         DX = 常に入れ替える大きさ
;
; Windows のリアルモードや DOS マルチタスカが、DOS 呼び出しの途中で
; タスクを切り替えるために使う。TSR も、この先頭 2 バイト (クリティカル
; エラーフラグと InDOS) だけを見て「いま入っていいか」を判断する。
.get_sda:
        mov     ax, cs
        mov     [u_ds], ax
        mov     word [u_si], sda_start
        mov     word [u_cx], SDA_SIZE
        mov     word [u_dx], SDA_ALWAYS
        jmp     int21_ok

; --- 5D0Bh: SDA の一覧を返す (DOS 4.0 以降) --------------------------------
;   出力: DS:SI = 一覧
;         +00 WORD  個数
;         +02 DWORD SDA の far ポインタ ┐
;         +06 WORD  常に入れ替える大きさ ├ 個数ぶん繰り返す
;         +08 WORD  中にいる間の大きさ  ┘
;
; DOS を複数の SDA に分けて持つ実装 (DOS 4.0 のマルチタスク版) があるため、
; 一覧という形になっている。MYDOS は 1 つしか持たない。
.get_sda_list:
        mov     ax, cs
        mov     [u_ds], ax
        mov     word [u_si], sda_list
        jmp     int21_ok

; --- 5D0Ah: 次のエラーで返す拡張エラー情報を差し替える ---------------------
;   入力: DS:DX = 11 ワード (AX BX CX DX SI DI DS ES 予約 3 つ)
;
; リダイレクタが「自分が処理した結果のエラー」を DOS に代弁させるための
; もの。DS:DX の先頭ワードが、次に AH=59h で返る値になる。
.set_exterr:
        push    ds
        mov     ds, [cs:u_ds]
        mov     si, [cs:u_dx]
        mov     ax, [si]
        pop     ds
        mov     [last_error], ax
        jmp     int21_ok

f_5E:
f_5F:
        mov     ax, ERR_FUNC
        jmp     int21_err

; ===========================================================================
; 69h - ディスクのシリアル番号とボリュームラベル
;   AL=0 取得 / AL=1 設定、DS:DX = 25 バイトのバッファ
; ===========================================================================
f_69:
        mov     al, [u_ax]
        test    al, al
        jnz     .set

        push    es
        mov     es, [u_ds]
        mov     di, [u_dx]
        mov     word [es:di], 0                 ; 情報レベル
        mov     eax, [volume_serial]
        mov     [es:di + 2], eax
        ; ボリュームラベル 11 バイトと FS 名 8 バイト
        push    si
        push    di
        add     di, 6
        mov     si, vol_label
        mov     cx, 19
        rep     movsb
        pop     di
        pop     si
        pop     es
        mov     byte [u_ax], 0
        jmp     int21_ok
.set:
        mov     byte [u_ax], 0
        jmp     int21_ok

; ===========================================================================
; 6Ah - ファイルの書き出しを確定する (AH=68h と同じ)
; ===========================================================================
f_6A:
f_68:
        mov     bx, [u_bx]
        call    jft_get
        jc      .bad
        call    sft_is_dirty
        jnz     .clean
        call    file_update_dirent
        jc      .io
        call    disk_flush_fat
        jc      .io
.clean:
        jmp     int21_ok
.bad:
        mov     ax, ERR_BAD_HANDLE
        jmp     int21_err
.io:
        mov     ax, ERR_BAD_DATA
        jmp     int21_err

; ===========================================================================
; 6Ch - 拡張オープン
;   BX = モード, CX = 作るときの属性, DX = 動作, DS:SI = パス
;   出力: AX = ハンドル, CX = 1 (開いた) / 2 (作った) / 3 (切り詰めた)
;
; 「あれば開く、なければ作る」を 1 回の呼び出しで書けるのが利点で、
; 後期の DOS 向けに書かれたプログラムはこちらを使う。
; ===========================================================================
f_6C:
        ; 動作の指定は DX、パスは DS:SI にある。copy_path は DS:DX を見るので、
        ; DX を潰す前に動作の指定を控えてから入れ替える。
        mov     ax, [u_dx]
        mov     [.action], ax
        mov     ax, [u_si]
        mov     [u_dx], ax
        call    copy_path

        ; 存在するか
        push    si
        call    path_resolve
        jc      .no_path
        push    si
        mov     si, name83
        mov     cl, 0
        call    dir_find
        pop     si
        pop     si
        jc      .not_exist

        ; --- すでにある ---
        mov     ax, [.action]
        and     ax, 0x000F
        cmp     ax, 1
        je      .just_open
        cmp     ax, 2
        je      .truncate
        mov     ax, ERR_FILE_EXISTS     ; 0 = 失敗させる
        jmp     int21_err

.just_open:
        mov     al, [u_bx]
        and     al, 0x07
        call    file_open
        jc      int21_err
        mov     [u_ax], ax
        mov     word [u_cx], 1
        jmp     int21_ok

.truncate:
        mov     cx, [u_cx]
        call    file_create
        jc      int21_err
        mov     [u_ax], ax
        mov     word [u_cx], 3
        jmp     int21_ok

.not_exist:
        mov     ax, [.action]
        test    ax, 0x00F0
        jz      .no_file                ; 上位 4bit が 0 なら作らない
        mov     cx, [u_cx]
        call    file_create
        jc      int21_err
        mov     [u_ax], ax
        mov     word [u_cx], 2
        jmp     int21_ok

.no_file:
        mov     ax, ERR_FILE_NOT_FOUND
        jmp     int21_err
.no_path:
        pop     si
        mov     ax, ERR_PATH_NOT_FOUND
        jmp     int21_err
.action: dw 0

; ===========================================================================
; 53h - BPB から DPB を組み立てる
;   DS:SI = BPB, ES:BP = 書き込み先の DPB
;
; デバイスドライバがメディアを読み替えたときに DOS へ渡すための変換。
; FORMAT や DISKCOPY の類が、扱おうとしているメディアの諸元を
; DOS の内部形式に直すのにも使う。
; ===========================================================================
f_53:
        push    ds
        push    es
        push    bp

        mov     ds, [cs:u_ds]
        mov     si, [cs:u_si]
        mov     es, [cs:u_es]
        mov     bp, [cs:u_bp]

        ; BPB からそのまま写せるもの
        mov     ax, [si + 0x00]                 ; セクタあたりバイト数
        mov     [es:bp + DPB_SECSIZE], ax
        mov     al, [si + 0x02]                 ; クラスタあたりセクタ数
        mov     ah, al
        dec     al
        mov     [es:bp + DPB_CLUSMASK], al

        ; シフト量 = log2(クラスタあたりセクタ数)
        mov     al, ah
        xor     cl, cl
.shift:
        shr     al, 1
        cmp     al, 1
        jb      .shift_done
        inc     cl
        jmp     .shift
.shift_done:
        mov     [es:bp + DPB_CLUSSHIFT], cl

        mov     ax, [si + 0x03]                 ; 予約セクタ数
        mov     [es:bp + DPB_RESERVED], ax
        mov     al, [si + 0x05]                 ; FAT の数
        mov     [es:bp + DPB_NUMFATS], al
        mov     ax, [si + 0x06]                 ; ルートのエントリ数
        mov     [es:bp + DPB_ROOTENT], ax
        mov     dx, [si + 0x08]                 ; 総セクタ数
        mov     al, [si + 0x0A]                 ; メディアディスクリプタ
        mov     [es:bp + DPB_MEDIA], al
        mov     bx, [si + 0x0B]                 ; FAT のセクタ数
        mov     [es:bp + DPB_SECPERFAT], bx

        ; ルートディレクトリの開始セクタ = 予約 + FAT の総数
        movzx   ax, byte [si + 0x05]
        mul     bx
        add     ax, [si + 0x03]
        mov     [es:bp + DPB_DIRSEC], ax
        mov     cx, ax

        ; データ領域の開始 = ルートの後ろ
        mov     ax, [si + 0x06]
        add     ax, 15
        shr     ax, 4
        add     ax, cx
        mov     [es:bp + DPB_DATASEC], ax

        ; 最大クラスタ番号
        push    dx
        sub     dx, ax
        mov     ax, dx
        pop     dx
        movzx   cx, byte [si + 0x02]
        xor     dx, dx
        div     cx
        inc     ax                              ; クラスタ番号は 2 から
        mov     [es:bp + DPB_MAXCLUS], ax

        mov     byte [es:bp + DPB_ACCESS], 0
        mov     word [es:bp + DPB_FREESRCH], 2
        mov     word [es:bp + DPB_FREECNT], 0xFFFF

        pop     bp
        pop     es
        pop     ds
        jmp     int21_ok

; ===========================================================================
; 55h - 子プロセスの PSP を作る
;   DX = 新しい PSP のセグメント, SI = 使えるメモリの大きさ (パラグラフ)
;
; AH=26h とほぼ同じだが、メモリの上限を呼び出し側が指定できる。
; EXEC が内部で使っているのもこれ。
; ===========================================================================
f_55:
        push    es
        push    ds

        mov     ax, [u_dx]
        mov     es, ax
        mov     ds, [cs:cur_psp]
        xor     si, si
        xor     di, di
        mov     cx, PSP_SIZE
        rep     movsb

        push    cs
        pop     ds

        mov     ax, [cur_psp]
        mov     [es:PSP_PARENT], ax
        mov     ax, [u_dx]
        add     ax, [u_si]
        mov     [es:PSP_MEMTOP], ax

        pop     ds
        pop     es
        jmp     int21_ok

; ===========================================================================
; 60h - パスを正規化する (TRUENAME)
;   DS:SI = 入力のパス, ES:DI = 128 バイトの書き込み先
;
; "..\FOO" や相対パスを "A:\DIR\FOO" の形に開く。当時のツールが
; 「同じファイルを指しているか」を文字列比較で判定するのに使った。
; ===========================================================================
f_60:
        ; 入力をカーネル側へ写す
        push    ds
        push    es
        push    ds
        pop     es
        mov     di, path_buf
        mov     ds, [cs:u_ds]
        mov     si, [cs:u_si]
        mov     cx, 127
.copy_in:
        lodsb
        stosb
        test    al, al
        jz      .copied
        loop    .copy_in
        xor     al, al
        stosb
.copied:
        pop     es
        pop     ds

        call    truename                ; path_buf → path_buf2

        ; 呼び出し元のバッファへ書き戻す
        push    es
        mov     es, [u_es]
        mov     di, [u_di]
        mov     si, path_buf2
.copy_out:
        lodsb
        mov     [es:di], al
        inc     di
        test    al, al
        jnz     .copy_out
        pop     es

        mov     byte [u_ax], 0
        jmp     int21_ok

; ---------------------------------------------------------------------------
; truename - path_buf を正規化して path_buf2 に入れる
;
; ルートからの絶対パスに開き、"." を捨て、".." で 1 つ戻す。
; 常に "A:" で始まり、ルート以外は末尾に区切りを付けない。
; ---------------------------------------------------------------------------
truename:
        push    ax
        push    cx
        push    si
        push    di
        push    es
        push    ds
        pop     es

        mov     si, path_buf
        mov     di, path_buf2

        ; ドライブ名。"X:" が付いていればそれ、無ければカレントドライブ。
        ; ここを 'A' 固定にしていると、C: に置いたプログラムのフルパスが
        ; A: のものとして子に渡る。自分の EXE を開き直す型のプログラム
        ; (DOS エクステンダなど) が見当違いのファイルを掴むことになる。
        mov     al, [cur_drive]
        cmp     byte [si + 1], ':'
        jne     .no_drive
        mov     al, [si]
        call    to_upper
        sub     al, 'A'
        add     si, 2
.no_drive:
        mov     [.drive], al
        add     al, 'A'
        stosb
        mov     al, ':'
        stosb

        ; 絶対パスならルートから、そうでなければカレントディレクトリから
        mov     al, [si]
        cmp     al, '\'
        je      .from_root
        cmp     al, '/'
        je      .from_root

        ; カレントディレクトリを土台にする。カレントドライブとは限らない
        ; ("C:FOO.TXT" のような書き方) ので、作業用の cur_dir_path では
        ; なく、そのドライブの CDS から引く。
        push    si
        push    di
        mov     al, [.drive]
        call    cds_addr                ; DI = そのドライブの CDS
        mov     si, di
        pop     di
        jc      .cur_done               ; 無効なドライブ → ルート扱い
        add     si, CDS_PATH + 3        ; "X:\" の 3 文字を飛ばす
        cmp     byte [si], 0
        je      .cur_done
        mov     al, '\'
        stosb
.cur_copy:
        lodsb
        test    al, al
        jz      .cur_done
        stosb
        jmp     .cur_copy
.cur_done:
        pop     si
        jmp     .walk

.from_root:
        inc     si

.walk:
        cmp     byte [si], 0
        je      .finish

        ; 区切りが続いていたら読み飛ばす
        mov     al, [si]
        cmp     al, '\'
        je      .skip_sep
        cmp     al, '/'
        jne     .component
.skip_sep:
        inc     si
        jmp     .walk

.component:
        ; "." か ".." か
        cmp     byte [si], '.'
        jne     .normal
        cmp     byte [si + 1], '.'
        je      .dotdot
        ; "." — 区切りか終端が続くならその場に留まる
        mov     al, [si + 1]
        test    al, al
        jz      .finish
        cmp     al, '\'
        je      .skip_dot
        cmp     al, '/'
        je      .skip_dot
        jmp     .normal
.skip_dot:
        inc     si
        jmp     .walk

.dotdot:
        ; ".." — 出力の末尾の要素を 1 つ削る
        mov     al, [si + 2]
        test    al, al
        jz      .do_up
        cmp     al, '\'
        je      .do_up
        cmp     al, '/'
        jne     .normal
.do_up:
        add     si, 2
.strip:
        cmp     di, path_buf2 + 2       ; "A:" より前へは戻らない
        jbe     .walk
        dec     di
        cmp     byte [di], '\'
        jne     .strip
        jmp     .walk

.normal:
        ; 要素を 1 つ写す (大文字に畳む)
        mov     al, '\'
        stosb
.copy_comp:
        mov     al, [si]
        test    al, al
        jz      .finish
        cmp     al, '\'
        je      .walk
        cmp     al, '/'
        je      .walk
        inc     si
        call    to_upper
        stosb
        jmp     .copy_comp

.finish:
        ; ルートだけなら "X:\" にする
        cmp     di, path_buf2 + 2
        jne     .terminate
        mov     al, '\'
        stosb
.terminate:
        xor     al, al
        stosb

        pop     es
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret
.drive:  db 0

; ===========================================================================
; 65h - 拡張された国別情報
;   AL = 種類, BX = コードページ, DX = 国番号, CX = バッファの大きさ,
;   ES:DI = 書き込み先
; ===========================================================================
f_65:
        push    es
        mov     es, [u_es]
        mov     di, [u_di]
        mov     cx, [u_cx]
        cmp     cx, 5
        jb      .too_small

        ; AL=02h/04h/05h/06h/07h は「表そのもの」ではなく
        ; 「表への far ポインタ」を返す約束になっている。
        ;
        ;   +00  byte   情報の種類 (AL をそのまま)
        ;   +01  dword  表の位置 (far)
        ;
        ; ここに国別情報を書いてしまうと、呼び出し側はその中身を
        ; アドレスとして読む。SORT は AL=06h で照合順序の表を取り、
        ; 文字の重みをそこから引くので、並びがめちゃくちゃになる。
        mov     al, [u_ax]
        cmp     al, 0x02
        je      .tab_upcase
        cmp     al, 0x04
        je      .tab_fname_upcase
        cmp     al, 0x05
        je      .tab_fname_term
        cmp     al, 0x06
        je      .tab_collate
        cmp     al, 0x07
        je      .tab_dbcs

        mov     [es:di], al                     ; 情報の種類
        mov     word [es:di + 1], 38            ; この後に続く長さ
        mov     word [es:di + 3], 932           ; コードページ (日本語)
        cmp     cx, 41
        jb      .done

        ; 続きは国別情報そのもの
        mov     word [es:di + 5], 0             ; 日付の形式 (0 = 月日年)
        mov     word [es:di + 7], '$ '          ; 通貨記号
        mov     byte [es:di + 9], 0
        mov     byte [es:di + 12], ','          ; 桁区切り
        mov     byte [es:di + 14], '.'          ; 小数点
.done:
        pop     es
        mov     byte [u_ax], 0
        jmp     int21_ok
.too_small:
        pop     es
        mov     ax, ERR_BAD_DATA
        jmp     int21_err

; --- 表への far ポインタを返す下位機能 ---
.tab_upcase:
        mov     bx, upcase_tab
        jmp     .ret_ptr
.tab_fname_upcase:
        mov     bx, upcase_tab
        jmp     .ret_ptr
.tab_fname_term:
        mov     bx, fname_term_tab
        jmp     .ret_ptr
.tab_collate:
        mov     bx, collate_tab
        jmp     .ret_ptr
.tab_dbcs:
        mov     bx, dbcs_tab
.ret_ptr:
        mov     [es:di], al                     ; 情報の種類
        mov     [es:di + 1], bx
        mov     ax, cs
        mov     [es:di + 3], ax
        pop     es
        mov     byte [u_ax], 0
        jmp     int21_ok

; ===========================================================================
; 66h - コードページの取得と設定
;   AL=1 取得 → BX = 使用中, DX = システムの既定
;   AL=2 設定
; ===========================================================================
f_66:
        mov     al, [u_ax]
        cmp     al, 1
        je      .get
        cmp     al, 2
        je      .set
        mov     ax, ERR_FUNC
        jmp     int21_err
.get:
        mov     ax, [code_page]
        mov     [u_bx], ax
        mov     word [u_dx], 932
        jmp     int21_ok
.set:
        mov     ax, [u_bx]
        mov     [code_page], ax
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
        dw f_08, f_09, f_0A, f_0B, f_0C, f_0D, f_0E, f_0F       ; 08-0F
        dw f_10, f_11, f_12, f_13, f_14, f_15, f_16, f_17       ; 10-17
        dw f_nop_ok, f_19, f_1A, f_1B, f_1C, f_bad, f_bad, f_bad ; 18-1F
        dw f_bad, f_21, f_22, f_23, f_24, f_25, f_26, f_27      ; 20-27
        dw f_28, f_29, f_2A, f_2B, f_2C, f_2D, f_2E, f_2F       ; 28-2F
        dw f_30, f_31, f_32, f_33, f_34, f_35, f_36, f_37       ; 30-37
        dw f_38, f_39, f_3A, f_3B, f_3C, f_3D, f_3E, f_3F       ; 38-3F
        dw f_40, f_41, f_42, f_43, f_44, f_45, f_46, f_47       ; 40-47
        dw f_48, f_49, f_4A, f_4B, f_4C, f_4D, f_4E, f_4F       ; 48-4F
        dw f_50, f_51, f_52, f_53, f_54, f_55, f_56, f_57       ; 50-57
        dw f_58, f_59, f_5A, f_5B, f_5C, f_5D, f_5E, f_5F       ; 58-5F
        dw f_60, f_bad, f_62, f_nop_ok, f_nop_ok, f_65, f_66, f_nop_ok ; 60-67
        dw f_68, f_69, f_6A, f_nop_ok, f_6C, f_bad, f_bad, f_bad ; 68-6F
        dw f_bad, f_71, f_bad, f_bad, f_bad, f_bad, f_bad, f_bad ; 70-77
        times (256 - 0x78) dw f_bad

; ===========================================================================
; 71h - 長いファイル名 (LFN)
;
; Windows 95 の DOS が足した窓口。AL に本来の機能番号を入れて呼ぶと、
; 8.3 ではなく長い名前で同じことができる。使えないときは CF=1 で
; AX=7100h を返す約束になっていて、呼ぶ側はそれを見て 8.3 の側に
; 落ちるようにできている。
; ===========================================================================
f_71:
        mov     al, [u_ax]              ; AL = 本来の機能番号

        cmp     al, 0x4E
        je      lfn_find_first
        cmp     al, 0x4F
        je      lfn_find_next
        cmp     al, 0xA1
        je      lfn_find_close
        cmp     al, 0xA0
        je      lfn_vol_info

        ; パス名を取るだけの機能は、そのまま本来の窓口へ回せる。
        ; path_resolve が長い名前を扱えるようになったので、中身は
        ; 何も変えなくてよい。
        cmp     al, 0x39
        je      f_39                    ; ディレクトリを作る
        cmp     al, 0x3A
        je      f_3A                    ; ディレクトリを消す
        cmp     al, 0x3B
        je      f_3B                    ; カレントディレクトリを変える
        cmp     al, 0x41
        je      f_41                    ; ファイルを消す
        cmp     al, 0x56
        je      f_56                    ; 名前を変える
        cmp     al, 0x6C
        je      f_6C                    ; 拡張オープン
        cmp     al, 0x43
        je      f_43                    ; 属性の取得と設定
        cmp     al, 0x47
        je      f_47                    ; カレントディレクトリを得る

        ; 7160h: パスを正規化する
        ;
        ; CL で 0=正規形 / 1=短い名前 / 2=長い名前 を選べることになっている。
        ; MYDOS の truename は 8.3 に畳んだ正規形を返すので、0 と 1 は正しく、
        ; 2 は「長い名前を持つファイルでも短いほうが返る」という違いが残る。
        ;
        ; ここを実装せずに置くと、71A0h (ボリュームの性質) だけが成功する
        ; という中途半端な状態になる。長い名前の窓口があると判断した
        ; プログラムは次に 7160h を呼び、それが「機能が無い」で返ってくると
        ; 混乱する。FreeDOS の FIND が毎回「Cannot change to directory」と
        ; 言うのはこれで、出力そのものは正しいのに苦情だけが出ていた。
        cmp     al, 0x60
        je      f_60

        ; 知らない下位機能。「長い名前の窓口は無い」ではなく
        ; 「その機能は無い」を返す。
        mov     ax, 0x7100
        jmp     int21_err

; --- 71A0h: ボリュームの性質を返す ---
;   出力: BX = 性質のビット, CX = 名前の最大長, DX = パスの最大長
lfn_vol_info:
        mov     word [u_bx], 0x4022     ; 大文字小文字を保つ / 大小を区別しない
        mov     word [u_cx], LFN_MAXLEN
        mov     word [u_dx], LFN_PATHMAX
        mov     word [u_ax], 0
        jmp     int21_ok

; --- 714Eh: 長い名前で検索を始める ---
;   入力: DS:DX = パス, CL = 属性マスク, ES:DI = 結果の置き場所 (318 バイト)
;   出力: AX = 検索の番号
lfn_find_first:
        call    lfn_slot_alloc
        jc      .no_slot
        mov     [lfn_cur_slot], al

        ; パスを控える。
        ;
        ; u_ds / u_dx はカーネル側の変数なので、DS を呼び出し側に
        ; 付け替える *前* に読まなければならない。順番を逆にすると
        ; 呼び出し側のセグメントの同じオフセットを読むことになり、
        ; たいてい 0 が返って PSP の先頭 (CD 20 ...) をパスとして
        ; 写してしまう。
        mov     si, [u_dx]
        mov     ax, [u_ds]
        push    ds
        mov     ds, ax
        push    cs
        pop     es
        mov     di, path_buf
        call    str_copy_z
        pop     ds

        ; DTA をこの検索用の場所に一時的に向ける
        call    lfn_dta_push
        call    lfn_long_on

        mov     si, path_buf
        mov     cl, [u_cx]              ; CL = 属性マスク
        call    find_first
        pushf
        call    lfn_long_off
        call    lfn_dta_pop
        popf
        jc      .none

        call    lfn_store_result
        movzx   ax, byte [lfn_cur_slot]
        mov     [u_ax], ax
        jmp     int21_ok
.none:
        mov     al, [lfn_cur_slot]
        call    lfn_slot_free
        mov     ax, ERR_NO_MORE_FILES
        jmp     int21_err
.no_slot:
        mov     ax, ERR_TOO_MANY_FILES
        jmp     int21_err

; --- 714Fh: 続きを探す ---
;   入力: BX = 検索の番号, ES:DI = 結果の置き場所
lfn_find_next:
        mov     bx, [u_bx]              ; BX = 検索の番号
        cmp     bx, LFN_SLOTS
        jae     .bad
        cmp     byte [lfn_slot_used + bx], 0
        je      .bad
        mov     [lfn_cur_slot], bl

        call    lfn_dta_push
        call    lfn_long_on
        call    find_next
        pushf
        call    lfn_long_off
        call    lfn_dta_pop
        popf
        jc      .none

        call    lfn_store_result
        mov     word [u_ax], 0
        jmp     int21_ok
.none:
        mov     ax, ERR_NO_MORE_FILES
        jmp     int21_err
.bad:
        mov     ax, ERR_BAD_HANDLE
        jmp     int21_err

; --- 71A1h: 検索を終わる ---
lfn_find_close:
        mov     ax, [u_bx]
        cmp     ax, LFN_SLOTS
        jae     .bad
        call    lfn_slot_free
        mov     word [u_ax], 0
        jmp     int21_ok
.bad:
        mov     ax, ERR_BAD_HANDLE
        jmp     int21_err

; --- 検索用の枠を取る / 返す ---
lfn_slot_alloc:
        push    bx
        xor     al, al
.loop:
        movzx   bx, al
        cmp     byte [lfn_slot_used + bx], 0
        je      .found
        inc     al
        cmp     al, LFN_SLOTS
        jb      .loop
        pop     bx
        stc
        ret
.found:
        mov     byte [lfn_slot_used + bx], 1
        pop     bx
        clc
        ret

lfn_slot_free:
        push    bx
        movzx   bx, al
        cmp     bx, LFN_SLOTS
        jae     .out
        mov     byte [lfn_slot_used + bx], 0
.out:
        pop     bx
        ret

; --- 長い名前での照合を入切する ---
lfn_long_on:
        mov     byte [find_long_mode], 1
        ret

lfn_long_off:
        mov     byte [find_long_mode], 0
        ret

; --- DTA を検索用の場所に向ける / 戻す ---
;
; find_first / find_next は DOS の DTA を使う。長い名前の検索は DTA を
; 使わない約束なので、呼んでいるあいだだけカーネル内の場所へ向けておく。
lfn_dta_push:
        push    ax
        push    bx
        mov     ax, [dta_seg]
        mov     [lfn_save_dta_seg], ax
        mov     ax, [dta_off]
        mov     [lfn_save_dta_off], ax

        mov     ax, cs
        mov     [dta_seg], ax
        movzx   bx, byte [lfn_cur_slot]
        mov     ax, LFN_STATE_SIZE
        mul     bl
        add     ax, lfn_states
        mov     [dta_off], ax
        pop     bx
        pop     ax
        ret

lfn_dta_pop:
        push    ax
        mov     ax, [lfn_save_dta_seg]
        mov     [dta_seg], ax
        mov     ax, [lfn_save_dta_off]
        mov     [dta_off], ax
        pop     ax
        ret

; --- 見つかったものを呼び出し側の構造体に書く ---
;
; Win32 の WIN32_FIND_DATA と同じ並び (318 バイト)。時刻の欄は本来
; 1601 年からの 100ns 単位だが、ここには DOS の日付と時刻をそのまま
; 下位 dword に置いている (SI=1 で呼ばれたときの形)。
lfn_store_result:
        push    ax
        push    bx
        push    cx
        push    si
        push    di
        push    es

        mov     es, [u_es]
        mov     di, [u_di]

        ; 検索状態を置いた場所 (カーネル内)
        movzx   bx, byte [lfn_cur_slot]
        mov     ax, LFN_STATE_SIZE
        mul     bl
        add     ax, lfn_states
        mov     si, ax

        ; 属性 (dword)
        movzx   ax, byte [si + FIND_ATTR_FOUND]
        mov     [es:di + 0x00], ax
        mov     word [es:di + 0x02], 0

        ; 作成 / 最終アクセス / 最終更新。DOS は最終更新しか持たない。
        xor     ax, ax
        mov     cx, 8
        push    di
        add     di, 0x04
        rep     stosw                   ; 作成とアクセスを 0 で埋める
        pop     di
        mov     ax, [si + FIND_TIME]
        mov     [es:di + 0x14], ax
        mov     ax, [si + FIND_DATE]
        mov     [es:di + 0x16], ax
        mov     word [es:di + 0x18], 0
        mov     word [es:di + 0x1A], 0

        ; 大きさ (上位, 下位)
        mov     word [es:di + 0x1C], 0
        mov     word [es:di + 0x1E], 0
        mov     ax, [si + FIND_SIZE]
        mov     [es:di + 0x20], ax
        mov     ax, [si + FIND_SIZE + 2]
        mov     [es:di + 0x22], ax
        mov     word [es:di + 0x24], 0
        mov     word [es:di + 0x26], 0
        mov     word [es:di + 0x28], 0
        mov     word [es:di + 0x2A], 0

        ; 長い名前
        push    si
        push    di
        add     di, 0x2C
        mov     si, find_longname
        call    str_copy_z
        pop     di
        pop     si

        ; 短い名前
        push    si
        push    di
        add     di, 0x130
        add     si, FIND_NAME
        call    str_copy_z
        pop     di
        pop     si

        pop     es
        pop     di
        pop     si
        pop     cx
        pop     bx
        pop     ax
        ret

; str_copy_z - DS:SI の ASCIZ を ES:DI へ写す (最大 LFN_PATHMAX)
str_copy_z:
        push    ax
        push    cx
        mov     cx, LFN_PATHMAX
.loop:
        lodsb
        stosb
        test    al, al
        jz      .done
        loop    .loop
        xor     al, al
        stosb
.done:
        pop     cx
        pop     ax
        ret

LFN_SLOTS       equ 4
LFN_STATE_SIZE  equ 64
LFN_PATHMAX     equ 260

lfn_slot_used:      times LFN_SLOTS db 0
lfn_cur_slot:       db 0
lfn_save_dta_seg:   dw 0
lfn_save_dta_off:   dw 0

; --- AH=65h が指し示す表 --------------------------------------------------
;
; どれも「長さ (word) + 中身」という形。DOS の国別情報の一部で、
; 文字の大小や並び順をプログラム側で DOS に合わせるためにある。

; 大文字への変換表 (0x80..0xFF)。ここはコードページ次第だが、
; 変換しない (そのまま) にしておけば少なくとも壊れない。
upcase_tab:
        dw      128
%assign i 0x80
%rep 128
        db      i
%assign i i+1
%endrep

; ファイル名に使えない文字の表。形は DOS が決めている。
fname_term_tab:
        dw      22
        db      1
        db      0x00            ; 使える文字の下限
        db      0xFF            ; 上限
        db      0
        db      0x00            ; 使えない範囲の下限
        db      0x20            ; 上限
        db      2
        db      14              ; 区切り文字の数
        db      '.', '"', '/', '\', '[', ']', ':', '|', '<', '>', '+', '=', ';', ','

; 照合順序の表。文字コードを「並べるときの重み」に写す。
; 小文字を大文字と同じ重みにすることで、SORT の並びが DOS と同じになる。
collate_tab:
        dw      256
%assign i 0
%rep 256
 %if i >= 0x61 && i <= 0x7A
        db      i - 0x20
 %else
        db      i
 %endif
%assign i i+1
%endrep

; 2 バイト文字の先頭バイトの範囲。空 (対応していない)。
dbcs_tab:
        dw      0
        dw      0

; AH=5D0Bh が返す一覧。SDA は 1 つだけ。
sda_list:
                dw      1                       ; 個数
                dw      sda_start               ; far ポインタ (オフセット)
kernel_seg_ptr: dw      0                       ; セグメントは起動時に埋める
                dw      SDA_ALWAYS
                dw      SDA_SIZE

; --- 変数 ------------------------------------------------------------------
verify_flag:    db 0
switch_char:    db '/'
temp_counter:   dw 0
code_page:      dw 932
vol_label:      db 'MYDOS      '        ; 11 バイト
                db 'FAT12   '           ; 8 バイト

