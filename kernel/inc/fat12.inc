; ============================================================================
; fat12.inc  -  FAT12 ファイルシステム
;
; FAT は起動時に丸ごとメモリへ載せてある (disk_init)。クラスタを 1 つ辿る
; たびにディスクを読みに行かずに済み、書き換えは fat_dirty を立てておいて
; まとめて 2 つのコピーへ書き戻す。
; ============================================================================

FAT_EOC         equ 0x0FF8      ; これ以上なら連鎖の終わり
FAT_EOC_MARK    equ 0x0FFF      ; 終端として書き込む値
FAT_BAD         equ 0x0FF7

; ---------------------------------------------------------------------------
; fat_get - クラスタ番号から FAT のエントリを引く
;   入力: AX = クラスタ番号
;   出力: AX = 次のクラスタ番号 (FAT_EOC 以上なら終端)
;
; FAT12 は 1 エントリ 12bit なので 2 エントリで 3 バイトを共有している。
; クラスタ n のエントリは FAT の n + n/2 バイト目から 16bit 読み、
; n が偶数なら下位 12bit、奇数なら上位 12bit を取る。
; ---------------------------------------------------------------------------
fat_get:
        push    bx
        push    cx

        mov     bx, ax
        shr     bx, 1
        add     bx, ax                  ; BX = n + n/2
        add     bx, fat_buf
        mov     cx, ax                  ; 奇偶の判定に使う
        mov     ax, [bx]

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

; ---------------------------------------------------------------------------
; fat_set - FAT のエントリを書き換える
;   入力: AX = クラスタ番号, DX = 書き込む値 (12bit)
; ---------------------------------------------------------------------------
fat_set:
        push    ax
        push    bx
        push    cx
        push    dx

        mov     bx, ax
        shr     bx, 1
        add     bx, ax
        add     bx, fat_buf
        mov     cx, ax
        mov     ax, [bx]                ; 隣のエントリを壊さないよう読んでから直す

        test    cx, 1
        jz      .even
        and     ax, 0x000F              ; 上位 12bit を入れ替える
        mov     cx, dx
        and     cx, 0x0FFF
        shl     cx, 4
        or      ax, cx
        jmp     .store
.even:
        and     ax, 0xF000              ; 下位 12bit を入れ替える
        and     dx, 0x0FFF
        or      ax, dx
.store:
        mov     [bx], ax
        mov     byte [fat_dirty], 1

        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; fat_alloc - 空きクラスタを 1 つ確保する
;   出力: CF=0 なら AX = クラスタ番号、CF=1 なら空きなし
;
; 確保したクラスタにはその場で終端マークを書き込む。ここを後回しにすると、
; 次に fat_alloc を呼んだときに同じクラスタがもう一度返ってきて、
; 「512 バイトを超えるファイルが 1 クラスタおきに壊れる」という
; 追いにくい不具合になる。
; ---------------------------------------------------------------------------
fat_alloc:
        push    bx
        push    dx

        mov     bx, 2                   ; クラスタ番号は 2 から
.loop:
        cmp     bx, [max_cluster]
        jae     .full
        mov     ax, bx
        call    fat_get
        test    ax, ax
        jz      .found
        inc     bx
        jmp     .loop

.found:
        mov     ax, bx
        mov     dx, FAT_EOC_MARK
        call    fat_set                 ; ← 確保した瞬間に印を付ける
        mov     ax, bx
        clc
        jmp     .out
.full:
        stc
.out:
        pop     dx
        pop     bx
        ret

; ---------------------------------------------------------------------------
; fat_free_chain - クラスタ連鎖をまとめて解放する
;   入力: AX = 先頭クラスタ
; ---------------------------------------------------------------------------
fat_free_chain:
        push    ax
        push    cx
        push    dx
.loop:
        cmp     ax, 2
        jb      .done
        cmp     ax, FAT_EOC
        jae     .done
        mov     cx, ax
        call    fat_get                 ; 先に次を読んでおく
        push    ax
        mov     ax, cx
        xor     dx, dx
        call    fat_set                 ; 0 = 空き
        pop     ax
        jmp     .loop
.done:
        pop     dx
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; fat_count_free - 空きクラスタ数を数える (AH=36h 用)
;   出力: AX = 空きクラスタ数
; ---------------------------------------------------------------------------
fat_count_free:
        push    bx
        push    cx
        xor     cx, cx
        mov     bx, 2
.loop:
        cmp     bx, [max_cluster]
        jae     .done
        mov     ax, bx
        call    fat_get
        test    ax, ax
        jnz     .next
        inc     cx
.next:
        inc     bx
        jmp     .loop
.done:
        mov     ax, cx
        pop     cx
        pop     bx
        ret

; ---------------------------------------------------------------------------
; clus_to_lba - クラスタ番号を LBA に直す
;   入力: AX = クラスタ番号
;   出力: AX = そのクラスタの先頭 LBA
; ---------------------------------------------------------------------------
clus_to_lba:
        push    dx
        sub     ax, 2
        mul     word [sec_per_clus]
        add     ax, [data_lba]
        pop     dx
        ret

; ============================================================================
; ディレクトリ
;
; ルートディレクトリは固定長の連続セクタ、サブディレクトリはクラスタ連鎖と
; いう二重構造になっている。呼び出し側がそれを意識しないで済むよう、
; 「ディレクトリの n セクタ目」を LBA に変換する層をかぶせている。
; クラスタ 0 はルートディレクトリを表す (FAT の '..' も親がルートのときは
; 0 を入れる約束なので、そのまま辻褄が合う)。
; ============================================================================

; ---------------------------------------------------------------------------
; dir_map_sector - ディレクトリの n セクタ目の LBA を求める
;   入力: AX = ディレクトリの先頭クラスタ (0 = ルート), BX = セクタ番号 (0 起算)
;   出力: CF=0 なら AX = LBA、CF=1 なら範囲外 (ディレクトリの終わり)
; ---------------------------------------------------------------------------
dir_map_sector:
        push    bx
        push    cx
        push    dx

        test    ax, ax
        jnz     .subdir

        ; --- ルートディレクトリ ---
        cmp     bx, [root_secs]
        jae     .oob
        mov     ax, [root_lba]
        add     ax, bx
        jmp     .ok

.subdir:
        ; セクタ番号を「何個先のクラスタか」と「クラスタ内の位置」に分ける
        push    ax
        mov     ax, bx
        xor     dx, dx
        div     word [sec_per_clus]
        mov     cx, ax                  ; CX = 辿るリンクの数
        mov     bx, dx                  ; BX = クラスタ内のセクタ番号
        pop     ax

.walk:
        jcxz    .got
        call    fat_get
        cmp     ax, 2
        jb      .oob
        cmp     ax, FAT_EOC
        jae     .oob
        dec     cx
        jmp     .walk

.got:
        call    clus_to_lba
        add     ax, bx

.ok:
        pop     dx
        pop     cx
        pop     bx
        clc
        ret
.oob:
        pop     dx
        pop     cx
        pop     bx
        stc
        ret

; ---------------------------------------------------------------------------
; dir_load - ディレクトリの n セクタ目を dir_buf に読み込む
;   入力: AX = ディレクトリのクラスタ, BX = セクタ番号
;   出力: CF=0 なら成功。[dir_cur_lba] に LBA が入る
; ---------------------------------------------------------------------------
dir_load:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es

        call    dir_map_sector
        jc      .fail
        mov     [dir_cur_lba], ax

        ; ディスクバッファ越しに読む。同じディレクトリを何度も辿るので、
        ; ここが素通しだとセクタを読み直す回数が跳ね上がる。
        mov     dl, [cur_drive]
        call    buf_get
        jc      .fail

        push    ds
        pop     es
        add     si, BUF_DATA
        mov     di, dir_buf
        mov     cx, SECTOR_SIZE
        rep     movsb

        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.fail:
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

; ---------------------------------------------------------------------------
; dir_load_lba - LBA を直接指定してディレクトリセクタを dir_buf に載せる
;   入力: AX = LBA
;   出力: CF=1 なら失敗。[dir_cur_lba] に LBA が入る
;
; ディレクトリエントリの場所を SFT に控えてある場合に使う。必ず
; ディスクバッファ越しに読むこと。素通しで読むと、まだ書き戻していない
; 新しいエントリを古い内容で上書きしてしまう。
; ---------------------------------------------------------------------------
dir_load_lba:
        push    ax
        push    cx
        push    dx
        push    si
        push    di
        push    es

        mov     [dir_cur_lba], ax
        mov     dl, [cur_drive]
        call    buf_get
        jc      .fail

        push    ds
        pop     es
        add     si, BUF_DATA
        mov     di, dir_buf
        mov     cx, SECTOR_SIZE
        rep     movsb

        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     ax
        clc
        ret
.fail:
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     ax
        stc
        ret

; ---------------------------------------------------------------------------
; dir_store - dir_buf を読み込んだ場所へ書き戻す
;   出力: CF=1 なら失敗
; ---------------------------------------------------------------------------
dir_store:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es

        push    ds
        pop     es
        mov     ax, [dir_cur_lba]
        mov     dl, [cur_drive]
        call    buf_get
        jc      .fail

        ; バッファの中身を書き換えて「汚れている」印を付ける。
        ; 実際にディスクへ戻るのは buf_flush のとき (クローズ、
        ; AH=0Dh のディスクリセット、プログラム終了)。
        mov     di, si
        add     di, BUF_DATA
        mov     si, dir_buf
        mov     cx, SECTOR_SIZE
        rep     movsb
        mov     si, di
        sub     si, BUF_DATA + SECTOR_SIZE
        call    buf_dirty

        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.fail:
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

; ---------------------------------------------------------------------------
; dir_find - ディレクトリから 8.3 名でエントリを探す
;   入力: AX = ディレクトリのクラスタ (0 = ルート)
;         DS:SI = 11 バイトの名前 (ワイルドカード '?' 可)
;         CL = 探す属性のマスク (ATTR_DIRECTORY を含めるとディレクトリも対象)
;   出力: CF=0 なら dir_buf に該当セクタが載っていて DI = エントリのオフセット
;         [dir_cur_lba] = そのセクタの LBA
;         CF=1 なら見つからなかった
; ---------------------------------------------------------------------------
dir_find:
        push    ax
        push    bx
        push    cx
        push    es

        mov     [.dirclus], ax
        mov     [.attr], cl
        push    ds
        pop     es

        xor     bx, bx                  ; セクタ番号
.next_sector:
        mov     ax, [.dirclus]
        call    dir_load
        jc      .not_found

        mov     di, dir_buf
        mov     cx, SECTOR_SIZE / DIR_ENTSIZE
.next_entry:
        mov     al, [di + DIR_NAME]
        cmp     al, DIR_END
        je      .not_found              ; ここから先は未使用
        cmp     al, DIR_FREE
        je      .skip

        mov     al, [di + DIR_ATTR]
        cmp     al, ATTR_LFN
        je      .skip                   ; 長いファイル名の断片は無視する
        test    al, ATTR_VOLUME
        jnz     .skip

        ; ディレクトリを探しているのか、ファイルを探しているのか
        test    al, ATTR_DIRECTORY
        jz      .want_file
        test    byte [.attr], ATTR_DIRECTORY
        jz      .skip
        jmp     .compare
.want_file:
        ; ファイルは常に対象
.compare:
        call    name_match
        je      .found

.skip:
        add     di, DIR_ENTSIZE
        loop    .next_entry

        inc     bx
        jmp     .next_sector

.found:
        pop     es
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.not_found:
        pop     es
        pop     cx
        pop     bx
        pop     ax
        stc
        ret
.dirclus: dw 0
.attr:    db 0

; ---------------------------------------------------------------------------
; dir_alloc_entry - ディレクトリに空きエントリを 1 つ確保する
;   入力: AX = ディレクトリのクラスタ (0 = ルート)
;   出力: CF=0 なら dir_buf に該当セクタが載っていて DI = エントリのオフセット
;         エントリは 32 バイト 0 クリア済み。呼び出し側が埋めて dir_store する
;         CF=1 なら空きがない
;
; サブディレクトリが満杯なら、クラスタを 1 つ足して伸ばす。ルートは
; 固定長なので伸ばせず、その場合は素直に失敗させる (本物と同じ制限)。
; ---------------------------------------------------------------------------
dir_alloc_entry:
        push    ax
        push    bx
        push    cx
        push    dx
        push    es

        mov     [.dirclus], ax
        push    ds
        pop     es

        xor     bx, bx
.next_sector:
        mov     ax, [.dirclus]
        call    dir_load
        jc      .extend                 ; ディレクトリの終端に達した

        mov     di, dir_buf
        mov     cx, SECTOR_SIZE / DIR_ENTSIZE
.next_entry:
        mov     al, [di + DIR_NAME]
        cmp     al, DIR_END
        je      .found
        cmp     al, DIR_FREE
        je      .found
        add     di, DIR_ENTSIZE
        loop    .next_entry

        inc     bx
        jmp     .next_sector

.found:
        ; 32 バイトを 0 で埋めてから返す
        push    di
        push    cx
        mov     cx, DIR_ENTSIZE
        xor     al, al
        rep     stosb
        pop     cx
        pop     di

        pop     es
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret

.extend:
        ; ルートディレクトリは伸ばせない
        cmp     word [.dirclus], 0
        je      .full

        call    fat_alloc
        jc      .full
        mov     [.newclus], ax

        ; 新しいクラスタを 0 で埋める (未使用エントリの印は 0x00)
        push    di
        mov     di, dir_buf
        mov     cx, SECTOR_SIZE
        xor     al, al
        rep     stosb
        pop     di

        mov     ax, [.newclus]
        call    clus_to_lba
        mov     cx, [sec_per_clus]
        mov     [dir_cur_lba], ax
        push    bx
        mov     bx, dir_buf
.zero_loop:
        push    cx
        push    ax
        mov     cx, 1
        call    disk_write
        pop     ax
        pop     cx
        inc     ax
        loop    .zero_loop
        pop     bx

        ; 連鎖の末尾に繋ぐ
        mov     ax, [.dirclus]
.find_tail:
        mov     dx, ax
        call    fat_get
        cmp     ax, FAT_EOC
        jae     .link
        cmp     ax, 2
        jb      .link
        jmp     .find_tail
.link:
        mov     ax, dx                  ; 末尾のクラスタ
        mov     dx, [.newclus]
        call    fat_set

        ; 伸ばしたので最初から探し直す
        xor     bx, bx
        jmp     .next_sector

.full:
        pop     es
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret
.dirclus: dw 0
.newclus: dw 0

; ---------------------------------------------------------------------------
; path_resolve - パス文字列を「親ディレクトリのクラスタ + 8.3 名」に分解する
;   入力: DS:SI = ASCIIZ のパス
;   出力: CF=0 なら AX = 親ディレクトリのクラスタ、name83 に 11 バイトの名前
;         CF=1 なら途中のディレクトリが見つからなかった
;
; "A:\DOS\FOO.TXT" / "\FOO.TXT" / "SUB\FOO.TXT" / "FOO.TXT" のいずれも扱う。
; 先頭に '\' があれば絶対パス、なければカレントディレクトリからの相対。
; ---------------------------------------------------------------------------
path_resolve:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es

        push    ds
        pop     es

        ; --- ドライブ指定 "X:" を読み飛ばす ---
        mov     al, [si]
        test    al, al
        jz      .bad
        cmp     byte [si + 1], ':'
        jne     .no_drive
        add     si, 2
.no_drive:

        ; --- 絶対パスか相対パスか ---
        mov     al, [si]
        cmp     al, '\'
        je      .absolute
        cmp     al, '/'
        je      .absolute
        mov     ax, [cur_dir_clus]
        jmp     .walk
.absolute:
        inc     si
        xor     ax, ax                  ; ルートから

.walk:
        mov     [.dirclus], ax

        ; 名前を 1 つ切り出す
        mov     di, name83
        call    name_to_83

        ; 区切り文字が続くならディレクトリ名だった
        mov     al, [si]
        cmp     al, '\'
        je      .descend
        cmp     al, '/'
        je      .descend

        ; ここが最後の要素。親ディレクトリのクラスタを返す
        mov     ax, [.dirclus]
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        clc
        ret

.descend:
        inc     si                      ; 区切りを飛ばす

        ; 空の要素 ("A:\\FOO" のような書き方) は無視する
        cmp     byte [name83], ' '
        je      .walk_again

        ; "." はそのまま
        cmp     word [name83], '. '     ; '.' + 空白
        je      .walk_again

        mov     ax, [.dirclus]
        push    si
        mov     si, name83
        mov     cl, ATTR_DIRECTORY
        call    dir_find
        pop     si
        jc      .bad

        test    byte [di + DIR_ATTR], ATTR_DIRECTORY
        jz      .bad                    ; ディレクトリではなかった

        mov     ax, [di + DIR_CLUSTER]
        jmp     .walk

.walk_again:
        mov     ax, [.dirclus]
        jmp     .walk

.bad:
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        stc
        ret
.dirclus: dw 0
