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

        ; 拡張読み込みが使えるかどうかを一度だけ調べる
        call    check_lba

        ; FAT は丸ごと読まない。必要なところだけ、そのつど読む。
        ; FAT12 のフロッピーなら 4.5KB で済むが、FAT16 の 40MB
        ; パーティションでは 40KB になり、置き場所がカーネルの
        ; ロード先 (0x10000) とぶつかる。

        ; ------------------------------------------------------------
        ; 2. ルートディレクトリから IO.SYS を探す
        ; ------------------------------------------------------------
        mov     si, kernel_name
        call    find_in_root
        jc      .no_kernel

        ; AX = 先頭クラスタ / DX:CX = ファイルサイズ
        mov     [kernel_clus], ax

        ; ------------------------------------------------------------
        ; 3. クラスタ連鎖を辿ってロード
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
        ; 4. カーネルへ
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

        ; 隠しセクタ = このボリュームがディスクの何セクタ目から始まるか。
        ; フロッピーは 0、ハードディスクのパーティションは MBR が示す
        ; 開始 LBA が入っている。以降 LBA はすべてこれを足して使う。
        mov     eax, [BOOTSEC + 0x1C]
        mov     [part_lba], eax

        ; ルートディレクトリの LBA = 予約 + FAT の総セクタ数
        mov     ax, [num_fats]
        mul     word [secs_per_fat]     ; DX:AX (FAT16 の大きな FAT でも入る)
        movzx   ecx, dx
        shl     ecx, 16
        movzx   eax, ax
        or      eax, ecx
        movzx   ecx, word [reserved_secs]
        add     eax, ecx
        mov     [root_lba], eax

        ; ルートディレクトリのセクタ数 = (エントリ数 * 32 + 511) / 512
        mov     ax, [root_entries]
        add     ax, 15                  ; 切り上げ (32 バイト * 16 = 1 セクタ)
        shr     ax, 4
        mov     [root_secs], ax

        ; データ領域の LBA
        movzx   eax, ax
        add     eax, [root_lba]
        mov     [data_lba], eax

        ; --- FAT12 か FAT16 か ---
        ;
        ; 決め手はクラスタの数だけ。ブートセクタの "FAT12   " という文字列は
        ; ただの飾りで、これを見て決めてはいけないことになっている
        ; (書き換えずにサイズだけ変えたディスクが実在するため)。
        ;
        ;   クラスタ数 = (総セクタ - データ領域の開始) / クラスタあたりセクタ
        ;   4085 未満なら FAT12、それ以上なら FAT16
        movzx   eax, word [BOOTSEC + 0x13]      ; 16bit の総セクタ数
        test    eax, eax
        jnz     .have_total
        mov     eax, [BOOTSEC + 0x20]           ; 0 なら 32bit のほうを見る
.have_total:
        sub     eax, [data_lba]
        xor     edx, edx
        movzx   ecx, word [sec_per_clus]
        div     ecx                             ; EAX = クラスタ数
        cmp     eax, 4085
        jb      .is12
        mov     byte [is_fat16], 1
        mov     word [fat_eof], 0xFFF8
        jmp     .fat_done
.is12:
        mov     byte [is_fat16], 0
        mov     word [fat_eof], 0x0FF8
.fat_done:

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
;   入力: EAX = 開始 LBA (ボリュームの先頭からの相対), CX = セクタ数,
;         ES:BX = 転送先
;   出力: CF=1 なら失敗。ES:BX を含め全レジスタは保存される
;
; 1 回の INT 13h で 1 セクタずつ読む。まとめて読むほうが速いが、
; トラック境界をまたぐ転送を BIOS が拒むことがあり、その扱いを間違えると
; 「途中まで読めているのに気づかない」という一番厄介な壊れ方をする。
; ブート時に数十セクタ読むだけなので、確実さを取る。
; ============================================================================
read_sectors:
        pushad
        push    es
        mov     [.lba], eax
        mov     [.count], cx
.loop:
        cmp     word [.count], 0
        je      .done

        mov     eax, [.lba]
        call    read_one
        jc      .fail

        add     bx, 512
        jnc     .no_wrap
        ; オフセットが 64KB を回ったのでセグメントを進める
        mov     ax, es
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
.fail:
        pop     es
        popad
        stc
        ret
.lba:   dd 0
.count: dw 0

; ============================================================================
; read_one - 1 セクタ読む (5 回までリトライ)
;   入力: EAX = LBA (ボリュームの先頭からの相対), ES:BX = 転送先
;   出力: CF=1 なら失敗
;
; まず隠しセクタ (このボリュームがディスクの何セクタ目から始まるか) を足して
; 絶対 LBA にする。フロッピーは 0 なのでそのまま、ハードディスクの
; パーティションでは MBR が示した開始位置が足される。
;
; 読み方は 2 通り。
;
;   ・INT 13h AH=42h (拡張読み込み)
;     アドレスを 64bit の LBA でそのまま渡せる。使えるかどうかは AH=41h で
;     聞く。1994 年以降の BIOS はほぼ持っている。
;   ・INT 13h AH=02h (CHS)
;     LBA を シリンダ/ヘッド/セクタ に直して渡す。当時の BIOS はこれしか
;     持っていなかった。シリンダが 1024 を超えると届かない。
;
;       sector   = LBA mod secs_per_track + 1   (セクタ番号は 1 起算)
;       head     = (LBA / secs_per_track) mod heads
;       cylinder = (LBA / secs_per_track) / heads
;
; フロッピーでは拡張読み込みを持っていない BIOS が多いので、必ず両方要る。
; ============================================================================
read_one:
        pushad
        push    es

        add     eax, [part_lba]         ; 絶対 LBA にする
        mov     [.lba], eax

        mov     di, 5                   ; リトライ回数
.retry:
        cmp     byte [use_lba], 0
        je      .chs

        ; --- 拡張読み込み (AH=42h) ---
        ; ディスクアドレスパケットを組み立てて渡す
        mov     word [.dap_size], 0x0010
        mov     word [.dap_count], 1
        mov     [.dap_off], bx
        mov     [.dap_seg], es
        mov     eax, [.lba]
        mov     [.dap_lba], eax
        mov     dword [.dap_lba + 4], 0
        push    ds
        pop     es
        mov     si, .dap_size
        mov     dl, [boot_drive]
        mov     ah, 0x42
        int     0x13
        pop     es
        push    es
        jnc     .ok
        jmp     .failed

        ; --- CHS (AH=02h) ---
.chs:
        mov     eax, [.lba]
        xor     edx, edx
        movzx   ecx, word [secs_per_track]
        div     ecx                     ; EAX = トラック, EDX = セクタ-1
        inc     dl
        mov     [.sector], dl
        xor     edx, edx
        movzx   ecx, word [num_heads]
        div     ecx                     ; EAX = シリンダ, EDX = ヘッド
        cmp     eax, 1023
        ja      .failed                 ; CHS では届かない位置
        mov     [.cyl], ax
        mov     [.head], dl

        mov     ax, [.cyl]
        mov     ch, al
        mov     cl, ah
        shl     cl, 6                   ; シリンダの上位 2bit
        or      cl, [.sector]
        mov     dh, [.head]
        mov     dl, [boot_drive]
        mov     ax, 0x0201              ; AH=02h 読み込み, AL=1 セクタ
        int     0x13
        jnc     .ok

.failed:
        ; 失敗したらディスクリセットを挟んでやり直す。
        ; 実機のフロッピーはモータの回転待ちで初回が失敗することがある。
        xor     ax, ax
        mov     dl, [boot_drive]
        int     0x13
        dec     di
        jnz     .retry
        pop     es
        popad
        stc
        ret
.ok:
        pop     es
        popad
        clc
        ret

.lba:       dd 0
.cyl:       dw 0
.head:      db 0
.sector:    db 0
            align 4
; ディスクアドレスパケット (INT 13h AH=42h 用)
.dap_size:  dw 0x0010
.dap_count: dw 0
.dap_off:   dw 0
.dap_seg:   dw 0
.dap_lba:   dq 0

; ============================================================================
; check_lba - INT 13h の拡張読み込みが使えるか調べる
;
; AH=41h に BX=55AAh を入れて呼び、BX が AA55h になって返り、CX の bit0 が
; 立っていれば使える。フロッピーの BIOS は持っていないことが多い。
; ============================================================================
check_lba:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     byte [use_lba], 0
        mov     ah, 0x41
        mov     bx, 0x55AA
        mov     dl, [boot_drive]
        int     0x13
        jc      .out
        cmp     bx, 0xAA55
        jne     .out
        test    cl, 1                   ; bit0 = 拡張読み書きが使える
        jz      .out
        mov     byte [use_lba], 1
.out:
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

        mov     eax, [root_lba]
        mov     [.lba], eax
        mov     ax, [root_secs]
        mov     [.left], ax

.next_sector:
        cmp     word [.left], 0
        je      .not_found

        mov     eax, [.lba]
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

        inc     dword [.lba]
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
.lba:   dd 0
.left:  dw 0

; ============================================================================
; fat_next - FAT のエントリを引いて次のクラスタ番号を得る
;   入力: AX = 現在のクラスタ
;   出力: AX = 次のクラスタ (終端なら fat_eof 以上)
;
; FAT は丸ごと読まず、要る 2 セクタだけをそのつど読む。FAT16 の 40MB
; パーティションでは FAT が 40KB あり、丸ごと置くとカーネルのロード先
; (0x10000) とぶつかるため。2 セクタ読むのは FAT12 のエントリが
; セクタ境界をまたぐことがあるから。
;
; FAT12 は 1 エントリ 12bit なので 2 エントリで 3 バイトを共有する。
; クラスタ n のエントリは FAT の n + n/2 バイト目から 16bit 読み、
; n が偶数なら下位 12bit、奇数なら上位 12bit。
; FAT16 は素直に n * 2 バイト目の 16bit そのもの。
; ============================================================================
fat_next:
        push    bx
        push    cx
        push    dx
        push    si
        push    es

        xor     cx, cx
        mov     es, cx                  ; FAT_BUF はセグメント 0

        ; FAT の何バイト目か
        cmp     byte [is_fat16], 0
        je      .fat12
        movzx   ebx, ax
        shl     ebx, 1
        jmp     .have_off
.fat12:
        movzx   ebx, ax
        mov     ecx, ebx
        shr     ecx, 1
        add     ebx, ecx                ; n + n/2
.have_off:
        mov     si, ax                  ; 奇偶の判定用に残す

        ; そのバイトが入っているセクタを読む
        mov     eax, ebx
        shr     eax, 9                  ; / 512
        and     bx, 0x01FF              ; セクタ内のずれ
        call    fat_load
        jc      .bad

        mov     ax, [es:FAT_BUF + bx]

        cmp     byte [is_fat16], 0
        jne     .done
        test    si, 1
        jz      .even
        shr     ax, 4                   ; 奇数クラスタは上位 12bit
        jmp     .done
.even:
        and     ax, 0x0FFF              ; 偶数クラスタは下位 12bit
.done:
        pop     es
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret
.bad:
        mov     ax, 0xFFFF              ; 読めなかったら終端扱い
        jmp     .done

; ============================================================================
; fat_load - FAT の EAX セクタ目 (FAT の先頭から数えて) を FAT_BUF に載せる
;   出力: CF=1 なら読めなかった
;
; 同じセクタが続けて要求されることが多いので、いま載っているものを覚えて
; おいて読み直しを省く。クラスタ連鎖を辿るときはたいてい隣のエントリを
; 続けて引くので、これが効く。
; ============================================================================
fat_load:
        push    eax
        push    bx
        push    cx
        push    es
        cmp     eax, [fat_cur]
        je      .hit

        mov     [fat_cur], eax
        movzx   ecx, word [reserved_secs]
        add     eax, ecx                ; FAT1 の中の位置 → ボリューム相対 LBA
        xor     cx, cx
        mov     es, cx
        mov     bx, FAT_BUF
        mov     cx, 2                   ; 12bit のエントリが境界をまたぐので 2 枚
        call    read_sectors
        jc      .fail
.hit:
        pop     es
        pop     cx
        pop     bx
        pop     eax
        clc
        ret
.fail:
        mov     dword [fat_cur], 0xFFFFFFFF
        pop     es
        pop     cx
        pop     bx
        pop     eax
        stc
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
        cmp     ax, [fat_eof]
        jae     .done                   ; 終端

        ; クラスタ番号 → LBA (32bit で計算する。FAT16 のパーティションでは
        ; 16bit に収まらない)
        sub     ax, 2
        movzx   eax, ax
        movzx   ecx, word [sec_per_clus]
        mul     ecx
        add     eax, [data_lba]

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
part_lba:       dd 0            ; このボリュームの先頭 (絶対 LBA)
is_fat16:       db 0            ; 1 なら FAT16
                align 2
fat_eof:        dw 0x0FF8       ; これ以上なら連鎖の終わり
fat_cur:        dd 0xFFFFFFFF   ; FAT_BUF に載っているセクタ
use_lba:        db 0            ; INT 13h の拡張読み込みが使えるか
                align 2
root_lba:       dd 0
root_secs:      dw 0
data_lba:       dd 0
