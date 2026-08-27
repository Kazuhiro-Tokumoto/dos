; ============================================================================
; tsrtest.asm  -  常駐終了 (AH=31h) が本当に常駐するかを確かめる
;
; 1 回目の起動で INT 60h を横取りして常駐し、2 回目の起動で
; 「さっき仕掛けたハンドラがまだ生きているか」を確かめる。
;
; これが通るということは:
;   * AH=31h がメモリを解放せずに残している
;   * 残したブロックが次のプログラムのロードで踏まれていない
;   * PSP から書き戻される INT 22h/23h/24h 以外のベクタが保たれている
; の 3 つが同時に成り立っているということ。常駐ソフトが動く条件そのもの。
; ============================================================================
        cpu     386
        bits    16
        org     0x100

        jmp     main

; ---------------------------------------------------------------------------
; ここから resident_end までがメモリに残る部分
; ---------------------------------------------------------------------------
signature:
        db      'MYTS'                  ; ハンドラの 4 バイト手前に置く目印

handler:
        ; AH=00h: 生存確認。AX に決まった値を返すだけ。
        cmp     ah, 0
        jne     .pass_through
        mov     ax, 0x1234
        iret
.pass_through:
        mov     ax, 0xFFFF
        iret

resident_end:

; ---------------------------------------------------------------------------
main:
        ; INT 60h のベクタを見て、もう入っているかを調べる
        mov     ax, 0x3560
        int     0x21                    ; ES:BX = 現在のハンドラ

        cmp     bx, 4
        jb      .install                ; 4 バイト手前が読めないなら未導入
        cmp     dword [es:bx - 4], 'MYTS'
        je      .already

.install:
        mov     si, msg_install
        call    puts

        ; INT 60h を自分のハンドラに向ける
        mov     dx, handler
        mov     ax, 0x2560
        int     0x21

        ; 常駐部分だけ残して終了する
        mov     dx, resident_end
        add     dx, 15
        mov     cl, 4
        shr     dx, cl                  ; DX = 残すパラグラフ数
        mov     ax, 0x3100
        int     0x21

.already:
        mov     si, msg_found
        call    puts

        ; 仕掛けたハンドラを実際に呼んでみる
        xor     ah, ah
        int     0x60
        cmp     ax, 0x1234
        jne     .bad

        mov     si, msg_pass
        call    puts
        mov     ax, 0x4C00
        int     0x21
.bad:
        mov     si, msg_fail
        call    puts
        mov     ax, 0x4C01
        int     0x21

; ---------------------------------------------------------------------------
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

msg_install: db 'TSR: hooking INT 60h and going resident', 13, 10, 0
msg_found:   db 'TSR: resident copy detected', 13, 10, 0
msg_pass:    db 'TSR-RESIDENT: PASS', 13, 10, 0
msg_fail:    db 'TSR-RESIDENT: FAIL', 13, 10, 0
