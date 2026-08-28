; ============================================================================
; mbr.asm  -  マスターブートレコード (ハードディスクの LBA 0)
;
; ハードディスクの先頭 1 セクタは、フロッピーと違って「ファイルシステムの
; 先頭」ではない。ディスクを何枚かの区画に切るための表 (パーティション
; テーブル) と、そこから 1 つ選んで起動を渡すだけの小さなコードが入っている。
;
;   0x000-0x1BD  コード
;   0x1BE-0x1FD  パーティションテーブル (16 バイト x 4)
;   0x1FE-0x1FF  55 AA
;
; パーティションの 1 つが「起動可能」(先頭バイトが 80h) の印を持っていて、
; MBR はそれを探し、その区画の先頭セクタ — つまり MYDOS の Stage1 — を
; 0x7C00 に読み込んで飛ぶ。BIOS がフロッピーに対してするのと同じことを、
; ソフトウェアでもう一段やっている形。
;
; --- 自分を 0x600 へ動かすのはなぜか ---------------------------------------
;
; BIOS は MBR を 0x7C00 に読み込む。ところが次に読み込む区画のブートセクタも
; 0x7C00 に置く決まりになっている (そこに置かれる前提で書かれているため)。
; そのままでは自分自身を上書きしてしまうので、先に 0x600 へ退避してから
; そちらへ飛ぶ。当時の MBR はどれもこれをやっている。
;
; --- 飛ぶときの約束 ---------------------------------------------------------
;
;   DL    = BIOS のドライブ番号 (BIOS が渡してきたものをそのまま)
;   DS:SI = 選んだパーティションテーブルの項目
;
; 後者を見るブートセクタが実在するので、崩さずに渡す。
; ============================================================================
        cpu     386
        bits    16
        org     0x600

RELOC           equ 0x0600      ; 自分を動かす先
BOOTSEC         equ 0x7C00      ; BIOS が読み込む場所 / 次を読む場所
PART_TABLE      equ RELOC + 0x1BE

start:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, BOOTSEC     ; スタックは 0x7C00 の下に伸ばす
        sti
        cld

        ; 自分自身を 0x7C00 から 0x600 へ写して、そちらへ飛ぶ。
        ; ここまでのコードは位置に依存しない書き方になっている。
        mov     si, BOOTSEC
        mov     di, RELOC
        mov     cx, 512
        rep     movsb
        jmp     0:relocated

relocated:
        mov     [drive], dl

        ; --- 起動可能な印のついた区画を探す ---
        mov     si, PART_TABLE
        mov     cx, 4
.scan:
        cmp     byte [si], 0x80
        je      .found
        add     si, 16
        loop    .scan
        mov     si, msg_noactive
        jmp     fail
.found:
        mov     [part], si

        ; --- 拡張読み込みが使えるか ---
        mov     ah, 0x41
        mov     bx, 0x55AA
        mov     dl, [drive]
        int     0x13
        jc      .no_lba
        cmp     bx, 0xAA55
        jne     .no_lba
        test    cl, 1
        jz      .no_lba
        mov     byte [use_lba], 1
.no_lba:

        mov     cx, 5                   ; 5 回までリトライ
.retry:
        push    cx
        xor     ax, ax
        mov     dl, [drive]
        int     0x13                    ; ディスクリセット

        mov     si, [part]
        cmp     byte [use_lba], 0
        je      .chs

        ; --- 拡張読み込み: 区画の先頭 LBA をそのまま渡す ---
        mov     eax, [si + 8]
        mov     [dap_lba], eax
        mov     si, dap
        mov     dl, [drive]
        mov     ah, 0x42
        int     0x13
        jmp     .after

        ; --- CHS: 区画テーブルが持っている CHS をそのまま使う ---
        ; テーブルの +1 がヘッド、+2 がセクタとシリンダ上位、+3 がシリンダ下位。
        ; BIOS に渡す形とほぼ同じ並びなので、そのまま移せる。
.chs:
        mov     dh, [si + 1]            ; ヘッド
        mov     cx, [si + 2]            ; CL = セクタ+シリンダ上位, CH = シリンダ
        mov     dl, [drive]
        xor     ax, ax
        mov     es, ax
        mov     bx, BOOTSEC
        mov     ax, 0x0201              ; 1 セクタ読み込み
        int     0x13

.after:
        pop     cx
        jnc     .read_ok
        loop    .retry
        mov     si, msg_readerr
        jmp     fail

.read_ok:
        cmp     word [BOOTSEC + 510], 0xAA55
        jne     .not_boot

        ; 約束どおり DL と DS:SI を整えて渡す
        mov     dl, [drive]
        mov     si, [part]
        jmp     0:BOOTSEC

.not_boot:
        mov     si, msg_notboot
        ; そのまま fail へ

fail:
        call    puts
        mov     si, msg_halt
        call    puts
.hang:
        hlt
        jmp     .hang

; ---------------------------------------------------------------------------
puts:
        push    ax
        push    bx
.loop:
        lodsb
        test    al, al
        jz      .done
        mov     ah, 0x0E
        mov     bx, 0x0007
        int     0x10
        jmp     .loop
.done:
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
msg_noactive:   db 'MBR: no active partition', 13, 10, 0
msg_readerr:    db 'MBR: read error', 13, 10, 0
msg_notboot:    db 'MBR: not a boot sector', 13, 10, 0
msg_halt:       db 'Halted', 13, 10, 0

drive:          db 0
use_lba:        db 0
part:           dw 0

                align 4
dap:            db 0x10, 0              ; 大きさ / 予約
                dw 1                    ; セクタ数
                dw BOOTSEC              ; 転送先オフセット
                dw 0                    ; 転送先セグメント
dap_lba:        dq 0

; --- 0x1BE までを 0 で埋める。そこから先はパーティションテーブルの場所 ---
%if ($ - $$) > 0x1BE
  %error "MBR のコードがパーティションテーブルに食い込んでいる"
%endif
                times 0x1BE - ($ - $$) db 0

; パーティションテーブルは書き込む側 (FDISK) が入れるので、ここでは空。
                times 64 db 0
                dw 0xAA55
