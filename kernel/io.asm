; ============================================================================
; io.asm  -  MYDOS カーネル (IO.SYS)
;
; Stage2 に 0x1000:0000 へ読み込まれ、自分自身を 0x0060:0000 へ降ろしてから
; 動き出す。低いところに居座るほどプログラムに渡せる連続領域が広くなる。
;
; ビルド: nasm -f bin -I kernel/inc kernel/io.asm -o io.sys
;         テスト用に画面出力をシリアルへも流す場合は -DSERIAL_CONSOLE を付ける
;
; 対象 CPU:
;   386 以降のリアルモード。ファイル位置とサイズが 32bit なので、
;   16bit レジスタ 2 本で持ち回るより 32bit レジスタを使うほうが
;   桁上がりの取りこぼしが起きにくい。8086 専用にはしていない。
; ============================================================================
        cpu     386
        bits    16
        org     0

%include "dosdef.inc"

; ============================================================================
; 入口 — 自分自身を KERNEL_SEG へ移す
;
; Stage2 は 0x1000:0000 (64KB の位置) に置いてくれる。そこから
; 0x0060:0000 (1.5KB の位置) へ降ろす。転送先が転送元より前にあるので
; 前進コピーで重なりの心配がない。
; ============================================================================
kernel_entry:
        cli
        cld

        ; ここではスタックを一切使わない。
        ;
        ; 呼び出し元 (Stage2) のスタックは 0x7C00 の直下にあり、これから
        ; 書き込む先 (0x600 から) はカーネルが大きくなるとそこへ届く。
        ; ブートドライブ番号を push で預けると、コピーの途中で自分の
        ; イメージに踏み潰されて、でたらめなドライブ番号で起動しようと
        ; することになる。DL は movsb が触らないのでそのまま持ち回る。
        mov     ax, LOAD_SEG
        mov     ds, ax
        mov     ax, KERNEL_SEG
        mov     es, ax
        xor     si, si
        xor     di, di
        mov     cx, kernel_end
        rep     movsb                   ; DL は保たれる

        jmp     KERNEL_SEG:kernel_main

; ============================================================================
kernel_main:
        cli
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, kernel_stack_top
        cld
        sti

        mov     [boot_drive], dl

%ifdef SERIAL_CONSOLE
        call    serial_init
%endif

        mov     si, msg_banner
        call    con_puts

        ; --- 割り込みベクタを立てる ---
        call    install_vectors

        ; --- デバイスドライバの連鎖を立てる ---
        ; ディスクを触る前に済ませておく。ブロックデバイスも連鎖の一員で、
        ; 以降のセクタ入出力はすべてここを通る。
        call    dev_init

        ; --- ディスクバッファ (BUFFERS=) ---
        mov     al, DEFAULT_BUFFERS
        call    buf_init

        ; --- ディスク (BPB の取り込みと FAT の読み込み) ---
        call    disk_init
        jc      .disk_fail

        ; --- ドライブごとのカレントディレクトリ (CDS) ---
        ; 見つかったドライブが LASTDRIVE より多ければそちらに合わせる。
        ; そうしないと検出したドライブに文字が割り当たらない。
        mov     al, DEFAULT_LASTDRIVE
        cmp     al, [num_drives]
        jae     .lastdrive_ok
        mov     al, [num_drives]
.lastdrive_ok:
        call    cds_init

        ; --- ファイルハンドルの土台 ---
        call    sft_init

        ; --- List of Lists の各ポインタを実体に向ける ---
        call    lol_init

        ; --- MCB アリーナはカーネルの直後から 640KB まで ---
        ; カーネルの大きさをパラグラフに切り上げて自分のセグメントに足す。
        ; (kernel_end は再配置可能なラベルなのでアセンブル時には割れない)
        mov     ax, kernel_end
        add     ax, 15
        mov     cl, 4
        shr     ax, cl
        mov     cx, cs
        add     ax, cx
        call    mem_init
        mov     [lol_first_mcb], ax

        ; --- 環境ブロックを DOS 自身の持ち物として確保する ---
        mov     bx, ENV_PARAS
        call    mem_alloc
        jc      .no_mem
        mov     [boot_env_seg], ax
        push    es
        mov     dx, ax
        dec     dx
        mov     es, dx
        mov     word [es:MCB_OWNER], MCB_OWNER_DOS
        pop     es
        mov     ax, [boot_env_seg]
        call    build_env

        ; --- COMMAND.COM を起動する ---
        jmp     start_shell

.disk_fail:
        mov     si, msg_disk_fail
        call    con_puts
        jmp     halt_forever
.no_mem:
        mov     si, msg_no_mem
        call    con_puts
        jmp     halt_forever

halt_forever:
        cli
.loop:
        hlt
        jmp     .loop

; ---------------------------------------------------------------------------
; start_shell - COMMAND.COM を読み込んで制御を渡す
;
; シェルは DOS の一部ではなく、ただの .COM プログラムとして起動する。
; 本物の DOS と同じ構造で、これのおかげでシェルを差し替えられる。
; ---------------------------------------------------------------------------
start_shell:
        cli
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, kernel_stack_top
        cld
        sti

        ; 前のシェルが残していたものを片付ける
        mov     word [cur_psp], 0
        mov     ax, [boot_env_seg]
        mov     [exec_envseg], ax
        mov     dword [exec_psp_cmdsrc], 0

        mov     si, shell_path
        call    prog_load
        jc      .no_shell

        mov     ax, [exec_psp]
        mov     [cur_psp], ax
        mov     [dta_seg], ax
        mov     word [dta_off], PSP_CMDTAIL_LEN ; 既定の DTA は PSP:0080

        ; シェルが終了したらここへ戻ってきて、また読み込み直す
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     word [INT_TERM_ADDR * 4], start_shell
        mov     [INT_TERM_ADDR * 4 + 2], cs
        pop     ds

        mov     es, [cur_psp]
        mov     word [es:PSP_INT22], start_shell
        mov     [es:PSP_INT22 + 2], cs

        cli
        mov     ax, [exec_psp]
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

.no_shell:
        mov     si, msg_no_shell
        call    con_puts
        jmp     halt_forever

; ---------------------------------------------------------------------------
; install_vectors - DOS が持つ割り込みを IVT に登録する
; ---------------------------------------------------------------------------
install_vectors:
        push    ax
        push    bx
        push    es

        xor     ax, ax
        mov     es, ax

        mov     bx, INT_TERMINATE * 4
        mov     word [es:bx], int20_handler
        mov     [es:bx + 2], cs

        mov     bx, INT_DOS * 4
        mov     word [es:bx], int21_handler
        mov     [es:bx + 2], cs

        mov     bx, INT_TERM_ADDR * 4
        mov     word [es:bx], start_shell
        mov     [es:bx + 2], cs

        mov     bx, INT_CTRLC * 4
        mov     word [es:bx], int23_handler
        mov     [es:bx + 2], cs

        mov     bx, INT_CRITERR * 4
        mov     word [es:bx], int24_handler
        mov     [es:bx + 2], cs

        mov     bx, INT_ABSREAD * 4
        mov     word [es:bx], int25_handler
        mov     [es:bx + 2], cs

        mov     bx, INT_ABSWRITE * 4
        mov     word [es:bx], int26_handler
        mov     [es:bx + 2], cs

        mov     bx, INT_TSR * 4
        mov     word [es:bx], int27_handler
        mov     [es:bx + 2], cs

        mov     bx, INT_IDLE * 4
        mov     word [es:bx], int_iret
        mov     [es:bx + 2], cs

        mov     bx, INT_MULTIPLEX * 4
        mov     word [es:bx], int2f_handler
        mov     [es:bx + 2], cs

        pop     es
        pop     bx
        pop     ax
        ret

; ============================================================================
; INT 21h の入口
;
; 呼び出し元のレジスタをすべて u_?? に写し、カーネル専用のスタックへ
; 切り替えてから処理に入る。プログラムのスタックをそのまま使うと、
; 数十バイトしかスタックを用意していない .COM で簡単に破綻する。
; ============================================================================
int21_handler:
        cli
        mov     [cs:u_ax], ax
        mov     [cs:u_ss], ss
        mov     [cs:u_sp], sp

        mov     ax, cs
        mov     ss, ax
        mov     sp, kernel_stack_top

        push    ds
        push    es
        mov     ds, ax
        mov     es, ax
        pop     word [u_es]
        pop     word [u_ds]

        mov     [u_bx], bx
        mov     [u_cx], cx
        mov     [u_dx], dx
        mov     [u_si], si
        mov     [u_di], di
        mov     [u_bp], bp

        mov     byte [ret_cf], 0
        mov     byte [ret_zf], 0
        mov     byte [indos_flag], 1

        ; 呼び出し元が DF=1 のままでも、カーネル内は必ず前進で動かす
        cld
        sti
        jmp     int21_dispatch

; ---------------------------------------------------------------------------
; INT 21h の出口
;
; CF (と AH=06h の ZF) は「呼び出し元のスタックに積まれている FLAGS」を
; 書き換えて返す。iret がそれを復元するので、値がプログラムに届く。
; ---------------------------------------------------------------------------
int21_exit:
        cli
        mov     byte [indos_flag], 0

        mov     al, [ret_cf]
        mov     ah, [ret_zf]

        mov     cx, [u_ss]
        mov     dx, [u_sp]
        mov     ss, cx
        mov     sp, dx
        mov     bp, sp                  ; [bp+4] = 呼び出し元の FLAGS

        test    al, al
        jz      .clear_cf
        or      word [bp + 4], 0x0001
        jmp     .do_zf
.clear_cf:
        and     word [bp + 4], ~0x0001 & 0xFFFF

.do_zf:
        cmp     ah, 1
        je      .set_zf
        cmp     ah, 2
        je      .clear_zf
        jmp     .restore
.set_zf:
        or      word [bp + 4], 0x0040
        jmp     .restore
.clear_zf:
        and     word [bp + 4], ~0x0040 & 0xFFFF

.restore:
        mov     ax, [u_ax]
        mov     bx, [u_bx]
        mov     cx, [u_cx]
        mov     dx, [u_dx]
        mov     si, [u_si]
        mov     di, [u_di]
        mov     bp, [u_bp]
        mov     es, [u_es]
        mov     ds, [u_ds]              ; DS は最後 (これ以降 u_?? は読めない)
        iret

; ============================================================================
; その他の割り込みハンドラ
; ============================================================================

; INT 20h - プログラム終了 (古い作法)
int20_handler:
        cli
        mov     ax, cs
        mov     ds, ax
        mov     ss, ax
        mov     sp, kernel_stack_top
        cld
        sti
        mov     ax, 0x4C00
        jmp     do_terminate

; INT 23h - Ctrl-C の既定のハンドラ。プログラムを打ち切る
int23_handler:
        cli
        mov     ax, cs
        mov     ds, ax
        mov     ss, ax
        mov     sp, kernel_stack_top
        cld
        sti
        mov     ax, 0x4C03
        jmp     do_terminate

; INT 24h - クリティカルエラーの既定のハンドラ
;
; 本物は「中止/再試行/無視」を聞いてくるが、ここでは常に「失敗」を返す。
; プログラム側はエラーコードを受け取って自分で判断できる。
int24_handler:
        mov     al, 3                   ; 3 = fail
        iret

; INT 27h - 常駐終了 (TSR)。常駐部分は残さず普通に終了させる
int27_handler:
        cli
        mov     ax, cs
        mov     ds, ax
        mov     ss, ax
        mov     sp, kernel_stack_top
        cld
        sti
        mov     ax, 0x4C00
        jmp     do_terminate

; INT 2Fh - マルチプレクサ。何も常駐していないので AL=0 を返す
int2f_handler:
        mov     al, 0
        iret

int_iret:
        iret

; ---------------------------------------------------------------------------
; INT 25h / 26h - 絶対セクタ読み書き
;   AL = ドライブ番号, CX = セクタ数, DX = 開始 LBA, DS:BX = バッファ
;   終了時、呼び出し元が積んだ FLAGS はスタックに残したまま返す (DOS の仕様)
; ---------------------------------------------------------------------------
int25_handler:
        push    ax
        push    cx
        push    dx
        push    si
        push    di
        push    es
        push    ds

        push    ds
        pop     es                      ; ES:BX = バッファ
        movzx   si, al                  ; SI = ドライブ番号 (0 = A:)
        movzx   eax, dx                 ; EAX = 開始 LBA
        mov     dx, si                  ; DL = ドライブ番号
        push    ds
        mov     si, cs
        mov     ds, si
        pop     si                      ; SI = 呼び出し元の DS (使わないが退避)
        call    disk_read
        jc      .fail

        pop     ds
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     ax
        xor     ax, ax
        clc
        retf    2                       ; FLAGS を残して返る
.fail:
        pop     ds
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     ax
        mov     ax, 0x0C04              ; 一般的な読み取りエラー
        stc
        retf    2

int26_handler:
        push    ax
        push    cx
        push    dx
        push    si
        push    di
        push    es
        push    ds

        push    ds
        pop     es
        movzx   si, al                  ; SI = ドライブ番号
        movzx   eax, dx                 ; EAX = 開始 LBA
        mov     dx, si                  ; DL = ドライブ番号
        push    ds
        mov     si, cs
        mov     ds, si
        pop     si
        call    disk_write
        jc      .fail

        pop     ds
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     ax
        xor     ax, ax
        clc
        retf    2
.fail:
        pop     ds
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     ax
        mov     ax, 0x0C04
        stc
        retf    2

; ============================================================================
; 各部品
; ============================================================================
%include "util.inc"
%include "con.inc"
%include "time.inc"
%include "disk.inc"
%include "drive.inc"
%include "buffer.inc"
%include "cds.inc"
%include "device.inc"
%include "fat.inc"
%include "mem.inc"
%include "file.inc"
%include "fcb.inc"
%include "dirops.inc"
%include "exec.inc"
%include "int21.inc"

; ============================================================================
; データ
; ============================================================================
msg_banner:     db 13, 10, 'MYDOS Version 1.0', 13, 10
                db 'Copyright (C) 2026', 13, 10, 13, 10, 0
msg_disk_fail:  db 'IO.SYS: disk initialization failed', 13, 10, 0
msg_no_mem:     db 'IO.SYS: not enough memory', 13, 10, 0
msg_no_shell:   db 'Bad or missing command interpreter', 13, 10, 0

shell_path:     db 'A:\COMMAND.COM', 0
boot_env_seg:   dw 0

; --- INT 21h の呼び出し元レジスタ退避領域 ---------------------------------
u_ax:           dw 0
u_bx:           dw 0
u_cx:           dw 0
u_dx:           dw 0
u_si:           dw 0
u_di:           dw 0
u_bp:           dw 0
u_ds:           dw 0
u_es:           dw 0
u_ss:           dw 0
u_sp:           dw 0
ret_cf:         db 0
ret_zf:         db 0                    ; 0=触らない 1=ZF を立てる 2=倒す

; --- バッファ --------------------------------------------------------------
name83:         times 11 db 0           ; 8.3 に畳んだ作業用の名前
dir_cur_lba:    dd 0                    ; dir_buf に載っているセクタの LBA
dir_buf:        times SECTOR_SIZE db 0
sector_buf:     times SECTOR_SIZE db 0
fat_buf:        times FAT_RESIDENT_SECS * SECTOR_SIZE db 0  ; 常駐 FAT (FAT12 用)

; --- カーネルスタック ------------------------------------------------------
; INT 21h の処理中はここを使う。プログラム側のスタックは触らない。
                times 1024 db 0
kernel_stack_top:

kernel_end:
