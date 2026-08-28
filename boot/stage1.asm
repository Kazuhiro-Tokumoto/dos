; ============================================================================
; stage1.asm  -  MYDOS Stage1 (ブートセクタ, 512 バイト)
;
; 役割:
;   フロッピーの 2 セクタ目以降 (Stage2) を 0x0000:0x7E00 に読み込み、
;   そこへ far jump するだけ。
;
; 互換性方針:
;   - レガシー BIOS 前提 / リアルモード
;   - 特定機種に依存しないよう、以下を徹底している:
;       * ブートドライブ番号は決め打ちせず BIOS が DL に入れてくれた値を使う
;         (00h 決め打ちでも VirtualBox では動くが、USB-FDD エミュレーション
;          などでは DL が 00h とは限らないため)
;       * 読み込みは枯れた CHS 方式 (INT 13h AH=02h) のみを使う。
;         LBA 拡張 (AH=42h) はフロッピー BIOS では未対応のことが多い。
;       * 読み込み失敗時はディスクリセット (AH=00h) を挟んで 5 回リトライ。
;         実機のフロッピーはモータ回転待ちで初回が失敗することがある。
; ============================================================================
BITS 16
ORG 0x7C00

; ---------------------------------------------------------------------------
; BPB (BIOS Parameter Block)
;
; MS-DOS 4.0 以降の FAT12 ブートセクタと同じ形にしてある。先頭 3 バイトの
; EB 3C 90 で BPB を飛び越し、0x3E から本体が始まる。
;
; ここを本物の値で埋めておくことには、互換性そのものより手前の実利がある:
;   * ホスト側の mtools (mcopy/mdir) や mount -t vfat がそのまま使える。
;     当時の .COM / .EXE をイメージに入れるのに自作ツールが要らない。
;   * fsck.fat でホスト側から FAT の整合性を検査できる。
;   * カーネル自身がディスクのジオメトリを BPB から読める (本来の DOS の作法)。
;
; reserved_sectors を 1 ではなく 18 にしているのがこの設計の肝。
; Stage1 は LBA 1 から 17 セクタを生読みして Stage2 をロードするが、
; reserved が 1 のままだと LBA 1 は FAT1 の先頭にあたり衝突する。
; 18 に増やすことで LBA 1-17 が正当な予約領域になり、FAT1 は LBA 18 から
; 始まる。失う容量は 8.5KB (1.44MB の 0.6%) で、その代わり 512 バイトの
; ブートセクタに FAT12 パーサを詰め込まずに済む。
;
;   LBA 0        Stage1 (このセクタ)
;   LBA 1-17     Stage2            ┐ reserved_sectors = 18
;   LBA 18-26    FAT1 (9 セクタ)
;   LBA 27-35    FAT2 (9 セクタ)
;   LBA 36-49    ルートディレクトリ (224 エントリ = 14 セクタ)
;   LBA 50-2879  データ領域 (2830 クラスタ)
; ---------------------------------------------------------------------------
        jmp     short start
        nop

bpb_oem:            db 'MYDOS1.0'       ; 0x03  OEM 名 (8 バイト)
bpb_bytes_per_sec:  dw 512              ; 0x0B
bpb_sec_per_clus:   db 1                ; 0x0D
bpb_reserved_secs:  dw 18               ; 0x0E  ← Stage1 1 + Stage2 17
bpb_num_fats:       db 2                ; 0x10
bpb_root_entries:   dw 224              ; 0x11
bpb_total_secs16:   dw 2880             ; 0x13  1.44MB = 2880 セクタ
bpb_media:          db 0xF0             ; 0x15  1.44MB フロッピー
bpb_secs_per_fat:   dw 9                ; 0x16
bpb_secs_per_track: dw 18               ; 0x18  ← USB 起動時に BIOS が
bpb_num_heads:      dw 2                ; 0x1A     書き換えてくる領域。
bpb_hidden_secs:    dd 0                ; 0x1C     コードを置いていないので
bpb_total_secs32:   dd 0                ; 0x20     踏まれても実害がない。
; --- 拡張 BPB (DOS 4.0+) ---
ebpb_drive_num:     db 0x00             ; 0x24
ebpb_reserved:      db 0                ; 0x25
ebpb_boot_sig:      db 0x29             ; 0x26  これ以降 3 項目が有効の印
ebpb_volume_id:     dd 0x1A2B3C4D       ; 0x27
ebpb_volume_label:  db 'MYDOS      '    ; 0x2B  11 バイト固定
ebpb_fs_type:       db 'FAT12   '       ; 0x36  8 バイト固定
                                        ; 0x3E で終わる

%if ($-$$) != 0x3E
  %error "BPB のサイズが合っていない (0x3E で終わる必要がある)"
%endif

STAGE2_OFF      equ 0x7E00      ; Stage2 のロード先オフセット (セグメント 0)
STAGE2_SECTORS  equ 17          ; 読み込むセクタ数 = reserved_sectors - 1
                                ; 1.44MB FD は 1 トラック 18 セクタ。
                                ; セクタ 1 は Stage1 自身なので、
                                ; シリンダ0/ヘッド0 の残り 17 セクタ (8704 byte)
                                ; を一括で読む。トラックをまたがないので
                                ; どの BIOS でも 1 回の INT 13h で完結する。
RETRY_COUNT     equ 5

; ---------------------------------------------------------------------------
start:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7C00      ; スタックは 0x7C00 の直下に伸ばす
        sti

        ; BIOS はブート時に DL = ブートドライブ番号を渡してくれる。
        ; これを保存して以後ずっと使う (決め打ちしない)。
        mov     [boot_drive], dl

        ; Stage2 の先頭 = このボリュームの先頭 + 1 セクタ。
        ; フロッピーは隠しセクタが 0 なので LBA 1 だが、ハードディスクの
        ; パーティションでは MBR が示した開始位置がここに入っている。
        mov     eax, [bpb_hidden_secs]
        inc     eax
        mov     [stage2_lba], eax

        ; INT 13h の拡張読み込みが使えるか調べる。
        ; ハードディスクのパーティションはトラック境界と揃っていないので、
        ; 17 セクタを CHS で一度に読むと BIOS に断られることがある。
        ; 拡張読み込みなら LBA をそのまま渡せるのでその心配がない。
        mov     ah, 0x41
        mov     bx, 0x55AA
        mov     dl, [boot_drive]
        int     0x13
        jc      .no_lba
        cmp     bx, 0xAA55
        jne     .no_lba
        test    cl, 1                   ; bit0 = 拡張読み書きが使える
        jz      .no_lba
        mov     byte [use_lba], 1
.no_lba:

        ; Stage2 が既に 0x7E00 に載っているなら読み込みは要らない。
        ; CD (El Torito のノーエミュレーション起動) では BIOS が
        ; stage1 と stage2 をまとめて 0x7C00 に読み込んでくれるため。
        ; Stage2 はオフセット 3 に 'MYS2' の目印を置いてある。
        cmp     dword [STAGE2_OFF + 3], 'MYS2'
        je      .read_ok

        mov     cx, RETRY_COUNT

.read_retry:
        push    cx

        ; --- ディスクリセット (INT 13h AH=00h) ---
        xor     ax, ax
        mov     dl, [boot_drive]
        int     0x13

        xor     ax, ax
        mov     es, ax              ; 転送先は 0000:7E00

        cmp     byte [use_lba], 0
        je      .chs

        ; --- 拡張読み込み (INT 13h AH=42h) ---
        mov     eax, [stage2_lba]
        mov     [dap_lba], eax
        mov     si, dap
        mov     dl, [boot_drive]
        mov     ah, 0x42
        int     0x13
        jmp     .after

        ; --- CHS 読み込み (INT 13h AH=02h) ---
.chs:
        mov     eax, [stage2_lba]
        xor     edx, edx
        movzx   ecx, word [bpb_secs_per_track]
        div     ecx                 ; EAX = トラック, EDX = セクタ-1
        inc     dl
        mov     [chs_sector], dl
        xor     edx, edx
        movzx   ecx, word [bpb_num_heads]
        div     ecx                 ; EAX = シリンダ, EDX = ヘッド
        mov     [chs_head], dl
        mov     ch, al
        mov     cl, ah
        shl     cl, 6               ; シリンダの上位 2bit
        or      cl, [chs_sector]
        mov     dh, [chs_head]
        mov     dl, [boot_drive]
        mov     bx, STAGE2_OFF
        mov     ah, 0x02            ; 機能: セクタ読み込み
        mov     al, STAGE2_SECTORS  ; 読み込むセクタ数
        int     0x13

.after:
        pop     cx
        jnc     .read_ok            ; CF=0 なら成功

        loop    .read_retry         ; CX を減らしてリトライ
        jmp     disk_error

.read_ok:
        ; Stage2 にもブートドライブ番号を伝える (DL で受け渡し)
        mov     dl, [boot_drive]
        jmp     0x0000:STAGE2_OFF   ; CS も 0 に確定させたいので far jump

; ---------------------------------------------------------------------------
disk_error:
        mov     si, msg_err
.puts:
        lodsb
        test    al, al
        jz      halt
        mov     ah, 0x0E
        mov     bx, 0x0007
        int     0x10
        jmp     .puts

halt:
        hlt
        jmp     halt

; ---------------------------------------------------------------------------
msg_err:        db 'STAGE1: DISK ERROR', 0
boot_drive:     db 0
use_lba:        db 0
chs_head:       db 0
chs_sector:     db 0
                align 4
stage2_lba:     dd 0

; ディスクアドレスパケット (INT 13h AH=42h 用)
dap:            db 0x10, 0          ; 大きさ / 予約
                dw STAGE2_SECTORS   ; セクタ数
                dw STAGE2_OFF       ; 転送先オフセット
                dw 0                ; 転送先セグメント
dap_lba:        dq 0                ; 開始 LBA

; --- 510 バイトまで 0 埋めし、末尾にブートシグネチャ ---
        times 510-($-$$) db 0
        dw 0xAA55
