; ============================================================================
; buffer.inc  -  ディスクバッファ (CONFIG.SYS の BUFFERS= の実体)
;
; DOS はセクタを読むたびにディスクを叩くのではなく、直前に触ったセクタを
; いくつか手元に残しておく。ディレクトリを辿るときや FAT を引くときに
; 同じセクタを何度も読むので、これが効く。
;
; バッファは連鎖していて、List of Lists の +12h がその先頭を指す。
; 当時のキャッシュツール (SMARTDRV の類) はここを見て、自分が横取りする
; べきかどうかを判断した。
;
; 入れ替えは「いちばん長く使っていないもの」から。書き換えられていた
; バッファは追い出す前にディスクへ戻す。
; ============================================================================

MAX_BUFFERS     equ 20
BUF_FREE        equ 0xFF        ; BUF_DRIVE がこれなら未使用
BUFF_DIRTY      equ 0x01        ; BUF_FLAGS: 書き換えられている

; ---------------------------------------------------------------------------
; buf_init - バッファを連鎖させる
;   入力: AL = 使うバッファの数
; ---------------------------------------------------------------------------
buf_init:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si

        cmp     al, 1
        jae     .ok_min
        mov     al, 1
.ok_min:
        cmp     al, MAX_BUFFERS
        jbe     .ok_max
        mov     al, MAX_BUFFERS
.ok_max:
        mov     [buf_count], al
        movzx   cx, al

        mov     si, buf_first
        mov     word [buf_tick], 0
.loop:
        mov     byte [si + BUF_DRIVE], BUF_FREE
        mov     byte [si + BUF_FLAGS], 0
        mov     dword [si + BUF_SECTOR], 0xFFFFFFFF
        mov     word [si + BUF_USE], 0
        mov     word [si + BUF_DPB], dpb
        mov     ax, cs
        mov     [si + BUF_DPB + 2], ax

        ; 次のバッファへ繋ぐ
        mov     bx, si
        add     bx, BUF_HDRSIZE + SECTOR_SIZE
        dec     cx
        jz      .last
        mov     [si + BUF_NEXT], bx
        mov     [si + BUF_NEXT + 2], ax
        mov     si, bx
        inc     cx
        loop    .loop
.last:
        mov     word [si + BUF_NEXT], 0xFFFF
        mov     word [si + BUF_NEXT + 2], 0xFFFF

        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; buf_find - すでに載っているセクタを探す
;   入力: AX = LBA, DL = ドライブ番号
;   出力: CF=0 なら SI = バッファヘッダ
; ---------------------------------------------------------------------------
buf_find:
        push    cx
        push    bx
        movzx   cx, byte [buf_count]
        mov     si, buf_first
.loop:
        cmp     [si + BUF_DRIVE], dl
        jne     .next
        cmp     [si + BUF_SECTOR], ax
        jne     .next
        cmp     word [si + BUF_SECTOR + 2], 0
        jne     .next
        ; 見つかった。使った印を新しくする
        inc     word [buf_tick]
        mov     bx, [buf_tick]
        mov     [si + BUF_USE], bx
        pop     bx
        pop     cx
        clc
        ret
.next:
        add     si, BUF_HDRSIZE + SECTOR_SIZE
        loop    .loop
        pop     bx
        pop     cx
        stc
        ret

; ---------------------------------------------------------------------------
; buf_victim - 追い出す相手を選ぶ (空きが無ければ最も長く使っていないもの)
;   出力: SI = バッファヘッダ。書き換えられていたら先に書き戻す
; ---------------------------------------------------------------------------
buf_victim:
        push    ax
        push    bx
        push    cx
        push    dx

        movzx   cx, byte [buf_count]
        mov     si, buf_first
        mov     bx, si                  ; BX = いまの候補
        mov     dx, 0xFFFF              ; DX = 候補の使用時刻
.loop:
        cmp     byte [si + BUF_DRIVE], BUF_FREE
        je      .take_free
        mov     ax, [si + BUF_USE]
        cmp     ax, dx
        jae     .next
        mov     dx, ax
        mov     bx, si
.next:
        add     si, BUF_HDRSIZE + SECTOR_SIZE
        loop    .loop
        mov     si, bx
        jmp     .flush
.take_free:
        ; 空きがそのまま使える
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret
.flush:
        ; 書き換えられていたら追い出す前に戻す
        test    byte [si + BUF_FLAGS], BUFF_DIRTY
        jz      .done
        call    buf_writeback
.done:
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; buf_writeback - 1 つのバッファをディスクへ戻す
;   入力: SI = バッファヘッダ
; ---------------------------------------------------------------------------
buf_writeback:
        push    ax
        push    bx
        push    cx
        push    dx
        push    es

        push    ds
        pop     es
        mov     ax, [si + BUF_SECTOR]
        mov     bx, si
        add     bx, BUF_DATA
        mov     cx, 1
        mov     dl, [si + BUF_DRIVE]
        call    disk_write_drive
        jc      .fail
        and     byte [si + BUF_FLAGS], ~BUFF_DIRTY & 0xFF
.fail:
        pop     es
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; buf_get - セクタをバッファに載せて、その場所を返す
;   入力: AX = LBA, DL = ドライブ番号
;   出力: CF=0 なら SI = バッファヘッダ (中身は SI + BUF_DATA から 512 バイト)
; ---------------------------------------------------------------------------
buf_get:
        push    ax
        push    bx
        push    cx
        push    es

        call    buf_find
        jnc     .hit

        ; 載っていないので読み込む
        call    buf_victim
        push    ds
        pop     es
        mov     bx, si
        add     bx, BUF_DATA
        mov     cx, 1
        call    disk_read_drive
        jc      .fail

        mov     [si + BUF_DRIVE], dl
        mov     [si + BUF_SECTOR], ax
        mov     word [si + BUF_SECTOR + 2], 0
        mov     byte [si + BUF_FLAGS], 0
        inc     word [buf_tick]
        mov     bx, [buf_tick]
        mov     [si + BUF_USE], bx

.hit:
        pop     es
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.fail:
        ; 読めなかったバッファは無効にしておく
        mov     byte [si + BUF_DRIVE], BUF_FREE
        pop     es
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

; ---------------------------------------------------------------------------
; buf_dirty - このバッファは書き換えられた、と印を付ける
;   入力: SI = バッファヘッダ
; ---------------------------------------------------------------------------
buf_dirty:
        or      byte [si + BUF_FLAGS], BUFF_DIRTY
        ret

; ---------------------------------------------------------------------------
; buf_flush - 書き換えられたバッファをすべてディスクへ戻す
;   出力: CF=1 なら 1 つでも失敗した
; ---------------------------------------------------------------------------
buf_flush:
        push    ax
        push    cx
        push    si

        xor     ax, ax                  ; AL = 失敗の有無
        movzx   cx, byte [buf_count]
        mov     si, buf_first
.loop:
        test    byte [si + BUF_FLAGS], BUFF_DIRTY
        jz      .next
        call    buf_writeback
        test    byte [si + BUF_FLAGS], BUFF_DIRTY
        jz      .next
        mov     al, 1                   ; まだ立っている = 書き戻せなかった
.next:
        add     si, BUF_HDRSIZE + SECTOR_SIZE
        loop    .loop

        test    al, al
        jnz     .fail
        pop     si
        pop     cx
        pop     ax
        clc
        ret
.fail:
        pop     si
        pop     cx
        pop     ax
        stc
        ret

; ---------------------------------------------------------------------------
; buf_invalidate - すべてのバッファを捨てる (メディア交換のときなど)
; ---------------------------------------------------------------------------
buf_invalidate:
        push    cx
        push    si
        call    buf_flush
        movzx   cx, byte [buf_count]
        mov     si, buf_first
.loop:
        mov     byte [si + BUF_DRIVE], BUF_FREE
        mov     byte [si + BUF_FLAGS], 0
        add     si, BUF_HDRSIZE + SECTOR_SIZE
        loop    .loop
        pop     si
        pop     cx
        ret

; --- 変数 ------------------------------------------------------------------
buf_count:      db DEFAULT_BUFFERS
buf_tick:       dw 0                    ; 使用順を決めるための通し番号
buf_first:      times MAX_BUFFERS * (BUF_HDRSIZE + SECTOR_SIZE) db 0
