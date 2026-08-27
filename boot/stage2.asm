; ============================================================================
; stage2.asm  -  MYDOS Stage2 (FAT12 ローダー)
;
; ロード先: 0x0000:0x7E00 (Stage1 が予約領域から 17 セクタ読み込む)
; 入力:     DL = BIOS が渡してきたブートドライブ番号 (Stage1 から中継)
;
; やること:
;   1. ブートセクタ (0x7C00 にまだ残っている) から BPB を読み取る
;   2. FAT1 を丸ごとメモリに読み込む
;   3. ルートディレクトリから "IO      SYS" を名前で探す
;   4. クラスタ連鎖を辿って IO.SYS を 0x1000:0x0000 に読み込む
;   5. DL にブートドライブ番号を入れて制御を渡す
;
; なぜ Stage1 ではなくここで FAT を扱うのか:
;   本物の MS-DOS はブートセクタ 512 バイトの中で FAT12 を辿って IO.SYS を
;   探している。同じことはできるが極端に窮屈で、少し機能を足すたびに
;   バイト単位の削り合いになる。Stage2 には 8704 バイトあるので、
;   ここに置けば素直に書けるうえ、カーネルの差し替えが mcopy 一回で済む。
;
; メモリの使い方 (すべてセグメント 0):
;   0x7C00 - 0x7DFF   Stage1 (BPB がここに残っている)
;   0x7E00 - 0x9EFF   Stage2 (このコード)
;   0xA000 - 0xB1FF   FAT1 のコピー (9 セクタ)
;   0xB400 - 0xB5FF   ディレクトリ読み取り用の 1 セクタ
;   0x10000 -         IO.SYS のロード先 (0x1000:0x0000)
; ============================================================================
BITS 16
ORG 0x7E00

        jmp     strict near stage2_start    ; 3 バイト固定 (E9 xx xx)
stage2_magic:
        db      'MYS2'          ; Stage1 が「Stage2 はもう載っているか」を
                                ; 見分けるための目印。オフセット 3 に固定。

BOOTSEC         equ 0x7C00      ; Stage1 が読み込まれた場所 = BPB の在処
FAT_BUF         equ 0xA000      ; FAT1 全体を置く
DIR_BUF         equ 0xB400      ; ディレクトリを 1 セクタずつ読む場所
KERNEL_SEG      equ 0x1000      ; IO.SYS のロード先セグメント
SERIAL_BASE     equ 0x3F8       ; COM1

; ============================================================================
stage2_start:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7C00
        sti

        mov     [boot_drive], dl

        call    serial_init

        mov     si, msg_banner
        call    puts

        ; ------------------------------------------------------------
        ; 1. BPB を取り込む
        ;
        ; Stage1 はスタックを 0x7C00 の直下に伸ばしているので、
        ; 0x7C00-0x7DFF のブートセクタは踏まれずに残っている。
        ; そこから必要な値だけ自分の変数に写しておく。
        ; ------------------------------------------------------------
        call    load_bpb
        jc      .bad_bpb

        ; ------------------------------------------------------------
        ; 2. FAT1 を丸ごと読む
        ; ------------------------------------------------------------
        mov     ax, [reserved_secs]     ; FAT1 の LBA
        mov     cx, [secs_per_fat]
        mov     bx, FAT_BUF
        push    es
        xor     dx, dx
        mov     es, dx
        call    read_sectors
        pop     es
        jc      .disk_err

        ; ------------------------------------------------------------
        ; 3. ルートディレクトリから IO.SYS を探す
        ; ------------------------------------------------------------
        mov     si, kernel_name
        call    find_in_root
        jc      .no_kernel

        ; AX = 先頭クラスタ / DX:CX = ファイルサイズ
        mov     [kernel_clus], ax

        ; ------------------------------------------------------------
        ; 4. クラスタ連鎖を辿ってロード
        ; ------------------------------------------------------------
        mov     si, msg_loading
        call    puts

        mov     ax, [kernel_clus]
        mov     bx, KERNEL_SEG
        call    load_chain
        jc      .disk_err

        mov     si, msg_ok
        call    puts

        ; ------------------------------------------------------------
        ; 5. カーネルへ
        ;
        ; 渡すのは DL (ブートドライブ番号) だけ。BPB はカーネルが自分で
        ; LBA 0 を読み直して取る。0x7C00 の内容が生きているかどうかに
        ; 依存させないため (カーネルは自分自身を 0x0060:0 へ再配置するので、
        ; そのとき 0x7C00 を踏み潰す可能性がある)。
        ; ------------------------------------------------------------
        mov     dl, [boot_drive]
        jmp     KERNEL_SEG:0x0000

.bad_bpb:
        mov     si, msg_bad_bpb
        jmp     fatal
.no_kernel:
        mov     si, msg_no_kernel
        jmp     fatal
.disk_err:
        mov     si, msg_disk_err
        jmp     fatal

fatal:
        call    puts
.hang:
        hlt
        jmp     .hang

; ============================================================================
; load_bpb - ブートセクタ (0x7C00) から BPB を自分の変数へ取り込む
;   出力: CF=1 なら値がおかしい
; ============================================================================
load_bpb:
        push    ax
        push    dx

        mov     ax, [BOOTSEC + 0x0B]    ; bytes per sector
        cmp     ax, 512
        jne     .bad                    ; 512 以外は扱わない

        mov     al, [BOOTSEC + 0x0D]
        xor     ah, ah
        test    al, al
        jz      .bad
        mov     [sec_per_clus], ax

        mov     ax, [BOOTSEC + 0x0E]
        mov     [reserved_secs], ax

        mov     al, [BOOTSEC + 0x10]
        xor     ah, ah
        mov     [num_fats], ax

        mov     ax, [BOOTSEC + 0x11]
        mov     [root_entries], ax

        mov     ax, [BOOTSEC + 0x16]
        test    ax, ax
        jz      .bad
        mov     [secs_per_fat], ax

        mov     ax, [BOOTSEC + 0x18]
        test    ax, ax
        jz      .bad
        mov     [secs_per_track], ax

        mov     ax, [BOOTSEC + 0x1A]
        test    ax, ax
        jz      .bad
        mov     [num_heads], ax

        ; ルートディレクトリの LBA = 予約 + FAT の総セクタ数
        mov     ax, [num_fats]
        mul     word [secs_per_fat]
        add     ax, [reserved_secs]
        mov     [root_lba], ax

        ; ルートディレクトリのセクタ数 = (エントリ数 * 32 + 511) / 512
        mov     ax, [root_entries]
        add     ax, 15                  ; 切り上げ (32 バイト * 16 = 1 セクタ)
        shr     ax, 4
        mov     [root_secs], ax

        ; データ領域の LBA
        add     ax, [root_lba]
        mov     [data_lba], ax

        clc
        jmp     .done
.bad:
        stc
.done:
        pop     dx
        pop     ax
        ret

; ============================================================================
; read_sectors - LBA から連続したセクタを読む
;   入力: AX = 開始 LBA, CX = セクタ数, ES:BX = 転送先
;   出力: CF=1 なら失敗。ES:BX を含め全レジスタは保存される
;
; 1 回の INT 13h で 1 セクタずつ読む。まとめて読むほうが速いが、
; トラック境界をまたぐ転送を BIOS が拒むことがあり、その扱いを間違えると
; 「途中まで読めているのに気づかない」という一番厄介な壊れ方をする。
; ブート時に数十セクタ読むだけなので、確実さを取る。
; ============================================================================
read_sectors:
        pusha
        push    es
        mov     [.lba], ax
        mov     [.count], cx
.loop:
        cmp     word [.count], 0
        je      .done

        mov     ax, [.lba]
        call    read_one
        jc      .fail

        add     bx, 512
        jnc     .no_wrap
        ; オフセットが 64KB を回ったのでセグメントを進める
        mov     ax, es
        add     ax, 0x1000
        mov     es, ax
.no_wrap:
        inc     word [.lba]
        dec     word [.count]
        jmp     .loop
.done:
        pop     es
        popa
        clc
        ret
.fail:
        pop     es
        popa
        stc
        ret
.lba:   dw 0
.count: dw 0

; ============================================================================
; read_one - 1 セクタを CHS で読む (5 回までリトライ)
;   入力: AX = LBA, ES:BX = 転送先
;   出力: CF=1 なら失敗
;
; LBA から CHS への変換:
;   sector   = LBA mod secs_per_track + 1   (セクタ番号は 1 起算)
;   head     = (LBA / secs_per_track) mod heads
;   cylinder = (LBA / secs_per_track) / heads
; ============================================================================
read_one:
        push    ax
        push    bx
        push    cx
        push    dx
        push    di

        xor     dx, dx
        div     word [secs_per_track]   ; AX = LBA/spt, DX = LBA mod spt
        mov     cl, dl
        inc     cl                      ; CL = セクタ番号 (1 起算)
        xor     dx, dx
        div     word [num_heads]        ; AX = シリンダ, DX = ヘッド
        mov     ch, al                  ; CH = シリンダ下位 8bit
        mov     dh, dl                  ; DH = ヘッド
        mov     dl, [boot_drive]

        mov     di, 5                   ; リトライ回数
.retry:
        mov     ax, 0x0201              ; AH=02h 読み込み, AL=1 セクタ
        int     0x13
        jnc     .ok

        ; 失敗したらディスクリセットを挟んでやり直す。
        ; 実機のフロッピーはモータの回転待ちで初回が失敗することがある。
        push    ax
        xor     ax, ax
        mov     dl, [boot_drive]
        int     0x13
        pop     ax
        ; DL がリセットで壊れている可能性があるので入れ直す
        mov     dl, [boot_drive]
        dec     di
        jnz     .retry
        stc
        jmp     .out
.ok:
        clc
.out:
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ============================================================================
; find_in_root - ルートディレクトリから 8.3 名でファイルを探す
;   入力: DS:SI = 11 バイトの 8.3 名 (空白パディング済み)
;   出力: CF=0 なら AX = 先頭クラスタ, DX:CX = サイズ
;         CF=1 なら見つからなかった
; ============================================================================
find_in_root:
        push    bx
        push    di
        push    bp
        push    es

        mov     [.name], si

        xor     ax, ax
        mov     es, ax

        mov     ax, [root_lba]
        mov     [.lba], ax
        mov     ax, [root_secs]
        mov     [.left], ax

.next_sector:
        cmp     word [.left], 0
        je      .not_found

        mov     ax, [.lba]
        mov     bx, DIR_BUF
        call    read_one
        jc      .not_found

        mov     bx, DIR_BUF
        mov     bp, 16                  ; 1 セクタに 16 エントリ
.next_entry:
        mov     al, [bx]
        test    al, al
        jz      .not_found              ; 0x00 = ここから先は未使用
        cmp     al, 0xE5
        je      .skip                   ; 削除済み
        mov     al, [bx + 11]
        test    al, 0x08                ; ボリュームラベル
        jnz     .skip
        test    al, 0x10                ; ディレクトリ
        jnz     .skip

        ; 名前を 11 バイト比較
        push    si
        push    di
        push    cx
        mov     si, [.name]
        mov     di, bx
        mov     cx, 11
        repe    cmpsb
        pop     cx
        pop     di
        pop     si
        je      .found

.skip:
        add     bx, 32
        dec     bp
        jnz     .next_entry

        inc     word [.lba]
        dec     word [.left]
        jmp     .next_sector

.found:
        mov     ax, [bx + 26]           ; 先頭クラスタ
        mov     cx, [bx + 28]           ; サイズ 下位
        mov     dx, [bx + 30]           ; サイズ 上位
        pop     es
        pop     bp
        pop     di
        pop     bx
        clc
        ret

.not_found:
        pop     es
        pop     bp
        pop     di
        pop     bx
        stc
        ret

.name:  dw 0
.lba:   dw 0
.left:  dw 0

; ============================================================================
; fat_next - FAT12 のエントリを引いて次のクラスタ番号を得る
;   入力: AX = 現在のクラスタ
;   出力: AX = 次のクラスタ (0xFF8 以上なら終端)
;
; FAT12 は 1 エントリ 12bit なので、2 エントリで 3 バイトを共有する。
; クラスタ n のエントリは FAT の n + n/2 バイト目から 16bit 読み、
; n が偶数なら下位 12bit、奇数なら上位 12bit を取る。
; ============================================================================
fat_next:
        push    bx
        push    cx

        mov     bx, ax
        mov     cx, ax
        shr     cx, 1
        add     bx, cx                  ; BX = n + n/2
        add     bx, FAT_BUF
        mov     cx, ax                  ; 奇偶の判定用に残す
        mov     ax, [bx]                ; 16bit まとめて読む

        test    cx, 1
        jz      .even
        shr     ax, 4                   ; 奇数クラスタは上位 12bit
        jmp     .done
.even:
        and     ax, 0x0FFF              ; 偶数クラスタは下位 12bit
.done:
        pop     cx
        pop     bx
        ret

; ============================================================================
; load_chain - クラスタ連鎖を辿ってファイルを読み込む
;   入力: AX = 先頭クラスタ, BX = 転送先セグメント (オフセット 0 から)
;   出力: CF=1 なら失敗
; ============================================================================
load_chain:
        push    es
        pusha

        mov     es, bx
        xor     bx, bx
        mov     [.clus], ax

.loop:
        mov     ax, [.clus]
        cmp     ax, 2
        jb      .done                   ; 0/1 は正規のクラスタ番号ではない
        cmp     ax, 0x0FF8
        jae     .done                   ; 終端

        ; クラスタ番号 → LBA
        sub     ax, 2
        mul     word [sec_per_clus]
        add     ax, [data_lba]

        mov     cx, [sec_per_clus]
        call    read_sectors
        jc      .fail

        ; read_sectors は ES:BX を進めてくれないので自分で進める
        mov     ax, [sec_per_clus]
        mov     cx, 5                   ; 512 = 1 << 9, セグメント換算は << 5
        shl     ax, cl                  ; AX = クラスタあたりのパラグラフ数
        mov     cx, es
        add     cx, ax
        mov     es, cx

        mov     ax, [.clus]
        call    fat_next
        mov     [.clus], ax

        ; '.' を出して進捗を見せる (実機で固まったときの切り分け用)
        mov     al, '.'
        call    putc
        jmp     .loop

.done:
        popa
        pop     es
        clc
        ret
.fail:
        popa
        pop     es
        stc
        ret
.clus:  dw 0

; ============================================================================
; 出力まわり
;
; 画面 (INT 10h) とシリアル (COM1) の両方に出す。シリアルに出しておくと
; QEMU を -serial stdio で回したときにホスト側でそのままログが取れるので、
; ブートの失敗を目視ではなくテキストで追える。実機でも画面が出ない種類の
; 故障をシリアルで切り分けられる。
; ============================================================================
putc:
        push    ax
        push    bx
        push    dx

        push    ax
        mov     ah, 0x0E
        mov     bx, 0x0007
        int     0x10
        pop     ax

        call    serial_putc

        pop     dx
        pop     bx
        pop     ax
        ret

puts:
        push    ax
        push    si
.loop:
        lodsb
        test    al, al
        jz      .done
        cmp     al, 10                  ; LF は CR+LF に直す
        jne     .out
        push    ax
        mov     al, 13
        call    putc
        pop     ax
.out:
        call    putc
        jmp     .loop
.done:
        pop     si
        pop     ax
        ret

serial_init:
        push    ax
        push    dx
        mov     dx, SERIAL_BASE + 1     ; 割り込みを止める
        xor     al, al
        out     dx, al
        mov     dx, SERIAL_BASE + 3     ; DLAB を立てる
        mov     al, 0x80
        out     dx, al
        mov     dx, SERIAL_BASE + 0     ; 分周比 = 1 (115200 bps)
        mov     al, 1
        out     dx, al
        mov     dx, SERIAL_BASE + 1
        xor     al, al
        out     dx, al
        mov     dx, SERIAL_BASE + 3     ; 8bit / パリティなし / ストップ 1
        mov     al, 0x03
        out     dx, al
        mov     dx, SERIAL_BASE + 2     ; FIFO 有効
        mov     al, 0xC7
        out     dx, al
        mov     dx, SERIAL_BASE + 4     ; DTR/RTS/OUT2
        mov     al, 0x0B
        out     dx, al
        pop     dx
        pop     ax
        ret

serial_putc:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     cx, ax                  ; 送る文字を退避
        mov     dx, SERIAL_BASE + 5     ; LSR
        mov     bx, 0xFFFF              ; 送信可能になるのを待つ (無限には待たない)
.wait:
        in      al, dx
        test    al, 0x20                ; bit5 = 送信バッファ空
        jnz     .send
        dec     bx
        jnz     .wait
        jmp     .out                    ; シリアルが無い環境では諦める
.send:
        mov     dx, SERIAL_BASE
        mov     ax, cx
        out     dx, al
.out:
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ============================================================================
; データ
; ============================================================================
msg_banner:     db 'MYDOS Stage2', 10, 0
msg_loading:    db 'Loading IO.SYS', 0
msg_ok:         db ' OK', 10, 0
msg_bad_bpb:    db 10, 'STAGE2: BPB is invalid', 10, 0
msg_no_kernel:  db 10, 'STAGE2: IO.SYS not found', 10, 0
msg_disk_err:   db 10, 'STAGE2: disk read error', 10, 0

kernel_name:    db 'IO      SYS'        ; 8.3 を空白で埋めた形

boot_drive:     db 0
kernel_clus:    dw 0

; BPB から取り込む値 (すべて word で持つ。掛け算・割り算で使うため)
sec_per_clus:   dw 0
reserved_secs:  dw 0
num_fats:       dw 0
root_entries:   dw 0
secs_per_fat:   dw 0
secs_per_track: dw 0
num_heads:      dw 0
root_lba:       dw 0
root_secs:      dw 0
data_lba:       dw 0
