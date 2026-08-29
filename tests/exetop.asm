; ============================================================================
; exetop.asm  -  .EXE に渡されるメモリの量を確かめるプログラム
;
; tools/mkexe.py に食わせて MZ 形式にする。先頭 8 バイトはそのための表。
;
; 確認していること:
;   DOS は .EXE の場所を「読み込むバイト数」ではなく「ページ数」から数える。
;
;       パラグラフ数 = ページ数 * 32 - ヘッダのパラグラフ数 + 16 + min_alloc
;
;   最終ページが半端でも切り上げるのが正しい。ここを実バイト数で計算すると、
;   半端なぶんだけブロックが小さくなり、PSP:0002 が本来より下を指す。
;   そこを上限に自分のスタックを置く常駐ソフト (当時の定石) が、すぐ上の
;   MCB を踏んで連鎖を壊す。CuteMouse がまさにこれだった。
;
;   本体はちょうど 1040 バイトに揃えてある。ヘッダ 32 バイトと合わせて
;   1072 バイト = 3 ページ (最終ページは 48 バイトだけ使用) になるので、
;
;       3 * 32 - 2 + 16 + 0x20 = 142 パラグラフ
;
;   が正解。実バイト数で数えると 113 パラグラフになり、この違いで落ちる。
; ============================================================================
        cpu     386
        bits    16
        org     0

BODY_SIZE   equ 1040                    ; 本体の大きさ (Makefile と揃えること)
EXPECT      equ 142                     ; 渡されるはずのパラグラフ数

; --- mkexe.py に渡す表 -----------------------------------------------------
        dw      entry                   ; 初期 IP
        dw      stk_top                 ; 初期 SP
        dw      0                       ; リロケーションなし
        dw      0

; ---------------------------------------------------------------------------
entry:
        ; .EXE の入口では ES が PSP を指している。PSP:0002 に入っている
        ; 「ブロックの直後のセグメント」との差が、渡されたパラグラフ数。
        mov     ax, [es:0x0002]
        mov     bx, es
        sub     ax, bx
        mov     bx, cs
        mov     ds, bx                  ; 自分のデータは CS 側にある
        mov     [got], ax

        mov     dx, msg_head
        mov     ah, 0x09
        int     0x21
        mov     ax, [got]
        call    put_hex
        mov     dx, msg_crlf
        mov     ah, 0x09
        int     0x21

        mov     ax, [got]
        cmp     ax, EXPECT
        jb      .failed                 ; 少なすぎる (ページ切り上げをしていない)
        cmp     ax, EXPECT + 2
        ja      .failed                 ; 多すぎる (max_alloc を見ていない)

        mov     dx, msg_pass
        mov     ah, 0x09
        int     0x21
        mov     ax, 0x4C00
        int     0x21

.failed:
        mov     dx, msg_fail
        mov     ah, 0x09
        int     0x21
        mov     ax, 0x4C01
        int     0x21

; --- AX を 4 桁の 16 進で表示する -------------------------------------------
put_hex:
        push    ax
        mov     cx, 4
.loop:
        rol     ax, 4
        push    ax
        and     al, 0x0F
        cmp     al, 10
        jb      .digit
        add     al, 'A' - 10 - '0'
.digit:
        add     al, '0'
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        pop     ax
        loop    .loop
        pop     ax
        ret

; ---------------------------------------------------------------------------
msg_head:   db 'EXE-MEMTOP paragraphs: $'
msg_crlf:   db 13, 10, '$'
msg_pass:   db 'EXE-MEMTOP: PASS', 13, 10, '$'
msg_fail:   db 'EXE-MEMTOP: FAIL', 13, 10, '$'
got:        dw 0

; 本体をちょうど BODY_SIZE バイトに揃える。ここが変わるとページ数が変わり、
; EXPECT も変わってしまうので、足すときは EXPECT も直すこと。
            times BODY_SIZE - 2 - ($ - $$) db 0
stk_top:
            dw 0
