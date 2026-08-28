; ============================================================================
; fat.inc  -  FAT12 / FAT16 ファイルシステム
;
; FAT の引き方は 2 通りある。
;
; FAT12 は 1 エントリ 12bit で、FAT 全体でも最大 12 セクタ (6KB) にしかならない。
; 起動時に丸ごとメモリへ載せてしまい (drive.inc の fat_resident_load)、
; クラスタを 1 つ辿るたびにディスクを読みに行かずに済ませる。書き換えは
; fat_dirty を立てておいて、まとめて全部のコピーへ書き戻す。
;
; FAT16 は 1 エントリ 16bit で、大きなパーティションでは 128KB を超える。
; 常駐させるわけにいかないので、ディスクバッファ越しに 1 セクタずつ引く。
; 1 エントリが 2 バイトちょうどなのでセクタ境界を跨がず、FAT12 のような
; 面倒はない。書き換えたバッファには「FAT のコピー数」を控えておき、
; 書き戻すときに全部のコピーへ同じ内容が行くようにしてある。
;
; 呼び出し側からはどちらでも同じに見えるよう、fat_get / fat_set が
; ドライブの種別 (fat_type) を見て振り分ける。終端の印は 16bit 幅に
; 揃えて返すので、FAT12 でも FAT16 でも FAT_EOC で比べられる。
; ============================================================================

FAT_EOC         equ 0xFFF8      ; これ以上なら連鎖の終わり
FAT_EOC_MARK    equ 0xFFFF      ; 終端として書き込む値
FAT_BAD         equ 0xFFF7      ; 不良クラスタ

; ---------------------------------------------------------------------------
; fat_get - クラスタ番号から FAT のエントリを引く
;   入力: AX = クラスタ番号
;   出力: AX = 次のクラスタ番号 (FAT_EOC 以上なら終端)
; ---------------------------------------------------------------------------
fat_get:
        cmp     byte [fat_type], 12
        je      fat12_get
        jmp     fat16_get

; ---------------------------------------------------------------------------
; fat_set - FAT のエントリを書き換える
;   入力: AX = クラスタ番号, DX = 書き込む値
; ---------------------------------------------------------------------------
fat_set:
        cmp     byte [fat_type], 12
        je      fat12_set
        jmp     fat16_set

; ---------------------------------------------------------------------------
; fat12_get - 常駐 FAT から 12bit のエントリを取り出す
;
; FAT12 は 1 エントリ 12bit なので 2 エントリで 3 バイトを共有している。
; クラスタ n のエントリは FAT の n + n/2 バイト目から 16bit 読み、
; n が偶数なら下位 12bit、奇数なら上位 12bit を取る。
;
; 取り出した値が終端や不良の範囲 (FF7h 以上) なら上位 4bit を立てて
; 16bit 幅に伸ばす。呼び出し側が FAT16 と同じ定数で比べられるようにするため。
; ---------------------------------------------------------------------------
fat12_get:
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
        cmp     ax, 0x0FF7
        jb      .plain
        or      ax, 0xF000              ; 終端・不良の印を 16bit 幅に伸ばす
.plain:
        pop     cx
        pop     bx
        ret

; ---------------------------------------------------------------------------
; fat12_set - 常駐 FAT の 12bit エントリを書き換える
; ---------------------------------------------------------------------------
fat12_set:
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
; fat_sector - FAT16 のエントリが載っているセクタをバッファに載せる
;   入力: AX = クラスタ番号
;   出力: CF=0 なら SI = バッファヘッダ、BX = セクタ内のオフセット
;
; エントリは 2 バイトなので、1 セクタに 256 個ぶん入る。
; ---------------------------------------------------------------------------
fat_sector:
        push    eax
        push    cx
        push    dx

        movzx   eax, ax
        shl     eax, 1                  ; バイト位置
        mov     ebx, eax
        and     ebx, SECTOR_SIZE - 1    ; セクタ内のオフセット
        shr     eax, 9                  ; セクタ番号
        movzx   ecx, word [reserved_secs]
        add     eax, ecx                ; パーティション先頭からの LBA

        mov     dl, [fs_drive]
        call    buf_get
        jc      .fail

        ; このバッファは FAT のセクタなので、書き戻すときは全部のコピーへ
        mov     al, [num_fats]
        mov     [si + BUF_COPIES], al
        mov     ax, [secs_per_fat]
        mov     [si + BUF_OFFSET], ax

        pop     dx
        pop     cx
        pop     eax
        clc
        ret
.fail:
        pop     dx
        pop     cx
        pop     eax
        stc
        ret

; ---------------------------------------------------------------------------
; fat16_get - FAT16 のエントリをバッファ越しに引く
; ---------------------------------------------------------------------------
fat16_get:
        push    bx
        push    si

        call    fat_sector
        jc      .fail
        add     bx, si
        add     bx, BUF_DATA
        mov     ax, [bx]

        pop     si
        pop     bx
        ret
.fail:
        mov     ax, FAT_EOC_MARK        ; 読めないときは終端として扱う
        pop     si
        pop     bx
        ret

; ---------------------------------------------------------------------------
; fat16_set - FAT16 のエントリをバッファ越しに書き換える
; ---------------------------------------------------------------------------
fat16_set:
        push    ax
        push    bx
        push    si

        call    fat_sector
        jc      .out
        add     bx, si
        add     bx, BUF_DATA
        mov     [bx], dx
        call    buf_dirty
.out:
        pop     si
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
        push    cx
        push    dx
        push    di

        ; 探し始める場所は DPB が覚えている (DPB_FREESRCH)。毎回クラスタ 2 から
        ; 数え直すと、大きなファイルを作るときに手間がクラスタ数の 2 乗で
        ; 効いてきて、数万クラスタのパーティションでは実用にならない。
        ; 本物の DOS も同じ場所に同じ目的の値を持っている。
        mov     di, [cur_dpb]
        mov     bx, [di + DPB_FREESRCH]
        cmp     bx, 2
        jae     .start_ok
        mov     bx, 2
.start_ok:
        mov     cx, [max_cluster]
        sub     cx, 2                   ; CX = 見て回る回数の上限
        jbe     .full

.loop:
        cmp     bx, [max_cluster]
        jb      .in_range
        mov     bx, 2                   ; 端まで来たら先頭へ回り込む
.in_range:
        mov     ax, bx
        call    fat_get
        test    ax, ax
        jz      .found
        inc     bx
        dec     cx
        jnz     .loop
        jmp     .full

.found:
        mov     ax, bx
        mov     dx, FAT_EOC_MARK
        call    fat_set                 ; ← 確保した瞬間に印を付ける (AX は保たれる)

        ; 次はこの続きから探す
        inc     bx
        cmp     bx, [max_cluster]
        jb      .save_hint
        mov     bx, 2
.save_hint:
        mov     [di + DPB_FREESRCH], bx
        clc
        jmp     .out
.full:
        stc
.out:
        pop     di
        pop     dx
        pop     cx
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
;   出力: EAX = そのクラスタの先頭 LBA (パーティション先頭からの相対)
;
; FAT16 のパーティションは軽く 65536 セクタを超えるので、ここは 32bit で
; 計算しなければならない。16bit のままだと 32MB を超えた途端に
; 先頭付近のセクタを壊しにいく。
; ---------------------------------------------------------------------------
clus_to_lba:
        push    edx
        push    ecx
        movzx   eax, ax
        sub     eax, 2
        movzx   ecx, word [sec_per_clus]
        mul     ecx                     ; EDX:EAX = (clus - 2) * sec_per_clus
        movzx   ecx, word [data_lba]
        add     eax, ecx
        pop     ecx
        pop     edx
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
;   出力: CF=0 なら EAX = LBA、CF=1 なら範囲外 (ディレクトリの終わり)
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
        movzx   eax, word [root_lba]
        movzx   ebx, bx
        add     eax, ebx
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
        movzx   ebx, bx
        add     eax, ebx

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
        push    eax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es

        call    dir_map_sector
        jc      .fail
        mov     [dir_cur_lba], eax

        ; ディスクバッファ越しに読む。同じディレクトリを何度も辿るので、
        ; ここが素通しだとセクタを読み直す回数が跳ね上がる。
        mov     dl, [fs_drive]
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
        pop     eax
        clc
        ret
.fail:
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     eax
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
        push    eax
        push    cx
        push    dx
        push    si
        push    di
        push    es

        mov     [dir_cur_lba], eax
        mov     dl, [fs_drive]
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
        pop     eax
        clc
        ret
.fail:
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     eax
        stc
        ret

; ---------------------------------------------------------------------------
; dir_store - dir_buf を読み込んだ場所へ書き戻す
;   出力: CF=1 なら失敗
; ---------------------------------------------------------------------------
dir_store:
        push    eax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es

        push    ds
        pop     es
        mov     eax, [dir_cur_lba]
        mov     dl, [fs_drive]
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
        pop     eax
        clc
        ret
.fail:
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     eax
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

        call    lfn_reset               ; 長い名前の組み立てをやり直す

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
        jne     .real_entry

        ; 長い名前の断片。組み立てに足して、次へ。
        call    lfn_take
        jmp     .advance

.real_entry:
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
        call    name_match              ; まず短い名前で
        je      .found

        ; 短い名前で合わなければ、直前の断片から組み立てた長い名前で見る。
        ; 8.3 に畳んだ時点で潰れてしまう名前 ("report-2026.txt" など) は
        ; こちらでしか一致しない。
        cmp     byte [lfn_match_enable], 0
        je      .skip
        call    lfn_ready
        jc      .skip
        push    si
        mov     si, name_long
        call    lfn_match
        pop     si
        je      .found

.skip:
        ; 本体のエントリを 1 つ跨いだので、組み立て中のものは捨てる。
        ; 残しておくと、次のファイルに前のファイルの名前が付いて見える。
        call    lfn_reset
.advance:
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
        mov     ax, [.dirclus]
        call    dir_extend
        jc      .full

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
; dir_extend - サブディレクトリをクラスタ 1 つぶん伸ばす
;   入力: AX = ディレクトリのクラスタ (0 = ルート)
;   出力: CF=0 なら伸ばせた
;
; ルートディレクトリは大きさが固定なので伸ばせない。本物と同じ制限。
; ---------------------------------------------------------------------------
dir_extend:
        push    ax
        push    bx
        push    cx
        push    dx
        push    di
        push    es

        push    ds
        pop     es

        test    ax, ax
        jz      .fail                   ; ルートは伸ばせない
        mov     [.dirclus], ax

        call    fat_alloc
        jc      .fail
        mov     [.newclus], ax

        ; 新しいクラスタを 0 で埋める (未使用エントリの印は 0x00)
        mov     di, dir_buf
        mov     cx, SECTOR_SIZE
        xor     al, al
        rep     stosb

        mov     ax, [.newclus]
        call    clus_to_lba
        mov     cx, [sec_per_clus]
        mov     [dir_cur_lba], eax
        mov     bx, dir_buf
.zero_loop:
        push    cx
        push    eax
        mov     cx, 1
        mov     dl, [fs_drive]
        call    disk_write
        pop     eax
        pop     cx
        inc     eax
        loop    .zero_loop

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

        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.fail:
        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret
.dirclus: dw 0
.newclus: dw 0

; ---------------------------------------------------------------------------
; dir_alloc_named - 長い名前の断片ごとディレクトリのエントリを確保する
;   入力: AX = ディレクトリのクラスタ, name_long = 付けたい名前
;   出力: CF=0 なら name83 に短い名前が入り、dir_buf に本体のセクタが載って
;         DI = 本体のエントリ (32 バイト 0 クリア済み)。
;         呼び出し側はそこを埋めて dir_store すればよい。
;
; 名前が 8.3 にそのまま収まるなら断片は作らない。収まらないときだけ、
; 本体の手前に断片を並べる。断片と本体は連続していなければならないので、
; まとめて空きを探す。
; ---------------------------------------------------------------------------
dir_alloc_named:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    es

        push    ds
        pop     es
        mov     [.dirclus], ax

        call    lfn_fits_83
        jnc     .plain                  ; 8.3 に収まる = 断片は要らない

        ; 短い名前を作る (重複しない ~N を選ぶ)
        mov     ax, [.dirclus]
        call    lfn_make_short
        jc      .fail

        call    lfn_entry_count         ; CX = 断片の数
        mov     [.nfrag], cx
        inc     cx
        mov     [.need], cx

        ; 連続した空きを探す
        mov     byte [.tries], 0
.scan:
        call    .find_run
        jnc     .got_run
        ; 見つからない → ディレクトリを伸ばして探し直す
        inc     byte [.tries]
        cmp     byte [.tries], 8
        ja      .fail
        mov     ax, [.dirclus]
        call    dir_extend
        jc      .fail
        jmp     .scan

.got_run:
        ; 断片を並べる。ディレクトリの中では「後ろの断片が先」に来るので、
        ; 走りの先頭に並び番号のいちばん大きいものを置く。
        mov     cx, [.nfrag]
        mov     [.seq], cx              ; 最初に書くのは番号 nfrag
        xor     dx, dx                  ; 走りの中の位置
.frag_loop:
        cmp     dx, [.nfrag]
        jae     .frags_done

        push    dx
        mov     ax, dx
        call    .seek                   ; dir_buf にそのセクタ、DI = エントリ
        pop     dx
        jc      .fail

        push    dx
        mov     ax, [.seq]
        mov     bx, dx                  ; 走りの中の位置 (未使用)
        cmp     dx, 0
        jne     .not_first
        or      ax, LFN_LAST            ; 走りの先頭 = 連鎖の先頭
.not_first:
        call    lfn_write_frag          ; AX = 並び番号(+0x40), DI = 置き場所
        call    dir_store
        pop     dx

        dec     word [.seq]
        inc     dx
        jmp     .frag_loop

.frags_done:
        ; 本体のエントリを 0 で埋めて返す
        mov     ax, [.nfrag]
        call    .seek
        jc      .fail
        push    di
        mov     cx, DIR_ENTSIZE
        xor     al, al
        rep     stosb
        pop     di

        pop     es
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret

.plain:
        ; 8.3 に収まる名前。path_resolve が作った name83 をそのまま使う。
        mov     ax, [.dirclus]
        call    dir_alloc_entry
        jc      .fail
        pop     es
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.fail:
        pop     es
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

; .find_run - [.need] 個ぶん続いた空きエントリを探す
;   出力: CF=0 なら [.runsec] / [.runent] に走りの先頭
.find_run:
        push    ax
        push    bx
        push    cx
        push    dx
        push    di

        xor     bx, bx                  ; セクタ番号
        xor     dx, dx                  ; いま続いている空きの数
.rs_sector:
        mov     ax, [.dirclus]
        call    dir_load
        jc      .rs_none                ; ディレクトリの終わり

        mov     di, dir_buf
        xor     cx, cx                  ; エントリ番号
.rs_entry:
        cmp     cx, SECTOR_SIZE / DIR_ENTSIZE
        jae     .rs_next_sector

        mov     al, [di + DIR_NAME]
        cmp     al, DIR_END
        je      .rs_free
        cmp     al, DIR_FREE
        je      .rs_free
        xor     dx, dx                  ; 使用中 = 走りは切れた
        jmp     .rs_advance
.rs_free:
        test    dx, dx
        jnz     .rs_grow
        mov     [.runsec], bx           ; 走りの先頭を覚える
        mov     [.runent], cx
.rs_grow:
        inc     dx
        cmp     dx, [.need]
        jae     .rs_found
.rs_advance:
        add     di, DIR_ENTSIZE
        inc     cx
        jmp     .rs_entry
.rs_next_sector:
        inc     bx
        jmp     .rs_sector

.rs_found:
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.rs_none:
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

; .seek - 走りの中の AX 番目のエントリを dir_buf に載せて DI を合わせる
.seek:
        push    ax
        push    bx
        push    cx
        push    dx

        ; 通し番号 = 走りの先頭のエントリ番号 + AX
        add     ax, [.runent]
        mov     cx, SECTOR_SIZE / DIR_ENTSIZE
        xor     dx, dx
        div     cx                      ; AX = セクタの繰り上がり, DX = 位置
        add     ax, [.runsec]
        mov     bx, ax                  ; BX = セクタ番号
        mov     [.entoff], dx

        mov     ax, [.dirclus]
        call    dir_load
        jc      .seek_fail

        mov     ax, [.entoff]
        mov     cx, DIR_ENTSIZE
        mul     cx
        mov     di, dir_buf
        add     di, ax

        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.seek_fail:
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

.dirclus: dw 0
.nfrag:   dw 0
.need:    dw 0
.seq:     dw 0
.runsec:  dw 0
.runent:  dw 0
.entoff:  dw 0
.tries:   db 0

; ---------------------------------------------------------------------------
; dir_purge_lfn - 取り残された長い名前の断片を片付ける
;   入力: AX = ディレクトリのクラスタ (0 = ルート)
;
; ファイルを消したり名前を変えたりすると、その手前に並んでいた断片が
; 宙に浮く。放っておくと、次にそこへ作られたファイルに前の名前が
; 付いて見える。本体が消えている、あるいはチェックサムが合わない断片の
; 連なりを 0xE5 (未使用) にする。
;
; 1 回の走査で 1 連なりだけ片付けて最初からやり直す。ディレクトリは
; 小さいので、これで十分に速い。
; ---------------------------------------------------------------------------
dir_purge_lfn:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es

        push    ds
        pop     es
        mov     [.dirclus], ax
        mov     word [.rounds], 0

.restart:
        inc     word [.rounds]
        cmp     word [.rounds], 256
        ja      .done                   ; 念のための歯止め

        mov     word [.runlen], 0
        xor     bx, bx                  ; セクタ番号
.sector:
        mov     ax, [.dirclus]
        call    dir_load
        jc      .tail                   ; ディレクトリの終わり

        mov     di, dir_buf
        xor     cx, cx
.entry:
        cmp     cx, SECTOR_SIZE / DIR_ENTSIZE
        jae     .next_sector

        mov     al, [di + DIR_NAME]
        cmp     al, DIR_END
        je      .tail
        cmp     al, DIR_FREE
        je      .terminator             ; 消されたもの = 連なりは宙に浮く

        mov     al, [di + DIR_ATTR]
        cmp     al, ATTR_LFN
        jne     .real

        ; 断片。連なりの始まりなら位置を覚える。
        cmp     word [.runlen], 0
        jne     .run_grow
        mov     [.runsec], bx
        mov     [.runent], cx
        mov     al, [di + LFN_CKSUM]
        mov     [.runsum], al
.run_grow:
        inc     word [.runlen]
        jmp     .advance

.real:
        ; 本体のエントリ。直前の連なりがこれのものか見る。
        cmp     word [.runlen], 0
        je      .advance                ; 連なりは無い

        call    lfn_checksum            ; ES:DI の 11 バイトから
        cmp     al, [.runsum]
        je      .run_ok
        jmp     .free_run
.run_ok:
        mov     word [.runlen], 0
        jmp     .advance

.terminator:
        ; 消されたエントリ。手前の連なりがあれば宙に浮いている。
        cmp     word [.runlen], 0
        jne     .free_run
.advance:
        add     di, DIR_ENTSIZE
        inc     cx
        jmp     .entry
.next_sector:
        inc     bx
        jmp     .sector

.tail:
        ; ディレクトリの終わりに達した。残っている連なりは宙に浮いている。
        cmp     word [.runlen], 0
        jne     .free_run
        jmp     .done

; --- 連なりを未使用にする ---
.free_run:
        mov     word [.idx], 0
.free_loop:
        mov     ax, [.idx]
        cmp     ax, [.runlen]
        jae     .freed

        ; 連なりの中の AX 番目のエントリの位置を出す
        add     ax, [.runent]
        mov     cx, SECTOR_SIZE / DIR_ENTSIZE
        xor     dx, dx
        div     cx
        add     ax, [.runsec]
        mov     bx, ax
        push    dx
        mov     ax, [.dirclus]
        call    dir_load
        pop     dx
        jc      .freed

        mov     ax, dx
        mov     cx, DIR_ENTSIZE
        mul     cx
        mov     di, dir_buf
        add     di, ax
        mov     byte [di + DIR_NAME], DIR_FREE
        call    dir_store

        inc     word [.idx]
        jmp     .free_loop
.freed:
        jmp     .restart

.done:
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret
.dirclus: dw 0
.runsec:  dw 0
.runent:  dw 0
.runlen:  dw 0
.runsum:  db 0
.idx:     dw 0
.rounds:  dw 0

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

        ; --- ドライブ指定 "X:" を読む ---
        ; ここでドライブを選び直すので、"B:\FOO" のように他のドライブを
        ; 名指ししたパスもそのまま通る。
        mov     al, [si]
        test    al, al
        jz      .bad
        mov     al, [cur_drive]
        cmp     byte [si + 1], ':'
        jne     .no_drive
        mov     al, [si]
        call    to_upper
        sub     al, 'A'
        cmp     al, MAX_DRIVES
        jae     .bad
        add     si, 2
.no_drive:
        mov     [.drive], al
        call    drive_select
        jc      .bad

        ; --- 絶対パスか相対パスか ---
        mov     al, [si]
        cmp     al, '\'
        je      .absolute
        cmp     al, '/'
        je      .absolute
        ; 相対パスの起点は、そのドライブのカレントディレクトリ
        mov     al, [.drive]
        cmp     al, [cur_drive]
        jne     .other_cwd
        mov     ax, [cur_dir_clus]
        jmp     .walk
.other_cwd:
        call    cds_addr
        jc      .bad
        mov     ax, [di + CDS_CLUSTER]
        jmp     .walk
.absolute:
        inc     si
        xor     ax, ax                  ; ルートから

.walk:
        mov     [.dirclus], ax

        ; 名前を 1 つ切り出す。8.3 に畳んだものと、生のつづりの両方を
        ; 控える。長い名前のファイルは前者では見つからない。
        call    name_copy_component
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
.drive:   db 0
