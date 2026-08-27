; ============================================================================
; ovltest.asm  -  AH=4Bh AL=3 (オーバーレイの読み込み) を確かめる
;
; オーバーレイは PSP も作らずメモリも確保しない。呼び出し側が用意した
; セグメントに中身を置いて、指定された係数でリロケーションを当てるだけ。
; 当時の大きめのアプリが、常駐部分から必要な部分だけ呼び出す形で使った。
; ============================================================================
        cpu     386
        bits    16
        org     0x100

start:
        ; 自分のメモリを縮めて、オーバーレイを置く場所を空ける
        mov     sp, stack_top
        mov     bx, stack_top
        add     bx, 15
        mov     cl, 4
        shr     bx, cl
        mov     ah, 0x4A
        int     0x21

        ; オーバーレイ用に 4KB (256 パラグラフ) 確保する
        mov     bx, 256
        mov     ah, 0x48
        int     0x21
        jc      .no_mem
        mov     [ovl_seg], ax

        ; パラメータブロック: 読み込み先セグメントとリロケーション係数。
        ; どちらも同じ値にすると、通常のプログラムと同じ当たり方になる。
        mov     [epb_seg], ax
        mov     [epb_factor], ax

        push    ds
        pop     es
        mov     dx, ovl_path
        mov     bx, epb
        mov     ax, 0x4B03
        int     0x21
        jc      .load_fail

        ; オーバーレイの先頭 word に入口のオフセットが入っている
        mov     es, [ovl_seg]
        mov     ax, [es:0]
        mov     [ovl_entry], ax
        mov     ax, [ovl_seg]
        mov     [ovl_entry + 2], ax

        mov     si, msg_calling
        call    puts

        call    far [ovl_entry]

        cmp     ax, 0x5A5A
        jne     .bad_result

        mov     si, msg_pass
        call    puts
        mov     ax, 0x4C00
        int     0x21

.bad_result:
        mov     si, msg_bad
        call    puts
        jmp     .quit_err
.no_mem:
        mov     si, msg_nomem
        call    puts
        jmp     .quit_err
.load_fail:
        mov     si, msg_loadfail
        call    puts
.quit_err:
        mov     ax, 0x4C01
        int     0x21

puts:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        mov     dx, si
        xor     cx, cx
.len:
        lodsb
        test    al, al
        jz      .go
        inc     cx
        jmp     .len
.go:
        jcxz    .done
        mov     bx, 1
        mov     ah, 0x40
        int     0x21
.done:
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

ovl_path:    db 'OVL.OVL', 0
msg_calling: db 'OVERLAY: loaded, calling into it', 13, 10, 0
msg_pass:    db 'OVERLAY-RELOC: PASS', 13, 10, 0
msg_bad:     db 'OVERLAY-RELOC: FAIL', 13, 10, 0
msg_nomem:   db 'OVERLAY: out of memory', 13, 10, 0
msg_loadfail:db 'OVERLAY: load failed', 13, 10, 0

ovl_seg:     dw 0
ovl_entry:   dd 0

; AL=3 のパラメータブロック
epb:
epb_seg:     dw 0
epb_factor:  dw 0

             times 256 db 0
stack_top:
