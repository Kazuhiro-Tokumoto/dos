; ============================================================================
; cfgtest.asm  -  CONFIG.SYS が本当に効いているかを確かめる
;
; CONFIG.SYS は「読めた」だけでは意味がなく、書いた数がカーネルの中の
; 表の大きさに反映されて初めて効いたことになる。だから確認は
; List of Lists を辿って、実際の表を数える形で行う。
;
;   FILES=30    → SFT の入るテーブルのエントリ数
;   BUFFERS=12  → ディスクバッファ連鎖の長さ
;   LASTDRIVE=H → List of Lists +21h
;
; DEVICE= で入れた 2 つのドライバも、外から見える形で確かめる。
;   TESTDEV.SYS  文字デバイス。名前で開いて読み書きし、IOCTL で
;                「何バイト書かれたか」と「INIT に渡った引数の長さ」を聞く
;   RAMDISK.SYS  ブロックデバイス。生えたドライブにファイルを作って読み返す
; ============================================================================
        cpu     386
        bits    16
        org     0x100

; --- List of Lists のオフセット ---
LOL_FIRST_SFT   equ 0x04
LOL_BUFFERS     equ 0x12
LOL_LASTDRIVE   equ 0x21
LOL_NULDEV      equ 0x22

SFTH_NEXT       equ 0x00
SFTH_COUNT      equ 0x04

BUF_NEXT        equ 0x00

DEV_NEXT        equ 0x00
DEV_ATTR        equ 0x04
DEV_NAME        equ 0x0A
DEVA_CHAR       equ 0x8000

start:
        mov     sp, stack_top
        mov     bx, stack_top
        add     bx, 15
        mov     cl, 4
        shr     bx, cl
        mov     ah, 0x4A
        int     0x21

        mov     si, msg_head
        call    puts

        mov     ah, 0x52
        int     0x21
        mov     [lol_seg], es
        mov     [lol_off], bx

        call    t_files
        call    t_buffers
        call    t_lastdrive
        call    t_devchain
        call    t_chardev
        call    t_ramdisk

        call    newline
        mov     si, msg_result
        call    puts
        mov     ax, [pass_count]
        call    put_dec
        mov     si, msg_result2
        call    puts
        mov     ax, [fail_count]
        call    put_dec
        call    newline
        mov     si, msg_end
        call    puts

        mov     ax, [fail_count]
        test    ax, ax
        jz      .clean
        mov     ax, 0x4C01
        int     0x21
.clean:
        mov     ax, 0x4C00
        int     0x21

; ============================================================================
; 1. FILES=30 が SFT テーブルの大きさになっているか
; ============================================================================
t_files:
        mov     si, n_files
        call    begin

        mov     es, [lol_seg]
        mov     bx, [lol_off]
        les     bx, [es:bx + LOL_FIRST_SFT]
        mov     byte [step], 1
        mov     ax, [es:bx + SFTH_COUNT]
        cmp     ax, 30
        jne     failx

        ; 実際に何本も同時に開けること。ここで数えているのは SFT ではなく
        ; ハンドルなので、上限は PSP の JFT (20 本) 側で決まる。うち数本は
        ; 標準入出力などで最初から埋まっていて、親から引き継いだ
        ; バッチファイルのぶんも埋まっている。FILES= が効いていることは
        ; 上の SFTH_COUNT で確かめてあるので、ここは「足りている」ことだけ見る。
        xor     cx, cx                  ; 開けた数
.open_loop:
        cmp     cx, 15
        jae     .enough
        mov     ax, 0x3D00
        mov     dx, f_readme
        int     0x21
        jc      .enough
        push    cx
        mov     bx, cx
        add     bx, handles
        mov     [bx], al
        pop     cx
        inc     cx
        jmp     .open_loop
.enough:
        push    cx
.close_loop:
        test    cx, cx
        jz      .closed
        dec     cx
        push    cx
        mov     bx, cx
        add     bx, handles
        movzx   bx, byte [bx]
        mov     ah, 0x3E
        int     0x21
        pop     cx
        jmp     .close_loop
.closed:
        pop     cx
        mov     byte [step], 2
        mov     ax, cx
        cmp     cx, 10                  ; 実際に何本か同時に開けること
        jb      failx
        jmp     pass

; ============================================================================
; 2. BUFFERS=12 がバッファ連鎖の長さになっているか
; ============================================================================
t_buffers:
        mov     si, n_buffers
        call    begin

        mov     es, [lol_seg]
        mov     bx, [lol_off]
        les     bx, [es:bx + LOL_BUFFERS]

        xor     cx, cx
.loop:
        inc     cx
        cmp     cx, 100
        jae     fail                    ; 終わりが来ない
        cmp     word [es:bx + BUF_NEXT + 2], 0xFFFF
        je      .done
        les     bx, [es:bx + BUF_NEXT]
        jmp     .loop
.done:
        cmp     cx, 12
        jne     fail
        jmp     pass

; ============================================================================
; 3. LASTDRIVE=H
; ============================================================================
t_lastdrive:
        mov     si, n_lastdrive
        call    begin

        mov     es, [lol_seg]
        mov     bx, [lol_off]
        cmp     byte [es:bx + LOL_LASTDRIVE], 8         ; A: から H: まで
        jne     fail
        jmp     pass

; ============================================================================
; 4. DEVICE= で入れたドライバが連鎖の先頭側にいるか
;
; あとから入れたドライバは NUL の直後に差し込まれる。名前で探すときは
; 先頭から見るので、組み込みの CON より先に見つかる。ANSI.SYS が CON を
; 乗っ取れるのはこの順序のおかげ。
; ============================================================================
t_devchain:
        mov     si, n_devchain
        call    begin

        mov     es, [lol_seg]
        mov     bx, [lol_off]
        add     bx, LOL_NULDEV          ; NUL のヘッダ = 連鎖の先頭

        xor     dx, dx                  ; DL=1 MYDEV を見つけた, DH=1 ブロックデバイス
        xor     cx, cx                  ; 辿った数
.walk:
        inc     cx
        cmp     cx, 30
        jae     fail                    ; 終わりが来ない
        cmp     word [es:bx + DEV_NEXT + 2], 0xFFFF
        je      fail                    ; CON に届かなかった
        les     bx, [es:bx + DEV_NEXT]

        test    word [es:bx + DEV_ATTR], DEVA_CHAR
        jz      .block

        ; 文字デバイス。CON なら、その手前までに 2 つとも見つかっていること。
        push    bx
        push    cx
        add     bx, DEV_NAME
        mov     si, e_con
        mov     cx, 8
.cmp_con:
        mov     al, [si]
        cmp     al, [es:bx]
        jne     .not_con
        inc     si
        inc     bx
        loop    .cmp_con
        pop     cx
        pop     bx
        cmp     dl, 1
        jne     fail
        cmp     dh, 1
        jne     fail
        jmp     pass
.not_con:
        pop     cx
        pop     bx

        ; MYDEV かどうか
        push    bx
        push    cx
        add     bx, DEV_NAME
        mov     si, e_mydev
        mov     cx, 8
.cmp_dev:
        mov     al, [si]
        cmp     al, [es:bx]
        jne     .not_dev
        inc     si
        inc     bx
        loop    .cmp_dev
        pop     cx
        pop     bx
        mov     dl, 1
        jmp     .walk
.not_dev:
        pop     cx
        pop     bx
        jmp     .walk

.block:
        ; ブロックデバイス。ユニット数 1 なら RAMDISK.SYS のもの。
        cmp     byte [es:bx + DEV_NAME], 1
        jne     .walk
        mov     dh, 1
        jmp     .walk

; ============================================================================
; 5. MYDEV.SYS を名前で開いて読み書きする
;
; ドライバが返す文字列がそのまま読めること、書いたバイト数が
; ドライバの中で数えられていること、INIT に CONFIG.SYS の引数が
; 渡っていることを、IOCTL 読み取りで確かめる。
; ============================================================================
t_chardev:
        mov     si, n_chardev
        call    begin

        mov     byte [step], 1
        mov     ax, 0x3D02              ; 読み書きで開く
        mov     dx, f_mydev
        int     0x21
        jc      failx
        mov     [handle], ax

        ; 読む
        mov     byte [step], 2
        mov     bx, [handle]
        mov     ah, 0x3F
        mov     cx, 20
        mov     dx, buf
        int     0x21
        jc      .closefail
        cmp     ax, 20
        jne     .closefail

        mov     byte [step], 3
        mov     si, buf
        mov     di, e_payload
        mov     cx, 20
        push    ds
        pop     es
        repe    cmpsb
        jne     .closefail

        ; 書く (7 バイト)
        mov     byte [step], 4
        mov     bx, [handle]
        mov     ah, 0x40
        mov     cx, 7
        mov     dx, e_payload
        int     0x21
        jc      .closefail

        ; IOCTL 読み取りで、書かれたバイト数と引数の長さを聞く
        mov     byte [step], 5
        mov     bx, [handle]
        mov     ax, 0x4402
        mov     cx, 4
        mov     dx, buf
        int     0x21
        jc      .closefail

        mov     bx, [handle]
        mov     ah, 0x3E
        int     0x21

        mov     byte [step], 6
        mov     ax, [buf]
        cmp     ax, 7                   ; 書いたバイト数
        jne     failx
        mov     byte [step], 7
        mov     ax, [buf + 2]
        cmp     ax, 17                  ; "hello-from-config" = 17 文字
        jne     failx
        jmp     pass
.closefail:
        push    ax
        mov     bx, [handle]
        mov     ah, 0x3E
        int     0x21
        pop     ax
        jmp     failx

; ============================================================================
; 6. RAMDISK.SYS が生やしたドライブを使う
;
; ドライブ文字は、その時点で使われていない次のものになる。
; フロッピー 1 台 + ハードディスクのパーティション 2 つがあれば E:。
; 空き容量が引けること、ファイルを作って読み返せることを見る。
; ============================================================================
t_ramdisk:
        mov     si, n_ramdisk
        call    begin

        ; 空き容量 (DL = 5 で E:)
        mov     ah, 0x36
        mov     dl, 5
        int     0x21
        cmp     ax, 0xFFFF
        je      fail
        cmp     ax, 1                   ; 1 クラスタ = 1 セクタ
        jne     fail
        cmp     cx, 512
        jne     fail
        test    bx, bx                  ; 空きがある
        jz      fail
        cmp     dx, 200                 ; 総クラスタ数は 128 セクタぶん
        ja      fail

        ; ファイルを作って書く
        mov     ah, 0x3C
        xor     cx, cx
        mov     dx, f_ramfile
        int     0x21
        jc      fail
        mov     [handle], ax
        mov     bx, ax
        mov     ah, 0x40
        mov     cx, payload_len
        mov     dx, payload
        int     0x21
        pushf
        mov     [wrote], ax
        mov     bx, [handle]
        mov     ah, 0x3E
        int     0x21
        popf
        jc      fail
        cmp     word [wrote], payload_len
        jne     fail

        ; 読み返す
        mov     ax, 0x3D00
        mov     dx, f_ramfile
        int     0x21
        jc      fail
        mov     [handle], ax
        mov     bx, ax
        mov     ah, 0x3F
        mov     cx, 128
        mov     dx, buf
        int     0x21
        pushf
        mov     [wrote], ax
        mov     bx, [handle]
        mov     ah, 0x3E
        int     0x21
        popf
        jc      fail
        cmp     word [wrote], payload_len
        jne     fail

        mov     si, payload
        mov     di, buf
        mov     cx, payload_len
        push    ds
        pop     es
        repe    cmpsb
        jne     fail
        jmp     pass

; ============================================================================
; 出力まわり
; ============================================================================
begin:
        push    si
        mov     si, str_indent
        call    puts
        pop     si
        mov     [test_name], si
        ret

pass:
        inc     word [pass_count]
        mov     si, str_pass
        call    puts
        mov     si, [test_name]
        call    puts
        call    newline
        ret

fail:
        inc     word [fail_count]
        mov     si, str_fail
        call    puts
        mov     si, [test_name]
        call    puts
        call    newline
        ret

; failx - どこで落ちたか (step) と、そのときの AX を添えて出す
failx:
        push    ax
        inc     word [fail_count]
        mov     si, str_fail
        call    puts
        mov     si, [test_name]
        call    puts
        mov     si, str_step
        call    puts
        movzx   ax, byte [step]
        call    put_dec
        mov     si, str_ax
        call    puts
        pop     ax
        call    put_dec
        call    newline
        ret

putc:
        push    ax
        push    bx
        push    cx
        push    dx
        push    ds
        push    cs
        pop     ds
        mov     [char_buf], al
        mov     dx, char_buf
        mov     cx, 1
        mov     bx, 1
        mov     ah, 0x40
        int     0x21
        pop     ds
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

newline:
        push    ax
        mov     al, 13
        call    putc
        mov     al, 10
        call    putc
        pop     ax
        ret

puts:
        push    ax
        push    si
.loop:
        lodsb
        test    al, al
        jz      .done
        call    putc
        jmp     .loop
.done:
        pop     si
        pop     ax
        ret

put_dec:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     bx, 10
        xor     cx, cx
.split:
        xor     dx, dx
        div     bx
        push    dx
        inc     cx
        test    ax, ax
        jnz     .split
.emit:
        pop     ax
        add     al, '0'
        call    putc
        loop    .emit
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ============================================================================
; データ
; ============================================================================
msg_head:    db 13, 10, '=== MYDOS CONFIG.SYS / installable driver test ===', 13, 10, 13, 10, 0
msg_result:  db '### RESULT pass=', 0
msg_result2: db ' fail=', 0
msg_end:     db '###TEST-END###', 13, 10, 0

str_indent:  db '  ', 0
str_pass:    db '[PASS] ', 0
str_fail:    db '[FAIL] ', 0
str_step:    db '  (step=', 0
str_ax:      db ' ax=', 0

n_files:     db 'FILES=30    the SFT table really holds 30 entries', 0
n_buffers:   db 'BUFFERS=12  the disk buffer chain is 12 long', 0
n_lastdrive: db 'LASTDRIVE=H is reported in the List of Lists', 0
n_devchain:  db 'DEVICE=     both drivers sit ahead of CON in the chain', 0
n_chardev:   db 'MYDEV.SYS   opens by name, reads, writes, IOCTL', 0
n_ramdisk:   db 'RAMDISK.SYS gave a usable drive with a real FAT12', 0

e_mydev:     db 'MYDEV   '
e_con:       db 'CON     '
e_payload:   db 'MYDEV says hello... '

f_mydev:     db 'MYDEV', 0
f_readme:    db 'README.TXT', 0
f_ramfile:   db 'E:\RAMFILE.TXT', 0

payload:     db 'MYDOS phase B: CONFIG.SYS and installable device drivers', 13, 10
payload_len  equ $ - payload

; --- 変数 ------------------------------------------------------------------
test_name:   dw 0
pass_count:  dw 0
fail_count:  dw 0
lol_seg:     dw 0
lol_off:     dw 0
handle:      dw 0
step:        db 0
wrote:       dw 0
char_buf:    db 0
handles:     times 24 db 0
buf:         times 160 db 0

             times 512 db 0
stack_top:
