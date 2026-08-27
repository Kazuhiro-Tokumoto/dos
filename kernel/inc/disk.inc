; ============================================================================
; disk.inc  -  ブロックデバイス (INT 13h によるセクタ入出力) と BPB
;
; ジオメトリは決め打ちせず、必ず BPB から取る。これが本来の DOS の作法で、
; 1.44MB 以外のフォーマットに広げるときもここを直さずに済む。
; ============================================================================

; ---------------------------------------------------------------------------
; disk_init - ブートセクタを読み直して BPB を取り込む
;   入力: [boot_drive] が設定済みであること
;   出力: CF=1 なら BPB がおかしい
;
; Stage2 から BPB を受け取るのではなく自分で読み直す。カーネルは起動直後に
; 自分自身を 0x0060:0 へ再配置するので、そのとき 0x7C00 のブートセクタを
; 踏み潰す可能性があるため。「たまたま残っている値」に依存させない。
; ---------------------------------------------------------------------------
disk_init:
        push    ax
        push    bx
        push    dx
        push    es

        push    ds
        pop     es
        mov     bx, sector_buf
        xor     ax, ax                  ; LBA 0 = ブートセクタ
        ; ジオメトリがまだ分からないので CHS(0,0,1) を直接指定する。
        ; LBA 0 は必ずシリンダ0/ヘッド0/セクタ1 なので変換は要らない。
        mov     word [secs_per_track], 18    ; read_one が使う暫定値
        mov     word [num_heads], 2
        call    disk_read_one
        jc      .bad

        mov     bx, sector_buf
        cmp     word [bx + 0x0B], SECTOR_SIZE
        jne     .bad

        mov     al, [bx + 0x0D]
        xor     ah, ah
        test    al, al
        jz      .bad
        mov     [sec_per_clus], ax

        mov     ax, [bx + 0x0E]
        test    ax, ax
        jz      .bad
        mov     [reserved_secs], ax

        mov     al, [bx + 0x10]
        xor     ah, ah
        test    al, al
        jz      .bad
        mov     [num_fats], ax

        mov     ax, [bx + 0x11]
        mov     [root_entries], ax

        mov     ax, [bx + 0x13]
        test    ax, ax
        jz      .bad
        mov     [total_secs], ax

        mov     al, [bx + 0x15]
        mov     [media_desc], al

        mov     ax, [bx + 0x16]
        test    ax, ax
        jz      .bad
        mov     [secs_per_fat], ax

        mov     ax, [bx + 0x18]
        test    ax, ax
        jz      .bad
        mov     [secs_per_track], ax

        mov     ax, [bx + 0x1A]
        test    ax, ax
        jz      .bad
        mov     [num_heads], ax

        ; ボリュームラベルとシリアル番号 (AH=69h や DIR が使う)
        mov     ax, [bx + 0x27]
        mov     [volume_serial], ax
        mov     ax, [bx + 0x29]
        mov     [volume_serial + 2], ax

        ; --- レイアウトを計算する ---
        mov     ax, [num_fats]
        mul     word [secs_per_fat]
        add     ax, [reserved_secs]
        mov     [root_lba], ax          ; ルートディレクトリの開始 LBA

        mov     ax, [root_entries]
        add     ax, 15
        shr     ax, 4                   ; (エントリ数 * 32 + 511) / 512
        mov     [root_secs], ax

        add     ax, [root_lba]
        mov     [data_lba], ax          ; データ領域の開始 LBA

        ; クラスタ 1 個のバイト数。ファイル位置の計算で毎回使う
        mov     ax, [sec_per_clus]
        mov     dx, SECTOR_SIZE
        mul     dx
        mov     [clus_bytes], ax

        ; 使えるクラスタ数 = (総セクタ - データ開始) / クラスタあたりセクタ数
        mov     ax, [total_secs]
        sub     ax, [data_lba]
        xor     dx, dx
        div     word [sec_per_clus]
        add     ax, 2                   ; クラスタ番号は 2 から始まる
        mov     [max_cluster], ax       ; これ以上は存在しないクラスタ番号

        ; FAT を丸ごとメモリに載せる。1.44MB なら 9 セクタ = 4608 バイトで、
        ; 常駐させても惜しくない。クラスタを辿るたびにディスクを叩かずに済む。
        mov     ax, [reserved_secs]
        mov     cx, [secs_per_fat]
        mov     bx, fat_buf
        push    ds
        pop     es
        call    disk_read
        jc      .bad
        mov     byte [fat_dirty], 0

        clc
        jmp     .done
.bad:
        stc
.done:
        pop     es
        pop     dx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; disk_read - LBA から連続したセクタを読む
;   入力: AX = 開始 LBA, CX = セクタ数, ES:BX = 転送先
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
;   入力: AX = 開始 LBA, CX = セクタ数, ES:BX = 転送元
;   出力: CF=1 なら失敗
; ---------------------------------------------------------------------------
disk_write:
        push    si
        mov     si, 1                   ; 1 = 書き込み
        call    disk_xfer
        pop     si
        ret

; ---------------------------------------------------------------------------
; disk_xfer - 読み書き共通の本体
;   入力: AX = LBA, CX = セクタ数, ES:BX = 転送先/元, SI = 0:読み 1:書き
; ---------------------------------------------------------------------------
disk_xfer:
        pusha
        push    es
        mov     [.lba], ax
        mov     [.count], cx
        mov     [.dir], si
.loop:
        cmp     word [.count], 0
        je      .done
        mov     ax, [.lba]
        mov     si, [.dir]
        test    si, si
        jz      .rd
        call    disk_write_one
        jmp     .check
.rd:
        call    disk_read_one
.check:
        jc      .fail
        add     bx, SECTOR_SIZE
        jnc     .no_wrap
        mov     ax, es                  ; オフセットが 64KB を回った
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
.dir:   dw 0

; ---------------------------------------------------------------------------
; disk_read_one / disk_write_one - 1 セクタを CHS で転送する
;   入力: AX = LBA, ES:BX = 転送先/元
;   出力: CF=1 なら失敗
;
; LBA から CHS への変換:
;   sector   = LBA mod secs_per_track + 1   (セクタ番号は 1 起算)
;   head     = (LBA / secs_per_track) mod heads
;   cylinder = (LBA / secs_per_track) / heads
; ---------------------------------------------------------------------------
; AX = LBA。機能番号はレジスタではなく変数で渡す。
; AH に入れて渡すと AX を LBA として使えなくなる (LBA 0 が 0x0200 に化ける)。
disk_read_one:
        mov     byte [disk_chs_op.func], 0x02
        jmp     disk_chs_op

disk_write_one:
        mov     byte [disk_chs_op.func], 0x03
        jmp     disk_chs_op

; AX = LBA, ES:BX = 転送先/元, [.func] = INT 13h の機能番号
disk_chs_op:
        push    ax
        push    bx
        push    cx
        push    dx
        push    di

        xor     dx, dx
        div     word [secs_per_track]   ; AX = LBA/spt, DX = LBA mod spt
        mov     cl, dl
        inc     cl                      ; セクタ番号は 1 起算
        xor     dx, dx
        div     word [num_heads]        ; AX = シリンダ, DX = ヘッド
        mov     ch, al
        mov     dh, dl
        mov     dl, [boot_drive]

        mov     di, 5                   ; リトライ回数
.retry:
        mov     ah, [.func]
        mov     al, 1                   ; 1 セクタ
        int     0x13
        jnc     .ok

        ; 失敗したらディスクリセットを挟む。実機のフロッピーはモータの
        ; 回転待ちで初回が失敗することがある。
        push    ax
        xor     ax, ax
        mov     dl, [boot_drive]
        int     0x13
        pop     ax
        mov     dl, [boot_drive]        ; リセットで壊れている可能性がある
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
.func:  db 0

; ---------------------------------------------------------------------------
; disk_flush_fat - 変更された FAT をディスクに書き戻す
;   出力: CF=1 なら失敗
;
; FAT はコピーが 2 つあり、両方を同じ内容に保つ必要がある。片方だけ
; 書いて落ちると CHKDSK が「FAT が食い違っている」と言い出す。
; ---------------------------------------------------------------------------
disk_flush_fat:
        cmp     byte [fat_dirty], 0
        je      .nothing

        pusha
        push    es
        push    ds
        pop     es

        mov     cx, [num_fats]
        mov     ax, [reserved_secs]
.loop:
        push    cx
        push    ax
        mov     bx, fat_buf
        mov     cx, [secs_per_fat]
        call    disk_write
        pop     ax
        pop     cx
        jc      .fail
        add     ax, [secs_per_fat]      ; 次の FAT のコピーへ
        loop    .loop

        mov     byte [fat_dirty], 0
        pop     es
        popa
        clc
        ret
.fail:
        pop     es
        popa
        stc
        ret
.nothing:
        clc
        ret

; --- BPB から取り込む値 ----------------------------------------------------
; すべて word で持つ。掛け算・割り算にそのまま使えるようにするため。
boot_drive:     db 0
media_desc:     db 0
sec_per_clus:   dw 0
reserved_secs:  dw 0
num_fats:       dw 0
root_entries:   dw 0
total_secs:     dw 0
secs_per_fat:   dw 0
secs_per_track: dw 0
num_heads:      dw 0
volume_serial:  dd 0

; --- 計算で求めるレイアウト ------------------------------------------------
root_lba:       dw 0            ; ルートディレクトリの開始 LBA
root_secs:      dw 0            ; ルートディレクトリのセクタ数
data_lba:       dw 0            ; データ領域の開始 LBA
max_cluster:    dw 0            ; 存在する最大クラスタ番号 + 1
clus_bytes:     dw 0            ; 1 クラスタのバイト数 (sec_per_clus * 512)
fat_dirty:      db 0            ; FAT を書き換えたか
