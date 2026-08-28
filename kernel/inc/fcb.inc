; ============================================================================
; fcb.inc  -  FCB (File Control Block) 系のファンクション
;
; ハンドル (AH=3Dh 以降) が導入される前、DOS 1.x の時代のファイル入出力。
; 古い仕組みだが 6.22 でも生きていて、当時のバイナリには次の理由で
; これを使うものが実在する:
;
;   * DOS 1.x 向けに書かれてそのまま配られ続けたもの
;   * CP/M からの移植で、FCB の形をそのまま引きずっているもの
;   * ハンドルは使うが、コマンドライン引数の解釈だけ AH=29h に頼るもの
;     (DOS が PSP:5C と PSP:6C に置いてくれる FCB を読む)
;
; 3 番目が地味に効く。EXEC のときに DOS がコマンドテイルの最初の 2 語を
; 解析して PSP に置いてくれる、という前提で書かれたプログラムは、
; そこが空だと引数なしで起動されたと判断してしまう。
;
; --- FCB のレイアウト (37 バイト) ------------------------------------------
;   +00  BYTE  ドライブ番号 (0 = カレント, 1 = A:)
;   +01  8B    ファイル名 (空白詰め)
;   +09  3B    拡張子 (空白詰め)
;   +0C  WORD  カレントブロック番号
;   +0E  WORD  レコード長 (開いた直後は 128)
;   +10  DWORD ファイルサイズ
;   +14  WORD  日付
;   +16  WORD  時刻
;   +18  8B    DOS の予約領域 (ここに開いているファイルの情報を置く)
;   +20  BYTE  ブロック内のレコード番号 (0-127)
;   +21  DWORD 乱数レコード番号 (レコード長が 64 以上なら 3 バイトのみ有効)
;
; 拡張 FCB は先頭に 7 バイトが付く: FFh + 予約 5 バイト + 属性 1 バイト。
; ============================================================================

FCB_DRIVE       equ 0x00
FCB_NAME        equ 0x01
FCB_EXT         equ 0x09
FCB_BLOCK       equ 0x0C
FCB_RECSIZE     equ 0x0E
FCB_FILESIZE    equ 0x10
FCB_DATE        equ 0x14
FCB_TIME        equ 0x16
FCB_SFTNO       equ 0x18        ; 予約領域: 我々の SFT 番号
FCB_MAGIC       equ 0x19        ; 予約領域: 上が有効であることの印
FCB_RECORD      equ 0x20
FCB_RANDOM      equ 0x21
FCB_LEN         equ 0x25

FCB_EXT_PREFIX  equ 0xFF        ; 拡張 FCB の目印
FCB_EXT_ATTR    equ 0x06        ; 拡張 FCB の属性の位置
FCB_EXT_SIZE    equ 0x07        ; 拡張 FCB の前置きの長さ

FCB_OPEN_MAGIC  equ 0x4D        ; 'M' — この FCB は MYDOS が開いたもの
FCB_DEFRECSIZE  equ 128

; FCB 系の戻り値
FCB_OK          equ 0x00
FCB_EOF         equ 0x01
FCB_DTA_SMALL   equ 0x02
FCB_PARTIAL     equ 0x03
FCB_ERROR       equ 0xFF

; ---------------------------------------------------------------------------
; fcb_get - 呼び出し元の DS:DX が指す FCB を捕まえる
;   出力: ES:DI = 実際の FCB の先頭 (拡張 FCB なら前置きを飛ばした位置)
;         [fcb_attr] = 検索属性 (拡張 FCB なら指定されたもの、でなければ 0)
;         [fcb_seg]:[fcb_off] に同じものを保存する
;
; ディレクトリを走査する関数は DI を戻り値に使うので、FCB の位置は
; レジスタだけで持ち回らず必ずここに控えておく。
; ---------------------------------------------------------------------------
fcb_get:
        push    ax
        mov     es, [u_ds]
        mov     di, [u_dx]
        mov     byte [fcb_attr], 0
        cmp     byte [es:di], FCB_EXT_PREFIX
        jne     .plain
        mov     al, [es:di + FCB_EXT_ATTR]
        mov     [fcb_attr], al
        mov     byte [fcb_extended], 1
        add     di, FCB_EXT_SIZE
        jmp     .save
.plain:
        mov     byte [fcb_extended], 0
.save:
        mov     [fcb_seg], es
        mov     [fcb_off], di
        pop     ax
        ret

; ---------------------------------------------------------------------------
; fcb_load - 控えておいた FCB の位置を ES:DI に戻す
; ---------------------------------------------------------------------------
fcb_load:
        mov     es, [fcb_seg]
        mov     di, [fcb_off]
        ret

; ---------------------------------------------------------------------------
; fcb_name83 - FCB の名前欄をカーネルの name83 に写す
;   入力: ES:DI = FCB
; ---------------------------------------------------------------------------
fcb_name83:
        push    ax
        push    cx
        push    si
        push    di
        mov     si, di
        add     si, FCB_NAME
        mov     di, name83
        mov     cx, 11
.loop:
        mov     al, [es:si]
        mov     [di], al
        inc     si
        inc     di
        loop    .loop
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; fcb_recsize - FCB のレコード長を取り出す (0 なら 128 とみなす)
;   出力: CX = レコード長
; ---------------------------------------------------------------------------
fcb_recsize:
        push    es
        push    di
        call    fcb_load
        mov     cx, [es:di + FCB_RECSIZE]
        test    cx, cx
        jnz     .ok
        mov     cx, FCB_DEFRECSIZE
.ok:
        pop     di
        pop     es
        ret

; ---------------------------------------------------------------------------
; fcb_seq_pos - 順次アクセスの位置を求める
;   出力: EAX = (ブロック * 128 + レコード) * レコード長
; ---------------------------------------------------------------------------
fcb_seq_pos:
        push    cx
        push    dx
        push    es
        push    di
        call    fcb_load
        movzx   eax, word [es:di + FCB_BLOCK]
        shl     eax, 7                          ; * 128
        movzx   edx, byte [es:di + FCB_RECORD]
        add     eax, edx
        call    fcb_recsize
        movzx   ecx, cx
        mul     ecx                             ; EDX:EAX = レコード番号 * 長さ
        pop     di
        pop     es
        pop     dx
        pop     cx
        ret

; ---------------------------------------------------------------------------
; fcb_rand_pos - 乱数アクセスの位置を求める
;   出力: EAX = 乱数レコード番号 * レコード長
;
; 乱数レコード番号の欄は 4 バイトあるが、レコード長が 64 以上のときは
; 上位 1 バイトを見ないのが DOS の決まり。
; ---------------------------------------------------------------------------
fcb_rand_pos:
        push    cx
        push    dx
        push    es
        push    di
        call    fcb_load
        mov     eax, [es:di + FCB_RANDOM]
        call    fcb_recsize
        cmp     cx, 64
        jb      .full
        and     eax, 0x00FFFFFF                 ; 3 バイトぶんだけ使う
.full:
        movzx   ecx, cx
        mul     ecx
        pop     di
        pop     es
        pop     dx
        pop     cx
        ret

; ---------------------------------------------------------------------------
; fcb_set_rand - 位置から乱数レコード番号を書き戻す
;   入力: EAX = レコード番号
; ---------------------------------------------------------------------------
fcb_set_rand:
        push    cx
        push    es
        push    di
        call    fcb_load
        call    fcb_recsize
        cmp     cx, 64
        jb      .full
        ; 3 バイトの欄なので、4 バイト目を壊さないように書く
        mov     [es:di + FCB_RANDOM], ax
        shr     eax, 16
        mov     [es:di + FCB_RANDOM + 2], al
        jmp     .done
.full:
        mov     [es:di + FCB_RANDOM], eax
.done:
        pop     di
        pop     es
        pop     cx
        ret

; ---------------------------------------------------------------------------
; fcb_set_block - レコード番号からブロック番号とブロック内位置を書き戻す
;   入力: EAX = レコード番号
; ---------------------------------------------------------------------------
fcb_set_block:
        push    ax
        push    dx
        push    es
        push    di
        call    fcb_load
        mov     edx, eax
        and     dl, 0x7F
        mov     [es:di + FCB_RECORD], dl        ; レコード番号 mod 128
        shr     eax, 7
        mov     [es:di + FCB_BLOCK], ax         ; レコード番号 / 128
        pop     di
        pop     es
        pop     dx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; fcb_sft - FCB が指している SFT を取り出す
;   出力: CF=0 なら SI = SFT、CF=1 なら開いていない
; ---------------------------------------------------------------------------
fcb_sft:
        push    ax
        push    es
        push    di
        call    fcb_load
        cmp     byte [es:di + FCB_MAGIC], FCB_OPEN_MAGIC
        jne     .bad
        mov     al, [es:di + FCB_SFTNO]
        cmp     al, [sft_count]
        jae     .bad
        call    sft_addr
        cmp     word [si + SFT_REFCNT], 0
        je      .bad
        pop     di
        pop     es
        pop     ax
        clc
        ret
.bad:
        pop     di
        pop     es
        pop     ax
        stc
        ret

; ---------------------------------------------------------------------------
; fcb_fill_open - ディレクトリエントリの内容を FCB に写して「開いた」状態にする
;   入力: SI = SFT, AL = SFT 番号
; ---------------------------------------------------------------------------
fcb_fill_open:
        push    ax
        push    cx
        push    es
        push    di
        push    si

        mov     cl, al                          ; SFT 番号を退避
        call    fcb_load

        mov     [es:di + FCB_SFTNO], cl
        mov     byte [es:di + FCB_MAGIC], FCB_OPEN_MAGIC
        mov     word [es:di + FCB_BLOCK], 0
        mov     byte [es:di + FCB_RECORD], 0
        mov     dword [es:di + FCB_RANDOM], 0
        mov     word [es:di + FCB_RECSIZE], FCB_DEFRECSIZE

        mov     ax, [si + SFT_SIZE]
        mov     [es:di + FCB_FILESIZE], ax
        mov     ax, [si + SFT_SIZE + 2]
        mov     [es:di + FCB_FILESIZE + 2], ax

        ; 日時はディレクトリエントリを読み直して入れる
        push    bx
        push    cx
        push    es
        push    ds
        pop     es
        mov     al, [si + SFT_DEVINFO]
        and     al, SFTI_DRIVE
        call    drive_select
        mov     eax, [si + SFT_DIRSEC]
        call    dir_load_lba
        pop     es
        jc      .no_stamp
        push    si
        movzx   ax, byte [si + SFT_DIRENT]
        mov     cl, DIR_ENTSIZE
        mul     cl
        mov     si, dir_buf
        add     si, ax
        mov     ax, [si + DIR_TIME]
        mov     [es:di + FCB_TIME], ax
        mov     ax, [si + DIR_DATE]
        mov     [es:di + FCB_DATE], ax
        pop     si
.no_stamp:
        pop     cx
        pop     bx

        pop     si
        pop     di
        pop     es
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; fcb_open_by_name - name83 のファイルを開いて SFT を用意する
;   出力: CF=0 なら SI = SFT / AL = SFT 番号、CF=1 なら見つからない
; ---------------------------------------------------------------------------
fcb_open_by_name:
        push    bx
        push    cx
        push    dx
        push    di
        push    es

        push    ds
        pop     es

        mov     ax, [cur_dir_clus]
        push    si
        mov     si, name83
        mov     cl, 0
        call    dir_find
        pop     si
        jc      .not_found

        push    di
        call    sft_alloc
        pop     di
        jc      .not_found

        call    file_open.fill_from_dirent
        mov     word [si + SFT_MODE], OPEN_RDWR
        call    sft_bind_file

        ; SFT 番号を求め直す
        mov     ax, si
        sub     ax, sft_table
        mov     bl, SFT_ENTSIZE
        div     bl                              ; AL = SFT 番号

        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        clc
        ret
.not_found:
        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        stc
        ret

; ---------------------------------------------------------------------------
; fcb_result - AL を戻り値として設定する
; ---------------------------------------------------------------------------
fcb_result:
        mov     [u_ax], al
        jmp     int21_ok

; ===========================================================================
; 0Fh - FCB でファイルを開く
; ===========================================================================
f_0F:
        call    fcb_get
        call    fcb_name83
        call    fcb_open_by_name
        jc      .fail
        call    fcb_fill_open
        mov     al, FCB_OK
        jmp     fcb_result
.fail:
        mov     al, FCB_ERROR
        jmp     fcb_result

; ===========================================================================
; 10h - FCB でファイルを閉じる
; ===========================================================================
f_10:
        call    fcb_get
        call    fcb_sft
        jc      .fail

        ; 書き換えられていたらディレクトリエントリを更新する
        call    sft_is_dirty
        jnz     .no_flush
        call    file_update_dirent
        call    disk_flush_fat
.no_flush:
        mov     word [si + SFT_REFCNT], 0       ; SFT を返す

        call    fcb_load
        mov     byte [es:di + FCB_MAGIC], 0
        mov     al, FCB_OK
        jmp     fcb_result
.fail:
        mov     al, FCB_ERROR
        jmp     fcb_result

; ===========================================================================
; 11h / 12h - FCB によるファイル検索
;
; 結果は DTA に「未オープンの FCB」の形で置く。中身はドライブ番号 1 バイトに
; 続けて 32 バイトのディレクトリエントリをそのまま写したもの。当時の
; プログラムはここからファイルサイズ (DTA+1+1Ch) を直接読む。
; ===========================================================================
f_11:
        call    fcb_get
        call    fcb_name83

        ; 検索状態をカーネル側に置く (DTA には結果だけを書く)
        mov     ax, [cur_dir_clus]
        mov     [fcb_find_dir], ax
        mov     word [fcb_find_idx], 0
        push    si
        push    di
        mov     si, name83
        mov     di, fcb_find_pat
        mov     cx, 11
        push    es
        push    ds
        pop     es
        rep     movsb
        pop     es
        pop     di
        pop     si
        jmp     fcb_find_common

f_12:
        call    fcb_get
        jmp     fcb_find_common

fcb_find_common:
        call    fcb_scan
        jc      .none
        mov     al, FCB_OK
        jmp     fcb_result
.none:
        mov     al, FCB_ERROR
        jmp     fcb_result

; ---------------------------------------------------------------------------
; fcb_scan - fcb_find_* の状態を進めて、次に一致するものを DTA に書く
;   出力: CF=1 なら もう無い
; ---------------------------------------------------------------------------
fcb_scan:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es

.loop:
        mov     ax, [fcb_find_idx]
        mov     cl, 4
        shr     ax, cl                          ; 1 セクタ 16 エントリ
        mov     bx, ax
        mov     ax, [fcb_find_dir]
        call    dir_load
        jc      .none

        mov     ax, [fcb_find_idx]
        and     ax, 15
        mov     cl, DIR_ENTSIZE
        mul     cl
        mov     si, dir_buf
        add     si, ax

        inc     word [fcb_find_idx]

        mov     al, [si + DIR_NAME]
        cmp     al, DIR_END
        je      .none
        cmp     al, DIR_FREE
        je      .loop
        mov     al, [si + DIR_ATTR]
        cmp     al, ATTR_LFN
        je      .loop

        ; 通常ファイルは常に対象。隠し・システム・ディレクトリ・ラベルは、
        ; 拡張 FCB で明示的に求められたときだけ返す。
        mov     ch, al
        and     ch, ATTR_DIRECTORY | ATTR_VOLUME | ATTR_HIDDEN | ATTR_SYSTEM
        jz      .attr_ok
        mov     ah, [fcb_attr]
        not     ah
        and     ch, ah                          ; 求められていない属性が残るか
        jnz     .loop
.attr_ok:

        ; 名前がパターンに一致するか
        push    si
        push    di
        push    es
        push    ds
        pop     es
        mov     di, si
        mov     si, fcb_find_pat
        call    name_match
        pop     es
        pop     di
        pop     si
        jne     .loop

        ; --- DTA に未オープン FCB を書く ---
        mov     es, [dta_seg]
        mov     di, [dta_off]

        cmp     byte [fcb_extended], 0
        je      .plain_out
        ; 拡張 FCB を求められたので、前置き 7 バイトを付ける
        mov     byte [es:di], FCB_EXT_PREFIX
        mov     word [es:di + 1], 0
        mov     word [es:di + 3], 0
        mov     byte [es:di + 5], 0
        mov     al, [si + DIR_ATTR]
        mov     [es:di + FCB_EXT_ATTR], al
        add     di, FCB_EXT_SIZE
.plain_out:
        mov     byte [es:di], 1                 ; ドライブ番号 (1 = A:)
        inc     di
        ; DS:SI = dir_buf の中のエントリ、ES:DI = DTA。DS はカーネルのまま。
        mov     cx, DIR_ENTSIZE
        rep     movsb                           ; ディレクトリエントリを丸ごと写す

        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret

.none:
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

; ===========================================================================
; 13h - FCB でファイルを消す (ワイルドカード可)
; ===========================================================================
f_13:
        call    fcb_get
        call    fcb_name83

        xor     bx, bx                          ; BX = 消した数
        mov     word [.idx], 0
.loop:
        push    bx
        mov     ax, [cur_dir_clus]
        push    si
        mov     si, name83
        mov     cl, 0
        call    dir_find
        pop     si
        pop     bx
        jc      .done

        test    byte [di + DIR_ATTR], ATTR_READONLY | ATTR_DIRECTORY
        jnz     .done

        push    bx
        mov     ax, [di + DIR_CLUSTER]
        call    fat_free_chain
        pop     bx
        mov     byte [di + DIR_NAME], DIR_FREE
        call    dir_store
        inc     bx
        jmp     .loop                           ; 同じ名前がまだあるかもしれない

.done:
        call    disk_flush_fat
        test    bx, bx
        jz      .none
        mov     al, FCB_OK
        jmp     fcb_result
.none:
        mov     al, FCB_ERROR
        jmp     fcb_result
.idx:   dw 0

; ===========================================================================
; 14h - 順次読み込み
; ===========================================================================
f_14:
        call    fcb_get
        call    fcb_sft
        jc      .bad
        call    fcb_seq_pos                     ; EAX = 読む位置
        call    fcb_do_read
        push    ax
        cmp     al, FCB_EOF
        je      .no_advance
        call    fcb_advance                     ; 次のレコードへ進める
.no_advance:
        pop     ax
        jmp     fcb_result
.bad:
        mov     al, FCB_ERROR
        jmp     fcb_result

; ===========================================================================
; 15h - 順次書き込み
; ===========================================================================
f_15:
        call    fcb_get
        call    fcb_sft
        jc      .bad
        call    fcb_seq_pos
        call    fcb_do_write
        push    ax
        test    al, al
        jnz     .no_advance
        call    fcb_advance
.no_advance:
        pop     ax
        jmp     fcb_result
.bad:
        mov     al, FCB_ERROR
        jmp     fcb_result

; ===========================================================================
; 16h - FCB でファイルを作る
; ===========================================================================
f_16:
        call    fcb_get
        call    fcb_name83

        ; 同じ名前があれば長さ 0 にし、なければ作る
        mov     ax, [cur_dir_clus]
        push    si
        mov     si, name83
        mov     cl, 0
        call    dir_find
        pop     si
        jnc     .truncate

        mov     ax, [cur_dir_clus]
        call    dir_alloc_entry
        jc      .fail
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
        mov     byte [di + DIR_ATTR], ATTR_ARCHIVE
        jmp     .init

.truncate:
        test    byte [di + DIR_ATTR], ATTR_READONLY | ATTR_DIRECTORY
        jnz     .fail
        push    di
        mov     ax, [di + DIR_CLUSTER]
        call    fat_free_chain
        pop     di

.init:
        mov     word [di + DIR_CLUSTER], 0
        mov     word [di + DIR_SIZE], 0
        mov     word [di + DIR_SIZE + 2], 0
        call    get_dos_time
        mov     [di + DIR_TIME], ax
        call    get_dos_date
        mov     [di + DIR_DATE], ax
        call    dir_store
        jc      .fail
        call    disk_flush_fat

        call    fcb_open_by_name
        jc      .fail
        call    fcb_fill_open
        mov     al, FCB_OK
        jmp     fcb_result
.fail:
        mov     al, FCB_ERROR
        jmp     fcb_result

; ===========================================================================
; 17h - FCB で名前を変える
;   古い名前は FCB+01h、新しい名前は FCB+11h に置かれている
; ===========================================================================
f_17:
        call    fcb_get
        call    fcb_name83                      ; 古い名前を name83 へ

        ; 探す
        mov     ax, [cur_dir_clus]
        push    si
        mov     si, name83
        mov     cl, 0
        call    dir_find
        pop     si
        jc      .fail

        ; 新しい名前を作る。'?' の位置は元の名前の文字を残すのが DOS の決まり。
        push    di
        call    fcb_load
        mov     si, di
        add     si, 0x11                        ; 新しい名前の位置
        pop     di

        push    di
        mov     cx, 11
        xor     bx, bx
.build:
        mov     al, [es:si + bx]
        cmp     al, '?'
        jne     .keep
        mov     al, [di + bx]                   ; '?' なら元の文字を残す
.keep:
        mov     [di + bx], al
        inc     bx
        loop    .build
        pop     di

        call    dir_store
        jc      .fail
        mov     al, FCB_OK
        jmp     fcb_result
.fail:
        mov     al, FCB_ERROR
        jmp     fcb_result

; ===========================================================================
; 21h - 乱数読み込み (位置は乱数レコード番号から。位置は進めない)
; ===========================================================================
f_21:
        call    fcb_get
        call    fcb_sft
        jc      .bad
        ; 乱数レコード番号からブロック/レコードを更新するのが DOS の挙動
        push    es
        push    di
        call    fcb_load
        mov     eax, [es:di + FCB_RANDOM]
        call    fcb_recsize
        cmp     cx, 64
        jb      .full
        and     eax, 0x00FFFFFF
.full:
        pop     di
        pop     es
        call    fcb_set_block
        call    fcb_rand_pos
        call    fcb_do_read
        jmp     fcb_result
.bad:
        mov     al, FCB_ERROR
        jmp     fcb_result

; ===========================================================================
; 22h - 乱数書き込み
; ===========================================================================
f_22:
        call    fcb_get
        call    fcb_sft
        jc      .bad
        push    es
        push    di
        call    fcb_load
        mov     eax, [es:di + FCB_RANDOM]
        call    fcb_recsize
        cmp     cx, 64
        jb      .full
        and     eax, 0x00FFFFFF
.full:
        pop     di
        pop     es
        call    fcb_set_block
        call    fcb_rand_pos
        call    fcb_do_write
        jmp     fcb_result
.bad:
        mov     al, FCB_ERROR
        jmp     fcb_result

; ===========================================================================
; 23h - ファイルの大きさをレコード数で得る
;   結果は乱数レコード番号の欄に入れる
; ===========================================================================
f_23:
        call    fcb_get
        call    fcb_name83

        mov     ax, [cur_dir_clus]
        push    si
        mov     si, name83
        mov     cl, 0
        call    dir_find
        pop     si
        jc      .fail

        mov     eax, [di + DIR_SIZE]
        call    fcb_recsize
        movzx   ecx, cx
        add     eax, ecx
        dec     eax                             ; 切り上げる
        xor     edx, edx
        div     ecx
        call    fcb_set_rand

        mov     al, FCB_OK
        jmp     fcb_result
.fail:
        mov     al, FCB_ERROR
        jmp     fcb_result

; ===========================================================================
; 24h - 順次アクセスの位置を乱数レコード番号に写す
; ===========================================================================
f_24:
        call    fcb_get
        call    fcb_load
        movzx   eax, word [es:di + FCB_BLOCK]
        shl     eax, 7
        movzx   ecx, byte [es:di + FCB_RECORD]
        add     eax, ecx
        call    fcb_set_rand
        jmp     int21_ok

; ===========================================================================
; 27h / 28h - 乱数ブロック読み書き (CX 個のレコードをまとめて)
; ===========================================================================
f_27:
        call    fcb_get
        call    fcb_sft
        jc      .bad
        mov     byte [.is_write], 0
        jmp     fcb_block_common
.bad:
        xor     cx, cx
        mov     [u_cx], cx
        mov     al, FCB_ERROR
        jmp     fcb_result
.is_write: db 0

f_28:
        call    fcb_get
        call    fcb_sft
        jc      f_27.bad
        mov     byte [f_27.is_write], 1
        jmp     fcb_block_common

fcb_block_common:
        mov     cx, [u_cx]
        mov     [.want], cx
        mov     word [.done], 0
        mov     byte [.status], FCB_OK

        call    fcb_rand_pos
        mov     [.pos], eax

        cmp     word [.want], 0
        je      .truncate

.loop:
        mov     cx, [.done]
        cmp     cx, [.want]
        jae     .finish

        mov     eax, [.pos]
        cmp     byte [f_27.is_write], 0
        je      .do_read
        call    fcb_do_write
        jmp     .check
.do_read:
        call    fcb_do_read
.check:
        cmp     al, FCB_OK
        jne     .stopped

        inc     word [.done]
        call    fcb_recsize
        movzx   ecx, cx
        add     [.pos], ecx

        ; DTA も 1 レコードぶん進める
        add     [dta_off], cx
        jmp     .loop

.stopped:
        mov     [.status], al
        ; 途中まで進んだぶんは反映する
        cmp     al, FCB_PARTIAL
        jne     .rewind
        inc     word [.done]
.rewind:
        jmp     .finish

.truncate:
        ; CX=0 は「現在位置でファイルを切り詰める」
        cmp     byte [f_27.is_write], 0
        je      .finish
        mov     eax, [.pos]
        mov     [si + SFT_POS], eax
        mov     [si + SFT_SIZE], eax
        call    sft_mark_dirty
        mov     byte [.status], FCB_OK
        jmp     .finish

.finish:
        ; ループの中で進めた DTA を元の位置に戻す
        call    fcb_recsize
        mov     ax, cx
        mul     word [.done]
        sub     [dta_off], ax

        ; 乱数レコード番号を進めた位置に更新する
        mov     eax, [.pos]
        push    eax
        call    fcb_recsize
        movzx   ecx, cx
        pop     eax
        xor     edx, edx
        div     ecx
        call    fcb_set_rand
        call    fcb_set_block

        mov     cx, [.done]
        mov     [u_cx], cx
        mov     al, [.status]
        jmp     fcb_result

.want:   dw 0
.done:   dw 0
.pos:    dd 0
.status: db 0

; ---------------------------------------------------------------------------
; fcb_do_read - 1 レコード読んで DTA に置く
;   入力: SI = SFT, EAX = 読む位置
;   出力: AL = FCB_OK / FCB_EOF / FCB_PARTIAL
;
; DOS は「レコードの途中でファイルが終わった場合、残りを 0 で埋めて
; AL=3 を返す」と決めている。この 0 埋めを省くと、読み込んだ側に
; 前のレコードの残骸が見えてしまう。
; ---------------------------------------------------------------------------
fcb_do_read:
        push    bx
        push    cx
        push    dx
        push    di
        push    es

        mov     [si + SFT_POS], eax
        mov     word [si + SFT_CIDX], 0xFFFF    ; 位置を飛ばしたのでキャッシュを捨てる

        call    fcb_recsize
        mov     [.want], cx

        mov     es, [dta_seg]
        mov     di, [dta_off]
        call    sft_read
        jc      .eof

        cmp     ax, [.want]
        je      .full
        test    ax, ax
        jz      .eof

        ; 半端に終わった。残りを 0 で埋める
        mov     cx, [.want]
        sub     cx, ax
        add     di, ax
        xor     al, al
        rep     stosb
        mov     al, FCB_PARTIAL
        jmp     .out
.full:
        mov     al, FCB_OK
        jmp     .out
.eof:
        mov     al, FCB_EOF
.out:
        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        ret
.want:  dw 0

; ---------------------------------------------------------------------------
; fcb_do_write - DTA の内容を 1 レコードぶん書く
;   入力: SI = SFT, EAX = 書く位置
;   出力: AL = FCB_OK / FCB_EOF (=ディスクが一杯)
; ---------------------------------------------------------------------------
fcb_do_write:
        push    bx
        push    cx
        push    dx
        push    di
        push    es

        mov     [si + SFT_POS], eax
        mov     word [si + SFT_CIDX], 0xFFFF

        call    fcb_recsize
        mov     [.want], cx

        mov     es, [dta_seg]
        mov     di, [dta_off]
        call    sft_write
        jc      .full_disk
        cmp     ax, [.want]
        jne     .full_disk

        mov     al, FCB_OK
        jmp     .out
.full_disk:
        mov     al, FCB_EOF                     ; FCB 系では 1 = ディスクが一杯
.out:
        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        ret
.want:  dw 0

; ---------------------------------------------------------------------------
; fcb_advance - 順次アクセスの位置を 1 レコード進める
; ---------------------------------------------------------------------------
fcb_advance:
        push    ax
        push    es
        push    di
        call    fcb_load
        mov     al, [es:di + FCB_RECORD]
        inc     al
        cmp     al, 128
        jb      .same_block
        xor     al, al
        inc     word [es:di + FCB_BLOCK]
.same_block:
        mov     [es:di + FCB_RECORD], al
        pop     di
        pop     es
        pop     ax
        ret

; ===========================================================================
; 29h - ファイル名の解析
;   入力: AL = 動作の指定, DS:SI = 文字列, ES:DI = FCB の置き場
;   出力: AL = 0 (ワイルドカードなし) / 1 (あり) / FFh (ドライブ指定が不正)
;         DS:SI は解析し終わった位置へ進む
;
; コマンドラインを FCB に落とす唯一の標準的な手段で、DOS 自身も EXEC の
; ときにこれを使って PSP:5C と PSP:6C を埋めている。
; ===========================================================================
f_29:
        push    ds
        push    es

        mov     al, [u_ax]
        mov     [.opts], al

        mov     ds, [cs:u_ds]
        mov     si, [cs:u_si]                   ; DOS の規約は DS:SI = 文字列
        mov     es, [cs:u_es]
        mov     di, [cs:u_di]

        call    fcb_parse

        mov     [cs:u_si], si                   ; 進んだ位置を返す
        mov     [cs:u_ax], al

        pop     es
        pop     ds
        jmp     int21_ok
.opts:  db 0

; ---------------------------------------------------------------------------
; fcb_parse - AH=29h の本体
;   入力: DS:SI = 文字列, ES:DI = FCB, [f_29.opts] = 動作の指定
;   出力: AL = 0 / 1 / FFh, SI は解析後の位置
;
; 動作の指定 (AL):
;   bit0  先頭の区切り文字を読み飛ばす
;   bit1  ドライブ指定がないとき FCB のドライブ欄をそのままにする
;   bit2  ファイル名がないとき FCB の名前欄をそのままにする
;   bit3  拡張子がないとき FCB の拡張子欄をそのままにする
; ---------------------------------------------------------------------------
fcb_parse:
        push    bx
        push    cx
        push    dx

        mov     byte [cs:.wild], 0

        ; --- 先頭の空白は常に飛ばす。区切り文字は指定があるときだけ ---
.skip_ws:
        mov     al, [si]
        cmp     al, ' '
        je      .adv_ws
        cmp     al, 9
        jne     .ws_done
.adv_ws:
        inc     si
        jmp     .skip_ws
.ws_done:
        test    byte [cs:f_29.opts], 0x01
        jz      .no_skip_sep
.skip_sep:
        mov     al, [si]
        call    fcb_is_sep
        jnc     .no_skip_sep
        inc     si
        jmp     .skip_sep
.no_skip_sep:

        ; --- ドライブ指定 "X:" ---
        mov     al, [si]
        test    al, al
        jz      .no_drive
        cmp     byte [si + 1], ':'
        jne     .no_drive
        call    to_upper
        sub     al, 'A' - 1                     ; A: → 1
        jbe     .bad_drive
        cmp     al, 26
        ja      .bad_drive
        mov     [es:di + FCB_DRIVE], al
        add     si, 2
        jmp     .have_drive
.no_drive:
        test    byte [cs:f_29.opts], 0x02
        jnz     .have_drive                     ; そのままにする指定
        mov     byte [es:di + FCB_DRIVE], 0     ; 0 = カレントドライブ
.have_drive:

        ; --- 基本名 (8 文字) ---
        mov     al, [si]
        call    fcb_is_term
        jc      .no_name

        push    di
        add     di, FCB_NAME
        mov     cx, 8
        call    fcb_parse_part
        pop     di
        jmp     .have_name
.no_name:
        test    byte [cs:f_29.opts], 0x04
        jnz     .have_name
        push    di
        add     di, FCB_NAME
        mov     cx, 8
        mov     al, ' '
        rep     stosb
        pop     di
.have_name:

        ; --- 拡張子 (3 文字) ---
        cmp     byte [si], '.'
        jne     .no_ext
        inc     si
        push    di
        add     di, FCB_EXT
        mov     cx, 3
        call    fcb_parse_part
        pop     di
        jmp     .have_ext
.no_ext:
        test    byte [cs:f_29.opts], 0x08
        jnz     .have_ext
        push    di
        add     di, FCB_EXT
        mov     cx, 3
        mov     al, ' '
        rep     stosb
        pop     di
.have_ext:

        ; 未オープンの FCB として使えるよう、残りを初期化しておく
        mov     word [es:di + FCB_BLOCK], 0
        mov     word [es:di + FCB_RECSIZE], 0

        mov     al, [cs:.wild]
        jmp     .out

.bad_drive:
        mov     al, 0xFF
.out:
        pop     dx
        pop     cx
        pop     bx
        ret
.wild:  db 0

; 名前の一部を CX 文字ぶん取り込む。'*' は残りを '?' で埋める。
fcb_parse_part:
        push    bx
.loop:
        mov     al, [si]
        call    fcb_is_term
        jc      .pad
        inc     si
        cmp     al, '*'
        je      .star
        cmp     al, '?'
        jne     .normal
        mov     byte [cs:fcb_parse.wild], 1
.normal:
        call    to_upper
        jcxz    .loop                           ; 溢れた分は捨てる
        stosb
        dec     cx
        jmp     .loop
.star:
        mov     byte [cs:fcb_parse.wild], 1
        mov     al, '?'
        rep     stosb
        jmp     .loop
.pad:
        mov     al, ' '
        jcxz    .done
        rep     stosb
.done:
        pop     bx
        ret

; AL が区切り文字なら CF=1
fcb_is_sep:
        cmp     al, ':'
        je      .yes
        cmp     al, ';'
        je      .yes
        cmp     al, ','
        je      .yes
        cmp     al, '='
        je      .yes
        cmp     al, '+'
        je      .yes
        clc
        ret
.yes:
        stc
        ret

; AL が名前の終わりなら CF=1 (区切り・空白・制御文字・パス区切り)
fcb_is_term:
        cmp     al, 0
        je      .yes
        cmp     al, '.'
        je      .yes
        cmp     al, ' '
        jbe     .yes
        cmp     al, '"'
        je      .yes
        cmp     al, '/'
        je      .yes
        cmp     al, '\'
        je      .yes
        cmp     al, '['
        je      .yes
        cmp     al, ']'
        je      .yes
        cmp     al, '|'
        je      .yes
        cmp     al, '<'
        je      .yes
        cmp     al, '>'
        je      .yes
        call    fcb_is_sep
        ret
.yes:
        stc
        ret

; --- 変数 ------------------------------------------------------------------
fcb_seg:        dw 0
fcb_off:        dw 0
fcb_attr:       db 0
fcb_extended:   db 0
fcb_find_dir:   dw 0
fcb_find_idx:   dw 0
fcb_find_pat:   times 11 db 0
