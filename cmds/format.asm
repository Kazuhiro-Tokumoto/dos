; ============================================================================
; format.asm  -  FORMAT コマンド (フロッピーを FAT12 で初期化する)
;
;   FORMAT d: [/S] [/V:ラベル]
;
;     /S        初期化したあと SYS を走らせて起動できるようにする
;     /V:名前   ボリュームラベルを付ける
;
; --- なぜ INT 13h を直接叩くのか -------------------------------------------
;
; DOS の絶対セクタ書き込み (INT 26h) は、そのドライブの諸元を DOS が
; 知っていることが前提になっている。諸元はブートセクタの BPB から作るので、
; まだ何も書かれていないディスクでは作れない。つまり「フォーマットするために
; フォーマット済みであることが要る」という循環になる。当時の FORMAT が
; BIOS を直接呼んでいたのはこのため。
;
; 書き終えたら AH=0Dh (ディスクリセット) を呼ぶ。MYDOS はこれを合図に
; ドライブの諸元を作り直すので、その場で新しいディスクとして使えるようになる。
;
; --- 対応するのはフロッピーだけ --------------------------------------------
;
; ハードディスクはパーティションを切る話 (FDISK) が先に来るので、ここでは
; 扱わない。BIOS のドライブ番号 80h 以降を指定されたら断る。
;
; --- 予約セクタが 18 なのはなぜか ------------------------------------------
;
; MYDOS のブートセクタは、自分の続き (Stage2) を LBA 1 から 17 セクタぶん
; 生読みする。予約セクタが 1 のままだと LBA 1 は FAT1 の先頭にあたるので、
; 18 に増やして LBA 1-17 を正当な予約領域にしている。1.44MB (1 トラック
; 18 セクタ) でしか成り立たない形なので、720KB のディスクは予約 1 の
; 普通の FAT12 として作り、起動用には使えないことにしてある。
; ============================================================================
        cpu     386
        bits    16
        org     0x100

SECTOR_SIZE     equ 512
MYDOS_RESERVED  equ 18

start:
        mov     sp, stack_top
        mov     bx, stack_top
        add     bx, 15
        mov     cl, 4
        shr     bx, cl
        mov     ah, 0x4A
        int     0x21

        call    parse_cmdline
        jc      .usage

        call    get_geometry
        jc      .no_drive

        call    pick_layout
        jc      .bad_media

        call    classify_fat

        call    confirm
        jc      .aborted

        mov     dx, msg_working
        call    puts

        call    write_boot
        jc      .wr_err
        call    write_reserved
        jc      .wr_err
        call    write_fats
        jc      .wr_err
        call    write_root
        jc      .wr_err

        ; DOS に諸元を作り直させる
        mov     ah, 0x0D
        int     0x21

        mov     dx, msg_done
        call    puts
        call    report_size

        cmp     byte [opt_sys], 0
        je      .out
        call    run_sys
.out:
        mov     ax, 0x4C00
        int     0x21

.usage:
        mov     dx, msg_usage
        call    puts
        jmp     .fail
.no_drive:
        mov     dx, msg_no_drive
        call    puts
        jmp     .fail
.bad_media:
        mov     dx, msg_bad_media
        call    puts
        jmp     .fail
.wr_err:
        mov     dx, msg_wr_err
        call    puts
        jmp     .fail
.aborted:
        mov     dx, msg_aborted
        call    puts
.fail:
        mov     ax, 0x4C01
        int     0x21

; ============================================================================
; コマンドラインを読む
;   出力: CF=1 なら書き方が違う
; ============================================================================
parse_cmdline:
        mov     si, 0x81
        movzx   cx, byte [0x80]
        test    cx, cx
        jz      .bad

        call    skip_space
        jcxz    .bad

        ; ドライブ文字
        lodsb
        dec     cx
        call    upcase
        cmp     al, 'A'
        jb      .bad
        cmp     al, 'Z'
        ja      .bad
        sub     al, 'A'
        mov     [dos_drive], al
        jcxz    .bad
        lodsb
        dec     cx
        cmp     al, ':'
        jne     .bad

        ; 残りは指定
.opts:
        call    skip_space
        jcxz    .done
        lodsb
        dec     cx
        cmp     al, '/'
        jne     .bad
        jcxz    .bad
        lodsb
        dec     cx
        call    upcase
        cmp     al, 'S'
        je      .opt_s
        cmp     al, 'V'
        je      .opt_v
        cmp     al, 'Q'
        je      .opt_q
        jmp     .bad
.opt_q:
        mov     byte [opt_quiet], 1
        jmp     .opts
.opt_s:
        mov     byte [opt_sys], 1
        jmp     .opts
.opt_v:
        ; "/V:名前"
        jcxz    .opts
        lodsb
        dec     cx
        cmp     al, ':'
        jne     .bad
        mov     di, vol_label
        mov     bx, 11
.vloop:
        jcxz    .opts
        lodsb
        dec     cx
        cmp     al, ' '
        je      .opts
        cmp     al, 13
        je      .opts
        test    bx, bx
        jz      .vloop
        call    upcase
        mov     [di], al
        inc     di
        dec     bx
        jmp     .vloop
.done:
        clc
        ret
.bad:
        stc
        ret

skip_space:
        jcxz    .out
.loop:
        cmp     byte [si], ' '
        jne     .out
        inc     si
        dec     cx
        jnz     .loop
.out:
        ret

upcase:
        cmp     al, 'a'
        jb      .out
        cmp     al, 'z'
        ja      .out
        sub     al, 0x20
.out:
        ret

; ============================================================================
; ドライブのジオメトリを BIOS に聞く
;
; DOS のドライブ番号と BIOS のドライブ番号は別物。A: と B: だけは 0 と 1 に
; そのまま対応するので、そこだけ扱う。
; ============================================================================
get_geometry:
        mov     al, [dos_drive]
        cmp     al, 2
        jae     hd_geometry             ; C: 以降はハードディスクの区画
        mov     byte [is_floppy], 1
        mov     [bios_drive], al

        push    es
        push    di
        xor     di, di
        mov     es, di                  ; 一部の BIOS 対策 (ES:DI = 0)
        mov     dl, al
        mov     ah, 0x08
        int     0x13
        pop     di
        pop     es
        jc      .bad
        test    dl, dl
        jz      .bad                    ; 接続されているドライブが 0 台

        ; CL の下位 6bit = セクタ/トラック
        mov     al, cl
        and     al, 0x3F
        movzx   ax, al
        test    ax, ax
        jz      .bad
        mov     [g_spt], ax

        ; DH = 最大ヘッド番号
        movzx   ax, dh
        inc     ax
        mov     [g_heads], ax

        ; CH + CL の上位 2bit = 最大シリンダ番号
        movzx   ax, ch
        mov     bl, cl
        and     bl, 0xC0
        shl     bx, 2
        and     bx, 0x0300
        or      ax, bx
        inc     ax
        mov     [g_cyls], ax

        ; 総セクタ数 = シリンダ * ヘッド * セクタ
        mov     ax, [g_cyls]
        mul     word [g_heads]
        mul     word [g_spt]
        movzx   eax, ax
        mov     [g_total], eax
        mov     dword [g_hidden], 0
        clc
        ret
.bad:
        stc
        ret

; ---------------------------------------------------------------------------
; hd_geometry - ハードディスクの区画は、いま入っている BPB から諸元を取る
;
; フロッピーと違って BIOS に「この区画はどこからどこまでか」を聞く方法が
; ない。区画の情報はパーティションテーブルにあり、そこは DOS のドライブ文字
; では指せない場所にある。そこで、すでに入っている BPB をそのまま信じる。
;
; つまりここでできるのは「一度フォーマットされた区画を作り直す」ことだけで、
; 何も書かれていない区画を初期化することはできない。DOS がその区画を
; ドライブとして認識していないと、そもそも書き込みの窓口 (INT 26h) が
; 使えないので、これは避けようがない。区画を新しく切ったときは、
; FDISK が最低限の BPB を置いてから FORMAT を掛ける、という順になる。
; ---------------------------------------------------------------------------
hd_geometry:
        mov     byte [is_floppy], 0

        push    ds
        pop     es
        mov     al, [dos_drive]
        mov     cx, 1
        xor     dx, dx
        mov     bx, secbuf
        int     0x25
        jc      .err
        add     sp, 2

        cmp     word [secbuf + 0x0B], SECTOR_SIZE
        jne     .bad

        mov     ax, [secbuf + 0x18]     ; セクタ/トラック
        test    ax, ax
        jz      .bad
        mov     [g_spt], ax
        mov     ax, [secbuf + 0x1A]     ; ヘッド数
        test    ax, ax
        jz      .bad
        mov     [g_heads], ax
        mov     eax, [secbuf + 0x1C]    ; 隠しセクタ (区画の開始位置)
        mov     [g_hidden], eax

        movzx   eax, word [secbuf + 0x13]
        test    eax, eax
        jnz     .have_total
        mov     eax, [secbuf + 0x20]    ; 16bit に収まらない大きさ
.have_total:
        test    eax, eax
        jz      .bad
        mov     [g_total], eax

        ; いま入っている割り付けをそのまま引き継ぐ。変えるのは予約セクタだけ。
        movzx   ax, byte [secbuf + 0x0D]
        mov     [b_secperclus], ax
        mov     ax, [secbuf + 0x11]
        mov     [b_rootent], ax
        mov     ax, [secbuf + 0x16]
        mov     [b_secperfat], ax
        mov     al, [secbuf + 0x15]
        mov     [b_media], al
        clc
        ret
.err:
        add     sp, 2
.bad:
        stc
        ret

; ============================================================================
; 総セクタ数から FAT12 の割り付けを決める
; ============================================================================
pick_layout:
        cmp     byte [is_floppy], 0
        je      .hd

        mov     eax, [g_total]
        cmp     eax, 2880
        je      .fd144
        cmp     eax, 1440
        je      .fd720
        stc
        ret

        ; ハードディスクの区画は hd_geometry がもう決めている。
        ; 予約セクタだけを 18 にして、Stage2 の置き場所を作る。
.hd:
        cmp     word [b_secperclus], 0
        je      .no
        cmp     word [b_secperfat], 0
        je      .no
        mov     word [b_reserved], MYDOS_RESERVED
        clc
        ret
.no:
        stc
        ret

.fd144:
        mov     word [b_secperclus], 1
        mov     word [b_rootent], 224
        mov     word [b_secperfat], 9
        mov     byte [b_media], 0xF0
        mov     word [b_reserved], MYDOS_RESERVED
        clc
        ret

        ; 720KB は 1 トラック 9 セクタしかないので、予約を 18 にすると
        ; Stage2 の読み込みがトラックをまたぐ。起動用には使えない形で作る。
.fd720:
        mov     word [b_secperclus], 2
        mov     word [b_rootent], 112
        mov     word [b_secperfat], 3
        mov     byte [b_media], 0xF9
        mov     word [b_reserved], 1
        mov     byte [opt_sys], 0
        mov     dx, msg_no_boot
        call    puts
        clc
        ret

; ============================================================================
; classify_fat - クラスタ数から FAT12 か FAT16 かを決める
;
; 決め手はクラスタの数だけで、ブートセクタに書く "FAT12   " の文字列は
; ただの飾り。とはいえ嘘を書くと、その文字列を見て判断する道具
; (mtools など) がディスクを読めなくなる。
; ============================================================================
classify_fat:
        push    eax
        push    ebx
        push    edx
        mov     eax, [g_total]
        movzx   ebx, word [b_reserved]
        sub     eax, ebx
        movzx   ebx, word [b_secperfat]
        shl     ebx, 1
        sub     eax, ebx
        movzx   ebx, word [b_rootent]
        add     ebx, 15
        shr     ebx, 4
        sub     eax, ebx
        xor     edx, edx
        movzx   ebx, word [b_secperclus]
        div     ebx                     ; EAX = クラスタ数
        cmp     eax, 4085
        jb      .fat12
        mov     byte [is_fat16], 1
        mov     dword [fs_type], 'FAT1'
        mov     dword [fs_type + 4], '6   '
        jmp     .out
.fat12:
        mov     byte [is_fat16], 0
        mov     dword [fs_type], 'FAT1'
        mov     dword [fs_type + 4], '2   '
.out:
        pop     edx
        pop     ebx
        pop     eax
        ret

; ============================================================================
; 確認を取る
; ============================================================================
confirm:
        ; /Q は「もう聞いた」の意味。SETUP から呼ばれるときに使う。
        ; 呼ぶ側が先に確認を取っているので、ここで二度聞くと、
        ; 流し込んだ答えが 1 つずれて噛み合わなくなる。
        cmp     byte [opt_quiet], 0
        je      .ask
        clc
        ret
.ask:
        mov     al, [dos_drive]
        add     al, 'A'
        mov     [msg_drive], al
        mov     dx, msg_warn1           ; ドライブ文字を挟んで 1 本に繋がっている
        call    puts

        ; 標準入力から 1 文字読む。AH=01h ではなくハンドル 0 を使うのは、
        ; 「FORMAT B: < YES.TXT」のように答えを流し込めるようにするため。
        ; AH=01h はキーボードを直接見るので、リダイレクトが効かない。
        xor     bx, bx
        mov     cx, 1
        mov     dx, answer
        mov     ah, 0x3F
        int     0x21
        jc      .no
        test    ax, ax
        jz      .no
        mov     dx, msg_crlf
        call    puts
        mov     al, [answer]
        call    upcase
        cmp     al, 'Y'
        je      .ok
.no:
        stc
        ret
.ok:
        clc
        ret

; ============================================================================
; ブートセクタを書く
;
; BPB を組み立て、そのうしろに「システムディスクではない」と言うだけの
; 小さなコードを置く。SYS を掛ければ本物の Stage1 で上書きされる。
; ============================================================================
write_boot:
        push    es
        push    ds
        pop     es
        mov     di, secbuf
        mov     cx, SECTOR_SIZE
        xor     al, al
        rep     stosb
        pop     es

        mov     di, secbuf
        ; EB 3C 90 = BPB を飛び越す短いジャンプ
        mov     byte [di + 0x00], 0xEB
        mov     byte [di + 0x01], 0x3C
        mov     byte [di + 0x02], 0x90

        push    si
        push    di
        mov     si, oem_name
        add     di, 0x03
        mov     cx, 8
        push    es
        push    ds
        pop     es
        rep     movsb
        pop     es
        pop     di
        pop     si

        mov     word [di + 0x0B], SECTOR_SIZE
        mov     ax, [b_secperclus]
        mov     [di + 0x0D], al
        mov     ax, [b_reserved]
        mov     [di + 0x0E], ax
        mov     byte [di + 0x10], 2             ; FAT は 2 本
        mov     ax, [b_rootent]
        mov     [di + 0x11], ax
        ; 総セクタ数は 65535 までなら 16bit の欄、それを超えるときは
        ; 16bit 側を 0 にして 32bit の欄に入れる、という決まり。
        mov     eax, [g_total]
        cmp     eax, 0x10000
        jae     .big
        mov     [di + 0x13], ax
        mov     dword [di + 0x20], 0
        jmp     .total_done
.big:
        mov     word [di + 0x13], 0
        mov     [di + 0x20], eax
.total_done:
        mov     al, [b_media]
        mov     [di + 0x15], al
        mov     ax, [b_secperfat]
        mov     [di + 0x16], ax
        mov     ax, [g_spt]
        mov     [di + 0x18], ax
        mov     ax, [g_heads]
        mov     [di + 0x1A], ax
        mov     eax, [g_hidden]
        mov     [di + 0x1C], eax                ; 区画の開始位置

        ; 拡張 BPB
        mov     al, [bios_drive]
        mov     [di + 0x24], al
        mov     byte [di + 0x25], 0
        mov     byte [di + 0x26], 0x29          ; これ以降 3 項目が有効の印
        call    make_serial                     ; EAX = ボリュームシリアル
        mov     [di + 0x27], eax

        push    si
        push    di
        mov     si, vol_label
        add     di, 0x2B
        mov     cx, 11
        push    es
        push    ds
        pop     es
        rep     movsb
        mov     si, fs_type
        mov     cx, 8
        rep     movsb
        pop     es
        pop     di
        pop     si

        ; 「システムディスクではない」と言うだけのコード
        push    si
        push    di
        mov     si, nonsys_code
        add     di, 0x3E
        mov     cx, nonsys_end - nonsys_code
        push    es
        push    ds
        pop     es
        rep     movsb
        pop     es
        pop     di
        pop     si

        mov     word [di + 0x1FE], 0xAA55

        xor     eax, eax                        ; LBA 0
        mov     cx, 1
        mov     bx, secbuf
        call    write_sectors
        ret

; ============================================================================
; 予約領域の残り (Stage2 が入る場所) を 0 で埋める
; ============================================================================
write_reserved:
        mov     cx, [b_reserved]
        dec     cx
        jcxz    .done
        call    clear_secbuf
        mov     eax, 1
.loop:
        push    cx
        mov     cx, 1
        mov     bx, secbuf
        call    write_sectors
        pop     cx
        jc      .out
        inc     eax
        loop    .loop
.done:
        clc
.out:
        ret

; ============================================================================
; FAT を 2 本書く
;
; 先頭 3 バイトだけ F0 FF FF のような形にして、あとは 0。
; クラスタ 0 にメディアディスクリプタ、クラスタ 1 に EOC を 12bit で
; 詰めたものがこの並びになる。
; ============================================================================
write_fats:
        mov     word [.fat], 0
.fat_loop:
        call    clear_secbuf
        mov     al, [b_media]
        mov     [secbuf], al
        mov     byte [secbuf + 1], 0xFF
        mov     byte [secbuf + 2], 0xFF
        ; FAT16 は 1 エントリ 16bit なので、クラスタ 1 の終端印は
        ; もう 1 バイト要る。FAT12 のつもりで 3 バイトだけ書くと、
        ; クラスタ 1 が 00FFh という中途半端な値になる。
        cmp     byte [is_fat16], 0
        je      .head_done
        mov     byte [secbuf + 3], 0xFF
.head_done:

        ; この FAT の先頭 LBA
        movzx   eax, word [.fat]
        mul     word [b_secperfat]
        movzx   eax, ax
        add     ax, [b_reserved]

        mov     cx, [b_secperfat]
        mov     [.left], cx
.sec_loop:
        mov     cx, 1
        mov     bx, secbuf
        call    write_sectors
        jc      .out
        inc     eax
        call    clear_secbuf            ; 2 セクタ目以降は全部 0
        dec     word [.left]
        jnz     .sec_loop

        inc     word [.fat]
        cmp     word [.fat], 2
        jb      .fat_loop
        clc
.out:
        ret
.fat:   dw 0
.left:  dw 0

; ============================================================================
; ルートディレクトリを 0 で埋め、ラベルがあれば置く
; ============================================================================
write_root:
        ; ルートの先頭 LBA = 予約 + FAT 2 本
        mov     ax, [b_secperfat]
        shl     ax, 1
        add     ax, [b_reserved]
        movzx   eax, ax
        mov     [.lba], eax

        ; セクタ数 = エントリ数 * 32 / 512
        mov     ax, [b_rootent]
        add     ax, 15
        mov     cl, 4
        shr     ax, cl
        mov     [.left], ax

        call    clear_secbuf

        ; 1 セクタ目にボリュームラベルを置く
        cmp     byte [vol_label], ' '
        je      .no_label
        push    si
        push    di
        mov     si, vol_label
        mov     di, secbuf
        mov     cx, 11
        push    es
        push    ds
        pop     es
        rep     movsb
        pop     es
        pop     di
        pop     si
        mov     byte [secbuf + 0x0B], 0x08      ; 属性 = ボリュームラベル
        call    get_stamp                       ; AX = 時刻, DX = 日付
        mov     [secbuf + 0x16], ax
        mov     [secbuf + 0x18], dx
.no_label:
        mov     eax, [.lba]
.loop:
        mov     cx, 1
        mov     bx, secbuf
        call    write_sectors
        jc      .out
        inc     eax
        call    clear_secbuf
        dec     word [.left]
        jnz     .loop
        clc
.out:
        ret
.lba:   dd 0
.left:  dw 0

clear_secbuf:
        push    ax
        push    cx
        push    di
        push    es
        push    ds
        pop     es
        mov     di, secbuf
        mov     cx, SECTOR_SIZE
        xor     al, al
        rep     stosb
        pop     es
        pop     di
        pop     cx
        pop     ax
        ret

; ============================================================================
; write_sectors - LBA から連続したセクタを BIOS に書かせる
;   入力: EAX = 開始 LBA, CX = セクタ数, DS:BX = 元データ
;   出力: CF=1 なら失敗
;
; DOS を通さず INT 13h を直接呼ぶ。まだ DOS が知らないディスクを
; 作っているところなので、通しようがない。
; ============================================================================
write_sectors:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    es
        mov     [.lba], eax
        mov     [.count], cx
        push    ds
        pop     es

        cmp     byte [is_floppy], 0
        jne     .loop

        ; ハードディスクの区画は DOS の絶対書き込みに任せる。区画の先頭が
        ; どこかを知っているのは DOS のほうなので、BIOS を直接呼ぶと
        ; 区画の外に書いてしまう。
        mov     al, [dos_drive]
        mov     cx, [.count]
        mov     dx, word [.lba]
        int     0x26
        jc      .fail_dos
        add     sp, 2
        jmp     .done
.loop:
        cmp     word [.count], 0
        je      .done

        ; LBA → CHS
        mov     eax, [.lba]
        xor     edx, edx
        movzx   ecx, word [g_spt]
        div     ecx                     ; EAX = トラック番号, EDX = セクタ-1
        inc     dl
        mov     [.sector], dl
        xor     edx, edx
        movzx   ecx, word [g_heads]
        div     ecx                     ; EAX = シリンダ, EDX = ヘッド
        mov     [.head], dl
        mov     [.cyl], ax

        mov     si, 3                   ; 失敗したら 3 回まで
.retry:
        mov     ax, [.cyl]
        mov     ch, al
        mov     cl, ah
        shl     cl, 6                   ; シリンダの上位 2bit
        or      cl, [.sector]
        mov     dh, [.head]
        mov     dl, [bios_drive]
        mov     ax, 0x0301              ; 1 セクタ書き込み
        int     0x13
        jnc     .ok

        push    ax
        xor     ax, ax
        mov     dl, [bios_drive]
        int     0x13                    ; ディスクリセット
        pop     ax
        dec     si
        jnz     .retry
        jmp     .fail
.ok:
        add     bx, SECTOR_SIZE
        inc     dword [.lba]
        dec     word [.count]
        jmp     .loop
.done:
        clc
        jmp     .out
.fail_dos:
        add     sp, 2
.fail:
        stc
.out:
        pop     es
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret
.lba:    dd 0
.count:  dw 0
.cyl:    dw 0
.head:   db 0
.sector: db 0

; ============================================================================
; ボリュームシリアルを日付と時刻から作る (本物の DOS と同じ考え方)
; ============================================================================
make_serial:
        push    bx
        push    cx
        push    dx
        mov     ah, 0x2A
        int     0x21                    ; CX = 年, DH = 月, DL = 日
        mov     bx, cx
        movzx   ax, dh
        shl     ax, 8
        movzx   cx, dl
        add     ax, cx
        push    ax
        mov     ah, 0x2C
        int     0x21                    ; CH = 時, CL = 分, DH = 秒, DL = 1/100
        mov     ax, cx
        pop     cx
        add     ax, cx
        push    ax
        movzx   ax, dh
        shl     ax, 8
        movzx   cx, dl
        add     ax, cx
        add     ax, bx
        pop     dx
        ; EAX = (DX << 16) | AX
        shl     edx, 16
        movzx   eax, ax
        or      eax, edx
        pop     dx
        pop     cx
        pop     bx
        ret

; get_stamp - AX = FAT の時刻, DX = FAT の日付
get_stamp:
        push    bx
        push    cx
        mov     ah, 0x2A
        int     0x21                    ; CX = 年, DH = 月, DL = 日
        sub     cx, 1980
        mov     ax, cx
        shl     ax, 9
        movzx   bx, dh
        shl     bx, 5
        or      ax, bx
        movzx   bx, dl
        or      ax, bx
        mov     [.date], ax

        mov     ah, 0x2C
        int     0x21                    ; CH = 時, CL = 分, DH = 秒
        movzx   ax, ch
        shl     ax, 11
        movzx   bx, cl
        shl     bx, 5
        or      ax, bx
        movzx   bx, dh
        shr     bx, 1
        or      ax, bx
        mov     dx, [.date]
        pop     cx
        pop     bx
        ret
.date:  dw 0

; ============================================================================
; 出来上がりの大きさを出す
; ============================================================================
report_size:
        ; 使える容量 = (総セクタ - 予約 - FAT 2 本 - ルート) * 512
        mov     eax, [g_total]
        movzx   ebx, word [b_reserved]
        sub     eax, ebx
        movzx   ebx, word [b_secperfat]
        shl     ebx, 1
        sub     eax, ebx
        movzx   ebx, word [b_rootent]
        add     ebx, 15
        shr     ebx, 4
        sub     eax, ebx

        ; セクタ数 → KB (1 セクタ 512 バイト = 0.5KB)
        shr     eax, 1
        call    put_dec32
        mov     dx, msg_kb
        call    puts
        ret

; ============================================================================
; /S: SYS.COM を起動して起動用にする
; ============================================================================
run_sys:
        mov     al, [dos_drive]
        add     al, 'A'
        mov     [sys_tail_drive], al

        ; 環境ブロックは親のものを引き継がせる (0 = 同じもの)
        mov     word [epb_env], 0
        mov     word [epb_tail], sys_tail
        mov     [epb_tail + 2], ds
        mov     word [epb_fcb1], 0x5C
        mov     [epb_fcb1 + 2], ds
        mov     word [epb_fcb2], 0x6C
        mov     [epb_fcb2 + 2], ds

        push    ds
        push    es
        mov     dx, sys_path
        mov     bx, exec_pb
        mov     ax, 0x4B00
        int     0x21
        pop     es
        pop     ds
        jc      .fail
        ret
.fail:
        mov     dx, msg_no_sys
        call    puts
        ret

; ============================================================================
; 出力
; ============================================================================
puts:
        push    ax
        mov     ah, 0x09
        int     0x21
        pop     ax
        ret

put_dec:
        movzx   eax, ax
put_dec32:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     ebx, 10
        xor     cx, cx
.split:
        xor     edx, edx
        div     ebx
        push    dx
        inc     cx
        test    eax, eax
        jnz     .split
.emit:
        pop     dx
        add     dl, '0'
        mov     ah, 0x02
        int     0x21
        loop    .emit
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

; ============================================================================
; ブートセクタに置く「システムディスクではない」コード
;
; org 0x7C00 の世界で動く。BPB のうしろ (0x7C3E) に置かれる。
; ============================================================================
nonsys_code:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7C00
        sti
        mov     si, 0x7C00 + 0x3E + (.msg - nonsys_code)
.putc:
        lodsb
        test    al, al
        jz      .wait
        mov     ah, 0x0E
        mov     bx, 7
        int     0x10
        jmp     .putc
.wait:
        xor     ah, ah
        int     0x16                    ; キーを待つ
        int     0x19                    ; もう一度起動をやり直す
.msg:   db 'Non-System disk', 13, 10
        db 'Replace and press any key', 13, 10, 0
nonsys_end:

; ============================================================================
; データ
; ============================================================================
msg_usage:    db 'FORMAT d: [/S] [/Q] [/V:label]', 13, 10
              db '  /S       make the disk bootable afterwards (runs SYS)', 13, 10
              db '  /Q       do not ask for confirmation (the caller already did)', 13, 10
              db '  /V:name  set the volume label', 13, 10, '$'
msg_no_drive: db 'FORMAT: cannot work out the layout of that drive', 13, 10
              db '  A hard disk partition must already carry a BPB (FDISK first).', 13, 10, '$'
msg_bad_media:db 'FORMAT: unsupported floppy size (1.44MB and 720KB only)', 13, 10, '$'
msg_wr_err:   db 13, 10, 'FORMAT: write failed', 13, 10, '$'
msg_aborted:  db 'FORMAT cancelled', 13, 10, '$'
msg_warn1:    db 'WARNING: everything on drive '
msg_drive:    db 'X'
msg_warn2:    db ': will be erased', 13, 10, 'Proceed with Format (Y/N)? $'
msg_crlf:     db 13, 10, '$'
msg_working:  db 'Formatting...', 13, 10, '$'
msg_done:     db 'Format complete.', 13, 10, '$'
msg_kb:       db ' KB available on disk', 13, 10, '$'
msg_no_boot:  db 'Note: a 720KB disk cannot hold the MYDOS boot loader.', 13, 10
              db '      It will be formatted as data only.', 13, 10, '$'
msg_no_sys:   db 'FORMAT: could not run SYS.COM', 13, 10, '$'

oem_name:     db 'MYDOS1.0'
is_fat16:     db 0
fs_type:      db 'FAT12   '
vol_label:    db '           '         ; 11 バイト (既定は空白)

sys_path:     db 'SYS.COM', 0
sys_tail:     db 3, ' '
sys_tail_drive: db 'X'
              db ':', 13

exec_pb:
epb_env:      dw 0
epb_tail:     dd 0
epb_fcb1:     dd 0
epb_fcb2:     dd 0

dos_drive:    db 0
bios_drive:   db 0
opt_sys:      db 0
opt_quiet:    db 0
is_floppy:    db 1
answer:       db 0

g_spt:        dw 0
g_heads:      dw 0
g_cyls:       dw 0
g_total:      dd 0
g_hidden:     dd 0

b_secperclus: dw 0
b_reserved:   dw 0
b_rootent:    dw 0
b_secperfat:  dw 0
b_media:      db 0

              align 2
secbuf:       times SECTOR_SIZE db 0
              times 512 db 0
stack_top:
