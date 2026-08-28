; ============================================================================
; pipetest.asm  -  標準入力を読むだけのフィルタ
;
; シェルのリダイレクトとパイプを確かめるための相手役。
; DOS のプログラムはハンドル 0 をそのまま読むだけで、自分が
; リダイレクトされていることを知らない。シェルが起動前にハンドルを
; 差し替えているだけ、という約束が守れているかを見る。
;
;   TYPE README.TXT | PIPETEST   → 1 行目が返ってくる
;   PIPETEST < README.TXT        → 同じ
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

        ; 標準入力を読めるだけ読む
        xor     bx, bx                  ; ハンドル 0
        mov     cx, IN_MAX
        mov     dx, in_buf
        mov     ah, 0x3F
        int     0x21
        jc      .no_input
        test    ax, ax
        jz      .no_input
        mov     [in_len], ax

        mov     si, msg_got
        call    puts

        ; 最初の 1 行だけ出す
        mov     si, in_buf
        mov     cx, [in_len]
.line:
        jcxz    .done
        lodsb
        cmp     al, 13
        je      .done
        cmp     al, 10
        je      .done
        call    putc
        dec     cx
        jmp     .line
.done:
        call    newline
        mov     ax, 0x4C00
        int     0x21

.no_input:
        mov     si, msg_none
        call    puts
        mov     ax, 0x4C01
        int     0x21

putc:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     [char_buf], al
        mov     dx, char_buf
        mov     cx, 1
        mov     bx, 1
        mov     ah, 0x40
        int     0x21
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

puts:
        push    ax
        push    si
.loop:
        lodsb
        test    al, al
        jz      .out
        call    putc
        jmp     .loop
.out:
        pop     si
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

IN_MAX   equ 1024

msg_got:  db 'PIPE-GOT: ', 0
msg_none: db 'PIPE-EMPTY', 13, 10, 0

in_len:   dw 0
char_buf: db 0
in_buf:   times IN_MAX db 0

          times 512 db 0
stack_top:
