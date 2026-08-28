; ============================================================================
; setup.asm  -  SETUP コマンド (MYDOS をハードディスクに入れる)
;
;   SETUP [d:]
;
; 起動フロッピーから走らせると、指定したドライブ (既定は C:) に MYDOS を
; 入れる。やっていることは FDISK / FORMAT / SYS を順に呼ぶのと同じで、
; そのうえで CONFIG.SYS と AUTOEXEC.BAT、それに配布物のファイルを移す。
;
; --- なぜ別のプログラムなのか -----------------------------------------------
;
; 当時の MS-DOS も、配布物の中身は FORMAT / SYS / FDISK という部品と、
; それを順に呼ぶ SETUP という薄い皮でできていた。部品のほうは単体でも
; 使えるようにしておき、「普通に入れたいだけ」の人には手順を 1 つに
; まとめて見せる、という分け方。MYDOS もそれに倣っている。
;
; --- 消える前に一度聞く -----------------------------------------------------
;
; 相手のディスクの中身は消える。FORMAT が自分で確認を取るが、その前に
; ここでも一度聞く。SETUP を打った人は「入れる」つもりであって
; 「消す」つもりではないことが多いので、何が起きるかを先に書いておく。
; ============================================================================
        cpu     386
        bits    16
        org     0x100

start:
        mov     sp, stack_top
        mov     bx, stack_top
        add     bx, 15
        mov     cl, 4
        shr     bx, cl
        mov     ah, 0x4A
        int     0x21

        call    parse_cmdline

        ; 元は「いま使っているドライブ」
        mov     ah, 0x19
        int     0x21
        mov     [src_drive], al
        add     al, 'A'
        mov     [src_letter], al
        cmp     al, [dst_letter]
        je      .same

        mov     dx, msg_banner
        call    puts
        mov     dx, msg_plan
        call    puts

        call    confirm
        jc      .aborted

        ; --- 1. マスターブートレコード ---
        mov     dx, msg_step1
        call    puts
        mov     si, arg_mbr
        mov     dx, p_fdisk
        call    run_child
        jc      .child_fail

        ; --- 2. 区画を作り直す ---
        mov     dx, msg_step2
        call    puts
        mov     si, arg_format
        mov     al, [dst_letter]
        mov     [fmt_drive], al
        mov     dx, p_format
        call    run_child
        jc      .child_fail

        ; --- 3. システムを移す ---
        mov     dx, msg_step3
        call    puts
        mov     si, arg_sys
        mov     al, [dst_letter]
        mov     [sys_drive], al
        mov     dx, p_sys
        call    run_child
        jc      .child_fail

        ; --- 4. CONFIG.SYS と AUTOEXEC.BAT を作る ---
        ;
        ; フロッピー側のものをそのまま写してはいけない。あちらは試験用で、
        ; SHELL=A:\COMMAND.COM のように「A: から起動している」前提の行が
        ; 入っている。それをハードディスクへ持っていくと、次に起動したとき
        ; 入っていないフロッピーを探しに行く。入れる側の姿に合わせて
        ; 書き下ろすのが正しい。
        mov     dx, msg_step4
        call    puts
        mov     si, f_config
        mov     bx, txt_config
        mov     cx, txt_config_len
        call    write_text
        mov     si, f_autoexec
        mov     bx, txt_autoexec
        mov     cx, txt_autoexec_len
        call    write_text

        ; --- 5. 残りのファイル ---
        mov     dx, msg_step5
        call    puts
        mov     si, file_list
.copy_loop:
        cmp     byte [si], 0
        je      .copied
        call    copy_one
        ; 見つからないファイルは黙って飛ばす (配布物の構成が変わっても
        ; SETUP が止まらないように)
        call    next_string
        jmp     .copy_loop
.copied:

        mov     al, [dst_letter]
        mov     [done_drive], al
        mov     dx, msg_done
        call    puts
        mov     ax, 0x4C00
        int     0x21

.same:
        mov     dx, msg_same
        call    puts
        jmp     .fail
.aborted:
        mov     dx, msg_aborted
        call    puts
        jmp     .fail
.child_fail:
        mov     dx, msg_child_fail
        call    puts
.fail:
        mov     ax, 0x4C01
        int     0x21

; ============================================================================
; コマンドライン (ドライブ文字だけ)
; ============================================================================
parse_cmdline:
        mov     si, 0x81
        movzx   cx, byte [0x80]
        jcxz    .out
.skip:
        cmp     byte [si], ' '
        jne     .have
        inc     si
        dec     cx
        jnz     .skip
        jmp     .out
.have:
        mov     al, [si]
        cmp     al, 'a'
        jb      .up_done
        cmp     al, 'z'
        ja      .up_done
        sub     al, 0x20
.up_done:
        cmp     al, 'A'
        jb      .out
        cmp     al, 'Z'
        ja      .out
        mov     [dst_letter], al
.out:
        ret

; ============================================================================
; 確認を取る
;
; 標準入力から読むので、答えを流し込んでの自動実行もできる。
; ============================================================================
confirm:
        mov     al, [dst_letter]
        mov     [warn_drive], al
        mov     dx, msg_warn
        call    puts

        xor     bx, bx
        mov     cx, 1
        mov     dx, answer
        mov     ah, 0x3F
        int     0x21
        jc      .no
        test    ax, ax
        jz      .no
        mov     dx, msg_crlf
        call    puts
        mov     al, [answer]
        cmp     al, 'y'
        je      .ok
        cmp     al, 'Y'
        je      .ok
.no:
        stc
        ret
.ok:
        clc
        ret

; ============================================================================
; run_child - 子プロセスを引数付きで起動する
;   入力: DS:DX = プログラムのパス, DS:SI = コマンドテイル (先頭が長さ)
;   出力: CF=1 なら起動できなかった、または子が失敗した
; ============================================================================
run_child:
        mov     [epb_tail], si
        mov     [epb_tail + 2], ds
        mov     word [epb_env], 0               ; 親の環境を引き継ぐ
        mov     word [epb_fcb1], 0x5C
        mov     [epb_fcb1 + 2], ds
        mov     word [epb_fcb2], 0x6C
        mov     [epb_fcb2 + 2], ds

        push    ds
        push    es
        push    bp
        mov     bx, exec_pb
        mov     ax, 0x4B00
        int     0x21
        pop     bp
        pop     es
        pop     ds
        jc      .fail

        ; 子の終了コードを見る
        mov     ah, 0x4D
        int     0x21
        test    al, al
        jnz     .fail
        clc
        ret
.fail:
        stc
        ret

; ============================================================================
; write_text - 先のドライブに、決まった中身のファイルを 1 つ作る
;   入力: DS:SI = ファイル名, DS:BX = 中身, CX = 長さ
; ============================================================================
write_text:
        push    bx
        push    cx
        call    build_paths
        pop     cx
        pop     bx

        push    bx
        push    cx
        mov     dx, dst_path
        xor     cx, cx
        mov     ah, 0x3C
        int     0x21
        pop     cx
        pop     bx
        jc      .fail
        mov     [dst_handle], ax

        mov     dx, bx
        mov     bx, [dst_handle]
        mov     ah, 0x40
        int     0x21

        mov     bx, [dst_handle]
        mov     ah, 0x3E
        int     0x21

        mov     dx, msg_dot
        call    puts
        clc
        ret
.fail:
        stc
        ret

; ============================================================================
; copy_one - DS:SI のファイル名を、元から先へ 1 つ写す
; ============================================================================
copy_one:
        push    si
        call    build_paths
        pop     si

        mov     dx, src_path
        mov     ax, 0x3D00
        int     0x21
        jc      .no_src
        mov     [src_handle], ax

        mov     dx, dst_path
        xor     cx, cx
        mov     ah, 0x3C
        int     0x21
        jc      .no_dst
        mov     [dst_handle], ax

        push    si
        mov     dx, msg_dot
        call    puts
        pop     si
.loop:
        mov     bx, [src_handle]
        mov     cx, COPY_BUF_SIZE
        mov     dx, copy_buf
        mov     ah, 0x3F
        int     0x21
        jc      .io_err
        test    ax, ax
        jz      .eof
        mov     cx, ax
        mov     bx, [dst_handle]
        mov     dx, copy_buf
        mov     ah, 0x40
        int     0x21
        jc      .io_err
        cmp     ax, cx
        jne     .io_err
        jmp     .loop
.eof:
        mov     bx, [dst_handle]
        mov     ah, 0x3E
        int     0x21
        mov     bx, [src_handle]
        mov     ah, 0x3E
        int     0x21
        clc
        ret
.io_err:
        mov     bx, [dst_handle]
        mov     ah, 0x3E
        int     0x21
.no_dst:
        mov     bx, [src_handle]
        mov     ah, 0x3E
        int     0x21
.no_src:
        stc
        ret

COPY_BUF_SIZE   equ 8192

; ============================================================================
; build_paths - "A:\NAME" と "C:\NAME" を組み立てる
; ============================================================================
build_paths:
        push    ax
        push    di
        push    si
        push    es
        push    ds
        pop     es

        mov     al, [src_letter]
        mov     [src_path], al
        mov     byte [src_path + 1], ':'
        mov     byte [src_path + 2], '\'
        mov     di, src_path + 3
        push    si
        call    .copy_name
        pop     si

        mov     al, [dst_letter]
        mov     [dst_path], al
        mov     byte [dst_path + 1], ':'
        mov     byte [dst_path + 2], '\'
        mov     di, dst_path + 3
        call    .copy_name

        pop     es
        pop     si
        pop     di
        pop     ax
        ret
.copy_name:
        lodsb
        stosb
        test    al, al
        jnz     .copy_name
        ret

; 0 終端の文字列を 1 つ進める
next_string:
        lodsb
        test    al, al
        jnz     next_string
        ret

puts:
        push    ax
        mov     ah, 0x09
        int     0x21
        pop     ax
        ret

; ============================================================================
; データ
; ============================================================================
msg_banner:   db 13, 10, 'MYDOS Setup', 13, 10
              db '===========', 13, 10, 13, 10, '$'
msg_plan:     db 'This will install MYDOS onto a hard disk. It does:', 13, 10
              db 13, 10
              db '  1. write the master boot record   (FDISK /MBR)', 13, 10
              db '  2. lay out the partition again    (FORMAT)', 13, 10
              db '  3. copy the boot loader and DOS   (SYS)', 13, 10
              db '  4. write CONFIG.SYS and AUTOEXEC.BAT', 13, 10
              db '  5. copy the rest of the files', 13, 10, 13, 10, '$'
msg_warn:     db 'Everything on drive '
warn_drive:   db 'C'
              db ': will be erased.', 13, 10
              db 'Proceed with Setup (Y/N)? $'
msg_crlf:     db 13, 10, '$'
msg_step1:    db 13, 10, '[1/5] Master boot record', 13, 10, '$'
msg_step2:    db '[2/5] Partition layout', 13, 10, '$'
msg_step3:    db '[3/5] Boot loader and system files', 13, 10, '$'
msg_step4:    db '[4/5] CONFIG.SYS and AUTOEXEC.BAT ', '$'
msg_step5:    db 13, 10, '[5/5] Remaining files ', '$'
msg_dot:      db '.$'
msg_done:     db 13, 10, 13, 10, 'Setup is finished.', 13, 10
              db 'Remove the floppy and restart. MYDOS will come up from '
done_drive:   db 'C'
              db ':', 13, 10, '$'
msg_same:     db 'SETUP: that is the drive you started from', 13, 10, '$'
msg_aborted:  db 'Setup cancelled', 13, 10, '$'
msg_child_fail: db 13, 10, 'SETUP: a step failed - stopping here', 13, 10, '$'

; 呼ぶプログラムと、その引数 (先頭がコマンドテイルの長さ、末尾は CR)
; 長さはアセンブラに数えさせる。手で書くとずれたときに気づけない
; (子プログラムが引数の終わりを越えて読み、書き方が違うと言って止まる)。
p_fdisk:      db 'FDISK.COM', 0
arg_mbr:      db arg_mbr_end - arg_mbr_txt
arg_mbr_txt:  db ' /MBR'
arg_mbr_end:  db 13

p_format:     db 'FORMAT.COM', 0
arg_format:   db arg_format_end - arg_format_txt
arg_format_txt:
              db ' '
fmt_drive:    db 'C'
              db ': /Q /V:MYDOS'
arg_format_end:
              db 13

p_sys:        db 'SYS.COM', 0
arg_sys:      db arg_sys_end - arg_sys_txt
arg_sys_txt:  db ' '
sys_drive:    db 'C'
              db ':'
arg_sys_end:  db 13

; 移すファイル。無いものは黙って飛ばす。
file_list:
              db 'FORMAT.COM', 0
              db 'SYS.COM', 0
              db 'FDISK.COM', 0
              db 'SETUP.COM', 0
              db 'EMM386.SYS', 0
              db 'README.TXT', 0
              db 0                      ; 一覧の終わり

f_config:     db 'CONFIG.SYS', 0
f_autoexec:   db 'AUTOEXEC.BAT', 0

; 入れる側に書き下ろす CONFIG.SYS。SHELL= は書かない。書かなければ DOS は
; 起動したドライブの COMMAND.COM を探すので、どのドライブに入れても合う。
txt_config:
              db 'FILES=30', 13, 10
              db 'BUFFERS=20', 13, 10
              db 'LASTDRIVE=E', 13, 10
              db 'DEVICE=\EMM386.SYS', 13, 10
txt_config_len equ $ - txt_config

txt_autoexec:
              db '@ECHO OFF', 13, 10
              db 'ECHO MYDOS is installed on this disk.', 13, 10
              db 'VER', 13, 10
txt_autoexec_len equ $ - txt_autoexec

exec_pb:
epb_env:      dw 0
epb_tail:     dd 0
epb_fcb1:     dd 0
epb_fcb2:     dd 0

src_drive:    db 0
src_letter:   db 'A'
dst_letter:   db 'C'
answer:       db 0
src_handle:   dw 0
dst_handle:   dw 0

src_path:     times 20 db 0
dst_path:     times 20 db 0

              align 2
copy_buf:     times COPY_BUF_SIZE db 0
              times 512 db 0
stack_top:
