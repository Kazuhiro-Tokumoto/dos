; ============================================================================
; dirops.inc  -  ディレクトリ操作 (MKDIR / RMDIR / CHDIR / 検索 / 名前変更)
; ============================================================================

; ---------------------------------------------------------------------------
; dir_mkdir - サブディレクトリを作る (AH=39h)
;   入力: DS:SI = パス
;   出力: CF=1 なら AX = エラーコード
;
; 新しいディレクトリには必ず "." と ".." の 2 エントリを置く。
; これを省くと当時のツール (CHKDSK、アーカイバ、ファイラ) が
; 「壊れたディレクトリ」と判断する。
; ---------------------------------------------------------------------------
dir_mkdir:
        push    bx
        push    cx
        push    dx
        push    di
        push    es

        push    ds
        pop     es

        call    path_resolve
        jc      .no_path
        mov     [.parent], ax

        ; 同じ名前がすでにないか
        push    si
        mov     si, name83
        mov     cl, ATTR_DIRECTORY
        call    dir_find
        pop     si
        jnc     .exists

        ; 中身用のクラスタを 1 つ確保する
        call    fat_alloc
        jc      .full
        mov     [.newclus], ax

        ; --- 新しいクラスタを "." と ".." だけの状態にする ---
        mov     di, dir_buf
        mov     cx, SECTOR_SIZE
        xor     al, al
        rep     stosb

        ; "." (自分自身)
        mov     di, dir_buf
        call    .blank_name
        mov     byte [di + 0], '.'
        mov     byte [di + DIR_ATTR], ATTR_DIRECTORY
        mov     ax, [.newclus]
        mov     [di + DIR_CLUSTER], ax
        call    .stamp

        ; ".." (親。親がルートなら 0 を入れるのが FAT の約束)
        mov     di, dir_buf + DIR_ENTSIZE
        call    .blank_name
        mov     byte [di + 0], '.'
        mov     byte [di + 1], '.'
        mov     byte [di + DIR_ATTR], ATTR_DIRECTORY
        mov     ax, [.parent]
        mov     [di + DIR_CLUSTER], ax
        call    .stamp

        ; クラスタの先頭セクタに書き、残りのセクタは 0 で埋める
        mov     ax, [.newclus]
        call    clus_to_lba
        mov     [dir_cur_lba], eax
        call    dir_store
        jc      .io_error

        mov     cx, [sec_per_clus]
        dec     cx
        jcxz    .cluster_ready
        push    eax
        mov     di, dir_buf
        push    cx
        mov     cx, SECTOR_SIZE
        xor     al, al
        rep     stosb
        pop     cx
        pop     eax
.zero_loop:
        inc     eax
        push    eax
        push    cx
        mov     [dir_cur_lba], eax
        call    dir_store
        pop     cx
        pop     eax
        jc      .io_error
        loop    .zero_loop
.cluster_ready:

        ; --- 親ディレクトリにエントリを作る ---
        mov     ax, [.parent]
        call    dir_alloc_entry
        jc      .full_dir

        push    si
        push    di
        mov     si, name83
        mov     cx, 11
        rep     movsb
        pop     di
        pop     si

        mov     byte [di + DIR_ATTR], ATTR_DIRECTORY
        mov     ax, [.newclus]
        mov     [di + DIR_CLUSTER], ax
        mov     word [di + DIR_SIZE], 0
        mov     word [di + DIR_SIZE + 2], 0
        call    .stamp
        call    dir_store
        jc      .io_error
        call    disk_flush_fat
        jc      .io_error

        xor     ax, ax
        clc
        jmp     .out

.blank_name:
        push    ax
        push    cx
        push    di
        mov     al, ' '
        mov     cx, 11
        rep     stosb
        pop     di
        pop     cx
        pop     ax
        ret

.stamp:
        push    ax
        call    get_dos_time
        mov     [di + DIR_TIME], ax
        call    get_dos_date
        mov     [di + DIR_DATE], ax
        pop     ax
        ret

.exists:
        mov     ax, ERR_ACCESS
        stc
        jmp     .out
.no_path:
        mov     ax, ERR_PATH_NOT_FOUND
        stc
        jmp     .out
.full:
.full_dir:
        mov     ax, ERR_DISK_FULL
        stc
        jmp     .out
.io_error:
        mov     ax, ERR_BAD_DATA
        stc
.out:
        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        ret
.parent:   dw 0
.newclus:  dw 0

; ---------------------------------------------------------------------------
; dir_rmdir - 空のサブディレクトリを消す (AH=3Ah)
;   入力: DS:SI = パス
;   出力: CF=1 なら AX = エラーコード
; ---------------------------------------------------------------------------
dir_rmdir:
        push    bx
        push    cx
        push    dx
        push    di
        push    es

        push    ds
        pop     es

        call    path_resolve
        jc      .no_path

        push    si
        mov     si, name83
        mov     cl, ATTR_DIRECTORY
        call    dir_find
        pop     si
        jc      .not_found

        test    byte [di + DIR_ATTR], ATTR_DIRECTORY
        jz      .not_dir

        mov     ax, [di + DIR_CLUSTER]
        mov     [.target], ax
        cmp     ax, [cur_dir_clus]
        je      .busy                   ; カレントディレクトリは消せない

        ; 中身が "." と ".." だけか確かめる
        call    .is_empty
        jc      .not_empty

        ; もう一度エントリを引き直す (dir_buf を上書きしたため)
        push    si
        mov     si, name83
        mov     cl, ATTR_DIRECTORY
        call    dir_find
        pop     si
        jc      .not_found

        mov     byte [di + DIR_NAME], DIR_FREE
        call    dir_store
        jc      .io_error

        mov     ax, [.target]
        call    fat_free_chain
        call    disk_flush_fat
        jc      .io_error

        xor     ax, ax
        clc
        jmp     .out

; 対象のディレクトリが空かどうか。CF=0 なら空
.is_empty:
        push    bx
        push    cx
        push    di
        xor     bx, bx
.scan_sector:
        mov     ax, [.target]
        call    dir_load
        jc      .empty                  ; 最後まで見た
        mov     di, dir_buf
        mov     cx, SECTOR_SIZE / DIR_ENTSIZE
.scan_entry:
        mov     al, [di + DIR_NAME]
        cmp     al, DIR_END
        je      .empty
        cmp     al, DIR_FREE
        je      .scan_next
        cmp     al, '.'
        jne     .not_empty_i            ; "." ".." 以外がある
.scan_next:
        add     di, DIR_ENTSIZE
        loop    .scan_entry
        inc     bx
        jmp     .scan_sector
.empty:
        pop     di
        pop     cx
        pop     bx
        clc
        ret
.not_empty_i:
        pop     di
        pop     cx
        pop     bx
        stc
        ret

.no_path:
.not_found:
        mov     ax, ERR_PATH_NOT_FOUND
        stc
        jmp     .out
.not_dir:
.busy:
        mov     ax, ERR_ACCESS
        stc
        jmp     .out
.not_empty:
        mov     ax, ERR_ACCESS
        stc
        jmp     .out
.io_error:
        mov     ax, ERR_BAD_DATA
        stc
.out:
        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        ret
.target:   dw 0

; ---------------------------------------------------------------------------
; dir_chdir - カレントディレクトリを変える (AH=3Bh)
;   入力: DS:SI = パス
;   出力: CF=1 なら AX = エラーコード
;
; クラスタ番号だけでなく、パス文字列も同時に組み立てる。AH=47h
; (カレントディレクトリの取得) が文字列を返す決まりなので、
; どこかで持っておく必要がある。
; ---------------------------------------------------------------------------
dir_chdir:
        push    bx
        push    cx
        push    dx
        push    di
        push    es

        push    ds
        pop     es

        ; ドライブ指定を読み飛ばす
        cmp     byte [si], 0
        je      .bad
        cmp     byte [si + 1], ':'
        jne     .no_drive
        add     si, 2
.no_drive:

        ; 絶対パスならルートから、そうでなければ今の場所から
        mov     al, [si]
        cmp     al, '\'
        je      .from_root
        cmp     al, '/'
        je      .from_root

        mov     ax, [cur_dir_clus]
        mov     [.clus], ax
        push    si
        mov     si, cur_dir_path
        mov     di, work_path
        call    .copy_z
        pop     si
        jmp     .loop

.from_root:
        inc     si
        mov     word [.clus], 0
        mov     byte [work_path], 0

.loop:
        cmp     byte [si], 0
        je      .commit

        mov     di, name83
        call    name_to_83

        ; 区切りを飛ばす
        cmp     byte [si], 0
        je      .have_comp
        inc     si
.have_comp:
        ; 空の要素は無視
        cmp     byte [name83], ' '
        je      .loop

        ; "." はその場に留まる
        cmp     word [name83], '. '
        je      .loop

        ; ".." は 1 つ上へ
        cmp     word [name83], '..'
        jne     .descend
        cmp     byte [name83 + 2], ' '
        jne     .descend
        call    .go_up
        jc      .bad
        jmp     .loop

.descend:
        mov     ax, [.clus]
        push    si
        mov     si, name83
        mov     cl, ATTR_DIRECTORY
        call    dir_find
        pop     si
        jc      .bad
        test    byte [di + DIR_ATTR], ATTR_DIRECTORY
        jz      .bad

        mov     ax, [di + DIR_CLUSTER]
        mov     [.clus], ax
        call    .append_name
        jmp     .loop

.commit:
        mov     ax, [.clus]
        mov     [cur_dir_clus], ax
        push    si
        mov     si, work_path
        mov     di, cur_dir_path
        call    .copy_z
        pop     si
        call    cds_sync                ; CDS にも書き戻す
        xor     ax, ax
        clc
        jmp     .out

.bad:
        mov     ax, ERR_PATH_NOT_FOUND
        stc
.out:
        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        ret

; work_path の末尾の要素を 1 つ削り、クラスタも親へ移す
.go_up:
        push    ax
        push    cx
        push    di
        push    si

        cmp     word [.clus], 0
        je      .up_done                ; もうルート

        ; ".." エントリを引いて親のクラスタを得る
        mov     ax, [.clus]
        mov     si, dotdot_name
        mov     cl, ATTR_DIRECTORY
        call    dir_find
        jc      .up_fail
        mov     ax, [di + DIR_CLUSTER]
        mov     [.clus], ax

        ; パス文字列の末尾の要素を削る
        mov     di, work_path
        mov     cx, -1
        xor     al, al
        push    es
        push    ds
        pop     es
        repne   scasb                   ; 終端を探す
        pop     es
        dec     di                      ; DI = 終端の 0
.strip:
        cmp     di, work_path
        jbe     .strip_done
        dec     di
        cmp     byte [di], '\'
        jne     .strip
.strip_done:
        mov     byte [di], 0

.up_done:
        pop     si
        pop     di
        pop     cx
        pop     ax
        clc
        ret
.up_fail:
        pop     si
        pop     di
        pop     cx
        pop     ax
        stc
        ret

; name83 の名前を work_path の末尾に "\NAME" の形で足す
.append_name:
        push    ax
        push    cx
        push    si
        push    di
        push    es

        push    ds
        pop     es

        mov     di, work_path
        mov     cx, -1
        xor     al, al
        repne   scasb
        dec     di                      ; DI = 終端の 0

        cmp     di, work_path
        je      .no_sep                 ; ルート直下なら区切りは要らない
        mov     al, '\'
        stosb
.no_sep:
        mov     si, name83
        call    name_from_83            ; DI に "NAME.EXT" + 0 を書く

        pop     es
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; DS:SI の 0 終端文字列を ES:DI へ写す
.copy_z:
        push    ax
        push    si
        push    di
.cz_loop:
        lodsb
        stosb
        test    al, al
        jnz     .cz_loop
        pop     di
        pop     si
        pop     ax
        ret

.clus:  dw 0

dotdot_name:    db '..         '        ; 11 バイトの ".." エントリ名

; ---------------------------------------------------------------------------
; dir_rename - ファイル/ディレクトリの名前を変える (AH=56h)
;   入力: DS:SI = 元の名前, DS:DI = 新しい名前
;   出力: CF=1 なら AX = エラーコード
;
; 同じディレクトリ内での改名のみ扱う。別ディレクトリへの移動 (DOS では
; RENAME で実現できる) は、エントリを作り直す形で対応している。
; ---------------------------------------------------------------------------
dir_rename:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es

        push    ds
        pop     es

        mov     [.newpath], di

        ; 元のファイルを探す
        call    path_resolve
        jc      .no_path
        mov     [.srcdir], ax

        ; 元の 8.3 名を控える (新しいパスを解決すると name83 が上書きされる)
        push    si
        push    di
        mov     si, name83
        mov     di, .oldname
        mov     cx, 11
        rep     movsb
        pop     di
        pop     si

        push    si
        mov     si, .oldname
        mov     cl, ATTR_DIRECTORY
        call    dir_find
        pop     si
        jc      .not_found

        ; 見つけたエントリを丸ごと控える
        push    si
        push    di
        mov     si, di
        mov     di, .entry
        mov     cx, DIR_ENTSIZE
        rep     movsb
        pop     di
        pop     si

        ; 新しい名前を解決する
        mov     si, [.newpath]
        call    path_resolve
        jc      .no_path
        mov     [.dstdir], ax

        ; 同名がすでにあれば失敗
        push    si
        mov     si, name83
        mov     cl, ATTR_DIRECTORY
        call    dir_find
        pop     si
        jnc     .exists

        ; 新しい場所にエントリを作る
        mov     ax, [.dstdir]
        call    dir_alloc_entry
        jc      .full

        push    di
        push    si
        mov     si, .entry
        mov     cx, DIR_ENTSIZE
        rep     movsb                   ; 控えておいた内容を写す
        pop     si
        pop     di

        push    si
        push    di
        mov     si, name83
        mov     cx, 11
        rep     movsb                   ; 名前だけ差し替える
        pop     di
        pop     si

        call    dir_store
        jc      .io_error

        ; 元のエントリを消す
        mov     ax, [.srcdir]
        push    si
        mov     si, .oldname
        mov     cl, ATTR_DIRECTORY
        call    dir_find
        pop     si
        jc      .io_error
        mov     byte [di + DIR_NAME], DIR_FREE
        call    dir_store
        jc      .io_error

        xor     ax, ax
        clc
        jmp     .out

.no_path:
.not_found:
        mov     ax, ERR_FILE_NOT_FOUND
        stc
        jmp     .out
.exists:
        mov     ax, ERR_FILE_EXISTS
        stc
        jmp     .out
.full:
        mov     ax, ERR_DISK_FULL
        stc
        jmp     .out
.io_error:
        mov     ax, ERR_BAD_DATA
        stc
.out:
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret
.newpath: dw 0
.srcdir:  dw 0
.dstdir:  dw 0
.oldname: times 11 db 0
.entry:   times DIR_ENTSIZE db 0

; ---------------------------------------------------------------------------
; find_first - AH=4Eh の本体
;   入力: DS:SI = 検索パターン, CX = 属性
;         [dta_seg]:[dta_off] = 結果の書き込み先
;   出力: CF=1 なら AX = エラーコード
;
; 検索の途中状態は DTA 自身に埋め込む。プログラムが DTA を切り替えて
; 複数の検索を並行させられるのが本物の仕様で、ファイラの類がこれに依存する。
; ---------------------------------------------------------------------------
find_first:
        push    bx
        push    cx
        push    dx
        push    di
        push    es

        mov     [.attr], cl

        call    path_resolve
        jc      .no_path
        mov     [.dirclus], ax

        ; DTA に検索状態を書き込む
        mov     es, [dta_seg]
        mov     di, [dta_off]
        mov     byte [es:di + FIND_DRIVE], 0
        mov     al, [.attr]
        mov     [es:di + FIND_ATTR], al
        mov     word [es:di + FIND_ENTRY], 0
        mov     ax, [.dirclus]
        mov     [es:di + FIND_DIRSEC], ax

        ; パターン (8.3 に畳んだもの) を保存する
        push    di
        push    si
        add     di, FIND_PATTERN
        mov     si, name83
        mov     cx, 11
        rep     movsb
        pop     si
        pop     di

        call    find_next
        jmp     .out

.no_path:
        mov     ax, ERR_PATH_NOT_FOUND
        stc
.out:
        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        ret
.attr:    db 0
.dirclus: dw 0

; ---------------------------------------------------------------------------
; find_next - AH=4Fh の本体。DTA に入っている検索状態の続きを探す
;   出力: CF=1 なら AX = ERR_NO_MORE_FILES
; ---------------------------------------------------------------------------
find_next:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es

        mov     es, [dta_seg]
        mov     di, [dta_off]
        mov     [.dta], di

        mov     ax, [es:di + FIND_DIRSEC]
        mov     [.dirclus], ax
        mov     ax, [es:di + FIND_ENTRY]
        mov     [.index], ax
        mov     al, [es:di + FIND_ATTR]
        mov     [.attr], al

        ; パターンをカーネル側へ写す (dir_buf を読む間 ES が変わるため)
        push    di
        push    si
        mov     si, di
        add     si, FIND_PATTERN
        push    ds
        push    es
        pop     ds                      ; DS = DTA のセグメント
        push    cs
        pop     es                      ; ES = カーネル
        mov     di, .pattern
        mov     cx, 11
        rep     movsb
        pop     ds
        pop     si
        pop     di
        mov     es, [dta_seg]           ; 上の写しで ES が動いたので戻す

.loop:
        ; エントリ番号をセクタ番号とスロット番号に分ける
        mov     ax, [.index]
        mov     cl, 4
        shr     ax, cl                  ; 1 セクタ 16 エントリ
        mov     bx, ax
        mov     ax, [.dirclus]
        push    es
        push    ds
        pop     es
        call    dir_load
        pop     es
        jc      .no_more

        mov     ax, [.index]
        and     ax, 15
        mov     cl, DIR_ENTSIZE
        mul     cl
        mov     si, dir_buf
        add     si, ax                  ; SI = エントリの位置

        inc     word [.index]

        mov     al, [si + DIR_NAME]
        cmp     al, DIR_END
        je      .no_more
        cmp     al, DIR_FREE
        je      .loop

        mov     al, [si + DIR_ATTR]
        cmp     al, ATTR_LFN
        je      .loop

        ; 属性の絞り込み。隠し/システム/ディレクトリは、検索属性で
        ; 明示的に求められたときだけ返す。ボリュームラベルも同様。
        mov     ah, [.attr]
        test    al, ATTR_DIRECTORY
        jz      .not_dir
        test    ah, ATTR_DIRECTORY
        jz      .loop
.not_dir:
        test    al, ATTR_VOLUME
        jz      .not_vol
        test    ah, ATTR_VOLUME
        jz      .loop
.not_vol:
        test    al, ATTR_HIDDEN
        jz      .not_hidden
        test    ah, ATTR_HIDDEN
        jz      .loop
.not_hidden:
        test    al, ATTR_SYSTEM
        jz      .not_sys
        test    ah, ATTR_SYSTEM
        jz      .loop
.not_sys:

        ; 名前がパターンに一致するか
        push    si
        push    di
        push    es
        push    ds
        pop     es
        mov     di, si
        mov     si, .pattern
        call    name_match
        pop     es
        pop     di
        pop     si
        jne     .loop

        ; --- 見つかった。DTA に結果を書く ---
        mov     di, [.dta]
        mov     ax, [.index]
        mov     [es:di + FIND_ENTRY], ax        ; 次回の続き

        mov     al, [si + DIR_ATTR]
        mov     [es:di + FIND_ATTR_FOUND], al
        mov     ax, [si + DIR_TIME]
        mov     [es:di + FIND_TIME], ax
        mov     ax, [si + DIR_DATE]
        mov     [es:di + FIND_DATE], ax
        mov     ax, [si + DIR_SIZE]
        mov     [es:di + FIND_SIZE], ax
        mov     ax, [si + DIR_SIZE + 2]
        mov     [es:di + FIND_SIZE + 2], ax

        ; 8.3 を "NAME.EXT" に開いて入れる
        add     di, FIND_NAME
        call    name_from_83

        xor     ax, ax
        clc
        jmp     .out

.no_more:
        mov     ax, ERR_NO_MORE_FILES
        stc
.out:
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret
.dta:      dw 0
.dirclus:  dw 0
.index:    dw 0
.attr:     db 0
.pattern:  times 11 db 0

; ---------------------------------------------------------------------------
; get_cwd - カレントディレクトリの文字列を返す (AH=47h)
;   入力: ES:DI = 書き込み先 (64 バイト以上)
;
; DOS は先頭の "\" を付けずに返す決まり。"A:\DOS\TOOLS" なら "DOS\TOOLS"。
; ---------------------------------------------------------------------------
get_cwd:
        push    ax
        push    si
        push    di
        mov     si, cur_dir_path
.loop:
        lodsb
        stosb
        test    al, al
        jnz     .loop
        pop     di
        pop     si
        pop     ax
        ret

; ---------------------------------------------------------------------------
; get_set_attr - ファイル属性の取得と設定 (AH=43h)
;   入力: DS:SI = パス, AL = 0:取得 1:設定, CX = 設定する属性
;   出力: CF=0 なら CX = 属性、CF=1 なら AX = エラーコード
; ---------------------------------------------------------------------------
get_set_attr:
        push    bx
        push    dx
        push    di
        push    es

        push    ds
        pop     es

        mov     [.op], al
        mov     [.attr], cx

        call    path_resolve
        jc      .no_path

        push    si
        mov     si, name83
        mov     cl, ATTR_DIRECTORY
        call    dir_find
        pop     si
        jc      .not_found

        cmp     byte [.op], 0
        je      .get

        ; 設定: ディレクトリとボリュームの印は変えさせない
        mov     ax, [.attr]
        and     ax, ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_ARCHIVE
        mov     ah, [di + DIR_ATTR]
        and     ah, ATTR_DIRECTORY | ATTR_VOLUME
        or      al, ah
        mov     [di + DIR_ATTR], al
        call    dir_store
        jc      .io_error
        xor     cx, cx
        mov     cl, al
        clc
        jmp     .out

.get:
        movzx   cx, byte [di + DIR_ATTR]
        clc
        jmp     .out

.no_path:
        mov     ax, ERR_PATH_NOT_FOUND
        stc
        jmp     .out
.not_found:
        mov     ax, ERR_FILE_NOT_FOUND
        stc
        jmp     .out
.io_error:
        mov     ax, ERR_BAD_DATA
        stc
.out:
        pop     es
        pop     di
        pop     dx
        pop     bx
        ret
.op:    db 0
.attr:  dw 0

; --- 変数 ------------------------------------------------------------------
cur_dir_clus:   dw 0            ; カレントディレクトリのクラスタ (0 = ルート)
cur_dir_path:   times 64 db 0   ; カレントディレクトリのパス ("\" を含まない)
work_path:      times 64 db 0   ; CHDIR の作業用
dta_seg:        dw 0
dta_off:        dw 0x0080
