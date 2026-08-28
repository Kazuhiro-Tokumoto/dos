; ============================================================================
; ramdisk.asm  -  インストール可能なブロックデバイスドライバ (RAM ディスク)
;
; CONFIG.SYS に
;   DEVICE=\RAMDISK.SYS
; と書くと、メモリ上に 64KB の FAT12 ボリュームができて、次のドライブ文字
; (フロッピー 1 台とハードディスク 2 つがあれば E:) に割り当てられる。
;
; ブロックデバイスの作法が文字デバイスと違うのは 3 つ。
;   ・名前を持たない。ヘッダの +0Ah はユニット数 (1 バイト) になる
;   ・INIT でユニット数と BPB 配列を返す。DOS はそれを見てドライブを生やす
;   ・READ / WRITE はバイト数ではなくセクタ数で来る
;
; ディスクの実体はドライバ本体の直後に置く。ファイルには入っていないので
; .SYS は小さいままだが、INIT で「常駐部分の末尾」をその先まで伸ばして
; 返すことで、DOS がそこまでを確保したままにしてくれる。RAMDRIVE.SYS の
; 類が実際にやっていたのと同じ手。
;
; 中身は INIT のときに自分でフォーマットする。ブートセクタ (BPB)、
; FAT 1 本、ルートディレクトリを書けば、あとは DOS がふつうに使える。
; ============================================================================
        cpu     386
        bits    16
        org     0

; --- 要求ヘッダのオフセット ---
REQ_LENGTH      equ 0x00
REQ_UNIT        equ 0x01
REQ_CMD         equ 0x02
REQ_STATUS      equ 0x03
REQ_MEDIA       equ 0x0D
REQ_BUFFER      equ 0x0E
REQ_COUNT       equ 0x12
REQ_START       equ 0x14
REQ_STARTL      equ 0x1A
REQ_INIT_UNITS  equ 0x0D
REQ_INIT_END    equ 0x0E
REQ_INIT_BPB    equ 0x12
REQ_INIT_DRIVE  equ 0x16

DEVS_DONE       equ 0x0100
DEVS_ERROR      equ 0x8000
DEVE_UNKNOWNCMD equ 0x03
DEVE_SECTORNOTFOUND equ 0x08

DEVA_NONIBM     equ 0x2000              ; BPB は自分で返す

; --- ディスクの諸元 ---
RD_SECTORS      equ 128                 ; 128 * 512 = 64KB
RD_SECPERCLUS   equ 1
RD_RESERVED     equ 1
RD_NUMFATS      equ 1
RD_ROOTENT      equ 32                  ; 32 * 32 = 1024 バイト = 2 セクタ
RD_SECPERFAT    equ 1
RD_MEDIA        equ 0xFA                ; RAM ディスクの慣習
SECTOR_SIZE     equ 512
RD_PARAS        equ RD_SECTORS * (SECTOR_SIZE / 16)

; ============================================================================
; デバイスヘッダ (18 バイト)
; ブロックデバイスなので +0Ah は名前ではなくユニット数。
; ============================================================================
header:
        dw      0xFFFF, 0xFFFF          ; 次のドライバは無い
        dw      DEVA_NONIBM
        dw      strategy
        dw      interrupt
        db      1                       ; ユニット数
        db      0, 0, 0, 0, 0, 0, 0

; ============================================================================
; STRATEGY
; ============================================================================
strategy:
        mov     [cs:req_off], bx
        mov     [cs:req_seg], es
        retf

; ============================================================================
; INTERRUPT
; ============================================================================
interrupt:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    ds
        push    es
        pushf
        cld

        push    cs
        pop     ds
        les     bx, [req_ptr]

        mov     al, [es:bx + REQ_CMD]
        cmp     al, 0
        je      .init
        cmp     al, 1
        je      .mediacheck
        cmp     al, 2
        je      .buildbpb
        cmp     al, 4
        je      .read
        cmp     al, 8
        je      .write
        cmp     al, 9
        je      .write
        cmp     al, 15
        je      .removable
        jmp     .unknown

; --- INIT (0) ---------------------------------------------------------------
.init:
        ; ディスクの実体はこのドライバの直後。段落境界に切り上げる。
        mov     ax, resident_end + 15
        mov     cl, 4
        shr     ax, cl
        mov     dx, cs
        add     ax, dx
        mov     [data_seg], ax

        call    format_disk

        ; ユニット数と BPB 配列を返す。配列の中身は far ポインタなので、
        ; 読み込まれた場所 (CS) が決まるここで埋める。
        mov     ax, cs
        mov     [bpb_array + 2], ax
        mov     byte [es:bx + REQ_INIT_UNITS], 1
        mov     word [es:bx + REQ_INIT_BPB], bpb_array
        mov     [es:bx + REQ_INIT_BPB + 2], ax

        ; 常駐部分の末尾 = ディスクの終わり
        mov     ax, [data_seg]
        add     ax, RD_PARAS
        mov     word [es:bx + REQ_INIT_END], 0
        mov     [es:bx + REQ_INIT_END + 2], ax

        ; 割り当てられるドライブ文字を控えて表示する
        mov     al, [es:bx + REQ_INIT_DRIVE]
        add     al, 'A'
        mov     [msg_drive], al
        push    es
        push    bx
        mov     dx, msg_hello
        mov     ah, 0x09
        int     0x21
        pop     bx
        pop     es
        jmp     .ok

; --- メディアチェック (1) ---------------------------------------------------
; RAM ディスクは入れ替わらないので「変わっていない」と答える。
.mediacheck:
        mov     byte [es:bx + REQ_MEDIA + 1], 1
        jmp     .ok

; --- BUILD BPB (2) ----------------------------------------------------------
.buildbpb:
        mov     word [es:bx + REQ_BUFFER], bpb
        mov     ax, cs
        mov     [es:bx + REQ_BUFFER + 2], ax
        jmp     .ok

; --- 取り外せるか (15) ------------------------------------------------------
.removable:
        mov     word [es:bx + REQ_STATUS], DEVS_DONE     ; busy=0 = 固定
        jmp     .leave

; --- 読み書き (4 / 8 / 9) ---------------------------------------------------
.read:
        mov     byte [dir], 0
        jmp     .xfer
.write:
        mov     byte [dir], 1
.xfer:
        mov     cx, [es:bx + REQ_COUNT]
        test    cx, cx
        jnz     .have_count
        jmp     .ok
.have_count:

        ; 開始セクタ。FFFFh なら 32bit の欄を見る (DOS 4 以降の約束)
        mov     ax, [es:bx + REQ_START]
        cmp     ax, 0xFFFF
        jne     .have_start
        cmp     byte [es:bx + REQ_LENGTH], 30
        jb      .bad_sector
        mov     eax, [es:bx + REQ_STARTL]
        cmp     eax, RD_SECTORS
        jae     .bad_sector
.have_start:
        mov     [start], ax

        ; 範囲の検査
        mov     dx, ax
        add     dx, cx
        cmp     dx, RD_SECTORS
        ja      .bad_sector

        ; 転送先のセグメント:オフセット
        mov     ax, [es:bx + REQ_BUFFER + 2]
        mov     [buf_seg], ax
        mov     ax, [es:bx + REQ_BUFFER]
        mov     [buf_off], ax

.loop:
        push    cx

        ; セクタ n は data_seg + n * 32 パラグラフの先頭にある
        mov     ax, [start]
        mov     dx, SECTOR_SIZE / 16
        mul     dx
        add     ax, [data_seg]
        mov     [sec_seg], ax

        cmp     byte [dir], 0
        je      .do_read

        ; 書き込み: 呼び出し側 → ディスク
        mov     ds, [cs:buf_seg]
        mov     si, [cs:buf_off]
        mov     es, [cs:sec_seg]
        xor     di, di
        jmp     .move
.do_read:
        ; 読み込み: ディスク → 呼び出し側
        mov     ds, [cs:sec_seg]
        xor     si, si
        mov     es, [cs:buf_seg]
        mov     di, [cs:buf_off]
.move:
        mov     cx, SECTOR_SIZE / 2
        rep     movsw

        push    cs
        pop     ds

        ; 次のセクタへ。オフセットが 64KB を回ったらセグメントを進める。
        inc     word [start]
        add     word [buf_off], SECTOR_SIZE
        jnc     .no_wrap
        add     word [buf_seg], 0x1000
.no_wrap:
        pop     cx
        loop    .loop

        les     bx, [req_ptr]
        jmp     .ok

.bad_sector:
        les     bx, [req_ptr]
        mov     word [es:bx + REQ_STATUS], DEVS_DONE | DEVS_ERROR | DEVE_SECTORNOTFOUND
        jmp     .leave
.unknown:
        mov     word [es:bx + REQ_STATUS], DEVS_DONE | DEVS_ERROR | DEVE_UNKNOWNCMD
        jmp     .leave
.ok:
        mov     word [es:bx + REQ_STATUS], DEVS_DONE
.leave:
        popf
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        retf

; ============================================================================
; format_disk - まっさらな FAT12 ボリュームを作る
;
; ブートセクタ (BPB 入り)、FAT 1 本、ルートディレクトリを置けば、
; あとは DOS が読み書きできる。ブートコードは要らない (このディスクから
; 起動することはない) が、BPB の形は本物どおりにしておく。
; ============================================================================
format_disk:
        push    ax
        push    cx
        push    dx
        push    si
        push    di
        push    ds
        push    es

        push    cs
        pop     ds

        ; --- 全体を 0 で埋める ---
        mov     dx, [data_seg]
        mov     cx, RD_SECTORS
.clear:
        mov     es, dx
        xor     di, di
        push    cx
        mov     cx, SECTOR_SIZE / 2
        xor     ax, ax
        rep     stosw
        pop     cx
        add     dx, SECTOR_SIZE / 16
        loop    .clear

        ; --- セクタ 0: ブートセクタ ---
        mov     es, [data_seg]
        xor     di, di
        mov     byte [es:di + 0x00], 0xEB       ; jmp short + nop
        mov     byte [es:di + 0x01], 0x3C
        mov     byte [es:di + 0x02], 0x90
        push    di
        add     di, 0x03
        mov     si, oem_name
        mov     cx, 8
        rep     movsb
        pop     di

        ; BPB (11h から) をそのまま写す
        push    di
        add     di, 0x0B
        mov     si, bpb
        mov     cx, bpb_end - bpb
        rep     movsb
        pop     di

        mov     byte [es:di + 0x26], 0x29       ; 拡張 BPB の印
        mov     dword [es:di + 0x27], 0x4D444F53 ; ボリュームシリアル
        push    di
        add     di, 0x2B
        mov     si, vol_label
        mov     cx, 11
        rep     movsb
        mov     si, fs_type
        mov     cx, 8
        rep     movsb
        pop     di
        mov     word [es:di + 0x1FE], 0xAA55

        ; --- FAT: 先頭 3 バイトはメディアバイト + FFh + FFh ---
        mov     ax, [data_seg]
        add     ax, RD_RESERVED * (SECTOR_SIZE / 16)
        mov     es, ax
        xor     di, di
        mov     byte [es:di + 0], RD_MEDIA
        mov     byte [es:di + 1], 0xFF
        mov     byte [es:di + 2], 0xFF

        ; --- ルートディレクトリ: ボリュームラベルだけ置いておく ---
        mov     ax, [data_seg]
        add     ax, (RD_RESERVED + RD_NUMFATS * RD_SECPERFAT) * (SECTOR_SIZE / 16)
        mov     es, ax
        xor     di, di
        mov     si, vol_label
        mov     cx, 11
        rep     movsb
        mov     byte [es:di], 0x08              ; 属性 = ボリュームラベル

        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     ax
        ret

; --- 変数 ------------------------------------------------------------------
req_ptr:
req_off:        dw 0
req_seg:        dw 0

data_seg:       dw 0                    ; ディスクの実体があるセグメント
sec_seg:        dw 0
buf_seg:        dw 0
buf_off:        dw 0
start:          dw 0
dir:            db 0

; BPB。ブートセクタの +0Bh に置くものと、INIT で DOS に返すものは同じ形。
bpb:
                dw SECTOR_SIZE          ; +00 セクタあたりバイト数
                db RD_SECPERCLUS        ; +02 クラスタあたりセクタ数
                dw RD_RESERVED          ; +03 予約セクタ数
                db RD_NUMFATS           ; +05 FAT の数
                dw RD_ROOTENT           ; +06 ルートのエントリ数
                dw RD_SECTORS           ; +08 総セクタ数
                db RD_MEDIA             ; +0A メディアディスクリプタ
                dw RD_SECPERFAT         ; +0B FAT 1 個のセクタ数
                dw RD_SECTORS           ; +0D セクタ/トラック (形だけ)
                dw 1                    ; +0F ヘッド数
                dd 0                    ; +11 隠しセクタ数
                dd 0                    ; +15 総セクタ数 (32bit 版)
bpb_end:

; INIT が返す BPB 配列。ユニットごとに far ポインタが並ぶ。
; セグメントは読み込まれるまで決まらないので INIT で埋める。
bpb_array:      dw bpb
                dw 0

oem_name:       db 'MYDOS1.0'
vol_label:      db 'RAMDISK    '
fs_type:        db 'FAT12   '

msg_hello:      db 'RAMDISK.SYS installed as drive '
msg_drive:      db 'E:', 13, 10, '$'

resident_end:
