; ============================================================================
; file.inc  -  ファイルハンドルによる入出力 (SFT と JFT)
;
; DOS のハンドルは 2 段構えになっている:
;
;   PSP の JFT (20 バイト)       システム全体の SFT
;   handle 0 ──> SFT 番号 ──────> [ 名前, モード, 先頭クラスタ, 位置, ... ]
;   handle 1 ──> SFT 番号 ──┘
;
; 2 段になっているおかげで AH=45h (ハンドル複製) や AH=46h (リダイレクト) が
; 「JFT の項目を同じ SFT に向け直す」だけで実現できる。COMMAND.COM の
; リダイレクト (> や <) はこの仕組みの上に成り立っている。
;
; ハンドル 0-4 は起動時に CON/CON/CON/AUX/PRN へ割り当てられ、子プロセスの
; PSP を作るときに親からそのまま引き継ぐ。
; ============================================================================

; ---------------------------------------------------------------------------
; sft_init - SFT を初期化し、先頭の 3 つをデバイスに割り当てる
; ---------------------------------------------------------------------------
sft_init:
        push    ax
        push    cx
        push    dx
        push    di
        push    si
        push    es

        push    ds
        pop     es

        ; テーブルのヘッダ。List of Lists の +04 がここを指す。
        mov     word [sft_header + SFTH_NEXT], 0xFFFF
        mov     word [sft_header + SFTH_NEXT + 2], 0xFFFF
        mov     word [sft_header + SFTH_COUNT], SFT_ENTRIES

        mov     di, sft_table
        mov     cx, SFT_ENTRIES * SFT_ENTSIZE
        xor     al, al
        rep     stosb                   ; refcnt = 0 で全部空き

        ; SFT 0 = CON (標準入力・標準出力・標準エラーが共有する)
        xor     al, al
        call    sft_addr
        mov     word [si + SFT_MODE], OPEN_RDWR
        mov     di, dev_con
        mov     dx, SFTI_CHAR | SFTI_IOCTL | SFTI_STDIN | SFTI_STDOUT | SFTI_CLEAN
        call    sft_bind_char

        ; SFT 1 = AUX
        mov     al, 1
        call    sft_addr
        mov     word [si + SFT_MODE], OPEN_RDWR
        mov     di, dev_aux
        mov     dx, SFTI_CHAR | SFTI_CLEAN
        call    sft_bind_char

        ; SFT 2 = PRN
        mov     al, 2
        call    sft_addr
        mov     word [si + SFT_MODE], OPEN_WRITE
        mov     di, dev_prn
        mov     dx, SFTI_CHAR | SFTI_CLEAN
        call    sft_bind_char

        pop     es
        pop     si
        pop     di
        pop     dx
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; sft_bind_char - SFT を文字デバイスに結びつける
;   入力: SI = SFT, DI = デバイスヘッダのオフセット, DX = デバイス情報のビット
;
; どの文字デバイスかは SFT_DEVPTR が指す先で見分ける。DOS も同じで、
; 「CON かどうか」は種類コードではなくポインタの一致で判定する。
; だから DEVICE= で CON を差し替えても筋が通る。
; ---------------------------------------------------------------------------
sft_bind_char:
        push    ax
        mov     word [si + SFT_REFCNT], 1
        mov     [si + SFT_DEVINFO], dx
        mov     [si + SFT_DEVPTR], di
        mov     ax, cs
        mov     [si + SFT_DEVPTR + 2], ax
        pop     ax
        ret

; ---------------------------------------------------------------------------
; sft_devkind - この SFT がどのデバイスを指しているかを調べる
;   入力: SI = SFT
;   出力: AL = SFT_DEV_xxx
; ---------------------------------------------------------------------------
sft_devkind:
        push    bx
        test    word [si + SFT_DEVINFO], SFTI_CHAR
        jz      .file
        mov     bx, [si + SFT_DEVPTR]
        cmp     bx, dev_con
        je      .con
        cmp     bx, dev_aux
        je      .aux
        cmp     bx, dev_prn
        je      .prn
        cmp     bx, dev_clock
        je      .clock
        mov     al, SFT_DEV_NUL         ; 知らない文字デバイスは NUL 扱い
        jmp     .out
.con:
        mov     al, SFT_DEV_CON
        jmp     .out
.aux:
        mov     al, SFT_DEV_AUX
        jmp     .out
.prn:
        mov     al, SFT_DEV_PRN
        jmp     .out
.clock:
        mov     al, SFT_DEV_CLOCK
        jmp     .out
.file:
        mov     al, SFT_DEV_FILE
.out:
        pop     bx
        ret

; ---------------------------------------------------------------------------
; 書き換えの印
;
; DOS はデバイス情報語の bit6 を「まだ書き込まれていない」として使う。
; 立っている = きれい、倒れている = 書き換え済み、という向きなので
; 読み違えないよう関数にしてある。
; ---------------------------------------------------------------------------
sft_mark_dirty:
        and     word [si + SFT_DEVINFO], ~SFTI_CLEAN & 0xFFFF
        ret

sft_mark_clean:
        or      word [si + SFT_DEVINFO], SFTI_CLEAN
        ret

; ZF=1 なら書き換えられている
sft_is_dirty:
        test    word [si + SFT_DEVINFO], SFTI_CLEAN
        ret

; ---------------------------------------------------------------------------
; sft_addr - SFT 番号から SFT の先頭アドレスを求める
;   入力: AL = SFT 番号
;   出力: SI = カーネル DS 内のオフセット
; ---------------------------------------------------------------------------
sft_addr:
        push    ax
        push    bx
        mov     bl, SFT_ENTSIZE
        mul     bl                      ; AX = AL * 59
        mov     si, sft_table
        add     si, ax
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; sft_alloc - 空いている SFT を 1 つ取る
;   出力: CF=0 なら AL = SFT 番号 / SI = そのアドレス
;         CF=1 なら空きなし
; ---------------------------------------------------------------------------
sft_alloc:
        push    cx
        xor     al, al
        mov     cx, SFT_ENTRIES
.loop:
        call    sft_addr
        cmp     word [si + SFT_REFCNT], 0
        je      .found
        inc     al
        loop    .loop
        stc
        pop     cx
        ret
.found:
        ; 中身を 0 で埋めてから返す
        push    ax
        push    di
        push    es
        push    ds
        pop     es
        mov     di, si
        mov     cx, SFT_ENTSIZE
        xor     al, al
        rep     stosb
        pop     es
        pop     di
        pop     ax
        mov     word [si + SFT_REFCNT], 1
        clc
        pop     cx
        ret

; ---------------------------------------------------------------------------
; jft_get - ハンドル番号を SFT 番号に変換する
;   入力: BX = ハンドル番号
;   出力: CF=0 なら AL = SFT 番号 / SI = SFT のアドレス
;         CF=1 なら無効なハンドル
; ---------------------------------------------------------------------------
jft_get:
        push    es
        cmp     bx, JFT_ENTRIES
        jae     .bad
        mov     es, [cur_psp]
        mov     al, [es:bx + PSP_JFT]
        cmp     al, JFT_FREE
        je      .bad
        cmp     al, SFT_ENTRIES
        jae     .bad
        call    sft_addr
        cmp     word [si + SFT_REFCNT], 0
        je      .bad
        pop     es
        clc
        ret
.bad:
        pop     es
        stc
        ret

; ---------------------------------------------------------------------------
; jft_alloc - 空いているハンドル番号を取る
;   出力: CF=0 なら BX = ハンドル番号、CF=1 なら空きなし
; ---------------------------------------------------------------------------
jft_alloc:
        push    ax
        push    es
        mov     es, [cur_psp]
        xor     bx, bx
.loop:
        cmp     bx, JFT_ENTRIES
        jae     .full
        cmp     byte [es:bx + PSP_JFT], JFT_FREE
        je      .found
        inc     bx
        jmp     .loop
.found:
        pop     es
        pop     ax
        clc
        ret
.full:
        pop     es
        pop     ax
        stc
        ret

; ---------------------------------------------------------------------------
; jft_set - ハンドルに SFT 番号を割り当てる
;   入力: BX = ハンドル番号, AL = SFT 番号
; ---------------------------------------------------------------------------
jft_set:
        push    es
        mov     es, [cur_psp]
        mov     [es:bx + PSP_JFT], al
        pop     es
        ret

; ---------------------------------------------------------------------------
; dev_check - パス名が文字デバイスを指しているか調べる
;   入力: DS:SI = パス
;   出力: CF=0 なら AL = SFT_DEV_xxx、CF=1 ならただのファイル
;
; "CON" "PRN" "AUX" "NUL" は当時のプログラムが「画面に出す」「捨てる」の
; ために普通に開いてくる。ファイルとして探しに行くと存在しないので、
; ここで先に横取りする。
; ---------------------------------------------------------------------------
dev_check:
        push    bx
        push    cx
        push    si
        push    di
        push    es

        push    ds
        pop     es

        ; パスからディレクトリ部分を外した最後の要素だけを見る
        mov     di, si
        mov     bx, si
.scan:
        mov     al, [si]
        test    al, al
        jz      .have_tail
        cmp     al, '\'
        je      .sep
        cmp     al, '/'
        je      .sep
        cmp     al, ':'
        je      .sep
        inc     si
        jmp     .scan
.sep:
        inc     si
        mov     bx, si
        jmp     .scan
.have_tail:
        mov     si, bx                  ; SI = 最後の要素

        mov     di, dev_names
        xor     cl, cl                  ; CL = デバイス番号 - 1
.try:
        cmp     byte [di], 0
        je      .not_device
        push    si
        push    di
        mov     ch, 3
.cmp_loop:
        mov     al, [si]
        call    to_upper
        cmp     al, [di]
        jne     .no_match
        inc     si
        inc     di
        dec     ch
        jnz     .cmp_loop
        ; 3 文字一致。次は終端か '.' でなければならない ("CONFIG.SYS" 対策)
        mov     al, [si]
        test    al, al
        jz      .match
        cmp     al, '.'
        je      .match
        cmp     al, ':'
        je      .match
.no_match:
        pop     di
        pop     si
        add     di, 3
        inc     cl
        jmp     .try
.match:
        pop     di
        pop     si
        mov     bl, cl
        xor     bh, bh
        mov     al, [dev_ids + bx]
        pop     es
        pop     di
        pop     si
        pop     cx
        pop     bx
        clc
        ret
.not_device:
        pop     es
        pop     di
        pop     si
        pop     cx
        pop     bx
        stc
        ret

dev_names:      db 'CON', 'PRN', 'AUX', 'NUL', 0
dev_ids:        db SFT_DEV_CON, SFT_DEV_PRN, SFT_DEV_AUX, SFT_DEV_NUL

; ---------------------------------------------------------------------------
; file_open - 既存のファイルを開く
;   入力: DS:SI = パス, AL = モード (0=読み, 1=書き, 2=読み書き)
;   出力: CF=0 なら AX = ハンドル番号
;         CF=1 なら AX = エラーコード
; ---------------------------------------------------------------------------
file_open:
        push    bx
        push    cx
        push    dx
        push    di

        mov     [.mode], al

        ; まず文字デバイスかどうか
        call    dev_check
        jnc     .device

        call    path_resolve
        jc      .no_path
        mov     [.dirclus], ax

        push    si
        mov     si, name83
        mov     cl, 0                   ; ディレクトリは対象外
        call    dir_find
        pop     si
        jc      .no_file

        ; 見つかった。SFT を確保して中身を写す
        push    di
        call    sft_alloc
        pop     di
        jc      .too_many

        call    .fill_from_dirent

        movzx   ax, byte [.mode]
        mov     [si + SFT_MODE], ax
        call    sft_bind_file
        jmp     .bind

.device:
        ; AL にデバイスの種類が入っている
        mov     [.dev], al
        call    sft_alloc
        jc      .too_many
        mov     al, [.dev]
        call    sft_bind_kind
        movzx   ax, byte [.mode]
        mov     [si + SFT_MODE], ax

.bind:
        ; SFT 番号を求め直してハンドルに結びつける
        push    si
        mov     ax, si
        sub     ax, sft_table
        mov     bl, SFT_ENTSIZE
        div     bl                      ; AL = SFT 番号
        mov     [.sftno], al
        pop     si

        call    jft_alloc
        jc      .too_many_h
        mov     al, [.sftno]
        call    jft_set

        mov     ax, bx                  ; AX = ハンドル番号
        clc
        jmp     .out

.too_many_h:
        mov     word [si + SFT_REFCNT], 0       ; 確保した SFT を戻す
.too_many:
        mov     ax, ERR_TOO_MANY_FILES
        stc
        jmp     .out
.no_path:
        mov     ax, ERR_PATH_NOT_FOUND
        stc
        jmp     .out
.no_file:
        mov     ax, ERR_FILE_NOT_FOUND
        stc
.out:
        pop     di
        pop     dx
        pop     cx
        pop     bx
        ret

; ディレクトリエントリ (dir_buf 内の DI) から SFT を埋める
.fill_from_dirent:
        push    ax
        push    cx
        push    si
        push    di

        ; 8.3 名は SFT の +20h に入る。DOS 4 以降のレイアウトでは先頭が
        ; 参照カウントなので、そこへ名前を書くとハンドルが解放されなくなる。
        push    si
        push    di
        push    es
        push    ds
        pop     es
        xchg    si, di                  ; SI = ディレクトリエントリ, DI = SFT
        add     di, SFT_NAME
        mov     cx, 11
        rep     movsb
        pop     es
        pop     di
        pop     si

        mov     al, [di + DIR_ATTR]
        mov     [si + SFT_ATTR], al
        mov     ax, [di + DIR_CLUSTER]
        mov     [si + SFT_CLUSTER], ax
        mov     ax, [di + DIR_SIZE]
        mov     [si + SFT_SIZE], ax
        mov     ax, [di + DIR_SIZE + 2]
        mov     [si + SFT_SIZE + 2], ax
        mov     word [si + SFT_POS], 0
        mov     word [si + SFT_POS + 2], 0
        mov     word [si + SFT_CIDX], 0xFFFF    ; キャッシュ無効
        mov     eax, [dir_cur_lba]
        mov     [si + SFT_DIRSEC], eax

        ; dir_buf の何番目のエントリかを覚えておく (閉じるとき書き戻す)
        mov     ax, di
        sub     ax, dir_buf
        mov     cl, DIR_ENTSIZE
        div     cl                      ; AL = エントリ番号
        mov     [si + SFT_DIRENT], al

        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

.mode:    db 0
.dev:     db 0
.sftno:   db 0
.dirclus: dw 0

; ---------------------------------------------------------------------------
; file_create - ファイルを作る (すでにあれば長さ 0 に切り詰める)
;   入力: DS:SI = パス, CX = 属性
;   出力: CF=0 なら AX = ハンドル番号、CF=1 なら AX = エラーコード
; ---------------------------------------------------------------------------
file_create:
        push    bx
        push    cx
        push    dx
        push    di

        mov     [.attr], cl

        call    dev_check
        jnc     .as_device

        call    path_resolve
        jc      .no_path
        mov     [.dirclus], ax

        ; 同じ名前がすでにあるか
        push    si
        mov     si, name83
        mov     cl, 0
        call    dir_find
        pop     si
        jc      .make_new

        ; あった。読み取り専用なら拒否する
        test    byte [di + DIR_ATTR], ATTR_READONLY
        jnz     .access
        test    byte [di + DIR_ATTR], ATTR_DIRECTORY
        jnz     .access

        ; 中身を捨てて長さ 0 にする
        push    di
        mov     ax, [di + DIR_CLUSTER]
        call    fat_free_chain
        pop     di
        mov     word [di + DIR_CLUSTER], 0
        mov     word [di + DIR_SIZE], 0
        mov     word [di + DIR_SIZE + 2], 0
        call    .stamp
        call    dir_store
        jc      .disk_error
        jmp     .have_entry

.make_new:
        mov     ax, [.dirclus]
        call    dir_alloc_entry
        jc      .disk_full

        ; 名前を書き込む
        push    si
        push    di
        push    es
        push    ds
        pop     es
        mov     si, name83
        mov     cx, 11
        rep     movsb
        pop     es
        pop     di
        pop     si

        mov     al, [.attr]
        or      al, ATTR_ARCHIVE
        mov     [di + DIR_ATTR], al
        mov     word [di + DIR_CLUSTER], 0
        mov     word [di + DIR_SIZE], 0
        mov     word [di + DIR_SIZE + 2], 0
        call    .stamp
        call    dir_store
        jc      .disk_error

.have_entry:
        push    di
        call    sft_alloc
        pop     di
        jc      .too_many

        call    file_open.fill_from_dirent
        mov     word [si + SFT_MODE], OPEN_RDWR
        call    sft_bind_file
        call    sft_mark_dirty
        jmp     file_open.bind

.as_device:
        mov     [.dev], al
        call    sft_alloc
        jc      .too_many
        mov     al, [.dev]
        call    sft_bind_kind
        mov     word [si + SFT_MODE], OPEN_RDWR
        jmp     file_open.bind

.too_many:
        mov     ax, ERR_TOO_MANY_FILES
        stc
        jmp     .out
.no_path:
        mov     ax, ERR_PATH_NOT_FOUND
        stc
        jmp     .out
.access:
        mov     ax, ERR_ACCESS
        stc
        jmp     .out
.disk_full:
        mov     ax, ERR_DISK_FULL
        stc
        jmp     .out
.disk_error:
        mov     ax, ERR_BAD_DATA
        stc
.out:
        pop     di
        pop     dx
        pop     cx
        pop     bx
        ret

; DI が指すディレクトリエントリに現在の日時を入れる
.stamp:
        push    ax
        call    get_dos_time            ; AX = DOS 形式の時刻
        mov     [di + DIR_TIME], ax
        call    get_dos_date            ; AX = DOS 形式の日付
        mov     [di + DIR_DATE], ax
        pop     ax
        ret

.attr:    db 0
.dev:     db 0
.dirclus: dw 0

; ---------------------------------------------------------------------------
; file_close - ハンドルを閉じる
;   入力: BX = ハンドル番号
;   出力: CF=1 なら AX = エラーコード
; ---------------------------------------------------------------------------
file_close:
        push    bx
        push    cx
        push    si
        push    di

        call    jft_get
        jc      .bad

        ; 書き換えられていたらディレクトリエントリを更新する
        call    sft_is_dirty
        jnz     .no_flush
        call    file_update_dirent
        jc      .io_error
        call    disk_flush_fat
        jc      .io_error
.no_flush:

        ; JFT から外す
        push    es
        mov     es, [cur_psp]
        mov     byte [es:bx + PSP_JFT], JFT_FREE
        pop     es

        ; 参照が 0 になったら SFT を解放する。CON/AUX/PRN の SFT は
        ; 起動時から refcnt=1 で押さえてあるので、ここで消えることはない。
        call    sft_devkind
        cmp     al, SFT_DEV_FILE
        jne     .done
        dec     word [si + SFT_REFCNT]

.done:
        xor     ax, ax
        clc
        jmp     .out
.bad:
        mov     ax, ERR_BAD_HANDLE
        stc
        jmp     .out
.io_error:
        mov     ax, ERR_BAD_DATA
        stc
.out:
        pop     di
        pop     si
        pop     cx
        pop     bx
        ret

; ---------------------------------------------------------------------------
; file_update_dirent - SFT の内容をディレクトリエントリに書き戻す
;   入力: SI = SFT
;   出力: CF=1 なら失敗
; ---------------------------------------------------------------------------
file_update_dirent:
        push    ax
        push    bx
        push    cx
        push    di
        push    es

        push    ds
        pop     es

        ; そのエントリのあるセクタを読み直す (必ずバッファ越しに)
        mov     al, [si + SFT_DEVINFO]
        and     al, SFTI_DRIVE
        call    drive_select
        jc      .fail
        mov     eax, [si + SFT_DIRSEC]
        call    dir_load_lba
        jc      .fail

        ; エントリ番号からオフセットを求める
        mov     al, [si + SFT_DIRENT]
        mov     bl, DIR_ENTSIZE
        mul     bl
        mov     di, dir_buf
        add     di, ax

        mov     ax, [si + SFT_CLUSTER]
        mov     [di + DIR_CLUSTER], ax
        mov     ax, [si + SFT_SIZE]
        mov     [di + DIR_SIZE], ax
        mov     ax, [si + SFT_SIZE + 2]
        mov     [di + DIR_SIZE + 2], ax
        or      byte [di + DIR_ATTR], ATTR_ARCHIVE
        call    get_dos_time
        mov     [di + DIR_TIME], ax
        call    get_dos_date
        mov     [di + DIR_DATE], ax

        call    dir_store
        jc      .fail

        call    sft_mark_clean
        clc
        jmp     .out
.fail:
        stc
.out:
        pop     es
        pop     di
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; file_cluster_at - ファイルの n 番目のクラスタを求める
;   入力: SI = SFT, AX = クラスタ番号 (ファイル先頭から 0 起算)
;         DX = 0 なら見つからなければ失敗、1 なら足りない分を確保する
;   出力: CF=0 なら AX = クラスタ番号
;         CF=1 なら足りない (DX=1 なら空き容量不足)
;
; 直前に辿った位置を SFT に覚えておき、順方向に読み進む場合は続きから
; 辿る。連続読み出しが O(n^2) になるのを避けるため。
; ---------------------------------------------------------------------------
file_cluster_at:
        push    bx
        push    cx
        push    dx

        mov     [.want], ax
        mov     [.grow], dx

        ; ファイルがまだ 1 クラスタも持っていない場合
        mov     ax, [si + SFT_CLUSTER]
        test    ax, ax
        jnz     .have_first
        cmp     word [.grow], 0
        je      .fail
        call    fat_alloc
        jc      .full
        mov     [si + SFT_CLUSTER], ax
        call    sft_mark_dirty

.have_first:
        ; キャッシュが使えるか (探している位置がキャッシュ以降なら続きから)
        mov     cx, [si + SFT_CIDX]
        cmp     cx, 0xFFFF
        je      .from_start
        cmp     cx, [.want]
        ja      .from_start
        mov     ax, [si + SFT_CCLUS]
        jmp     .walk
.from_start:
        xor     cx, cx
        mov     ax, [si + SFT_CLUSTER]

.walk:
        cmp     cx, [.want]
        je      .found
        mov     bx, ax                  ; いまのクラスタを覚えておく
        call    fat_get
        cmp     ax, 2
        jb      .need_more
        cmp     ax, FAT_EOC
        jae     .need_more
        inc     cx
        jmp     .walk

.need_more:
        cmp     word [.grow], 0
        je      .fail

        ; 連鎖の末尾に新しいクラスタを繋ぐ
        call    fat_alloc
        jc      .full
        push    ax
        mov     dx, ax
        mov     ax, bx
        call    fat_set                 ; 直前のクラスタ → 新しいクラスタ
        pop     ax
        call    sft_mark_dirty
        inc     cx
        jmp     .walk

.found:
        mov     [si + SFT_CIDX], cx
        mov     [si + SFT_CCLUS], ax
        clc
        jmp     .out
.full:
        mov     ax, ERR_DISK_FULL
        stc
        jmp     .out
.fail:
        stc
.out:
        pop     dx
        pop     cx
        pop     bx
        ret
.want:  dw 0
.grow:  dw 0

; ---------------------------------------------------------------------------
; sft_read - SFT を直接指定して読む
;   入力: SI = SFT, CX = バイト数, ES:DI = バッファ
;   出力: CF=0 なら AX = 実際に読んだバイト数
;
; FCB 系のファンクションはハンドル (JFT) を経由しないので、ここから入る。
; ハンドル版との違いは入口だけで、以降の処理は完全に共通。
; ---------------------------------------------------------------------------
sft_read:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        jmp     file_read.have_sft

; ---------------------------------------------------------------------------
; file_read - ハンドルから読む
;   入力: BX = ハンドル, CX = バイト数, ES:DI = バッファ
;   出力: CF=0 なら AX = 実際に読んだバイト数
;         CF=1 なら AX = エラーコード
; ---------------------------------------------------------------------------
file_read:
        push    bx
        push    cx
        push    dx
        push    si
        push    di

        call    jft_get
        jc      .bad_handle

.have_sft:
        call    sft_devkind
        cmp     al, SFT_DEV_FILE
        jne     .device

        ; この SFT が載っているドライブを作業対象にする。クラスタの計算も
        ; FAT の引き方もドライブごとに違うので、これを忘れると別の
        ; ドライブのレイアウトで読み書きしてしまう。
        mov     al, [si + SFT_DEVINFO]
        and     al, SFTI_DRIVE
        call    drive_select

        ; 残りバイト数 = サイズ - 位置
        mov     eax, [si + SFT_SIZE]
        sub     eax, [si + SFT_POS]
        jbe     .eof                    ; 位置がサイズ以上なら 0 バイト
        movzx   edx, cx
        cmp     eax, edx
        jbe     .have_len
        mov     eax, edx
.have_len:
        mov     [.left], eax
        mov     word [.total], 0

.loop:
        cmp     dword [.left], 0
        je      .done

        ; 位置をクラスタ番号とクラスタ内オフセットに分ける
        mov     eax, [si + SFT_POS]
        movzx   ecx, word [clus_bytes]
        xor     edx, edx
        div     ecx                     ; EAX = クラスタ番号, EDX = 内部オフセット
        mov     [.coff], dx

        push    dx
        xor     dx, dx                  ; 伸ばさない
        call    file_cluster_at
        pop     dx
        jc      .done                   ; 連鎖が途切れていた。読めた分で返す

        ; クラスタ内オフセットをセクタ番号とセクタ内オフセットに分ける
        call    clus_to_lba             ; EAX = クラスタ先頭の LBA
        mov     cx, [.coff]
        shr     cx, 9                   ; / 512
        movzx   ecx, cx
        add     eax, ecx                ; EAX = 読むべき LBA

        mov     cx, [.coff]
        and     cx, SECTOR_SIZE - 1
        mov     [.soff], cx

        ; セクタをバッファに載せる
        push    si
        mov     dl, [si + SFT_DEVINFO]
        and     dl, SFTI_DRIVE
        call    buf_get
        mov     [.bufptr], si
        pop     si
        jc      .io_error

        ; このセクタから取れるバイト数
        mov     ax, SECTOR_SIZE
        sub     ax, [.soff]
        movzx   eax, ax
        cmp     eax, [.left]
        jbe     .chunk_ok
        mov     eax, [.left]
.chunk_ok:
        mov     [.chunk], ax

        ; バッファの中身から ES:DI へ写す
        push    cx
        push    si
        mov     si, [.bufptr]
        add     si, BUF_DATA
        add     si, [.soff]
        mov     cx, [.chunk]
        rep     movsb                   ; DS:SI -> ES:DI, DI が進む
        pop     si
        pop     cx

        ; 位置と残りを進める
        movzx   eax, word [.chunk]
        add     [si + SFT_POS], eax
        sub     [.left], eax
        add     [.total], ax
        jmp     .loop

.done:
        mov     ax, [.total]
        clc
        jmp     .out
.eof:
        xor     ax, ax
        clc
        jmp     .out

.device:
        ; いまのところ CON からの読み取りだけ扱う
        call    sft_devkind
        cmp     al, SFT_DEV_CON
        jne     .eof
        call    con_read_block
        clc
        jmp     .out

.bad_handle:
        mov     ax, ERR_BAD_HANDLE
        stc
        jmp     .out
.io_error:
        mov     ax, ERR_BAD_DATA
        stc
.out:
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret
.left:   dd 0
.total:  dw 0
.coff:   dw 0
.soff:   dw 0
.chunk:  dw 0
.bufptr: dw 0

; ---------------------------------------------------------------------------
; con_read_block - CON から 1 行読んで ES:DI へ入れる (AH=3Fh のハンドル 0)
;   入力: CX = 最大バイト数, ES:DI = バッファ
;   出力: AX = 読んだバイト数
;
; DOS はハンドル 0 からの読み取りを行単位で扱い、Enter を CR+LF として返す。
; ---------------------------------------------------------------------------
con_read_block:
        push    bx
        push    cx
        push    di
        xor     bx, bx                  ; BX = 読んだ数
.loop:
        cmp     bx, cx
        jae     .done
        call    con_in
        test    al, al
        jz      .loop                   ; 拡張キーは無視
        cmp     al, 8
        je      .backspace
        cmp     al, 13
        je      .enter
        cmp     al, 26                  ; Ctrl-Z = 入力の終わり
        je      .done
        stosb
        call    con_out
        inc     bx
        jmp     .loop
.backspace:
        test    bx, bx
        jz      .loop
        dec     bx
        dec     di
        mov     al, 8
        call    con_out
        mov     al, ' '
        call    con_out
        mov     al, 8
        call    con_out
        jmp     .loop
.enter:
        mov     al, 13
        stosb
        inc     bx
        call    con_out
        cmp     bx, cx
        jae     .done
        mov     al, 10
        stosb
        inc     bx
        call    con_out
.done:
        mov     ax, bx
        pop     di
        pop     cx
        pop     bx
        ret

; ---------------------------------------------------------------------------
; sft_write - SFT を直接指定して書く (FCB 系が使う)
;   入力: SI = SFT, CX = バイト数, ES:DI = データ
;   出力: CF=0 なら AX = 実際に書いたバイト数
; ---------------------------------------------------------------------------
sft_write:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        jmp     file_write.have_sft

; ---------------------------------------------------------------------------
; file_write - ハンドルへ書く
;   入力: BX = ハンドル, CX = バイト数, ES:DI = データ
;   出力: CF=0 なら AX = 実際に書いたバイト数
;         CF=1 なら AX = エラーコード
;
; CX=0 は「現在位置でファイルを切り詰める」の意味 (DOS の仕様)。
; ---------------------------------------------------------------------------
file_write:
        push    bx
        push    cx
        push    dx
        push    si
        push    di

        call    jft_get
        jc      .bad_handle

.have_sft:
        call    sft_devkind
        cmp     al, SFT_DEV_FILE
        jne     .device

        ; この SFT が載っているドライブを作業対象にする。クラスタの計算も
        ; FAT の引き方もドライブごとに違うので、これを忘れると別の
        ; ドライブのレイアウトで読み書きしてしまう。
        mov     al, [si + SFT_DEVINFO]
        and     al, SFTI_DRIVE
        call    drive_select

        test    cx, cx
        jz      .truncate

        movzx   eax, cx
        mov     [.left], eax
        mov     word [.total], 0

.loop:
        cmp     dword [.left], 0
        je      .done

        mov     eax, [si + SFT_POS]
        movzx   ecx, word [clus_bytes]
        xor     edx, edx
        div     ecx
        mov     [.coff], dx

        push    dx
        mov     dx, 1                   ; 足りなければ確保する
        call    file_cluster_at
        pop     dx
        jc      .disk_full

        call    clus_to_lba             ; EAX = クラスタ先頭の LBA
        mov     cx, [.coff]
        shr     cx, 9
        movzx   ecx, cx
        add     eax, ecx
        mov     [.lba], eax

        mov     ax, [.coff]
        and     ax, SECTOR_SIZE - 1
        mov     [.soff], ax

        ; このセクタに書けるバイト数
        mov     ax, SECTOR_SIZE
        sub     ax, [.soff]
        movzx   eax, ax
        cmp     eax, [.left]
        jbe     .chunk_ok
        mov     eax, [.left]
.chunk_ok:
        mov     [.chunk], ax

        ; セクタをバッファに載せる。セクタの一部だけを書き換える場合、
        ; 残りは元の内容でなければならないので、必ず先に読んでおく。
        push    si
        mov     eax, [.lba]
        mov     dl, [si + SFT_DEVINFO]
        and     dl, SFTI_DRIVE
        call    buf_get
        mov     [.bufptr], si
        pop     si
        jc      .io_error

        ; ユーザのデータ (ES:DI) を sector_buf + soff へ写す。
        ; movsb は DS:SI -> ES:DI なので、セグメントを入れ替えてから使う。
        push    ds
        push    es
        push    si
        push    di
        push    cx

        mov     cx, [.chunk]            ; DS はまだカーネルなので変数が読める
        mov     si, di                  ; SI = ユーザバッファのオフセット
        mov     di, [.bufptr]
        add     di, BUF_DATA
        add     di, [.soff]
        mov     ax, es                  ; AX = ユーザのセグメント
        push    ds
        pop     es                      ; ES = カーネル
        mov     ds, ax                  ; DS = ユーザ
        rep     movsb

        pop     cx
        pop     di
        pop     si
        pop     es
        pop     ds

        ; バッファに書いたので汚れの印を付ける。ディスクへ戻るのは
        ; buf_flush のとき (クローズやディスクリセット)。
        push    si
        mov     si, [.bufptr]
        call    buf_dirty
        pop     si

        movzx   eax, word [.chunk]
        add     [si + SFT_POS], eax
        add     di, [.chunk]
        sub     [.left], eax
        add     [.total], ax

        ; ファイルが伸びたらサイズを更新する
        mov     eax, [si + SFT_POS]
        cmp     eax, [si + SFT_SIZE]
        jbe     .no_extend
        mov     [si + SFT_SIZE], eax
.no_extend:
        call    sft_mark_dirty
        jmp     .loop

.done:
        mov     ax, [.total]
        clc
        jmp     .out

.truncate:
        ; 現在位置でファイルを切る
        mov     eax, [si + SFT_POS]
        mov     [si + SFT_SIZE], eax
        call    sft_mark_dirty
        xor     ax, ax
        clc
        jmp     .out

.device:
        call    sft_devkind
        cmp     al, SFT_DEV_NUL
        je      .nul
        cmp     al, SFT_DEV_CON
        jne     .nul                    ; PRN / AUX は捨てる
        ; CON へ 1 バイトずつ出す
        push    cx
        push    di
        jcxz    .con_done
.con_loop:
        mov     al, [es:di]
        inc     di
        call    con_out
        loop    .con_loop
.con_done:
        pop     di
        pop     cx
        mov     ax, cx
        clc
        jmp     .out
.nul:
        mov     ax, cx
        clc
        jmp     .out

.bad_handle:
        mov     ax, ERR_BAD_HANDLE
        stc
        jmp     .out
.disk_full:
        mov     ax, ERR_DISK_FULL
        stc
        jmp     .out
.io_error:
        mov     ax, ERR_BAD_DATA
        stc
.out:
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret
.left:   dd 0
.total:  dw 0
.coff:   dw 0
.soff:   dw 0
.chunk:  dw 0
.lba:    dd 0
.bufptr: dw 0

; ---------------------------------------------------------------------------
; file_seek - 読み書き位置を移動する
;   入力: BX = ハンドル, AL = 起点 (0=先頭 1=現在 2=末尾), CX:DX = 移動量 (符号付き)
;   出力: CF=0 なら DX:AX = 新しい位置、CF=1 なら AX = エラーコード
; ---------------------------------------------------------------------------
file_seek:
        push    bx
        push    cx
        push    si
        push    di

        mov     [.origin], al
        call    jft_get
        jc      .bad

        ; CX:DX を 32bit にまとめる
        push    dx
        shl     ecx, 16
        pop     dx
        mov     cx, dx                  ; ECX = 移動量 (符号付き)
        mov     eax, ecx

        cmp     byte [.origin], 1
        je      .from_cur
        cmp     byte [.origin], 2
        je      .from_end
        ; 先頭から
        jmp     .set
.from_cur:
        add     eax, [si + SFT_POS]
        jmp     .set
.from_end:
        add     eax, [si + SFT_SIZE]
.set:
        mov     [si + SFT_POS], eax
        mov     word [si + SFT_CIDX], 0xFFFF    ; キャッシュを捨てる
        mov     edx, eax
        shr     edx, 16
        clc
        jmp     .out
.bad:
        mov     ax, ERR_BAD_HANDLE
        stc
.out:
        pop     di
        pop     si
        pop     cx
        pop     bx
        ret
.origin: db 0

; ---------------------------------------------------------------------------
; file_delete - ファイルを消す
;   入力: DS:SI = パス
;   出力: CF=1 なら AX = エラーコード
; ---------------------------------------------------------------------------
file_delete:
        push    bx
        push    cx
        push    di

        call    path_resolve
        jc      .no_path

        push    si
        mov     si, name83
        mov     cl, 0
        call    dir_find
        pop     si
        jc      .no_file

        test    byte [di + DIR_ATTR], ATTR_READONLY
        jnz     .access
        test    byte [di + DIR_ATTR], ATTR_DIRECTORY
        jnz     .access

        push    di
        mov     ax, [di + DIR_CLUSTER]
        call    fat_free_chain
        pop     di

        mov     byte [di + DIR_NAME], DIR_FREE
        call    dir_store
        jc      .io_error
        call    disk_flush_fat
        jc      .io_error

        xor     ax, ax
        clc
        jmp     .out
.no_path:
        mov     ax, ERR_PATH_NOT_FOUND
        stc
        jmp     .out
.no_file:
        mov     ax, ERR_FILE_NOT_FOUND
        stc
        jmp     .out
.access:
        mov     ax, ERR_ACCESS
        stc
        jmp     .out
.io_error:
        mov     ax, ERR_BAD_DATA
        stc
.out:
        pop     di
        pop     cx
        pop     bx
        ret

; ---------------------------------------------------------------------------
; sft_bind_file - SFT をディスク上のファイルに結びつける
;   入力: SI = SFT
; ---------------------------------------------------------------------------
sft_bind_file:
        push    ax
        push    bx
        ; ブロックデバイス。下位 6bit がドライブ番号 (0 = A:)
        movzx   ax, byte [fs_drive]
        or      ax, SFTI_CLEAN
        mov     [si + SFT_DEVINFO], ax
        mov     al, [fs_drive]
        call    dpb_for_drive           ; BX = そのドライブの DPB
        mov     [si + SFT_DEVPTR], bx
        mov     ax, cs
        mov     [si + SFT_DEVPTR + 2], ax
        pop     bx
        mov     ax, [cur_psp]
        mov     [si + SFT_OWNER], ax
        pop     ax
        ret

; ---------------------------------------------------------------------------
; sft_bind_kind - SFT_DEV_xxx で指定された文字デバイスに結びつける
;   入力: SI = SFT, AL = SFT_DEV_xxx
; ---------------------------------------------------------------------------
sft_bind_kind:
        push    ax
        push    dx
        push    di
        mov     dx, SFTI_CHAR | SFTI_CLEAN
        cmp     al, SFT_DEV_CON
        je      .con
        cmp     al, SFT_DEV_AUX
        je      .aux
        cmp     al, SFT_DEV_PRN
        je      .prn
        cmp     al, SFT_DEV_CLOCK
        je      .clock
        mov     di, dev_chain_head              ; NUL
        or      dx, SFTI_NUL
        jmp     .bind
.con:
        mov     di, dev_con
        or      dx, SFTI_IOCTL | SFTI_STDIN | SFTI_STDOUT
        jmp     .bind
.aux:
        mov     di, dev_aux
        jmp     .bind
.prn:
        mov     di, dev_prn
        jmp     .bind
.clock:
        mov     di, dev_clock
        or      dx, SFTI_CLOCK
.bind:
        call    sft_bind_char
        mov     ax, [cur_psp]
        mov     [si + SFT_OWNER], ax
        pop     di
        pop     dx
        pop     ax
        ret

; --- 変数 ------------------------------------------------------------------
; テーブルのヘッダと本体。List of Lists の +04 が sft_header を指す。
sft_header:
                dd 0xFFFFFFFF                   ; 次のテーブルは無い
                dw SFT_ENTRIES
sft_table:      times SFT_ENTRIES * SFT_ENTSIZE db 0
