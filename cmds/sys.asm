; ============================================================================
; sys.asm  -  SYS コマンド (ディスクを起動できるようにする)
;
;   SYS d:
;
; いま使っているドライブから、指定したドライブへ「起動に要るもの」を移す。
; 移すのは 3 つ。
;
;   1. ブートセクタ (LBA 0)      … Stage1。ただし BPB は相手のものを残す
;   2. 予約領域 (LBA 1-17)       … Stage2
;   3. IO.SYS と COMMAND.COM     … ふつうのファイルとして
;
; --- BPB を残すのが肝 -------------------------------------------------------
;
; ブートセクタは「コード + BPB」が 1 つのセクタに同居している。コードは
; どのディスクでも同じでよいが、BPB はそのディスクの形そのものなので、
; 丸ごと上書きすると相手のディスクの構造が分からなくなる。1.44MB のディスクの
; Stage1 を 720KB のディスクに書いたら、720KB のディスクが「自分は 2880
; セクタある」と名乗り始める。だから
;
;   00-0A  相手のディスクへ移す (ジャンプ命令と OEM 名)
;   0B-3D  相手のものを残す     (BPB と拡張 BPB)
;   3E-1FF 相手のディスクへ移す (ブートコード)
;
; という混ぜ方をする。本物の SYS もこれと同じことをしている。
;
; --- 予約セクタが 18 必要 ---------------------------------------------------
;
; MYDOS の Stage1 は LBA 1 から 17 セクタを生読みして Stage2 を読み込む。
; 相手の予約セクタが 18 未満だと、そこは FAT の場所なので置けない。
; その場合は断る。MYDOS の FORMAT で作ったディスクなら 18 になっている。
;
; --- IO.SYS の位置について --------------------------------------------------
;
; 本物の MS-DOS は IO.SYS がルートディレクトリの先頭エントリで、しかも
; 連続したクラスタに置かれていることを要求した (ブートセクタ 512 バイトに
; FAT を辿るコードが入らなかったため)。MYDOS の Stage2 は 8.5KB あって
; FAT12 を自分で辿れるので、名前さえ合っていればどこに置かれていてもよい。
; だから普通にファイルとしてコピーするだけで済む。
; ============================================================================
        cpu     386
        bits    16
        org     0x100

SECTOR_SIZE     equ 512
NEED_RESERVED   equ 18                  ; Stage1 + Stage2
COPY_BUF_SIZE   equ 8192

ATTR_HIDDEN     equ 0x02
ATTR_SYSTEM     equ 0x04

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

        ; 元は「いま使っているドライブ」
        mov     ah, 0x19
        int     0x21
        mov     [src_drive], al
        cmp     al, [dst_drive]
        je      .same

        call    read_boot_sectors
        jc      .read_err

        call    check_target
        jc      .not_enough

        call    write_boot
        jc      .write_err

        call    copy_stage2
        jc      .write_err

        mov     si, f_io
        call    copy_system_file
        jc      .file_err
        mov     si, f_io
        mov     cx, ATTR_HIDDEN | ATTR_SYSTEM
        call    set_dst_attr

        mov     si, f_command
        call    copy_system_file
        jc      .file_err

        ; 書きかけを落として、諸元を作り直させる
        mov     ah, 0x0D
        int     0x21

        mov     dx, msg_done
        call    puts
        mov     ax, 0x4C00
        int     0x21

.usage:
        mov     dx, msg_usage
        call    puts
        jmp     .fail
.same:
        mov     dx, msg_same
        call    puts
        jmp     .fail
.read_err:
        mov     dx, msg_read_err
        call    puts
        jmp     .fail
.not_enough:
        mov     dx, msg_not_mydos
        call    puts
        jmp     .fail
.write_err:
        mov     dx, msg_write_err
        call    puts
        jmp     .fail
.file_err:
        mov     dx, msg_file_err
        call    puts
.fail:
        mov     ax, 0x4C01
        int     0x21

; ============================================================================
; コマンドラインを読む
; ============================================================================
parse_cmdline:
        mov     si, 0x81
        movzx   cx, byte [0x80]
        test    cx, cx
        jz      .bad
.skip:
        cmp     byte [si], ' '
        jne     .have
        inc     si
        dec     cx
        jnz     .skip
        jmp     .bad
.have:
        lodsb
        dec     cx
        cmp     al, 'a'
        jb      .up_done
        cmp     al, 'z'
        ja      .up_done
        sub     al, 0x20
.up_done:
        cmp     al, 'A'
        jb      .bad
        cmp     al, 'Z'
        ja      .bad
        mov     [dst_letter], al
        sub     al, 'A'
        mov     [dst_drive], al
        jcxz    .bad
        lodsb
        cmp     al, ':'
        jne     .bad
        clc
        ret
.bad:
        stc
        ret

; ============================================================================
; 元と先のブートセクタを読む
;
; INT 25h は DOS の中で唯一 iret で戻らない割り込みで、INT が積んだ FLAGS を
; スタックに残したまま retf で返る。CF を先に見てから add sp,2 で始末する。
; ============================================================================
read_boot_sectors:
        push    ds
        pop     es

        mov     al, [src_drive]
        mov     cx, 1
        xor     dx, dx
        mov     bx, src_boot
        int     0x25
        jc      .err
        add     sp, 2

        mov     al, [dst_drive]
        mov     cx, 1
        xor     dx, dx
        mov     bx, dst_boot
        int     0x25
        jc      .err
        add     sp, 2
        clc
        ret
.err:
        add     sp, 2
        stc
        ret

; ============================================================================
; 相手のディスクが MYDOS の Stage2 を置ける形か
; ============================================================================
check_target:
        cmp     word [dst_boot + 0x0B], SECTOR_SIZE
        jne     .bad
        cmp     word [dst_boot + 0x0E], NEED_RESERVED
        jb      .bad
        clc
        ret
.bad:
        stc
        ret

; ============================================================================
; ブートセクタを作って書く
;
; 相手の BPB (0B-3D) を残し、それ以外を元のディスクのもので置き換える。
; ============================================================================
write_boot:
        push    ds
        pop     es

        ; まず元のブートセクタを丸ごと下敷きにする
        mov     si, src_boot
        mov     di, new_boot
        mov     cx, SECTOR_SIZE
        rep     movsb

        ; そのうえに相手の BPB を貼り直す
        mov     si, dst_boot + 0x0B
        mov     di, new_boot + 0x0B
        mov     cx, 0x3E - 0x0B
        rep     movsb

        mov     al, [dst_drive]
        mov     cx, 1
        xor     dx, dx
        mov     bx, new_boot
        int     0x26
        jc      .err
        add     sp, 2
        clc
        ret
.err:
        add     sp, 2
        stc
        ret

; ============================================================================
; Stage2 (LBA 1 から予約セクタの終わりまで) を移す
; ============================================================================
copy_stage2:
        mov     dx, 1
.loop:
        cmp     dx, NEED_RESERVED
        jae     .done

        push    dx
        push    ds
        pop     es
        mov     al, [src_drive]
        mov     cx, 1
        mov     bx, secbuf
        int     0x25
        jc      .err
        add     sp, 2
        pop     dx

        push    dx
        mov     al, [dst_drive]
        mov     cx, 1
        mov     bx, secbuf
        int     0x26
        jc      .err
        add     sp, 2
        pop     dx

        inc     dx
        jmp     .loop
.done:
        clc
        ret
.err:
        add     sp, 2
        pop     dx
        stc
        ret

; ============================================================================
; システムファイルを 1 つコピーする
;   入力: DS:SI = "IO.SYS" のような 8.3 の名前 (0 終端)
; ============================================================================
copy_system_file:
        push    si
        call    build_paths
        pop     si

        ; 相手に同じ名前のものがあれば、属性を外してから上書きする
        push    si
        mov     dx, dst_path
        mov     ax, 0x4301
        xor     cx, cx
        int     0x21
        pop     si

        push    si
        mov     dx, msg_copying
        call    puts
        pop     si
        push    si
        call    puts_z
        mov     dx, msg_crlf
        call    puts
        pop     si

        ; 元を開く
        mov     dx, src_path
        mov     ax, 0x3D00
        int     0x21
        jc      .no_src
        mov     [src_handle], ax

        ; 先を作る
        mov     dx, dst_path
        xor     cx, cx
        mov     ah, 0x3C
        int     0x21
        jc      .no_dst
        mov     [dst_handle], ax

.loop:
        mov     bx, [src_handle]
        mov     cx, COPY_BUF_SIZE
        mov     dx, copy_buf
        mov     ah, 0x3F
        int     0x21
        jc      .io_err
        test    ax, ax
        jz      .eof
        mov     cx, ax

        mov     bx, [dst_handle]
        mov     dx, copy_buf
        mov     ah, 0x40
        int     0x21
        jc      .io_err
        cmp     ax, cx
        jne     .io_err                 ; 書ききれていない = ディスクが一杯
        jmp     .loop
.eof:
        mov     bx, [dst_handle]
        mov     ah, 0x3E
        int     0x21
        mov     bx, [src_handle]
        mov     ah, 0x3E
        int     0x21
        clc
        ret

.io_err:
        mov     bx, [dst_handle]
        mov     ah, 0x3E
        int     0x21
.no_dst:
        mov     bx, [src_handle]
        mov     ah, 0x3E
        int     0x21
.no_src:
        stc
        ret

; ============================================================================
; build_paths - "A:\NAME" と "B:\NAME" を組み立てる
;   入力: DS:SI = 名前
; ============================================================================
build_paths:
        push    ax
        push    di
        push    si
        push    es
        push    ds
        pop     es

        mov     al, [src_drive]
        add     al, 'A'
        mov     [src_path], al
        mov     byte [src_path + 1], ':'
        mov     byte [src_path + 2], '\'
        mov     di, src_path + 3
        push    si
        call    .copy_name
        pop     si

        mov     al, [dst_letter]
        mov     [dst_path], al
        mov     byte [dst_path + 1], ':'
        mov     byte [dst_path + 2], '\'
        mov     di, dst_path + 3
        call    .copy_name

        pop     es
        pop     si
        pop     di
        pop     ax
        ret
.copy_name:
        lodsb
        stosb
        test    al, al
        jnz     .copy_name
        ret

; ============================================================================
; set_dst_attr - 相手側のファイルに属性を付ける
;   入力: DS:SI = 名前, CX = 属性
; ============================================================================
set_dst_attr:
        push    cx
        call    build_paths
        pop     cx
        mov     dx, dst_path
        mov     ax, 0x4301
        int     0x21
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

; 0 終端の文字列を出す
puts_z:
        push    ax
        push    dx
        push    si
.loop:
        lodsb
        test    al, al
        jz      .done
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        jmp     .loop
.done:
        pop     si
        pop     dx
        pop     ax
        ret

; ============================================================================
; データ
; ============================================================================
msg_usage:     db 'SYS d:', 13, 10
               db '  Copies the boot loader, IO.SYS and COMMAND.COM to that drive.', 13, 10, '$'
msg_same:      db 'SYS: source and destination are the same drive', 13, 10, '$'
msg_read_err:  db 'SYS: cannot read the boot sector', 13, 10, '$'
msg_not_mydos: db 'SYS: that disk has no room for the MYDOS boot loader', 13, 10
               db '     (it needs 18 reserved sectors - format it with FORMAT)', 13, 10, '$'
msg_write_err: db 'SYS: write failed', 13, 10, '$'
msg_file_err:  db 'SYS: could not copy the system files', 13, 10, '$'
msg_copying:   db 'Copying $'
msg_crlf:      db 13, 10, '$'
msg_done:      db 'System transferred', 13, 10, '$'

f_io:          db 'IO.SYS', 0
f_command:     db 'COMMAND.COM', 0

src_drive:     db 0
dst_drive:     db 0
dst_letter:    db 'A'
src_handle:    dw 0
dst_handle:    dw 0

src_path:      times 20 db 0
dst_path:      times 20 db 0

               align 2
src_boot:      times SECTOR_SIZE db 0
dst_boot:      times SECTOR_SIZE db 0
new_boot:      times SECTOR_SIZE db 0
secbuf:        times SECTOR_SIZE db 0
copy_buf:      times COPY_BUF_SIZE db 0
               times 512 db 0
stack_top:
