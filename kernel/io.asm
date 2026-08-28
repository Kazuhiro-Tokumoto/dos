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
; --- 先頭 16 バイトは HMA のための余白 ---------------------------------------
;
; HMA (High Memory Area) は FFFF:0010 から始まる。1MB ちょうどを指す
; セグメント:オフセットの組が FFFF:0010 しか無いので、そこへ載せるものは
; 「オフセット 10h から始まる」形でなければならない。
;
; そこでカーネル本体をオフセット 10h から置き、手前の 16 バイトは
; Stage2 が飛んでくる先として使う。DOS=HIGH のときは、この 16 バイトを
; 除いた本体だけを FFFF:0010 へ写せば、オフセットが 1 バイトもずれない。
                jmp     kernel_entry
                times 16 - ($ - $$) db 0

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
        mov     cx, kernel_bss          ; 実体のある部分だけ写せばよい
        rep     movsb                   ; DL は保たれる

        jmp     KERNEL_SEG:kernel_main

; ============================================================================
kernel_main:
        cli
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, boot_stack_top
        cld

        ; ファイルに入っていない領域を 0 で埋める。開いているファイルの表の
        ; ように「0 = 未使用」で始まらないと困るものがここにある。
        push    di
        push    cx
        mov     di, kernel_bss
        mov     cx, kernel_end - kernel_bss
        xor     al, al
        rep     stosb
        pop     cx
        pop     di

        ; SDA のうち 0 が正しくない項目だけ入れ直す。
        mov     byte [sda_errdrive], 0xFF       ; まだクリティカルエラー無し
        mov     word [dta_off], PSP_CMDTAIL_LEN ; 既定の DTA は PSP:0080
        mov     ax, cs
        mov     [kernel_seg_ptr], ax            ; AH=5D0Bh が返す far ポインタ
        sti

        mov     [boot_drive], dl

%ifdef SERIAL_CONSOLE
        call    serial_init
%endif

        mov     si, msg_banner
        call    con_puts

        ; --- 割り込みベクタを立てる ---
        call    install_vectors

        ; --- A20 と拡張メモリを調べる ---
        ; ここで見ておかないと DOS=HIGH の判断ができない。A20 は
        ; 要求されるまで閉じたままにする (当時の作法)。
        call    xms_init

        ; --- DOS の内部テーブルを組み立てる ---
        call    dos_init_tables
        jc      .disk_fail

        ; --- カーネル自身の PSP ---
        ; ファイルを開くにはハンドル表 (JFT) が要る。それが載るのは PSP なので、
        ; カーネルにも 1 つ持たせる。これが無いと cur_psp が 0 のまま
        ; ファイルを開くことになり、JFT の書き込みが 0000:0018 — 割り込み
        ; ベクタの真ん中 — に飛ぶ。
        call    boot_psp_init

        ; --- CONFIG.SYS を読む ---
        ; まだ反映はしない。DOS=HIGH が書いてあるかどうかで、この先の
        ; メモリの配り方が変わるため、先に読むだけ読んでおく。
        call    config_load


        ; --- MCB アリーナはカーネルの直後から 640KB まで ---
        ; カーネルの大きさをパラグラフに切り上げて自分のセグメントに足す。
        ; (kernel_end は再配置可能なラベルなのでアセンブル時には割れない)
        ; 32bit で計算する。kernel_end は 64KB のすぐ手前まで来ることが
        ; あり、16bit のまま 15 を足すと回り込んで 0 近くになる。そうなると
        ; アリーナの先頭がカーネル自身の上に重なり、最初の MCB を書いた
        ; 時点でカーネルの先頭を壊す。
        mov     eax, kernel_end
        add     eax, 15
        shr     eax, 4
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

        ; カーネルの PSP にも環境ブロックを教える
        push    es
        mov     es, [boot_psp_seg]
        mov     ax, [boot_env_seg]
        mov     [es:PSP_ENVSEG], ax
        pop     es

        ; 既定のシェルは起動したドライブから読む
        mov     al, [cur_drive]
        add     al, 'A'
        mov     [shell_path], al

        ; --- CONFIG.SYS の残りを反映する ---
        ; 数の指定でテーブルを組み直し、DEVICE= を読み込み、INSTALL= を走らせる。
        call    config_finish

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
; dos_init_tables - DOS の内部テーブルを一式組み立てる
;   出力: CF=1 ならディスクが読めなかった
;
; 起動時に 1 回、DOS=HIGH で HMA へ移ったあとにもう 1 回呼ぶ。構造体の
; 中には far ポインタがあり、それはどれも「いまの CS」から作られるので、
; セグメントが変わったら作り直すのが一番確実。
; ---------------------------------------------------------------------------
dos_init_tables:
        push    ax
        push    cx

        ; デバイスドライバの連鎖。ディスクを触る前に済ませておく。
        ; ブロックデバイスも連鎖の一員で、セクタ入出力はここを通る。
        call    dev_init

        ; ディスクバッファ (BUFFERS=)
        mov     al, DEFAULT_BUFFERS
        call    buf_init

        ; ドライブの検出と BPB の取り込み
        call    disk_init
        jc      .fail

        ; ドライブごとのカレントディレクトリ (CDS)。
        ; 見つかったドライブが LASTDRIVE より多ければそちらに合わせる。
        mov     al, DEFAULT_LASTDRIVE
        cmp     al, [num_drives]
        jae     .lastdrive_ok
        mov     al, [num_drives]
.lastdrive_ok:
        call    cds_init

        ; ファイルハンドルの土台
        mov     al, SFT_ENTRIES
        call    sft_init

        ; List of Lists の各ポインタを実体に向ける
        call    lol_init

        pop     cx
        pop     ax
        clc
        ret
.fail:
        pop     cx
        pop     ax
        stc
        ret

; ---------------------------------------------------------------------------
; boot_psp_init - カーネル自身の PSP を作る
;
; ファイルを開くにはハンドル表 (JFT) が要り、それが載るのは PSP。
; カーネルもファイルを読む (CONFIG.SYS、デバイスドライバ) ので 1 つ要る。
; ---------------------------------------------------------------------------
boot_psp_init:
        push    ax
        push    bx
        push    cx
        push    dx

        mov     ax, cs
        mov     bx, boot_psp
        mov     cl, 4
        shr     bx, cl
        add     ax, bx
        mov     [boot_psp_seg], ax
        mov     bx, 0xA000              ; 使える上限
        xor     cx, cx                  ; 親はいない
        mov     dx, [boot_env_seg]
        call    build_psp
        mov     ax, [boot_psp_seg]
        mov     [cur_psp], ax

        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

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
        mov     sp, boot_stack_top
        cld
        sti

        ; 前のシェルが残していたものを片付ける
        mov     byte [indos_flag], 0    ; ここが一番外側
        mov     ax, [boot_psp_seg]
        mov     [cur_psp], ax
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
; INT 21h の中からもう一度 INT 21h が呼ばれることがある。
; デバイスドライバの INIT が画面に何か出すとき、常駐ソフトが割り込みの中で
; 呼ぶときなど。作業領域を 1 組しか持っていないと、内側の呼び出しが
; 外側の状態を上書きして帰ってこられなくなる。
;
; DOS はこのために作業用のスタックを何本か持っている (I/O スタック、
; ディスクスタック、補助スタック)。ここでも同じことをする:
; 入れ子の深さでスタックを選び、外側のレジスタ退避領域は積んで避けておく。
;
; ただし本物と同じ制限も残る。ファイルシステム側の作業用変数は 1 組しか
; 無いので、入れ子で安全に呼べるのは画面・キーボード・ベクタ操作
; (AH=01h〜0Ch, 25h, 30h, 35h) まで。当時のデバイスドライバの INIT に
; 許されていた範囲と同じ。InDOS フラグ (AH=34h) はそのための目印。
INT21_STACKS    equ 4                   ; 入れ子の深さの上限
INT21_STKSIZE   equ 1024
FRAME_SIZE      equ 24                  ; u_ax から ret_zf までの大きさ

int21_handler:
        cli
        ; 呼び出し元のレジスタは、まず入口専用の場所へ置く。
        ; 入れ子で呼ばれた場合、u_?? にはまだ外側の値が入っていて、
        ; それを控えるまでは触れない。ここは割り込みを止めているので、
        ; 1 組しか無くても取り合いにならない。
        mov     [cs:.save_ax], ax
        mov     [cs:.save_bx], bx
        mov     [cs:.save_cx], cx
        mov     [cs:.save_dx], dx
        mov     [cs:.save_si], si
        mov     [cs:.save_di], di
        mov     [cs:.save_bp], bp
        mov     [cs:.save_ss], ss
        mov     [cs:.save_sp], sp
        mov     [cs:.save_ds], ds
        mov     [cs:.save_es], es

        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        cld

        ; --- 入れ子なら、外側の作業領域を深さに応じた場所へ控える ---
        mov     al, [indos_flag]
        cmp     al, INT21_STACKS - 1
        jae     .too_deep
        test    al, al
        jz      .level0

        movzx   si, al
        dec     si
        mov     ax, FRAME_SIZE
        mul     si
        mov     di, frame_save
        add     di, ax
        mov     si, u_ax
        mov     cx, FRAME_SIZE
        rep     movsb

.level0:
        ; --- 深さに応じたスタックへ移る ---
        movzx   ax, byte [indos_flag]
        mov     cx, INT21_STKSIZE
        mul     cx
        mov     cx, kernel_stack_top
        sub     cx, ax
        mov     ax, cs
        mov     ss, ax
        mov     sp, cx
        inc     byte [indos_flag]

        ; --- 今回の作業領域を作る ---
        mov     ax, [.save_ax]
        mov     [u_ax], ax
        mov     ax, [.save_bx]
        mov     [u_bx], ax
        mov     ax, [.save_cx]
        mov     [u_cx], ax
        mov     ax, [.save_dx]
        mov     [u_dx], ax
        mov     ax, [.save_si]
        mov     [u_si], ax
        mov     ax, [.save_di]
        mov     [u_di], ax
        mov     ax, [.save_bp]
        mov     [u_bp], ax
        mov     ax, [.save_ds]
        mov     [u_ds], ax
        mov     ax, [.save_es]
        mov     [u_es], ax
        mov     ax, [.save_ss]
        mov     [u_ss], ax
        mov     ax, [.save_sp]
        mov     [u_sp], ax

        mov     byte [ret_cf], 0
        mov     byte [ret_zf], 0

        mov     ax, [u_ax]
        mov     bx, [u_bx]
        mov     cx, [u_cx]
        mov     dx, [u_dx]
        mov     si, [u_si]
        mov     di, [u_di]
        mov     bp, [u_bp]

        sti
        jmp     int21_dispatch

.too_deep:
        ; これ以上は受けられない。いちばん内側のスタックのまま、
        ; 機能番号が無いときと同じ返し方をする。
        mov     ax, [.save_ax]
        mov     [u_ax], ax
        mov     ax, [.save_ss]
        mov     [u_ss], ax
        mov     ax, [.save_sp]
        mov     [u_sp], ax
        mov     ax, [.save_ds]
        mov     [u_ds], ax
        mov     ax, [.save_es]
        mov     [u_es], ax
        mov     ax, cs
        mov     ss, ax
        mov     sp, kernel_stack_top - (INT21_STACKS - 1) * INT21_STKSIZE
        inc     byte [indos_flag]
        mov     word [u_ax], ERR_FUNC
        mov     byte [ret_cf], 1
        mov     byte [ret_zf], 0
        sti
        jmp     int21_exit

.save_ax: dw 0
.save_bx: dw 0
.save_cx: dw 0
.save_dx: dw 0
.save_si: dw 0
.save_di: dw 0
.save_bp: dw 0
.save_ss: dw 0
.save_sp: dw 0
.save_ds: dw 0
.save_es: dw 0

; ---------------------------------------------------------------------------
; INT 21h の出口
;
; CF (と AH=06h の ZF) は「呼び出し元のスタックに積まれている FLAGS」を
; 書き換えて返す。iret がそれを復元するので、値がプログラムに届く。
; ---------------------------------------------------------------------------
; ---------------------------------------------------------------------------
; int21_exit - 呼び出し元へ返る
;
; 入れ子で呼ばれていた場合は、ここで外側の作業領域を書き戻す。書き戻すと
; いまの値が読めなくなるので、先に一式を exit_frame へ写してから使う。
; 割り込みは止めたままなので、この間に別の INT 21h が入ることはない。
; ---------------------------------------------------------------------------
int21_exit:
        cli
%ifdef INT21_TRACE_ALL
        pusha
        push    ds
        push    cs
        pop     ds
        mov     al, '='
        call    trace_putc
        cmp     byte [ret_cf], 0
        je      .tr_ok
        mov     al, 'E'
        call    trace_putc
        jmp     .tr_ax
.tr_ok:
        mov     al, 'o'
        call    trace_putc
.tr_ax:
        mov     bx, [u_ax]
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
.tr_done:
        mov     al, ' '
        call    trace_putc
        pop     ds
        popa
%endif

        ; いまの作業領域を退避用に写す
        push    ds
        pop     es
        mov     si, u_ax
        mov     di, exit_frame
        mov     cx, FRAME_SIZE
        rep     movsb

        ; 深さを 1 つ戻し、外側があればその作業領域を書き戻す
        cmp     byte [indos_flag], 0
        je      .no_nest
        dec     byte [indos_flag]
        cmp     byte [indos_flag], 0
        je      .no_nest
        movzx   si, byte [indos_flag]
        dec     si
        mov     ax, FRAME_SIZE
        mul     si
        mov     si, frame_save
        add     si, ax
        mov     di, u_ax
        mov     cx, FRAME_SIZE
        rep     movsb
.no_nest:

        mov     al, [exit_frame + 22]   ; ret_cf
        mov     ah, [exit_frame + 23]   ; ret_zf

        mov     cx, [exit_frame + 18]   ; u_ss
        mov     dx, [exit_frame + 20]   ; u_sp
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
        mov     ax, [exit_frame + 0]
        mov     bx, [exit_frame + 2]
        mov     cx, [exit_frame + 4]
        mov     dx, [exit_frame + 6]
        mov     si, [exit_frame + 8]
        mov     di, [exit_frame + 10]
        mov     bp, [exit_frame + 12]
        mov     es, [exit_frame + 16]
        mov     ds, [exit_frame + 14]   ; DS は最後 (これ以降 exit_frame は読めない)
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
        mov     sp, boot_stack_top
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
        mov     sp, boot_stack_top
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
        mov     sp, boot_stack_top
        cld
        sti
        mov     ax, 0x4C00
        jmp     do_terminate

; INT 2Fh - マルチプレクサ。何も常駐していないので AL=0 を返す
int2f_handler:
        cmp     ah, 0x43                ; XMS の窓口
        je      .xms

        ; 知らない機能は、レジスタを何も変えずに返す。
        ;
        ; ここで AL=0 を書いてしまうと、INT 2Fh を「導入確認」に使う
        ; 約束事を壊す。呼ぶ側は AL=0 を入れて呼び、常駐したものが
        ; あれば AL=FFh が返る、という取り決めなので、DOS が勝手に
        ; AL を触ってよい理由がない。AL に引数を入れて呼ぶ機能
        ; (DPMI の 1687h など) では、返り値そのものが壊れる。
        iret
.xms:
        call    xms_int2f
        iret

int_iret:
        iret

; ---------------------------------------------------------------------------
; INT 25h / 26h - 絶対セクタ読み書き
;   AL = ドライブ番号, CX = セクタ数, DX = 開始 LBA, DS:BX = バッファ
;   出力: CF=0 成功 / CF=1 のとき AX = エラーコード
;
; この 2 つは DOS の中で唯一「iret で戻らない」割り込みで、INT が積んだ
; FLAGS をスタックに残したまま retf で返る。呼び出し側が自分で
;
;       int     0x25
;       add     sp, 2           ; または pushf/popf で拾う
;
; と後始末をする決まりになっている。なぜそんな形なのかというと、
; 呼び出し元に CF を返しつつ、割り込み前のフラグも見せたかった名残で、
; 理由はともかく当時のディスクユーティリティはこの形を前提に書かれている。
;
; ここを retf 2 (FLAGS を捨てて返る) にすると、呼び出し側の add sp,2 が
; 余計にスタックを 2 バイト削る。戻り先アドレスが 2 バイトずれるので、
; ret した瞬間にどことも知れない場所へ飛ぶ。CHKDSK のように起動直後に
; 絶対読み込みを走らせるプログラムは、これで無言のまま行方不明になる。
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
        retf                            ; FLAGS はスタックに残したまま返る
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
        retf

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
        retf
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
        retf

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
%include "lfn.inc"
%include "fat.inc"
%include "mem.inc"
%include "file.inc"
%include "fcb.inc"
%include "dirops.inc"
%include "exec.inc"
%include "int21.inc"
%include "xms.inc"
%include "config.inc"

; ============================================================================
; データ
; ============================================================================
msg_banner:     db 13, 10, 'MYDOS Version 1.0', 13, 10
                db 'Copyright (C) 2026', 13, 10, 13, 10, 0
msg_disk_fail:  db 'IO.SYS: disk initialization failed', 13, 10, 0
msg_no_mem:     db 'IO.SYS: not enough memory', 13, 10, 0
msg_no_shell:   db 'Bad or missing command interpreter', 13, 10, 0

; 既定は起動したドライブの \COMMAND.COM。CONFIG.SYS に SHELL= が
; 書かれていればここが差し替わる。
shell_path:     db 'A:\COMMAND.COM', 0
                times 68 - 16 db 0
boot_env_seg:   dw 0
boot_psp_seg:   dw 0

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

; --- カーネル自身の PSP (段落境界に置く) -----------------------------------
                align 16
boot_psp:       times PSP_SIZE db 0

; --- バッファ --------------------------------------------------------------
dir_cur_lba:    dd 0                    ; dir_buf に載っているセクタの LBA
; BIOS のディスク転送に渡すバッファは、この「場所だけ確保する」領域
; ではなく、必ずカーネルの前のほうに置くこと。
;
; INT 13h の転送は DMA で行われ、DMA は 64KB の物理境界をまたげない。
; カーネルは 0x0060:0000 — 物理 0x600 — に居るので、セグメントの終わり近く
; (オフセット 0xF800 あたり) に置いたバッファは物理 0x10000 をまたいでしまい、
; 転送がまるごと失敗する。ディレクトリが 1 つも読めなくなり、
; 「COMMAND.COM が無い」という形で表に出た。
dir_buf:        times SECTOR_SIZE db 0
sector_buf:     times SECTOR_SIZE db 0
fat_buf:        times FAT_RESIDENT_SECS * SECTOR_SIZE db 0  ; 常駐 FAT (FAT12 用)

; --- 入れ子の INT 21h 用に、外側の作業領域を控える場所 ---------------------
frame_save:     times (INT21_STACKS - 1) * FRAME_SIZE db 0
exit_frame:     times FRAME_SIZE db 0

; --- カーネルスタック ------------------------------------------------------
;
; INT 21h の処理中に使うスタック。入れ子の深さぶんだけ並べてあり、
; いちばん外側が一番上を使う。プログラム側のスタックは触らない。
                times INT21_STACKS * INT21_STKSIZE db 0
kernel_stack_top:

; 起動処理とシェルの起動に使うスタック。INT 21h のものとは別にしてある。
; CONFIG.SYS の処理中にデバイスドライバの INIT が INT 21h を呼ぶと、
; 同じスタックだと外側 (CONFIG.SYS の処理) の足元を崩してしまう。
; DOS が I/O スタックとディスクスタックを分けているのと同じ理由。
                times 1024 db 0
boot_stack_top:

; ============================================================================
; ここから先は IO.SYS のファイルには入らない
;
; 大きな表やバッファを、中身のない「場所の確保」に変えてある。0 で埋めた
; ものをそのままファイルに持たせると、IO.SYS がその分だけ大きくなる。
; カーネルは 0x0060:0000 に置いた 1 つのセグメントの中で動くので、
; 64KB を 1 バイトでも超えるとオフセットが回り込んで即座に壊れる。
; 実際、長い名前のための表を足した時点で 63 バイト超えて起動しなくなった。
;
; 中身は不定なので、起動時に 0 で埋めてから使う (kernel_main の頭)。
; ============================================================================
kernel_bss:

; absolute を使うと、NASM は場所の割り当てだけして何も出力しない。
; resb を普通に並べると -f bin では 0 が書き出されてしまい、
; ファイルを小さくするという目的が果たせない。
                absolute kernel_bss

; 開いているファイルの表。この 3 つはこの順で隣り合っていなければ
; ならない。List of Lists の +04 が sft_header を指し、その +06 から
; エントリが並んでいる、というのが外から見える約束になっている。
sft_count:      resb 1                  ; FILES= で決まった実際のエントリ数
sft_header:     resb 6                  ; 次のテーブルへの far ポインタ + 個数
sft_table:      resb MAX_SFT_ENTRIES * SFT_ENTSIZE
drive_tab:      resb MAX_PHYS_DRIVES * DRV_ENTSIZE        ; ドライブごとの諸元
cds_table:      resb MAX_DRIVES * CDS_ENTSIZE        ; カレントディレクトリの表

; InDOS / カレント PSP / DTA / 作業用バッファは SDA にまとめてある。
; AH=5D06h でこの場所と大きさを外に教える。
%include "sda.inc"

kernel_end:

                __SECT__        ; 元のセクションに戻す
