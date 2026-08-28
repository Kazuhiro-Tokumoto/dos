; ============================================================================
; instest.asm  -  CONFIG.SYS の INSTALL= から起動されるプログラム
;
; INSTALL= は COMMAND.COM より先に走る。環境変数もカレントディレクトリも
; まだ整っていないので、当時も常駐ソフトの類しか置かなかった。
; ここでは「本当に走ったか」と「引数が渡っているか」だけを見せて終わる。
; ============================================================================
        cpu     386
        bits    16
        org     0x100

start:
        mov     dx, msg
        mov     ah, 0x09
        int     0x21

        ; PSP:0080 のコマンドテイルをそのまま出す
        mov     si, 0x81
        movzx   cx, byte [0x80]
        jcxz    .no_args
.loop:
        lodsb
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        loop    .loop
.no_args:
        mov     dx, msg_end
        mov     ah, 0x09
        int     0x21

        mov     ax, 0x4C00
        int     0x21

msg:     db 'INSTALL: instest ran with args:$'
msg_end: db 13, 10, '$'
