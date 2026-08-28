; ============================================================================
; fdisk.asm  -  FDISK コマンド (ハードディスクの先頭セクタを扱う)
;
;   FDISK /STATUS       パーティションテーブルを表示する
;   FDISK /MBR          マスターブートレコードのコードだけを書き直す
;   FDISK /D:n          対象の物理ドライブを選ぶ (既定は 80h = 1 台目)
;
; --- なぜ DOS のドライブ文字ではなく物理ドライブなのか ---------------------
;
; パーティションテーブルはディスクの LBA 0 にあり、そこはどの区画にも
; 属していない。DOS のドライブ文字は区画に付くものなので、そもそも
; 指しようがない。だから FDISK だけは BIOS のドライブ番号 (80h, 81h ...) で
; 相手を指す。当時からそうなっている。
;
; --- /MBR がコードだけを書き直す理由 ---------------------------------------
;
; LBA 0 の 512 バイトは「コード 446 + パーティションテーブル 64 + 55AA 2」。
; テーブルを一緒に書き換えると、そのディスクの中身が全部行方不明になる。
; だから先頭 446 バイトだけを差し替える。本物の FDISK /MBR も同じ。
; ============================================================================
        cpu     386
        bits    16
        org     0x100

SECTOR_SIZE     equ 512
MBR_CODE_SIZE   equ 446         ; パーティションテーブルの手前まで

start:
        mov     sp, stack_top
        mov     bx, stack_top
        add     bx, 15
        mov     cl, 4
        shr     bx, cl
        mov     ah, 0x4A
        int     0x21

        mov     byte [drive], 0x80
        call    parse_cmdline
        jc      .usage

        call    read_mbr
        jc      .read_err

        cmp     byte [op], 'S'
        je      .status
        cmp     byte [op], 'M'
        je      .mbr
        jmp     .usage

.status:
        call    show_table
        jmp     .out

.mbr:
        ; 先頭 446 バイトだけを差し替え、テーブルと 55AA は残す
        push    ds
        pop     es
        mov     si, mbr_image
        mov     di, secbuf
        mov     cx, MBR_CODE_SIZE
        rep     movsb
        mov     word [secbuf + 0x1FE], 0xAA55

        call    write_mbr
        jc      .write_err
        mov     dx, msg_mbr_done
        call    puts
        call    show_table
.out:
        mov     ax, 0x4C00
        int     0x21

.usage:
        mov     dx, msg_usage
        call    puts
        jmp     .fail
.read_err:
        mov     dx, msg_read_err
        call    puts
        jmp     .fail
.write_err:
        mov     dx, msg_write_err
        call    puts
.fail:
        mov     ax, 0x4C01
        int     0x21

; ============================================================================
; コマンドライン
; ============================================================================
parse_cmdline:
        mov     si, 0x81
        movzx   cx, byte [0x80]
.next:
        call    skip_space
        jcxz    .done
        lodsb
        dec     cx
        cmp     al, '/'
        jne     .bad
        jcxz    .bad
        lodsb
        dec     cx
        call    upcase

        cmp     al, 'S'
        je      .op_here
        cmp     al, 'M'
        je      .op_here
        cmp     al, 'D'
        je      .opt_d
        jmp     .bad
.op_here:
        mov     [op], al
        call    skip_word
        jmp     .next
.opt_d:
        ; "/D:80" のように 16 進で書く
        call    skip_word_start
        jcxz    .bad
        lodsb
        dec     cx
        cmp     al, ':'
        jne     .bad
        xor     bx, bx
.dloop:
        jcxz    .dset
        mov     al, [si]
        cmp     al, ' '
        je      .dset
        cmp     al, 13
        je      .dset
        inc     si
        dec     cx
        call    upcase
        sub     al, '0'
        cmp     al, 9
        jbe     .digit
        sub     al, 7
.digit:
        shl     bx, 4
        movzx   ax, al
        or      bx, ax
        jmp     .dloop
.dset:
        mov     [drive], bl
        jmp     .next
.done:
        cmp     byte [op], 0
        je      .bad
        clc
        ret
.bad:
        stc
        ret

; 語の残りを読み飛ばす (':' で始まる指定は残す)
skip_word:
        jcxz    .out
.loop:
        mov     al, [si]
        cmp     al, ' '
        je      .out
        cmp     al, 13
        je      .out
        inc     si
        dec     cx
        jnz     .loop
.out:
        ret
skip_word_start:
        ret

skip_space:
        jcxz    .out
.loop:
        cmp     byte [si], ' '
        jne     .out
        inc     si
        dec     cx
        jnz     .loop
.out:
        ret

upcase:
        cmp     al, 'a'
        jb      .out
        cmp     al, 'z'
        ja      .out
        sub     al, 0x20
.out:
        ret

; ============================================================================
; LBA 0 を読む / 書く
;
; LBA 0 は必ずシリンダ 0 / ヘッド 0 / セクタ 1 なので、CHS で確実に届く。
; 拡張読み込みの有無を気にしなくてよい唯一のセクタ。
; ============================================================================
read_mbr:
        push    es
        push    ds
        pop     es
        mov     bx, secbuf
        mov     dl, [drive]
        mov     dh, 0
        mov     cx, 0x0001              ; シリンダ 0, セクタ 1
        mov     ax, 0x0201
        int     0x13
        pop     es
        ret

write_mbr:
        push    es
        push    ds
        pop     es
        mov     bx, secbuf
        mov     dl, [drive]
        mov     dh, 0
        mov     cx, 0x0001
        mov     ax, 0x0301
        int     0x13
        pop     es
        ret

; ============================================================================
; パーティションテーブルを表示する
; ============================================================================
show_table:
        cmp     word [secbuf + 0x1FE], 0xAA55
        je      .have_sig
        mov     dx, msg_no_sig
        call    puts
        ret
.have_sig:
        mov     dx, msg_head
        call    puts

        mov     si, secbuf + 0x1BE
        mov     byte [.n], 1
.loop:
        ; 番号
        mov     dl, ' '
        call    putc
        movzx   ax, byte [.n]
        call    put_dec
        mov     dl, ' '
        call    putc

        ; 起動可能の印
        cmp     byte [si], 0x80
        jne     .not_active
        mov     dl, 'A'
        call    putc
        jmp     .type
.not_active:
        mov     dl, ' '
        call    putc
.type:
        mov     dl, ' '
        call    putc
        mov     al, [si + 4]            ; 種別
        call    put_hex8
        mov     dx, msg_sp
        call    puts

        mov     eax, [si + 8]           ; 開始 LBA
        call    put_dec32
        mov     dx, msg_sp
        call    puts
        mov     eax, [si + 12]          ; セクタ数
        shr     eax, 11                 ; 512 バイト単位 → MB
        call    put_dec32
        mov     dx, msg_mb
        call    puts

        add     si, 16
        inc     byte [.n]
        cmp     byte [.n], 5
        jb      .loop
        ret
.n:     db 0

; ============================================================================
; 出力
; ============================================================================
puts:
        push    ax
        mov     ah, 0x09
        int     0x21
        pop     ax
        ret

putc:
        push    ax
        mov     ah, 0x02
        int     0x21
        pop     ax
        ret

put_hex8:
        push    ax
        push    cx
        mov     cl, 4
        push    ax
        shr     al, cl
        call    .nib
        pop     ax
        and     al, 0x0F
        call    .nib
        pop     cx
        pop     ax
        ret
.nib:
        cmp     al, 10
        jb      .dig
        add     al, 7
.dig:
        add     al, '0'
        mov     dl, al
        call    putc
        ret

put_dec:
        movzx   eax, ax
; そのまま下へ
put_dec32:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     ebx, 10
        xor     cx, cx
.split:
        xor     edx, edx
        div     ebx
        push    dx
        inc     cx
        test    eax, eax
        jnz     .split
.emit:
        pop     dx
        add     dl, '0'
        call    putc
        loop    .emit
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

; ============================================================================
; データ
; ============================================================================
msg_usage:     db 'FDISK /STATUS | /MBR [/D:hh]', 13, 10
               db '  /STATUS  show the partition table', 13, 10
               db '  /MBR     rewrite the master boot record code (keeps the table)', 13, 10
               db '  /D:hh    BIOS drive number in hex (default 80)', 13, 10, '$'
msg_read_err:  db 'FDISK: cannot read the master boot record', 13, 10, '$'
msg_write_err: db 'FDISK: cannot write the master boot record', 13, 10, '$'
msg_no_sig:    db 'FDISK: no 55AA signature - this disk has no partition table', 13, 10, '$'
msg_mbr_done:  db 'Master boot record written.', 13, 10, '$'
msg_head:      db 13, 10, ' #  A Type  Start LBA  Size', 13, 10, '$'
msg_sp:        db '  $'
msg_mb:        db ' MB', 13, 10, '$'

op:            db 0
drive:         db 0x80

; 書き込む MBR の中身。boot/mbr.asm をそのまま抱えている。
mbr_image:
               incbin "build/mbr.bin"

               align 2
secbuf:        times SECTOR_SIZE db 0
               times 512 db 0
stack_top:
