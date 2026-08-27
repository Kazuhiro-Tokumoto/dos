; ============================================================================
; hellox.asm  -  .EXE のリロケーションが効いているかを確かめるプログラム
;
; tools/mkexe.py に食わせて MZ 形式にする。先頭 8 バイトはそのための表。
;
; 確認していること:
;   far_ptr のセグメント部分はファイル上では 0 になっている。DOS のローダが
;   リロケーションテーブルを見て、ここに実際のロードセグメントを足しているはず。
;   足されていなければ lds は DS=0 を読み込み、割り込みベクタの中身を
;   文字列だと思って表示してしまう。
; ============================================================================
        cpu     386
        bits    16
        org     0

; --- mkexe.py に渡す表 -----------------------------------------------------
        dw      entry                   ; 初期 IP
        dw      stk_top                 ; 初期 SP
        dw      1                       ; リロケーションの数
        dw      reloc_table             ; リロケーションテーブルの位置

reloc_table:
        dw      far_ptr + 2             ; ここのセグメント値を直してもらう

; ---------------------------------------------------------------------------
entry:
        ; .EXE の入口では DS と ES が PSP を指している。自分のデータは
        ; CS と同じセグメントにあるので、まず DS を移す。
        mov     ax, cs
        mov     ds, ax

        mov     dx, msg_start
        mov     ah, 0x09
        int     0x21

        ; --- リロケーションが当たっているか、値そのものを見て確かめる ---
        mov     ax, [far_ptr + 2]
        mov     bx, cs
        cmp     ax, bx
        jne     .failed

        ; --- 実際に far ポインタとして使ってみる ---
        push    ds
        lds     si, [cs:far_ptr]        ; DS:SI = リロケーション済みの far ポインタ
        mov     dx, si
        mov     ah, 0x09
        int     0x21
        pop     ds

        mov     dx, msg_pass
        mov     ah, 0x09
        int     0x21
        mov     ax, 0x4C00
        int     0x21

.failed:
        mov     dx, msg_fail
        mov     ah, 0x09
        int     0x21
        mov     ax, 0x4C01              ; 終了コード 1
        int     0x21

; ---------------------------------------------------------------------------
msg_start:  db 'HELLO.EXE started', 13, 10, '$'
msg_via:    db 'text reached through a relocated far pointer', 13, 10, '$'
msg_pass:   db 'EXE-RELOC: PASS', 13, 10, '$'
msg_fail:   db 'EXE-RELOC: FAIL', 13, 10, '$'

; ファイル上ではセグメント部分が 0。ローダがロードセグメントを足す。
align 2
far_ptr:    dw msg_via
            dw 0

            times 256 db 0
stk_top:
