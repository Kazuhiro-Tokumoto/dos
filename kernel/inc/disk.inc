; ============================================================================
; disk.inc  -  セクタ入出力 (INT 13h)
;
; LBA は 32bit で扱う。フロッピーだけなら 16bit で足りるが、ハードディスクの
; パーティションは軽く 65536 セクタを超えるので、ここを 16bit にしたままだと
; 32MB の壁がそのまま残る。
;
; 転送方式は 2 通り:
;   CHS (AH=02h/03h)   どの BIOS にもある。フロッピーはこちら。
;   LBA (AH=42h/43h)   INT 13h 拡張。あるかどうかを AH=41h で確かめてから使う。
; CHS はシリンダ番号を 10bit しか渡せず 8GB あたりで頭打ちになるので、
; 使えるなら LBA を選ぶ。
;
; ドライブごとの諸元 (ジオメトリ、パーティションの開始位置、拡張の可否) は
; drive.inc の表から引く。ここはその表に従って転送するだけ。
; ============================================================================

; ---------------------------------------------------------------------------
; disk_detect_ext - そのドライブが INT 13h 拡張に対応しているか調べる
;   入力: DL = BIOS のドライブ番号
;   出力: CF=0 なら対応している
; ---------------------------------------------------------------------------
disk_detect_ext:
        push    ax
        push    bx
        push    cx
        mov     ah, 0x41
        mov     bx, 0x55AA
        int     0x13
        jc      .no
        cmp     bx, 0xAA55              ; 対応していれば入れ替わって返る
        jne     .no
        test    cl, 1                   ; bit0 = セクタ単位の読み書きに対応
        jz      .no
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.no:
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

; ---------------------------------------------------------------------------
; disk_geometry - BIOS にジオメトリを聞く
;   入力: DL = BIOS のドライブ番号
;   出力: CF=0 なら CX = セクタ/トラック, DX = ヘッド数
; ---------------------------------------------------------------------------
disk_geometry:
        push    ax
        push    bx
        push    di
        push    es
        xor     ax, ax
        mov     es, ax
        xor     di, di                  ; 一部の BIOS 対策で ES:DI を 0 に
        mov     ah, 0x08
        int     0x13
        jc      .fail
        movzx   bx, dh
        inc     bx                      ; ヘッド数 = 最大ヘッド番号 + 1
        and     cl, 0x3F                ; セクタ/トラックは下位 6bit
        movzx   cx, cl
        jcxz    .fail
        mov     dx, bx
        pop     es
        pop     di
        pop     bx
        pop     ax
        clc
        ret
.fail:
        pop     es
        pop     di
        pop     bx
        pop     ax
        stc
        ret

; ---------------------------------------------------------------------------
; disk_read - LBA から連続したセクタを読む
;   入力: EAX = 開始 LBA (そのドライブの先頭から), CX = セクタ数,
;         ES:BX = 転送先, DL = ドライブ番号 (0 = A:)
;   出力: CF=1 なら失敗。全レジスタは保存される
; ---------------------------------------------------------------------------
disk_read:
        push    si
        xor     si, si                  ; 0 = 読み込み
        call    disk_xfer
        pop     si
        ret

; ---------------------------------------------------------------------------
; disk_write - LBA へ連続したセクタを書く
; ---------------------------------------------------------------------------
disk_write:
        push    si
        mov     si, 1                   ; 1 = 書き込み
        call    disk_xfer
        pop     si
        ret

; ---------------------------------------------------------------------------
; disk_xfer - 読み書き共通の本体
;   入力: EAX = LBA, CX = セクタ数, ES:BX = 転送先/元, DL = ドライブ, SI = 向き
; ---------------------------------------------------------------------------
disk_xfer:
        pushad
        push    es

        ; ドライブの諸元を引いて、パーティションの先頭を足す
        push    eax
        mov     al, dl
        call    drive_entry             ; DI = そのドライブの情報
        pop     eax
        jc      .fail

        ; DEVICE= で入れたブロックドライバが持つドライブは、BIOS の
        ; INT 13h では読めない。ここで振り分けずに下へ落ちると、
        ; ドライバのユニット番号がそのまま BIOS のドライブ番号として
        ; 使われてしまう。ユニット 0 のラムディスクなら「BIOS の 0 番」=
        ; 起動フロッピーになり、相手のセクタ 1 (= このディスクでは
        ; Stage2 の置き場所) を書き潰す。
        test    byte [di + DRV_FLAGS], DRVF_EXTERNAL
        jnz     .external

        add     eax, [di + DRV_PARTLBA]
        mov     [.lba], eax
        mov     [.count], cx
        mov     [.dir], si
        mov     al, [di + DRV_PHYS]
        mov     [.phys], al
        mov     al, [di + DRV_USEEXT]
        mov     [.useext], al
        mov     ax, [di + DRV_SECPERTRK]
        mov     [.spt], ax
        mov     ax, [di + DRV_HEADS]
        mov     [.heads], ax

.loop:
        cmp     word [.count], 0
        je      .done
        mov     eax, [.lba]
        cmp     byte [.useext], 0
        jne     .ext
        call    disk_chs_one
        jmp     .check
.ext:
        call    disk_lba_one
.check:
        jc      .fail
        add     bx, SECTOR_SIZE
        jnc     .no_wrap
        mov     ax, es                  ; オフセットが 64KB を回った
        add     ax, 0x1000
        mov     es, ax
.no_wrap:
        inc     dword [.lba]
        dec     word [.count]
        jmp     .loop
.done:
        pop     es
        popad
        clc
        ret
.external:
        call    drive_dev_xfer
        jc      .fail
        pop     es
        popad
        clc
        ret
.fail:
        pop     es
        popad
        stc
        ret
.lba:    dd 0
.count:  dw 0
.dir:    dw 0
.phys:   db 0
.useext: db 0
.spt:    dw 0
.heads:  dw 0

; ---------------------------------------------------------------------------
; disk_chs_one - 1 セクタを CHS で転送する (5 回までリトライ)
;   入力: EAX = ディスク全体での LBA, ES:BX = 転送先/元
;   出力: CF=1 なら失敗
;
; LBA から CHS への変換:
;   sector   = LBA mod secs_per_track + 1   (セクタ番号は 1 起算)
;   head     = (LBA / secs_per_track) mod heads
;   cylinder = (LBA / secs_per_track) / heads
; ---------------------------------------------------------------------------
disk_chs_one:
        push    eax
        push    ecx
        push    edx
        push    di

        movzx   ecx, word [disk_xfer.spt]
        xor     edx, edx
        div     ecx                     ; EAX = LBA/spt, EDX = セクタ番号-1
        mov     [.sector], dl
        movzx   ecx, word [disk_xfer.heads]
        xor     edx, edx
        div     ecx                     ; EAX = シリンダ, EDX = ヘッド

        cmp     eax, 1023
        ja      .too_big                ; CHS では届かない範囲

        mov     [.head], dl

        ; CH = シリンダ下位 8bit, CL = シリンダ上位 2bit + セクタ番号
        mov     ch, al
        mov     cl, ah
        and     cl, 0x03
        shl     cl, 6
        mov     dl, [.sector]
        inc     dl                      ; セクタ番号は 1 起算
        or      cl, dl

        mov     dh, [.head]
        mov     dl, [disk_xfer.phys]

        mov     di, 5                   ; リトライ回数
.retry:
        mov     ah, [disk_xfer.dir]
        add     ah, 2                   ; 0 → 02h (読み), 1 → 03h (書き)
        mov     al, 1
        int     0x13
        jnc     .ok

        ; 失敗したらリセットを挟む。実機のフロッピーはモータの回転待ちで
        ; 初回が失敗することがある。
        push    ax
        push    cx
        xor     ax, ax
        mov     dl, [disk_xfer.phys]
        int     0x13
        pop     cx
        pop     ax
        mov     dl, [disk_xfer.phys]
        dec     di
        jnz     .retry
        stc
        jmp     .out
.ok:
        clc
.out:
        pop     di
        pop     edx
        pop     ecx
        pop     eax
        ret
.too_big:
        pop     di
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret
.sector: db 0
.head:   db 0

; ---------------------------------------------------------------------------
; disk_lba_one - 1 セクタを INT 13h 拡張 (LBA) で転送する
;   入力: EAX = ディスク全体での LBA, ES:BX = 転送先/元
;
; ディスクアドレスパケットを組んで DS:SI で渡す。CHS のシリンダ 10bit と
; いう枷が無いので、大きなディスクはこちらでしか届かない。
; ---------------------------------------------------------------------------
disk_lba_one:
        push    ax
        push    cx
        push    dx
        push    si
        push    ds

        push    cs
        pop     ds

        mov     byte [dap_size], 16
        mov     byte [dap_zero], 0
        mov     word [dap_count], 1
        mov     [dap_off], bx
        mov     [dap_seg], es
        mov     [dap_lba], eax
        mov     dword [dap_lba + 4], 0

        mov     cx, 3                   ; リトライ回数
.retry:
        mov     si, dap
        mov     ah, [disk_xfer.dir]
        add     ah, 0x42                ; 0 → 42h (読み), 1 → 43h (書き)
        xor     al, al
        mov     dl, [disk_xfer.phys]
        int     0x13
        jnc     .ok
        push    ax
        xor     ax, ax
        mov     dl, [disk_xfer.phys]
        int     0x13                    ; リセット
        pop     ax
        loop    .retry
        stc
        jmp     .out
.ok:
        clc
.out:
        pop     ds
        pop     si
        pop     dx
        pop     cx
        pop     ax
        ret

; --- ディスクアドレスパケット (INT 13h 拡張が読む形) ---
                align 4
dap:
dap_size:       db 16
dap_zero:       db 0
dap_count:      dw 1
dap_off:        dw 0
dap_seg:        dw 0
dap_lba:        dd 0, 0

; ---------------------------------------------------------------------------
; disk_read_unit / disk_write_unit - ブロックデバイスドライバから呼ばれる入口
;   入力: EAX = LBA, CX = セクタ数, ES:BX = バッファ, [blk_unit] = ユニット
; ---------------------------------------------------------------------------
disk_read_unit:
        push    dx
        mov     dl, [blk_unit]
        call    disk_read
        pop     dx
        ret

disk_write_unit:
        push    dx
        mov     dl, [blk_unit]
        call    disk_write
        pop     dx
        ret
