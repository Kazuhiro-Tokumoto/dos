; ============================================================================
; drive.inc  -  ドライブ表 (どの物理ドライブの、どこから始まる何なのか)
;
; DOS が持っている DPB は「そのドライブのファイルシステムの形」しか
; 表さない。フロッピー 1 台だけならそれで足りたが、ハードディスクを
; 相手にすると
;   ・BIOS のドライブ番号 (00h か 80h か)
;   ・パーティションの先頭 LBA (C: の LBA 0 はディスクの LBA 63 だったりする)
;   ・CHS で届くのか INT 13h 拡張が要るのか
; を別に覚えておかないと転送先が決まらない。ここがその表で、DPB は
; この表の中に埋め込んである (List of Lists から辿れるのは DPB の側)。
;
; ドライブ文字の割り当ては当時の DOS と同じ:
;   A: / B:  フロッピー (2 台目が無くても B: は予約される)
;   C: 以降  ハードディスクの基本パーティションを順に
;
; いま作業対象になっているドライブの諸元は作業用の変数に載せておく。
; クラスタを 1 つ辿るたびに表を引き直すのは無駄なので、drive_select で
; 載せ替える形にしてある (本物の DOS が「カレント DPB」を持つのと同じ)。
; ============================================================================

; ---------------------------------------------------------------------------
; drive_entry - ドライブ番号から表の位置を求める
;   入力: AL = DOS のドライブ番号 (0 = A:)
;   出力: CF=0 なら DI = 表のエントリ、CF=1 なら存在しない
; ---------------------------------------------------------------------------
drive_entry:
        push    ax
        push    dx
        cmp     al, MAX_PHYS_DRIVES
        jae     .bad
        push    ax
        movzx   di, al
        mov     ax, DRV_ENTSIZE
        mul     di
        mov     di, ax
        pop     ax
        add     di, drive_tab
        test    byte [di + DRV_FLAGS], DRVF_PRESENT
        jz      .bad
        pop     dx
        pop     ax
        clc
        ret
.bad:
        pop     dx
        pop     ax
        stc
        ret

; ---------------------------------------------------------------------------
; drive_slot - 表の位置を求める (実在するかどうかは見ない)
;   入力: AL = ドライブ番号
;   出力: DI = 表のエントリ
; ---------------------------------------------------------------------------
drive_slot:
        push    ax
        push    dx
        movzx   di, al
        mov     ax, DRV_ENTSIZE
        mul     di
        mov     di, ax
        add     di, drive_tab
        pop     dx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; dpb_for_drive - そのドライブの DPB の位置
;   入力: AL = ドライブ番号
;   出力: BX = DPB のオフセット (存在しなければ A: の DPB)
; ---------------------------------------------------------------------------
dpb_for_drive:
        push    di
        call    drive_entry
        jc      .fallback
        mov     bx, di
        add     bx, DRV_DPB
        pop     di
        ret
.fallback:
        mov     bx, drive_tab + DRV_DPB
        pop     di
        ret

; ---------------------------------------------------------------------------
; drive_mark_reload - 全ドライブに「BPB を読み直せ」の印を立てる (AH=0Dh)
; ---------------------------------------------------------------------------
drive_mark_reload:
        push    ax
        push    cx
        push    di
        xor     al, al
        mov     cx, MAX_PHYS_DRIVES
.loop:
        call    drive_slot
        test    byte [di + DRV_FLAGS], DRVF_PRESENT
        jz      .next
        or      byte [di + DRV_FLAGS], DRVF_RELOAD
.next:
        inc     al
        loop    .loop
        pop     di
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; drive_reload_check - DRVF_RELOAD が立っていたらブートセクタを読み直す
;   入力: AL = ドライブ番号
;
; FORMAT のようにディスクの構造を書き換えるプログラムのあと、DOS が古い
; 諸元を信じたままにならないようにするためのもの。印は AH=0Dh で立つ。
; ---------------------------------------------------------------------------
drive_reload_check:
        push    ax
        push    di
        cmp     al, MAX_PHYS_DRIVES
        jae     .out
        call    drive_slot
        test    byte [di + DRV_FLAGS], DRVF_RELOAD
        jz      .out
        and     byte [di + DRV_FLAGS], ~DRVF_RELOAD & 0xFF
        test    byte [di + DRV_FLAGS], DRVF_PRESENT
        jz      .out
        ; DEVICE= で入れたドライバのドライブは自分で諸元を持っているので触らない
        test    byte [di + DRV_FLAGS], DRVF_EXTERNAL
        jnz     .out

        call    drive_read_bpb
        jc      .out
        call    dpb_build
        ; いま載っているのがこのドライブなら、載せ直させる
        cmp     al, [fs_drive]
        jne     .out
        mov     byte [fs_drive], 0xFF
.out:
        pop     di
        pop     ax
        ret

; ---------------------------------------------------------------------------
; drive_select - 作業対象のドライブを切り替える
;   入力: AL = ドライブ番号
;   出力: CF=1 なら存在しないドライブ
;
; 表から作業用の変数へ諸元を写す。FAT12 のドライブなら FAT を丸ごと
; fat_buf へ載せる (FAT12 は最大でも 12 セクタなので常駐させても惜しくない)。
; FAT16 は数百 KB になるのでディスクバッファ越しに引く。
; ---------------------------------------------------------------------------
drive_select:
        push    ax
        push    bx
        push    cx
        push    dx
        push    di

        call    drive_reload_check      ; 印が立っていれば BPB を読み直す

        cmp     al, [fs_drive]
        je      .already

        call    drive_entry
        jc      .bad

        ; 前のドライブの FAT を書き戻してから載せ替える
        push    ax
        call    fat_resident_flush
        pop     ax

        mov     [fs_drive], al

        mov     dx, [di + DRV_SECPERCLUS]
        mov     [sec_per_clus], dx
        mov     dx, [di + DRV_RESERVED]
        mov     [reserved_secs], dx
        mov     dx, [di + DRV_NUMFATS]
        mov     [num_fats], dx
        mov     dx, [di + DRV_ROOTENT]
        mov     [root_entries], dx
        mov     dx, [di + DRV_SECPERFAT]
        mov     [secs_per_fat], dx
        mov     dx, [di + DRV_ROOTLBA]
        mov     [root_lba], dx
        mov     dx, [di + DRV_ROOTSECS]
        mov     [root_secs], dx
        mov     dx, [di + DRV_DATALBA]
        mov     [data_lba], dx
        mov     dx, [di + DRV_MAXCLUS]
        mov     [max_cluster], dx
        mov     dx, [di + DRV_CLUSBYTES]
        mov     [clus_bytes], dx
        mov     dx, [di + DRV_SECPERTRK]
        mov     [secs_per_track], dx
        mov     dx, [di + DRV_HEADS]
        mov     [num_heads], dx
        mov     dl, [di + DRV_MEDIA]
        mov     [media_desc], dl
        mov     dl, [di + DRV_FATTYPE]
        mov     [fat_type], dl
        mov     edx, [di + DRV_TOTALSEC]
        mov     [total_secs], edx
        mov     edx, [di + DRV_SERIAL]
        mov     [volume_serial], edx

        mov     bx, di
        add     bx, DRV_DPB
        mov     [cur_dpb], bx

        ; FAT12 なら FAT を常駐させる
        cmp     byte [fat_type], 12
        jne     .done
        call    fat_resident_load
        jc      .bad

.done:
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.already:
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.bad:
        mov     byte [fs_drive], 0xFF
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

; ---------------------------------------------------------------------------
; fat_resident_load - いま選んでいるドライブの FAT を fat_buf へ載せる
; ---------------------------------------------------------------------------
fat_resident_load:
        push    eax
        push    bx
        push    cx
        push    dx
        push    es

        cmp     byte [fat_resident_drv], 0xFF
        je      .load
        mov     al, [fat_resident_drv]
        cmp     al, [fs_drive]
        je      .hit                    ; すでに載っている
.load:
        push    ds
        pop     es
        movzx   eax, word [reserved_secs]
        mov     cx, [secs_per_fat]
        cmp     cx, FAT_RESIDENT_SECS
        ja      .too_big
        mov     bx, fat_buf
        mov     dl, [fs_drive]
        call    disk_read
        jc      .fail
        mov     al, [fs_drive]
        mov     [fat_resident_drv], al
        mov     byte [fat_dirty], 0
.hit:
        pop     es
        pop     dx
        pop     cx
        pop     bx
        pop     eax
        clc
        ret
.too_big:
.fail:
        mov     byte [fat_resident_drv], 0xFF
        pop     es
        pop     dx
        pop     cx
        pop     bx
        pop     eax
        stc
        ret

; ---------------------------------------------------------------------------
; fat_resident_flush - 常駐 FAT が書き換わっていればディスクへ戻す
;   出力: CF=1 なら失敗
;
; FAT はコピーが 2 つあり、両方を同じ内容に保つ必要がある。片方だけ
; 書いて落ちると CHKDSK が「FAT が食い違っている」と言い出す。
; ---------------------------------------------------------------------------
fat_resident_flush:
        cmp     byte [fat_dirty], 0
        je      .nothing
        cmp     byte [fat_resident_drv], 0xFF
        je      .nothing

        pushad
        push    es
        push    ds
        pop     es

        mov     al, [fat_resident_drv]
        call    drive_entry
        jc      .fail

        mov     cx, [di + DRV_NUMFATS]
        movzx   eax, word [di + DRV_RESERVED]
        mov     dl, [fat_resident_drv]
.loop:
        push    cx
        push    eax
        mov     bx, fat_buf
        mov     cx, [di + DRV_SECPERFAT]
        call    disk_write
        pop     eax
        pop     cx
        jc      .fail
        movzx   ebx, word [di + DRV_SECPERFAT]
        add     eax, ebx                ; 次の FAT のコピーへ
        loop    .loop

        mov     byte [fat_dirty], 0
        pop     es
        popad
        clc
        ret
.fail:
        pop     es
        popad
        stc
        ret
.nothing:
        clc
        ret

; ---------------------------------------------------------------------------
; disk_flush_fat - FAT とディスクバッファをまとめて書き戻す
;
; ファイルを閉じたとき、AH=0Dh を呼ばれたとき、プログラムが終わったときに
; 呼ばれる。ここを通ればディスクの内容は矛盾のない状態になる。
; ---------------------------------------------------------------------------
disk_flush_fat:
        call    fat_resident_flush
        jc      .fail
        call    buf_flush
        ret
.fail:
        call    buf_flush
        stc
        ret

; ---------------------------------------------------------------------------
; disk_read_drive / disk_write_drive - ドライブ番号つきの入口
;   入力: EAX = LBA (パーティション先頭からの相対), CX = セクタ数,
;         ES:BX = バッファ, DL = ドライブ番号
;   出力: CF=1 なら失敗
;
; どのドライブも、扱うのはデバイスドライバ。組み込みのブロックデバイス
; (dev_block) が担当しているなら INT 13h を直に叩き、CONFIG.SYS の DEVICE=
; で入れたドライバが担当しているならそちらを呼ぶ。
;
; dev_block を特別扱いしているのは速さのためではなく、再帰を避けるため。
; dev_block の READ ハンドラ自身が disk_read を呼ぶので、ここで区別せずに
; ドライバを呼ぶと自分に戻ってきてしまう。
; ---------------------------------------------------------------------------
disk_read_drive:
        push    si
        xor     si, si                  ; 0 = 読み込み
        call    drive_xfer
        pop     si
        ret

disk_write_drive:
        push    si
        mov     si, 1                   ; 1 = 書き込み
        call    drive_xfer
        pop     si
        ret

; ---------------------------------------------------------------------------
; drive_xfer - ドライブ表を見て転送先を振り分ける
;   入力: EAX = LBA, CX = セクタ数, ES:BX = バッファ, DL = ドライブ, SI = 向き
; ---------------------------------------------------------------------------
drive_xfer:
        push    di
        push    ax
        mov     al, dl
        call    drive_entry
        pop     ax
        jc      .fail

        test    byte [di + DRV_FLAGS], DRVF_EXTERNAL
        jnz     .external

        pop     di
        test    si, si
        jnz     .int13_write
        jmp     disk_read
.int13_write:
        jmp     disk_write

.external:
        call    drive_dev_xfer
        pop     di
        ret
.fail:
        pop     di
        stc
        ret

; ---------------------------------------------------------------------------
; drive_dev_xfer - 外部ドライバに読み書きさせる
;   入力: EAX = LBA, CX = セクタ数, ES:BX = バッファ, SI = 向き,
;         DI = ドライブ表のエントリ
;
; 要求ヘッダの開始セクタは 16bit しかない。65535 を超えるときは
; そこに FFFFh を入れて、32bit の欄 (+1Ah) に本当の値を置く。
; DOS 4.0 で 32MB の壁を越えるために足された約束で、ドライバ側は
; ヘッダの長さ (26 バイト) を見てどちらか判断する。
; ---------------------------------------------------------------------------
drive_dev_xfer:
        pushad
        push    es

        mov     byte [cs:req_packet + REQ_LENGTH], 30
        mov     dl, [di + DRV_UNIT]
        mov     [cs:req_packet + REQ_UNIT], dl
        mov     dl, DEVC_READ
        test    si, si
        jz      .have_cmd
        mov     dl, DEVC_WRITE
.have_cmd:
        mov     [cs:req_packet + REQ_CMD], dl
        mov     word [cs:req_packet + REQ_STATUS], 0
        mov     dl, [di + DRV_MEDIA]
        mov     [cs:req_packet + REQ_MEDIA], dl
        mov     [cs:req_packet + REQ_BUFFER], bx
        mov     dx, es
        mov     [cs:req_packet + REQ_BUFFER + 2], dx
        mov     [cs:req_packet + REQ_COUNT], cx

        add     eax, [di + DRV_PARTLBA]         ; 通常は 0
        mov     [cs:req_packet + REQ_STARTL], eax
        cmp     eax, 0xFFFF
        jbe     .small
        mov     word [cs:req_packet + REQ_START], 0xFFFF
        jmp     .go
.small:
        mov     [cs:req_packet + REQ_START], ax
.go:
        push    ds
        push    cs
        pop     ds
        les     si, [di + DRV_DRIVER]
        call    dev_call
        pop     ds
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

; ============================================================================
; 起動時のドライブ検出
; ============================================================================

; ---------------------------------------------------------------------------
; disk_init - 繋がっているドライブを探して表を組み立てる
;   入力: [boot_drive] が設定済みであること
;   出力: CF=1 なら 1 台も使えるドライブが見つからなかった
;
; 表を作り終えたら A: (正しくは起動ドライブ) を選んでおく。
; ---------------------------------------------------------------------------
disk_init:
        pushad
        push    es

        ; 表を 0 で埋める
        push    ds
        pop     es
        mov     di, drive_tab
        mov     cx, MAX_PHYS_DRIVES * DRV_ENTSIZE
        xor     al, al
        rep     stosb

        mov     byte [fs_drive], 0xFF
        mov     byte [fat_resident_drv], 0xFF
        mov     byte [num_drives], 0

        ; --- フロッピー (A: / B:) ---
        xor     bl, bl                  ; BL = BIOS のドライブ番号
        xor     bh, bh                  ; BH = DOS のドライブ番号
.fd_loop:
        mov     al, bh
        mov     dl, bl
        call    drive_try_floppy
        inc     bl
        inc     bh
        cmp     bl, 2
        jb      .fd_loop

        ; A: と B: は実在しなくても文字を占有する (当時の DOS と同じ)
        mov     bh, 2

        ; --- ハードディスク (C: 以降) ---
        mov     bl, 0x80
.hd_loop:
        cmp     bh, MAX_PHYS_DRIVES
        jae     .scan_done
        mov     al, bh
        mov     dl, bl
        call    drive_try_hd            ; AL = 次の空きドライブ番号
        mov     bh, al
        inc     bl
        cmp     bl, 0x84
        jb      .hd_loop

.scan_done:
        call    dpb_relink              ; DPB の連鎖を組み直す

        ; 実在するドライブの数を数える (末尾の空きは勘定に入れない)
        mov     cl, MAX_PHYS_DRIVES
        mov     ch, 0
.count:
        dec     cl
        mov     al, cl
        call    drive_slot
        test    byte [di + DRV_FLAGS], DRVF_PRESENT
        jnz     .count_got
        test    cl, cl
        jnz     .count
        jmp     .none
.count_got:
        mov     al, cl
        inc     al
        mov     [num_drives], al

        ; 起動したドライブを選ぶ
        mov     al, [boot_drive]
        cmp     al, 0x80
        jb      .boot_fd
        mov     al, 2                   ; ハードディスクから起動したなら C:
        jmp     .boot_sel
.boot_fd:
        ; そのまま A: / B:
.boot_sel:
        call    drive_select
        jnc     .ok

        ; 起動ドライブが読めないときは最初に見つかったものにする
        xor     al, al
.find_any:
        call    drive_select
        jnc     .ok
        inc     al
        cmp     al, MAX_PHYS_DRIVES
        jb      .find_any
        jmp     .none

.ok:
        pop     es
        popad
        clc
        ret
.none:
        pop     es
        popad
        stc
        ret

; ---------------------------------------------------------------------------
; drive_try_floppy - フロッピーを 1 台調べて表に入れる
;   入力: AL = DOS のドライブ番号, DL = BIOS のドライブ番号
; ---------------------------------------------------------------------------
drive_try_floppy:
        push    ax
        push    bx
        push    cx
        push    dx
        push    di

        call    drive_slot
        mov     [di + DRV_PHYS], dl
        mov     byte [di + DRV_USEEXT], 0
        mov     dword [di + DRV_PARTLBA], 0
        mov     byte [di + DRV_FLAGS], DRVF_PRESENT | DRVF_REMOVABLE
        call    drive_use_builtin

        ; ジオメトリを BIOS に聞く。答えられないドライブは無いものとする。
        call    disk_geometry           ; CX = セクタ/トラック, DX = ヘッド数
        jc      .absent
        mov     [di + DRV_SECPERTRK], cx
        mov     [di + DRV_HEADS], dx

        ; ブートセクタの BPB を読む
        call    drive_read_bpb
        jc      .absent

        call    dpb_build
        jmp     .out

.absent:
        mov     byte [di + DRV_FLAGS], 0
.out:
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; drive_use_builtin - このドライブは組み込みのブロックデバイスが扱う
;   入力: AL = ドライブ番号, DI = ドライブ表のエントリ
; ---------------------------------------------------------------------------
drive_use_builtin:
        push    ax
        mov     word [di + DRV_DRIVER], dev_block
        mov     [di + DRV_UNIT], al
        mov     ax, cs
        mov     [di + DRV_DRIVER + 2], ax
        pop     ax
        ret

; ---------------------------------------------------------------------------
; drive_try_hd - ハードディスクを 1 台調べ、基本パーティションを表に入れる
;   入力: AL = 次に使う DOS のドライブ番号, DL = BIOS のドライブ番号
;   出力: AL = 使ったあとの次のドライブ番号
;
; MBR の +1BEh から 16 バイト x 4 のパーティションテーブルを読み、
; FAT12/FAT16 のものだけを順に割り当てる。拡張パーティションの中までは
; 辿らない (MS-DOS も 6.22 の時点で拡張の扱いは複雑だったが、
; ここでは基本パーティションだけで十分実用になる)。
; ---------------------------------------------------------------------------
drive_try_hd:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es

        mov     [.next], al
        mov     [.phys], dl

        ; 拡張が使えるか。使えないディスクは CHS でジオメトリを聞く。
        call    disk_detect_ext
        setnc   al
        mov     [.useext], al
        call    disk_geometry
        jc      .geo_fail
        mov     [.spt], cx
        mov     [.heads], dx
        jmp     .read_mbr
.geo_fail:
        cmp     byte [.useext], 0
        je      .out                    ; 繋がっていない
        mov     word [.spt], 63         ; 拡張で読むので実際には使わない
        mov     word [.heads], 255

.read_mbr:
        ; MBR を読む。表にまだ載っていないドライブなので、いちばん後ろの
        ; 枠を作業用に使って disk_read を通す。
        mov     al, MAX_PHYS_DRIVES - 1
        call    drive_slot
        mov     dl, [.phys]
        mov     [di + DRV_PHYS], dl
        mov     al, [.useext]
        mov     [di + DRV_USEEXT], al
        mov     dword [di + DRV_PARTLBA], 0
        mov     cx, [.spt]
        mov     [di + DRV_SECPERTRK], cx
        mov     cx, [.heads]
        mov     [di + DRV_HEADS], cx
        mov     byte [di + DRV_FLAGS], DRVF_PRESENT
        call    drive_use_builtin

        push    ds
        pop     es
        mov     bx, sector_buf
        xor     eax, eax
        mov     cx, 1
        mov     dl, MAX_PHYS_DRIVES - 1
        call    disk_read
        pushf
        mov     byte [di + DRV_FLAGS], 0        ; 作業用の枠は片付ける
        popf
        jc      .out

        cmp     word [sector_buf + 0x1FE], 0xAA55
        jne     .out

        ; パーティションテーブルを手元に写しておく。このあと各パーティションの
        ; ブートセクタを読むのに sector_buf を使い回すので、MBR はそこで
        ; 消える。写さずに辿ると、2 つ目からは別のセクタの中身を
        ; パーティションエントリとして読むことになる。
        push    si
        push    di
        mov     si, sector_buf + PART_TABLE
        mov     di, part_table
        mov     cx, PART_ENTSIZE * 4
        rep     movsb
        pop     di
        pop     si

        ; --- パーティションテーブルを 4 つ見る ---
        mov     si, part_table
        mov     cx, 4
.part_loop:
        push    cx
        mov     al, [si + PART_TYPE]
        call    part_is_fat
        jc      .part_next

        mov     al, [.next]
        cmp     al, MAX_PHYS_DRIVES - 1
        jae     .part_next

        call    drive_slot
        mov     dl, [.phys]
        mov     [di + DRV_PHYS], dl
        mov     al, [.useext]
        mov     [di + DRV_USEEXT], al
        mov     eax, [si + PART_LBA]
        mov     [di + DRV_PARTLBA], eax
        mov     cx, [.spt]
        mov     [di + DRV_SECPERTRK], cx
        mov     cx, [.heads]
        mov     [di + DRV_HEADS], cx
        mov     byte [di + DRV_FLAGS], DRVF_PRESENT
        call    drive_use_builtin

        push    si
        mov     al, [.next]
        call    drive_read_bpb
        pop     si
        jc      .part_bad

        push    si
        mov     al, [.next]
        call    dpb_build
        pop     si
        inc     byte [.next]
        jmp     .part_next
.part_bad:
        mov     byte [di + DRV_FLAGS], 0
.part_next:
        pop     cx
        add     si, PART_ENTSIZE
        loop    .part_loop

.out:
        mov     al, [.next]
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret
.next:   db 0
.phys:   db 0
.useext: db 0
.spt:    dw 0
.heads:  dw 0

; MBR から写し取ったパーティションテーブル (16 バイト x 4)
part_table:     times PART_ENTSIZE * 4 db 0

; ---------------------------------------------------------------------------
; part_is_fat - パーティションの種別が FAT12/FAT16 かどうか
;   入力: AL = システム ID
;   出力: CF=0 なら FAT
; ---------------------------------------------------------------------------
part_is_fat:
        cmp     al, 0x01                ; FAT12
        je      .yes
        cmp     al, 0x04                ; FAT16 (32MB 未満)
        je      .yes
        cmp     al, 0x06                ; FAT16B
        je      .yes
        cmp     al, 0x0E                ; FAT16B (LBA)
        je      .yes
        stc
        ret
.yes:
        clc
        ret

; ---------------------------------------------------------------------------
; drive_read_bpb - そのドライブの先頭セクタから BPB を取り込む
;   入力: AL = DOS のドライブ番号 (表に PHYS / PARTLBA / ジオメトリが
;              入っていて DRVF_PRESENT が立っていること)
;   出力: CF=1 なら FAT のボリュームではない。CF=0 なら DI = 表のエントリ
;
; ジオメトリは BPB の +18h / +1Ah より BIOS の答えを優先する。
; パーティションを切り直したディスクでは BPB 側が古いままのことがある。
; ---------------------------------------------------------------------------
drive_read_bpb:
        push    eax
        push    bx
        push    cx
        push    dx
        push    si
        push    es

        push    ax
        push    ds
        pop     es
        mov     bx, sector_buf
        xor     eax, eax                ; パーティション先頭 = 相対 LBA 0
        mov     cx, 1
        pop     dx                      ; DL = ドライブ番号
        push    dx
        call    disk_read
        pop     ax
        jc      .bad
        call    drive_slot              ; DI = 表のエントリ

        mov     si, sector_buf
        cmp     word [si + 0x0B], SECTOR_SIZE
        jne     .bad

        movzx   ax, byte [si + 0x0D]    ; セクタ/クラスタ
        test    al, al
        jz      .bad
        mov     dx, ax
        dec     dx
        test    ax, dx
        jnz     .bad                    ; 2 のべき乗でなければ FAT ではない
        mov     [di + DRV_SECPERCLUS], ax

        mov     ax, [si + 0x0E]         ; 予約セクタ数
        test    ax, ax
        jz      .bad
        mov     [di + DRV_RESERVED], ax

        movzx   ax, byte [si + 0x10]    ; FAT の数
        test    al, al
        jz      .bad
        cmp     al, 4
        ja      .bad
        mov     [di + DRV_NUMFATS], ax

        mov     ax, [si + 0x11]         ; ルートのエントリ数
        test    ax, ax
        jz      .bad
        mov     [di + DRV_ROOTENT], ax

        mov     al, [si + 0x15]         ; メディアディスクリプタ
        mov     [di + DRV_MEDIA], al

        mov     ax, [si + 0x16]         ; FAT 1 個のセクタ数
        test    ax, ax
        jz      .bad
        mov     [di + DRV_SECPERFAT], ax

        ; 総セクタ数。16bit 版が 0 なら 32bit 版 (+20h) を見る
        movzx   eax, word [si + 0x13]
        test    eax, eax
        jnz     .have_total
        mov     eax, [si + 0x20]
        test    eax, eax
        jz      .bad
.have_total:
        mov     [di + DRV_TOTALSEC], eax

        ; ジオメトリが未設定なら BPB のものを使う
        cmp     word [di + DRV_SECPERTRK], 0
        jne     .have_geo
        mov     ax, [si + 0x18]
        mov     [di + DRV_SECPERTRK], ax
        mov     ax, [si + 0x1A]
        mov     [di + DRV_HEADS], ax
.have_geo:
        cmp     word [di + DRV_SECPERTRK], 0
        je      .bad
        cmp     word [di + DRV_HEADS], 0
        je      .bad

        ; 拡張 BPB (+26h が 29h) ならシリアルとラベルが入っている
        mov     dword [di + DRV_SERIAL], 0
        cmp     byte [si + 0x26], 0x29
        jne     .no_ext
        mov     eax, [si + 0x27]
        mov     [di + DRV_SERIAL], eax
        push    di
        push    si
        add     si, 0x2B
        add     di, DRV_LABEL
        mov     cx, 11
        rep     movsb
        pop     si
        pop     di
.no_ext:

        ; --- レイアウトを計算する ---
        call    drive_layout
        jc      .bad

        pop     es
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     eax
        clc
        ret
.bad:
        pop     es
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     eax
        stc
        ret

; ---------------------------------------------------------------------------
; dpb_build - そのドライブの DPB を組み立てる
;   入力: AL = ドライブ番号 (表の値が埋まっていること)
;
; DOS が各ドライブについて持っている構造体。AH=32h でプログラムに直接
; 渡され、List of Lists からも辿れる。当時のディスクツール
; (CHKDSK、デフラグ、セクタエディタの類) はここからジオメトリを読む。
; BPB を読み直すのではなく DPB を見るのが正しい作法とされていた。
; ---------------------------------------------------------------------------
dpb_build:
        push    ax
        push    bx
        push    cx
        push    dx
        push    di

        push    ax
        call    drive_slot
        mov     bx, di
        add     bx, DRV_DPB
        pop     ax

        mov     [bx + DPB_DRIVE], al
        mov     word [bx + DPB_SECSIZE], SECTOR_SIZE

        mov     ax, [di + DRV_SECPERCLUS]
        dec     ax
        mov     [bx + DPB_CLUSMASK], al         ; クラスタ内の最大セクタ番号

        ; クラスタ→セクタの変換に使うシフト量 (sec_per_clus の log2)
        mov     ax, [di + DRV_SECPERCLUS]
        xor     cl, cl
.shift:
        shr     ax, 1
        cmp     ax, 1
        jb      .shift_done
        inc     cl
        jmp     .shift
.shift_done:
        mov     [bx + DPB_CLUSSHIFT], cl

        mov     ax, [di + DRV_RESERVED]
        mov     [bx + DPB_RESERVED], ax
        mov     ax, [di + DRV_NUMFATS]
        mov     [bx + DPB_NUMFATS], al
        mov     ax, [di + DRV_ROOTENT]
        mov     [bx + DPB_ROOTENT], ax
        mov     ax, [di + DRV_DATALBA]
        mov     [bx + DPB_DATASEC], ax
        mov     ax, [di + DRV_MAXCLUS]
        dec     ax
        mov     [bx + DPB_MAXCLUS], ax          ; 最大クラスタ番号
        mov     ax, [di + DRV_SECPERFAT]
        mov     [bx + DPB_SECPERFAT], ax
        mov     ax, [di + DRV_ROOTLBA]
        mov     [bx + DPB_DIRSEC], ax
        mov     al, [di + DRV_MEDIA]
        mov     [bx + DPB_MEDIA], al
        mov     byte [bx + DPB_ACCESS], 0       ; 0 = アクセス済み
        mov     word [bx + DPB_FREESRCH], 2
        mov     word [bx + DPB_FREECNT], 0xFFFF ; 不明 (聞かれたら数える)

        ; このドライブを扱うデバイスドライバ
        mov     ax, [di + DRV_DRIVER]
        mov     [bx + DPB_DRIVER], ax
        mov     ax, [di + DRV_DRIVER + 2]
        mov     [bx + DPB_DRIVER + 2], ax
        mov     al, [di + DRV_UNIT]
        mov     [bx + DPB_UNIT], al

        ; DPB は連鎖している。次に実在するドライブの DPB へ繋ぐ。
        mov     dword [bx + DPB_NEXT], 0xFFFFFFFF
        mov     al, [bx + DPB_DRIVE]
.chain:
        inc     al
        cmp     al, MAX_PHYS_DRIVES
        jae     .chain_done
        push    ax
        call    drive_entry
        pop     ax
        jc      .chain
        mov     cx, di
        add     cx, DRV_DPB
        mov     [bx + DPB_NEXT], cx
        mov     cx, cs
        mov     [bx + DPB_NEXT + 2], cx
.chain_done:

        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; drive_add_external - DEVICE= で入れたドライバのドライブを表に足す
;   入力: AL = ドライブ番号, DL = ドライバ内のユニット番号,
;         ES:SI = そのユニットの BPB (25 バイト),
;         CX:BX = デバイスドライバのヘッダ (CX = セグメント)
;   出力: CF=1 なら BPB がおかしくて使えない
;
; INT 13h を通らないドライブ。読み書きは drive_xfer がドライバへ回す。
; BPB の形は BUILD BPB がドライバに返させるものと同じで、
; 「セクタあたりバイト数」から「隠しセクタ数」までが並んでいる。
; ---------------------------------------------------------------------------
drive_add_external:
        push    ax
        push    bx
        push    cx
        push    dx
        push    di

        push    ax
        call    drive_slot              ; DI = 表のエントリ
        pop     ax

        cmp     word [es:si + 0x00], SECTOR_SIZE
        jne     .bad

        mov     [di + DRV_UNIT], dl
        mov     [di + DRV_DRIVER], bx
        mov     [di + DRV_DRIVER + 2], cx
        mov     dword [di + DRV_PARTLBA], 0
        mov     byte [di + DRV_PHYS], 0
        mov     byte [di + DRV_USEEXT], 0

        movzx   dx, byte [es:si + 0x02] ; セクタ/クラスタ
        test    dl, dl
        jz      .bad
        mov     [di + DRV_SECPERCLUS], dx

        mov     dx, [es:si + 0x03]      ; 予約セクタ数
        test    dx, dx
        jz      .bad
        mov     [di + DRV_RESERVED], dx

        movzx   dx, byte [es:si + 0x05] ; FAT の数
        test    dl, dl
        jz      .bad
        mov     [di + DRV_NUMFATS], dx

        mov     dx, [es:si + 0x06]      ; ルートのエントリ数
        test    dx, dx
        jz      .bad
        mov     [di + DRV_ROOTENT], dx

        mov     dl, [es:si + 0x0A]      ; メディアディスクリプタ
        mov     [di + DRV_MEDIA], dl

        mov     dx, [es:si + 0x0B]      ; FAT 1 個のセクタ数
        test    dx, dx
        jz      .bad
        mov     [di + DRV_SECPERFAT], dx

        mov     dx, [es:si + 0x0D]      ; セクタ/トラック (使わないが控える)
        mov     [di + DRV_SECPERTRK], dx
        mov     dx, [es:si + 0x0F]      ; ヘッド数
        mov     [di + DRV_HEADS], dx

        ; 総セクタ数。16bit 版が 0 なら 32bit 版 (+15h) を見る
        push    eax
        movzx   eax, word [es:si + 0x08]
        test    eax, eax
        jnz     .have_total
        mov     eax, [es:si + 0x15]
        test    eax, eax
        jz      .bad_pop
.have_total:
        mov     [di + DRV_TOTALSEC], eax
        pop     eax

        mov     dword [di + DRV_SERIAL], 0
        mov     byte [di + DRV_FLAGS], DRVF_PRESENT | DRVF_EXTERNAL

        call    drive_layout
        jc      .bad

        push    ax
        call    dpb_build
        pop     ax

        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.bad_pop:
        pop     eax
.bad:
        mov     byte [di + DRV_FLAGS], 0
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

; ---------------------------------------------------------------------------
; drive_layout - BPB から求まる位置とクラスタ数を計算する
;   入力: DI = ドライブ表のエントリ (BPB 由来の値が埋まっていること)
;   出力: CF=1 ならつじつまが合わない
;
; BPB を INT 13h 経由で読んだ場合も、ドライバから受け取った場合も、
; ここから先の計算は同じ。
; ---------------------------------------------------------------------------
drive_layout:
        push    eax
        push    ecx
        push    edx

        mov     ax, [di + DRV_NUMFATS]
        mul     word [di + DRV_SECPERFAT]
        add     ax, [di + DRV_RESERVED]
        mov     [di + DRV_ROOTLBA], ax          ; ルートディレクトリの開始

        mov     ax, [di + DRV_ROOTENT]
        add     ax, 15
        shr     ax, 4                           ; (エントリ数 * 32 + 511) / 512
        mov     [di + DRV_ROOTSECS], ax

        add     ax, [di + DRV_ROOTLBA]
        mov     [di + DRV_DATALBA], ax          ; データ領域の開始

        mov     ax, [di + DRV_SECPERCLUS]
        mov     dx, SECTOR_SIZE
        mul     dx
        mov     [di + DRV_CLUSBYTES], ax        ; 1 クラスタのバイト数

        ; 使えるクラスタ数 = (総セクタ - データ開始) / クラスタあたりセクタ数
        mov     eax, [di + DRV_TOTALSEC]
        movzx   edx, word [di + DRV_DATALBA]
        cmp     eax, edx
        jbe     .bad
        sub     eax, edx
        movzx   ecx, word [di + DRV_SECPERCLUS]
        xor     edx, edx
        div     ecx
        add     eax, 2                          ; クラスタ番号は 2 から
        cmp     eax, 0xFFF6
        jbe     .clus_ok
        mov     eax, 0xFFF6                     ; FAT16 の上限で頭打ちにする
.clus_ok:
        mov     [di + DRV_MAXCLUS], ax

        ; FAT12 と FAT16 の別れ目はクラスタ数。4085 個未満なら FAT12。
        ; パーティション種別ではなくここで決めるのが本来の判定。
        mov     byte [di + DRV_FATTYPE], 16
        cmp     ax, 4085
        jae     .type_ok
        mov     byte [di + DRV_FATTYPE], 12
.type_ok:

        ; FAT12 は常駐させるので、入り切らない大きさなら受け付けない
        cmp     byte [di + DRV_FATTYPE], 12
        jne     .size_ok
        cmp     word [di + DRV_SECPERFAT], FAT_RESIDENT_SECS
        ja      .bad
.size_ok:
        pop     edx
        pop     ecx
        pop     eax
        clc
        ret
.bad:
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret

; ---------------------------------------------------------------------------
; dpb_relink - DPB の連鎖を組み直す
;
; ドライブを 1 台ずつ見つけながら DPB を組むので、あとから増えたドライブへの
; 繋ぎ直しが要る。検出が終わったところで一度だけ呼ぶ。
; ---------------------------------------------------------------------------
dpb_relink:
        push    ax
        push    di
        xor     al, al
.loop:
        push    ax
        call    drive_entry
        pop     ax
        jc      .next
        push    ax
        call    dpb_build
        pop     ax
.next:
        inc     al
        cmp     al, MAX_PHYS_DRIVES
        jb      .loop
        pop     di
        pop     ax
        ret

; ---------------------------------------------------------------------------
; bpb_image_build - ドライバの BUILD BPB (コマンド 2) が返す BPB を組み立てる
;   入力: AL = ユニット番号 (= DOS のドライブ番号)
;
; DOS のデバイスドライバは「メディアを見て BPB を返せ」と言われる。
; ここでは表に取り込んである値をそのまま 25 バイトの形に並べ直す。
; ---------------------------------------------------------------------------
bpb_image_build:
        push    ax
        push    bx
        push    cx
        push    dx
        push    di

        call    drive_entry
        jc      .out

        mov     bx, bpb_image
        mov     word [bx + 0x00], SECTOR_SIZE   ; セクタあたりバイト数
        mov     ax, [di + DRV_SECPERCLUS]
        mov     [bx + 0x02], al
        mov     ax, [di + DRV_RESERVED]
        mov     [bx + 0x03], ax
        mov     ax, [di + DRV_NUMFATS]
        mov     [bx + 0x05], al
        mov     ax, [di + DRV_ROOTENT]
        mov     [bx + 0x06], ax

        ; 総セクタ数は 65536 未満なら 16bit 版、そうでなければ 32bit 版に入れる
        mov     eax, [di + DRV_TOTALSEC]
        mov     dword [bx + 0x15], 0
        cmp     eax, 0x10000
        jb      .small
        mov     word [bx + 0x08], 0
        mov     [bx + 0x15], eax
        jmp     .after_total
.small:
        mov     [bx + 0x08], ax
.after_total:
        mov     al, [di + DRV_MEDIA]
        mov     [bx + 0x0A], al
        mov     ax, [di + DRV_SECPERFAT]
        mov     [bx + 0x0B], ax
        mov     ax, [di + DRV_SECPERTRK]
        mov     [bx + 0x0D], ax
        mov     ax, [di + DRV_HEADS]
        mov     [bx + 0x0F], ax
        mov     eax, [di + DRV_PARTLBA]
        mov     [bx + 0x11], eax        ; 隠しセクタ数
.out:
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; --- 変数 ------------------------------------------------------------------
boot_drive:     db 0                    ; 起動した BIOS のドライブ番号

; いま作業対象になっているドライブの諸元。drive_select が載せ替える。
fs_drive:       db 0xFF                 ; 載っているドライブ (FFh = なし)
fat_type:       db 12                   ; 12 か 16
media_desc:     db 0
sec_per_clus:   dw 0
reserved_secs:  dw 0
num_fats:       dw 0
root_entries:   dw 0
secs_per_fat:   dw 0
secs_per_track: dw 0
num_heads:      dw 0
root_lba:       dw 0                    ; ルートディレクトリの開始 (相対)
root_secs:      dw 0                    ; ルートディレクトリのセクタ数
data_lba:       dw 0                    ; データ領域の開始 (相対)
max_cluster:    dw 0                    ; 存在する最大クラスタ番号 + 1
clus_bytes:     dw 0                    ; 1 クラスタのバイト数
total_secs:     dd 0
volume_serial:  dd 0
cur_dpb:        dw drive_tab + DRV_DPB

; 常駐 FAT (FAT12 専用)
fat_resident_drv: db 0xFF               ; fat_buf に載っているドライブ
fat_dirty:        db 0                  ; 書き換えたか

