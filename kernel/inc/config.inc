; ============================================================================
; config.inc  -  CONFIG.SYS の読み込みと、インストール可能デバイスドライバ
;
; DOS は起動の途中で、ルートディレクトリの CONFIG.SYS を読んで自分の形を
; 決める。バッファの数もファイルハンドルの数もドライブの数も、ここで
; 決まってから初めて確定する。だから順序が要る:
;
;   1. 既定値で一通り組み立てる (これが無いとファイルが読めない)
;   2. CONFIG.SYS を読んで、書いてあることを控える
;   3. 数の指定 (FILES= / BUFFERS= / LASTDRIVE=) を反映して組み直す
;   4. DEVICE= を上から順に読み込む
;   5. INSTALL= を実行する
;   6. SHELL= で指定されたコマンドインタプリタを起動する
;
; 3 で組み直せるのは、この時点でまだ何も開いていないから。CONFIG.SYS
; 自身は 2 の終わりで閉じてある。
;
; インストール可能デバイスドライバ (.SYS) は、ヘッダが先頭に置かれた
; ただのバイナリ。カーネル組み込みのドライバと同じ形をしていて、
; 読み込んだあと INIT (コマンド 0) を一度だけ呼び、ドライバが
; 「ここまでが常駐部分」と答えた位置でメモリを切り詰める。
; そのあと連鎖の先頭 (NUL の直後) に差し込む。名前で探すときは先頭から
; 順に見るので、あとから入れたドライバが組み込みより優先される。
; ANSI.SYS が CON を乗っ取れるのはこの仕組みによる。
; ============================================================================

CONFIG_BUFSIZE  equ 2048        ; CONFIG.SYS の最大サイズ
CONFIG_MAXLINE  equ 128

; ---------------------------------------------------------------------------
; config_process - CONFIG.SYS を読んで反映する
;
; 失敗しても起動は続ける。CONFIG.SYS が無いのは異常ではない。
; ---------------------------------------------------------------------------
config_process:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di

        call    config_read
        jc      .done                   ; 無ければ既定値のまま

        call    config_scan             ; 数の指定を控える
        call    config_apply            ; 控えた数で組み直す
        call    config_devices          ; DEVICE= を順に読み込む
        call    config_installs         ; INSTALL= を実行する

.done:
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; config_read - CONFIG.SYS を丸ごと config_buf に読む
;   出力: CF=1 なら読めなかった。[config_len] に長さ
; ---------------------------------------------------------------------------
config_read:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es

        mov     word [config_len], 0

        mov     si, config_path
        xor     al, al                  ; 読み取り専用
        call    file_open
        jc      .fail
        mov     bx, ax

        push    ds
        pop     es
        mov     di, config_buf
        mov     cx, CONFIG_BUFSIZE
        call    file_read
        pushf
        mov     [config_len], ax
        popf
        pushf
        push    ax
        call    file_close              ; BX はまだハンドル
        pop     ax
        popf
        jc      .fail
        test    ax, ax
        jz      .fail

        ; 終端を置いておく (行の切り出しが楽になる)
        mov     di, config_buf
        add     di, ax
        mov     byte [di], 0

        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.fail:
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

; ---------------------------------------------------------------------------
; config_scan - 数と文字列の指定を控える (DEVICE= と INSTALL= はここでは扱わない)
; ---------------------------------------------------------------------------
config_scan:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di

        mov     si, config_buf
.next_line:
        call    config_next_line        ; cfg_line に 1 行、SI = 次の行の先頭
        jc      .done

        ; 行の中身を見ている間、SI は次の行を指したまま取っておく。
        ; 値の解析で SI を作業用に使うので、そのままだと読む場所を見失う。
        push    si

        mov     di, cfg_line
        call    config_keyword          ; DI = '=' の次、cfg_key に大文字の見出し
        jc      .after

        mov     bx, cfg_key

        ; --- FILES= ---
        mov     dx, k_files
        call    config_keyeq
        jne     .not_files
        mov     si, di
        call    config_number
        mov     [cfg_files], al
        jmp     .after
.not_files:

        ; --- BUFFERS= (「BUFFERS=20,4」の 2 つ目は先読みバッファ数。
        ;               読み飛ばして構わない) ---
        mov     dx, k_buffers
        call    config_keyeq
        jne     .not_buffers
        mov     si, di
        call    config_number
        mov     [cfg_buffers], al
        jmp     .after
.not_buffers:

        ; --- LASTDRIVE= (ドライブ文字で書く) ---
        mov     dx, k_lastdrive
        call    config_keyeq
        jne     .not_lastdrive
        mov     al, [di]
        call    to_upper
        sub     al, 'A'
        jc      .after
        inc     al                      ; 「E: まで」= 5 台
        mov     [cfg_lastdrive], al
        jmp     .after
.not_lastdrive:

        ; --- FCBS=n,m ---
        mov     dx, k_fcbs
        call    config_keyeq
        jne     .not_fcbs
        mov     si, di
        call    config_number
        mov     [cfg_fcbs], al
        jmp     .after
.not_fcbs:

        ; --- STACKS=n,m ---
        mov     dx, k_stacks
        call    config_keyeq
        jne     .not_stacks
        mov     si, di
        call    config_number
        mov     [cfg_stacks], al
        jmp     .after
.not_stacks:

        ; --- BREAK=ON|OFF ---
        mov     dx, k_break
        call    config_keyeq
        jne     .not_break
        mov     byte [cfg_break], 0
        mov     al, [di]
        call    to_upper
        cmp     al, 'O'
        jne     .after
        mov     al, [di + 1]
        call    to_upper
        cmp     al, 'N'
        jne     .after
        mov     byte [cfg_break], 1
        jmp     .after
.not_break:

        ; --- DOS=HIGH,UMB ---
        mov     dx, k_dos
        call    config_keyeq
        jne     .not_dos
        push    di
        mov     si, di
        mov     di, w_high
        call    config_haveword
        jc      .no_high
        mov     byte [cfg_dos_high], 1
.no_high:
        pop     si
        push    si
        mov     di, w_umb
        call    config_haveword
        jc      .no_umb
        mov     byte [cfg_dos_umb], 1
.no_umb:
        pop     di
        jmp     .after
.not_dos:

        ; --- SHELL=パス 引数 ---
        mov     dx, k_shell
        call    config_keyeq
        jne     .not_shell
        mov     si, di
        call    config_shell
        jmp     .after
.not_shell:

.after:
        pop     si
        jmp     .next_line

.done:
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; config_apply - 控えた数を実際に反映する
;
; ここまでで開いているファイルは無いので、テーブルを作り直してよい。
; ---------------------------------------------------------------------------
config_apply:
        push    ax

        ; 書きかけを吐き出してからバッファを組み直す
        call    disk_flush_fat
        mov     al, [cfg_buffers]
        call    buf_init

        ; ドライブの数は LASTDRIVE より少なくできない
        mov     al, [cfg_lastdrive]
        cmp     al, [num_drives]
        jae     .lastdrive_ok
        mov     al, [num_drives]
.lastdrive_ok:
        call    cds_init

        mov     al, [cfg_files]
        call    sft_init

        call    lol_init                ; 位置が変わっていないか結び直す

        pop     ax
        ret

; ---------------------------------------------------------------------------
; config_devices - DEVICE= / DEVICEHIGH= の行を上から順に処理する
; ---------------------------------------------------------------------------
config_devices:
        push    ax
        push    bx
        push    dx
        push    si
        push    di

        mov     si, config_buf
.next_line:
        call    config_next_line
        jc      .done

        push    si
        mov     di, cfg_line
        call    config_keyword
        jc      .skip

        mov     bx, cfg_key
        mov     dx, k_device
        call    config_keyeq
        je      .load
        mov     dx, k_devicehigh
        call    config_keyeq
        jne     .skip
        ; UMB がまだ無いので DEVICEHIGH= は DEVICE= と同じ扱いにする
.load:
        mov     si, di
        call    dev_load
.skip:
        pop     si
        jmp     .next_line
.done:
        pop     di
        pop     si
        pop     dx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; config_installs - INSTALL= の行を実行する
;
; 常駐ソフトを CONFIG.SYS から起こすための指定。COMMAND.COM より先に
; 走るので、環境変数を見ないものに限られる。
; ---------------------------------------------------------------------------
config_installs:
        push    ax
        push    bx
        push    dx
        push    si
        push    di

        mov     si, config_buf
.next_line:
        call    config_next_line
        jc      .done

        push    si
        mov     di, cfg_line
        call    config_keyword
        jc      .skip

        mov     bx, cfg_key
        mov     dx, k_install
        call    config_keyeq
        jne     .skip

        mov     si, di
        call    config_run
.skip:
        pop     si
        jmp     .next_line
.done:
        pop     di
        pop     si
        pop     dx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; config_run - INSTALL= で指定されたプログラムを実行する
;   入力: DS:SI = 「パス 引数」
;
; INT 21h の AH=4Bh を通らずに親役をやる。DOS の親子の約束はこれだけ:
;   * 自分の PSP の +2Eh に SS:SP を入れておく
;   * INT 22h を「子が終わったら来てほしい場所」に向けておく
;     (build_psp が子の PSP にそれを写し取り、終了処理が書き戻して飛ぶ)
; その 2 つを整えて子へジャンプすれば、子が AH=4Ch でも AH=31h (常駐) でも
; ここへ戻ってくる。スタックはそのまま残っているので、戻ったところから
; ふつうに続けられる。
; ---------------------------------------------------------------------------
config_run:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es

        mov     di, cfg_path
        call    config_split            ; cfg_path = パス, cfg_args = 引数

        ; コマンドテイルを組み立てる (長さ + 文字列 + CR)。
        ; DOS のコマンドテイルは区切りの空白から始まる約束。
        mov     di, cfg_tail + 1
        mov     si, cfg_args
        xor     cx, cx
        cmp     byte [si], 0
        je      .copy
        mov     al, ' '
        stosb
        inc     cx
.copy:
        lodsb
        test    al, al
        jz      .copy_done
        stosb
        inc     cx
        cmp     cx, 120
        jb      .copy
.copy_done:
        mov     byte [di], 13
        mov     [cfg_tail], cl

        mov     word [exec_psp_cmdsrc], cfg_tail
        mov     [exec_psp_cmdsrc + 2], cs
        mov     ax, [boot_env_seg]
        mov     [exec_envseg], ax

        ; 子が終わったらここへ戻す。prog_load より先に立てること。
        ; build_psp がこの時点の INT 22h を子の PSP へ写し取る。
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     word [INT_TERM_ADDR * 4], config_run.back
        mov     [INT_TERM_ADDR * 4 + 2], cs
        pop     ds

        mov     si, cfg_path
        call    prog_load
        jc      .fail

        ; INT 21h の入れ子の深さも控える。子の AH=4Ch は戻ってこない
        ; 呼び出しなので、放っておくと深さが 1 段ずれたままになる。
        mov     al, [indos_flag]
        mov     [.level], al

        ; 自分の PSP に、いまのスタックを控える
        cli
        mov     es, [cur_psp]
        mov     [es:PSP_STACK], sp
        mov     [es:PSP_STACK + 2], ss

        mov     ax, [exec_psp]
        mov     [cur_psp], ax

        mov     bx, [exec_stack + 2]
        mov     cx, [exec_stack]
        mov     ss, bx
        mov     sp, cx
        mov     ds, ax
        mov     es, ax
        xor     ax, ax
        xor     bx, bx
        xor     cx, cx
        xor     dx, dx
        xor     si, si
        xor     di, di
        xor     bp, bp
        sti
        jmp     far [cs:exec_entry]

; 子が終わるとここへ来る。SS:SP は控えたときのまま。
.back:
        cli
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     al, [.level]
        mov     [indos_flag], al
        sti
        cld
        jmp     .out

.fail:
        mov     si, msg_install_bad
        call    con_puts
        mov     si, cfg_path
        call    con_puts
        call    config_newline
.out:
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret
.level:  db 0

; ---------------------------------------------------------------------------
; config_newline - 改行を 1 つ出す
; ---------------------------------------------------------------------------
config_newline:
        push    ax
        mov     al, 13
        call    con_out
        mov     al, 10
        call    con_out
        pop     ax
        ret

; ============================================================================
; インストール可能デバイスドライバ
; ============================================================================

; ---------------------------------------------------------------------------
; dev_load - .SYS を読み込んで連鎖に入れる
;   入力: DS:SI = 「パス 引数」
;
; DOS のやり方をそのままなぞる:
;   1. 空きの一番大きいブロックを丸ごと取り、そこへファイルを読む
;   2. 先頭のデバイスヘッダから連鎖を辿り、それぞれに INIT を投げる
;   3. INIT が返した「常駐部分の末尾」までブロックを切り詰める
;   4. ヘッダを NUL の直後に差し込む
;   5. ブロックデバイスなら、返された BPB からドライブを生やす
;
; 3 で切り詰められるようにするため、いったん大きく取ってから縮める。
; RAMDRIVE の類は「ファイルの大きさ」より「使うメモリ」のほうが
; ずっと大きいので、この順序でないと入らない。
; ---------------------------------------------------------------------------
dev_load:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es

        mov     di, cfg_path
        call    config_split            ; cfg_path / cfg_args

        ; --- ファイルを開いて大きさを見る ---
        mov     si, cfg_path
        xor     al, al
        call    file_open
        jc      .no_file
        mov     [.handle], ax
        mov     bx, ax

        mov     al, 2                   ; 末尾へシーク = ファイルサイズ
        xor     cx, cx
        xor     dx, dx
        call    file_seek
        jc      .close_fail
        mov     [.size], ax
        mov     [.size + 2], dx

        mov     bx, [.handle]
        xor     al, al
        xor     cx, cx
        xor     dx, dx
        call    file_seek               ; 先頭に戻す

        ; --- 空きの一番大きいブロックを取る ---
        call    mem_largest
        mov     bx, ax
        test    bx, bx
        jz      .close_fail
        call    mem_alloc
        jc      .close_fail
        mov     [.seg], ax
        mov     [.paras], bx

        ; そのブロックは DOS の持ち物にする (プログラムが終わっても消えない)
        push    es
        mov     dx, ax
        dec     dx
        mov     es, dx
        mov     word [es:MCB_OWNER], MCB_OWNER_DOS
        pop     es

        ; --- ファイルを読み込む ---
        mov     bx, [.handle]
        mov     ecx, [.size]
        mov     dx, [.seg]
        call    file_read_big
        pushf
        mov     bx, [.handle]
        call    file_close
        popf
        jc      .free_fail

        ; --- 連鎖を辿って INIT を投げる ---
        mov     word [.hdr], 0          ; 最初のヘッダはブロックの先頭
        mov     ax, [.seg]
        mov     [.hdr + 2], ax
        mov     word [.end], 0
        mov     ax, [.seg]
        mov     [.end + 2], ax
        mov     byte [.installed], 0

.hdr_loop:
        call    .init_one
        jc      .next_hdr

        ; 次のヘッダの位置を控えておく。dev_install がこのヘッダの
        ; DEV_NEXT を連鎖につなぎ替えてしまうので、その前に読む。
        ; 1 つの .SYS の中の連鎖はオフセットだけで繋がっていて、
        ; セグメントは読み込んだ場所そのもの。FFFFh が終わりの印。
        push    es
        les     di, [.hdr]
        mov     ax, [es:di + DEV_NEXT]
        mov     [.nexthdr], ax
        pop     es

        ; ドライバを連鎖に入れる
        mov     ax, [.hdr]
        mov     dx, [.hdr + 2]
        push    ax
        push    dx
        call    dev_install
        pop     dx
        pop     ax
        inc     byte [.installed]

        ; ブロックデバイスならドライブを生やす
        push    es
        les     di, [.hdr]
        test    word [es:di + DEV_ATTR], DEVA_CHAR
        pop     es
        jnz     .after_install
        call    .add_drives
.after_install:

        ; 次のヘッダへ
        mov     ax, [.nexthdr]
        cmp     ax, 0xFFFF
        je      .chain_done
        mov     [.hdr], ax
        jmp     .hdr_loop
.next_hdr:
        ; INIT が失敗したドライバは入れない。連鎖もそこで打ち切る。
.chain_done:

        cmp     byte [.installed], 0
        je      .free_fail

        ; --- 常駐部分の末尾までブロックを切り詰める ---
        mov     ax, [.end + 2]
        sub     ax, [.seg]              ; セグメントの差 = パラグラフ数
        mov     bx, [.end]
        add     bx, 15
        mov     cl, 4
        shr     bx, cl
        add     bx, ax                  ; BX = 要るパラグラフ数
        test    bx, bx
        jnz     .have_size
        inc     bx
.have_size:
        cmp     bx, [.paras]
        jae     .keep_all               ; 縮まないなら何もしない
        push    es
        mov     es, [.seg]
        call    mem_resize
        pop     es
.keep_all:

        mov     si, msg_dev_ok
        call    con_puts
        mov     si, cfg_path
        call    con_puts
        call    config_newline
        jmp     .out

.no_file:
        mov     si, msg_dev_missing
        jmp     .complain
.close_fail:
        mov     bx, [.handle]
        call    file_close
        mov     si, msg_dev_bad
        jmp     .complain
.free_fail:
        push    es
        mov     es, [.seg]
        call    mem_free
        pop     es
        mov     si, msg_dev_bad
.complain:
        call    con_puts
        mov     si, cfg_path
        call    con_puts
        call    config_newline
.out:
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; --- INIT (コマンド 0) を 1 つのヘッダに投げる ---
;   入力: [.hdr] = デバイスヘッダの far アドレス
;   出力: CF=1 なら INIT が失敗した。成功なら [.end] を更新する
;
; INIT を呼ぶときの約束:
;   +0Eh に「ここまでなら使ってよい」上限を入れて渡し、
;   ドライバはそこに「ここまでが常駐部分」を入れて返す。
;   +12h には CONFIG.SYS の行 (引数の文字列) を渡し、ブロックデバイスは
;   そこに BPB 配列へのポインタを入れて返す。
;   +16h はこのドライバに割り当てられる最初のドライブ番号。
.init_one:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    es

        mov     byte [cs:req_packet + REQ_LENGTH], 26
        mov     byte [cs:req_packet + REQ_UNIT], 0
        mov     byte [cs:req_packet + REQ_CMD], DEVC_INIT
        mov     word [cs:req_packet + REQ_STATUS], 0
        mov     byte [cs:req_packet + REQ_INIT_UNITS], 0

        ; 使ってよい上限 = 取ったブロックの末尾
        mov     ax, [.seg]
        add     ax, [.paras]
        mov     word [cs:req_packet + REQ_INIT_END], 0
        mov     [cs:req_packet + REQ_INIT_END + 2], ax

        ; CONFIG.SYS の行 (引数)
        mov     word [cs:req_packet + REQ_INIT_BPB], cfg_args
        mov     ax, cs
        mov     [cs:req_packet + REQ_INIT_BPB + 2], ax

        mov     al, [num_drives]
        mov     [cs:req_packet + REQ_INIT_DRIVE], al

        les     si, [.hdr]
        call    dev_call
        jc      .init_fail

        ; 常駐部分の末尾を控える (いちばん先まで伸びたものを採る)
        mov     ax, [cs:req_packet + REQ_INIT_END + 2]
        cmp     ax, [.end + 2]
        ja      .take_end
        jb      .keep_end
        mov     ax, [cs:req_packet + REQ_INIT_END]
        cmp     ax, [.end]
        jbe     .keep_end
        mov     [.end], ax
        jmp     .keep_end
.take_end:
        mov     [.end + 2], ax
        mov     ax, [cs:req_packet + REQ_INIT_END]
        mov     [.end], ax
.keep_end:
        pop     es
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.init_fail:
        pop     es
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

; --- ブロックデバイスが申告したユニット数だけドライブを生やす ---
;   INIT が返した BPB 配列 (ユニットごとの BPB への far ポインタが並ぶ) を
;   読んで、ドライブ表を埋める。ここを通ったドライブは INT 13h ではなく、
;   このドライバを呼んで読み書きする。
.add_drives:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es

        movzx   cx, byte [cs:req_packet + REQ_INIT_UNITS]
        jcxz    .ad_done
        xor     dx, dx                  ; DX = ユニット番号
.ad_loop:
        mov     al, [num_drives]
        cmp     al, MAX_DRIVES
        jae     .ad_done

        ; BPB 配列から、このユニットの BPB を取り出す
        push    cx
        push    dx
        les     di, [cs:req_packet + REQ_INIT_BPB]
        mov     si, dx
        shl     si, 2                   ; far ポインタが並ぶので 4 バイト刻み
        add     di, si
        les     si, [es:di]             ; ES:SI = この BPB

        mov     bx, [.hdr]
        mov     cx, [.hdr + 2]
        mov     al, [num_drives]
        pop     dx
        push    dx
        call    drive_add_external
        pop     dx
        pop     cx
        jc      .ad_next

        inc     byte [num_drives]

        ; CDS にも席を用意する (LASTDRIVE が足りなければ広げる)
        push    cx
        push    dx
        mov     al, [num_drives]
        cmp     al, [lol_lastdrive]
        jbe     .ad_cds
        mov     [lol_lastdrive], al
.ad_cds:
        mov     al, [lol_lastdrive]
        call    cds_init
        pop     dx
        pop     cx
.ad_next:
        inc     dx
        loop    .ad_loop
.ad_done:
        call    dpb_relink
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

.handle:    dw 0
.size:      dd 0
.seg:       dw 0
.paras:     dw 0
.hdr:       dd 0
.nexthdr:   dw 0
.end:       dd 0
.installed: db 0

; ============================================================================
; 行の切り出しと語句の判定
; ============================================================================

; ---------------------------------------------------------------------------
; config_next_line - 次の行を cfg_line に切り出す
;   入力: DS:SI = 読む位置
;   出力: CF=0 なら cfg_line に 0 終端の行、SI = 次の行の先頭
;         CF=1 なら終わり
; ---------------------------------------------------------------------------
config_next_line:
        push    ax
        push    cx
        push    di

        cmp     byte [si], 0
        je      .eof

        mov     di, cfg_line
        xor     cx, cx
.copy:
        lodsb
        test    al, al
        jz      .end_line
        cmp     al, 13
        je      .skip_eol
        cmp     al, 10
        je      .end_line
        cmp     cx, CONFIG_MAXLINE - 1
        jae     .copy                   ; 長すぎる行は切り捨てる
        stosb
        inc     cx
        jmp     .copy
.skip_eol:
        cmp     byte [si], 10
        jne     .end_line
        inc     si
.end_line:
        mov     byte [di], 0
        pop     di
        pop     cx
        pop     ax
        clc
        ret
.eof:
        pop     di
        pop     cx
        pop     ax
        stc
        ret

; ---------------------------------------------------------------------------
; config_keyword - 行を「見出し」と「値」に割る
;   入力: cfg_line に行が入っていること
;   出力: CF=0 なら cfg_key に大文字の見出し、DI = 値の先頭
;         CF=1 なら '=' が無い行 (空行やコメント)
; ---------------------------------------------------------------------------
config_keyword:
        push    ax
        push    cx
        push    si

        mov     si, cfg_line
        call    config_skipsp
        mov     di, cfg_key
        xor     cx, cx
.copy:
        mov     al, [si]
        test    al, al
        jz      .no_eq
        cmp     al, '='
        je      .got_eq
        cmp     al, ' '
        je      .got_end
        cmp     al, 9
        je      .got_end
        call    to_upper
        cmp     cx, 15
        jae     .skip_char
        stosb
        inc     cx
.skip_char:
        inc     si
        jmp     .copy
.got_end:
        ; 見出しと '=' の間に空白がある書き方も通す
        call    config_skipsp
        cmp     byte [si], '='
        jne     .no_eq
.got_eq:
        inc     si
        mov     byte [di], 0
        mov     di, si
        push    si
        mov     si, di
        call    config_skipsp
        mov     di, si
        pop     si
        pop     si
        pop     cx
        pop     ax
        clc
        ret
.no_eq:
        mov     byte [di], 0
        pop     si
        pop     cx
        pop     ax
        stc
        ret

; ---------------------------------------------------------------------------
; config_skipsp - DS:SI の空白とタブを飛ばす
; ---------------------------------------------------------------------------
config_skipsp:
        push    ax
.loop:
        mov     al, [si]
        cmp     al, ' '
        je      .skip
        cmp     al, 9
        je      .skip
        pop     ax
        ret
.skip:
        inc     si
        jmp     .loop

; ---------------------------------------------------------------------------
; config_keyeq - 見出しが指定の語かどうか
;   入力: BX = 見出し (cfg_key), DX = 比べる語 (0 終端)
;   出力: ZF=1 なら一致
; ---------------------------------------------------------------------------
config_keyeq:
        push    ax
        push    si
        push    di
        mov     si, bx
        mov     di, dx
.loop:
        mov     al, [si]
        cmp     al, [di]
        jne     .differ
        test    al, al
        jz      .same
        inc     si
        inc     di
        jmp     .loop
.same:
        xor     al, al                  ; ZF=1
        jmp     .out
.differ:
        mov     al, 1
        or      al, al                  ; ZF=0
.out:
        pop     di
        pop     si
        pop     ax
        ret

; ---------------------------------------------------------------------------
; config_number - DS:SI から 10 進数を読む
;   出力: AL = 値 (255 で頭打ち)
; ---------------------------------------------------------------------------
config_number:
        push    bx
        push    cx
        push    dx
        push    si
        call    config_skipsp
        xor     bx, bx
.loop:
        mov     al, [si]
        cmp     al, '0'
        jb      .done
        cmp     al, '9'
        ja      .done
        sub     al, '0'
        movzx   ax, al
        push    ax
        mov     ax, bx
        mov     cx, 10
        mul     cx
        mov     bx, ax
        pop     ax
        add     bx, ax
        cmp     bx, 255
        jbe     .in_range
        mov     bx, 255
.in_range:
        inc     si
        jmp     .loop
.done:
        mov     al, bl
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret

; ---------------------------------------------------------------------------
; config_haveword - 値の中に指定の語があるか (DOS=HIGH,UMB の判定用)
;   入力: DS:SI = 値, DS:DI = 探す語 (大文字, 0 終端)
;   出力: CF=0 なら見つかった
; ---------------------------------------------------------------------------
config_haveword:
        push    ax
        push    bx
        push    si
        push    di
.next:
        call    config_skipsp
        cmp     byte [si], 0
        je      .not_found

        push    si
        push    di
.cmp:
        mov     al, [di]
        test    al, al
        jz      .word_end
        mov     bl, [si]
        call    to_upper_bl
        cmp     bl, al
        jne     .no_match
        inc     si
        inc     di
        jmp     .cmp
.word_end:
        ; 語の切れ目で終わっていること
        mov     al, [si]
        test    al, al
        jz      .found
        cmp     al, ','
        je      .found
        cmp     al, ' '
        je      .found
.no_match:
        pop     di
        pop     si
        ; 次の区切りまで飛ばす
.skip:
        mov     al, [si]
        test    al, al
        jz      .not_found
        inc     si
        cmp     al, ','
        jne     .skip
        jmp     .next
.found:
        pop     di
        pop     si
        pop     di
        pop     si
        pop     bx
        pop     ax
        clc
        ret
.not_found:
        pop     di
        pop     si
        pop     bx
        pop     ax
        stc
        ret

; to_upper の BL 版 (AL を壊さずに使いたい場面用)
to_upper_bl:
        cmp     bl, 'a'
        jb      .done
        cmp     bl, 'z'
        ja      .done
        sub     bl, 'a' - 'A'
.done:
        ret

; ---------------------------------------------------------------------------
; config_split - 「パス 引数」を 2 つに割る
;   入力: DS:SI = 値, DI = パスの置き場 (cfg_path)
;   出力: cfg_path と cfg_args に 0 終端で入る
; ---------------------------------------------------------------------------
config_split:
        push    ax
        push    cx
        push    si
        push    di

        call    config_skipsp
        mov     di, cfg_path
        xor     cx, cx
.path:
        mov     al, [si]
        test    al, al
        jz      .path_done
        cmp     al, ' '
        je      .path_done
        cmp     al, 9
        je      .path_done
        stosb
        inc     si
        inc     cx
        cmp     cx, 78
        jb      .path
.path_done:
        mov     byte [di], 0

        call    config_skipsp
        mov     di, cfg_args
        xor     cx, cx
.args:
        mov     al, [si]
        test    al, al
        jz      .args_done
        stosb
        inc     si
        inc     cx
        cmp     cx, 78
        jb      .args
.args_done:
        mov     byte [di], 0

        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; config_shell - SHELL= の指定を控える
;   入力: DS:SI = 「パス 引数」
; ---------------------------------------------------------------------------
config_shell:
        push    ax
        push    cx
        push    si
        push    di

        mov     di, cfg_path
        call    config_split

        ; パスをシェルの起動に使うところへ写す
        mov     si, cfg_path
        mov     di, shell_path
        mov     cx, 64
.copy:
        lodsb
        stosb
        test    al, al
        jz      .done
        loop    .copy
        mov     byte [di], 0
.done:
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; --- 見出しの語 ------------------------------------------------------------
k_files:        db 'FILES', 0
k_buffers:      db 'BUFFERS', 0
k_lastdrive:    db 'LASTDRIVE', 0
k_fcbs:         db 'FCBS', 0
k_stacks:       db 'STACKS', 0
k_break:        db 'BREAK', 0
k_dos:          db 'DOS', 0
k_shell:        db 'SHELL', 0
k_device:       db 'DEVICE', 0
k_devicehigh:   db 'DEVICEHIGH', 0
k_install:      db 'INSTALL', 0
w_high:         db 'HIGH', 0
w_umb:          db 'UMB', 0

config_path:    db '\CONFIG.SYS', 0

msg_dev_ok:      db 'DEVICE: ', 0
msg_dev_missing: db 'Bad or missing device driver: ', 0
msg_dev_bad:     db 'Device driver failed to install: ', 0
msg_install_bad: db 'Bad or missing INSTALL program: ', 0

; --- CONFIG.SYS で決まる値 (既定値は当時の DOS に合わせてある) -------------
cfg_files:      db SFT_ENTRIES
cfg_buffers:    db DEFAULT_BUFFERS
cfg_lastdrive:  db DEFAULT_LASTDRIVE
cfg_fcbs:       db 4
cfg_stacks:     db 9
cfg_break:      db 0
cfg_dos_high:   db 0
cfg_dos_umb:    db 0

; --- 作業用 ----------------------------------------------------------------
config_len:     dw 0
cfg_line:       times CONFIG_MAXLINE db 0
cfg_key:        times 16 db 0
cfg_path:       times 80 db 0
cfg_args:       times 80 db 0
cfg_tail:       times 130 db 0
config_buf:     times CONFIG_BUFSIZE + 1 db 0
