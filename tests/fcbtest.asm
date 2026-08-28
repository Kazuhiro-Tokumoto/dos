; ============================================================================
; fcbtest.asm  -  FCB 系と 6.22 で追加されたファンクションの自動テスト
;
; FCB はハンドルより前の時代のファイル入出力で、DOS 6.22 でも生きている。
; ここで確かめているのは主に次の 3 点:
;
;   * レコード単位の位置計算 (ブロック * 128 + レコード) * レコード長
;   * 半端に終わったレコードを 0 で埋めて AL=3 を返す約束
;   * DTA に「未オープン FCB」を書く検索の形式
;
; どれも仕様の細部で、外しても「なんとなく動く」ため見逃しやすい。
; ============================================================================
        cpu     386
        bits    16
        org     0x100

ATTR_VOLUME equ 0x08
RECSIZE equ 128
NRECS   equ 8                           ; 128 * 8 = 1024 バイト = 2 クラスタ

start:
        mov     sp, stack_top
        mov     bx, stack_top
        add     bx, 15
        mov     cl, 4
        shr     bx, cl
        mov     ah, 0x4A
        int     0x21

        ; --- 既定の DTA を、自分で動かす前に確かめておく ---
        ;
        ; DOS は EXEC のたびに DTA を新しい PSP:0080 に向け直す。ここで
        ; AH=1Ah を呼んだあとでは元の状態が分からないので、いちばん最初に見る。
        mov     byte [defdta_ok], 0
        mov     ah, 0x2F
        int     0x21                    ; ES:BX = いまの DTA
        mov     ax, es
        mov     dx, cs
        cmp     ax, dx
        jne     .dta_checked
        cmp     bx, 0x80
        jne     .dta_checked
        ; AH=1Ah を呼ばずに検索して、自分の PSP:0080 に結果が入ること
        mov     byte [0x80 + 0x1E], 0
        mov     dx, pat_all
        xor     cx, cx
        mov     ah, 0x4E
        int     0x21
        jc      .dta_checked
        cmp     byte [0x80 + 0x1E], 0
        je      .dta_checked
        mov     byte [defdta_ok], 1
.dta_checked:

        mov     dx, dta_buf
        mov     ah, 0x1A
        int     0x21

        mov     si, msg_head
        call    puts

        call    t_default_dta   ; DTA を触る試験より先に
        call    t_psp_fcb
        call    t_parse
        call    t_create_write
        call    t_open_read
        call    t_filesize
        call    t_random
        call    t_block
        call    t_find
        call    t_rename
        call    t_delete
        call    t_dpb
        call    t_allocinfo
        call    t_createnew
        call    t_extopen
        call    t_truename
        call    t_vollabel
        call    t_fcb_drive
        call    t_fcb_device
        call    t_fcb_label

        call    newline
        mov     si, msg_result
        call    puts
        mov     ax, [pass_count]
        call    put_dec
        mov     si, msg_result2
        call    puts
        mov     ax, [fail_count]
        call    put_dec
        call    newline
        mov     si, msg_end
        call    puts

        mov     ax, [fail_count]
        test    ax, ax
        jz      .clean
        mov     ax, 0x4C01
        int     0x21
.clean:
        mov     ax, 0x4C00
        int     0x21

; ============================================================================
; 1. PSP の FCB が EXEC のときに埋められているか
;
; COMMAND.COM は "FCBTEST ALPHA BETA" の形で起動する。DOS は最初の 2 語を
; 解析して PSP:5C と PSP:6C に置くことになっている。ここが空だと
; 「引数なしで呼ばれた」と誤解するプログラムがある。
; ============================================================================
t_psp_fcb:
        mov     si, n_psp
        call    begin

        ; PSP:5C の名前が "ALPHA" になっているはず
        mov     si, 0x5C + 1
        mov     di, e_alpha
        mov     cx, 11
        push    ds
        pop     es
        repe    cmpsb
        jne     fail

        ; PSP:6C の名前が "BETA" になっているはず
        mov     si, 0x6C + 1
        mov     di, e_beta
        mov     cx, 11
        repe    cmpsb
        jne     fail
        jmp     pass

; ============================================================================
; 2. AH=29h でのファイル名の解析
; ============================================================================
t_parse:
        mov     si, n_parse
        call    begin

        push    ds
        pop     es
        mov     si, p_input
        mov     di, fcb1
        mov     ax, 0x2901              ; bit0 = 先頭の区切りを飛ばす
        int     0x21
        cmp     al, 0                   ; ワイルドカードは無いはず
        jne     fail

        mov     si, fcb1 + 1
        mov     di, e_readme
        mov     cx, 11
        repe    cmpsb
        jne     fail

        ; ワイルドカードを含む場合は AL=1 が返り、'*' が '?' に開かれるはず
        mov     si, p_wild
        mov     di, fcb1
        mov     ax, 0x2900
        int     0x21
        cmp     al, 1
        jne     fail
        mov     si, fcb1 + 1
        mov     di, e_wild
        mov     cx, 11
        repe    cmpsb
        jne     fail
        jmp     pass

; ============================================================================
; 3. AH=16h 作成 → AH=15h 順次書き込み → AH=10h クローズ
; ============================================================================
t_create_write:
        mov     si, n_create
        call    begin

        ; パターンを作る: レコード n の全バイトが (n * 17 + 5)
        mov     di, big_buf
        xor     bx, bx
.gen_rec:
        mov     al, bl
        mov     ah, 17
        mul     ah
        add     al, 5
        mov     cx, RECSIZE
.gen_byte:
        mov     [di], al
        inc     di
        loop    .gen_byte
        inc     bx
        cmp     bx, NRECS
        jb      .gen_rec

        ; FCB を組み立てる
        call    setup_fcb1

        mov     dx, fcb1
        mov     ah, 0x16                ; 作成
        int     0x21
        test    al, al
        jnz     fail

        mov     word [fcb1 + 0x0E], RECSIZE

        ; 1 レコードずつ DTA に置いて書く
        xor     bx, bx
.wloop:
        push    bx
        ; DTA を該当レコードの位置へ移す
        mov     ax, RECSIZE
        mul     bx
        add     ax, big_buf
        mov     dx, ax
        mov     ah, 0x1A
        int     0x21

        mov     dx, fcb1
        mov     ah, 0x15                ; 順次書き込み
        int     0x21
        pop     bx
        test    al, al
        jnz     fail
        inc     bx
        cmp     bx, NRECS
        jb      .wloop

        ; ブロック内レコード番号が進んでいるはず
        cmp     byte [fcb1 + 0x20], NRECS
        jne     fail

        mov     dx, fcb1
        mov     ah, 0x10                ; クローズ
        int     0x21
        test    al, al
        jnz     fail

        ; DTA を戻す
        mov     dx, dta_buf
        mov     ah, 0x1A
        int     0x21
        jmp     pass

; ============================================================================
; 4. AH=0Fh オープン → AH=14h 順次読み込み → 内容の突き合わせ
; ============================================================================
t_open_read:
        mov     si, n_read
        call    begin

        call    setup_fcb1
        mov     dx, fcb1
        mov     ah, 0x0F                ; オープン
        int     0x21
        test    al, al
        jnz     fail

        ; 開いた直後のレコード長は 128 でなければならない
        cmp     word [fcb1 + 0x0E], 128
        jne     fail
        ; ファイルサイズも入っているはず
        cmp     word [fcb1 + 0x10], RECSIZE * NRECS
        jne     fail

        ; 読み戻し先を消してから読む
        mov     di, verify_buf
        mov     cx, RECSIZE * NRECS
        xor     al, al
        push    ds
        pop     es
        rep     stosb

        xor     bx, bx
.rloop:
        push    bx
        mov     ax, RECSIZE
        mul     bx
        add     ax, verify_buf
        mov     dx, ax
        mov     ah, 0x1A
        int     0x21

        mov     dx, fcb1
        mov     ah, 0x14                ; 順次読み込み
        int     0x21
        pop     bx
        test    al, al
        jnz     fail
        inc     bx
        cmp     bx, NRECS
        jb      .rloop

        ; もう 1 回読むと EOF (AL=1) が返るはず
        mov     dx, dta_buf
        mov     ah, 0x1A
        int     0x21
        mov     dx, fcb1
        mov     ah, 0x14
        int     0x21
        cmp     al, 1
        jne     fail

        mov     dx, fcb1
        mov     ah, 0x10
        int     0x21

        ; 中身を 1 バイトずつ突き合わせる
        mov     si, big_buf
        mov     di, verify_buf
        mov     cx, RECSIZE * NRECS
        push    ds
        pop     es
        repe    cmpsb
        jne     fail
        jmp     pass

; ============================================================================
; 5. AH=23h ファイルの大きさをレコード数で得る
; ============================================================================
t_filesize:
        mov     si, n_size
        call    begin

        call    setup_fcb1
        mov     word [fcb1 + 0x0E], RECSIZE
        mov     dx, fcb1
        mov     ah, 0x23
        int     0x21
        test    al, al
        jnz     fail
        cmp     word [fcb1 + 0x21], NRECS
        jne     fail

        ; レコード長を 256 にすると半分の数になるはず
        call    setup_fcb1
        mov     word [fcb1 + 0x0E], 256
        mov     dx, fcb1
        mov     ah, 0x23
        int     0x21
        test    al, al
        jnz     fail
        cmp     word [fcb1 + 0x21], NRECS / 2
        jne     fail
        jmp     pass

; ============================================================================
; 6. AH=21h / 22h 乱数アクセス、AH=24h 位置の写し取り
; ============================================================================
t_random:
        mov     si, n_random
        call    begin

        call    setup_fcb1
        mov     dx, fcb1
        mov     ah, 0x0F
        int     0x21
        test    al, al
        jnz     fail
        mov     word [fcb1 + 0x0E], RECSIZE

        ; レコード 5 を直接読む
        mov     dword [fcb1 + 0x21], 5
        mov     dx, io_buf
        mov     ah, 0x1A
        int     0x21
        mov     dx, fcb1
        mov     ah, 0x21
        int     0x21
        test    al, al
        jnz     .close_fail

        ; レコード 5 の中身は (5 * 17 + 5) = 90 のはず
        cmp     byte [io_buf], 90
        jne     .close_fail
        cmp     byte [io_buf + RECSIZE - 1], 90
        jne     .close_fail

        ; 乱数読み込みはブロック/レコードも更新する決まり
        cmp     byte [fcb1 + 0x20], 5
        jne     .close_fail

        ; レコード 2 を別の値で上書きする
        mov     di, io_buf
        mov     cx, RECSIZE
        mov     al, 0xA7
        push    ds
        pop     es
        rep     stosb
        mov     dword [fcb1 + 0x21], 2
        mov     dx, io_buf
        mov     ah, 0x1A
        int     0x21
        mov     dx, fcb1
        mov     ah, 0x22
        int     0x21
        test    al, al
        jnz     .close_fail

        ; 読み直して確かめる
        mov     di, io_buf
        mov     cx, RECSIZE
        xor     al, al
        rep     stosb
        mov     dword [fcb1 + 0x21], 2
        mov     dx, fcb1
        mov     ah, 0x21
        int     0x21
        test    al, al
        jnz     .close_fail
        cmp     byte [io_buf], 0xA7
        jne     .close_fail
        cmp     byte [io_buf + RECSIZE - 1], 0xA7
        jne     .close_fail

        ; AH=24h: 順次アクセスの位置を乱数レコード番号に写す
        mov     word [fcb1 + 0x0C], 1           ; ブロック 1
        mov     byte [fcb1 + 0x20], 3           ; レコード 3
        mov     dx, fcb1
        mov     ah, 0x24
        int     0x21
        cmp     word [fcb1 + 0x21], 128 + 3
        jne     .close_fail

        mov     dx, fcb1
        mov     ah, 0x10
        int     0x21
        mov     dx, dta_buf
        mov     ah, 0x1A
        int     0x21
        jmp     pass

.close_fail:
        mov     dx, fcb1
        mov     ah, 0x10
        int     0x21
        mov     dx, dta_buf
        mov     ah, 0x1A
        int     0x21
        jmp     fail

; ============================================================================
; 7. AH=27h 乱数ブロック読み込み
; ============================================================================
t_block:
        mov     si, n_block
        call    begin

        call    setup_fcb1
        mov     dx, fcb1
        mov     ah, 0x0F
        int     0x21
        test    al, al
        jnz     fail
        mov     word [fcb1 + 0x0E], RECSIZE

        ; レコード 1 から 3 個まとめて読む
        mov     di, verify_buf
        mov     cx, RECSIZE * 3
        xor     al, al
        push    ds
        pop     es
        rep     stosb

        mov     dx, verify_buf
        mov     ah, 0x1A
        int     0x21

        mov     dword [fcb1 + 0x21], 1
        mov     cx, 3
        mov     dx, fcb1
        mov     ah, 0x27
        int     0x21
        push    ax
        push    cx
        mov     dx, dta_buf
        mov     ah, 0x1A
        int     0x21
        pop     cx
        pop     ax

        test    al, al
        jnz     .close_fail
        cmp     cx, 3                   ; 3 レコード読めたはず
        jne     .close_fail

        ; レコード 1, 2, 3 の中身をそれぞれ確かめる (レコード 2 は上書き済み)
        cmp     byte [verify_buf], 22           ; 1 * 17 + 5
        jne     .close_fail
        cmp     byte [verify_buf + RECSIZE], 0xA7
        jne     .close_fail
        cmp     byte [verify_buf + RECSIZE * 2], 56     ; 3 * 17 + 5
        jne     .close_fail

        ; 乱数レコード番号が 4 に進んでいるはず
        cmp     word [fcb1 + 0x21], 4
        jne     .close_fail

        mov     dx, fcb1
        mov     ah, 0x10
        int     0x21
        jmp     pass
.close_fail:
        mov     dx, fcb1
        mov     ah, 0x10
        int     0x21
        jmp     fail

; ============================================================================
; 8. AH=11h / 12h FCB による検索
; ============================================================================
t_find:
        mov     si, n_find
        call    begin

        ; 2 件ヒットする状況を作る。パターンは "FCB?????DAT" にして、
        ; 同じディレクトリにある FCBTEST.COM (自分自身) を巻き込まないようにする。
        call    setup_fcb_second
        mov     dx, fcb1
        mov     ah, 0x16                ; FCBTWO.DAT を作る
        int     0x21
        test    al, al
        jnz     fail
        mov     dx, fcb1
        mov     ah, 0x10
        int     0x21

        call    setup_fcb_wild
        mov     dx, fcb1
        mov     ah, 0x11
        int     0x21
        test    al, al
        jnz     .cleanup_fail

        xor     bx, bx                  ; BX = 見つかった数
        mov     word [found_size], 0
.scan:
        ; DTA の先頭はドライブ番号、その後ろが 32 バイトのディレクトリエントリ
        cmp     byte [dta_buf], 1               ; A: = 1
        jne     .cleanup_fail

        ; 名前がパターンどおりか (先頭 3 文字と拡張子だけ見る)
        cmp     word [dta_buf + 1], 'FC'
        jne     .cleanup_fail
        cmp     byte [dta_buf + 3], 'B'
        jne     .cleanup_fail
        cmp     word [dta_buf + 9], 'DA'
        jne     .cleanup_fail
        cmp     byte [dta_buf + 11], 'T'
        jne     .cleanup_fail

        ; FCBDATA.DAT に当たったら、サイズが正しいことも確かめる。
        ; サイズはエントリの +1Ch、DTA から見ると +1Dh。
        push    si
        push    di
        push    cx
        mov     si, dta_buf + 1
        mov     di, e_fcbdata
        mov     cx, 11
        push    ds
        pop     es
        repe    cmpsb
        pop     cx
        pop     di
        pop     si
        jne     .not_target
        mov     ax, [dta_buf + 1 + 0x1C]
        mov     [found_size], ax
.not_target:

        inc     bx
        cmp     bx, 8
        jae     .cleanup_fail           ; 際限なく返ってくるのはおかしい

        mov     dx, fcb1
        mov     ah, 0x12                ; 次を探す
        int     0x21
        test    al, al
        jz      .scan

        ; 打ち切りは AL=FFh でなければならない
        cmp     al, 0xFF
        jne     .cleanup_fail

        cmp     bx, 2                   ; ちょうど 2 件のはず
        jne     .cleanup_fail
        cmp     word [found_size], RECSIZE * NRECS
        jne     .cleanup_fail

        call    delete_second
        jmp     pass

.cleanup_fail:
        call    delete_second
        jmp     fail

; FCBTWO.DAT を消す
delete_second:
        push    ax
        push    dx
        call    setup_fcb_second
        mov     dx, fcb1
        mov     ah, 0x13
        int     0x21
        pop     dx
        pop     ax
        ret

; ============================================================================
; 9. AH=17h FCB で名前を変える
; ============================================================================
t_rename:
        mov     si, n_rename
        call    begin

        call    setup_fcb1
        ; 新しい名前は FCB+11h に置く
        push    ds
        pop     es
        mov     si, e_renamed
        mov     di, fcb1 + 0x11
        mov     cx, 11
        rep     movsb

        mov     dx, fcb1
        mov     ah, 0x17
        int     0x21
        test    al, al
        jnz     fail

        ; 新しい名前で開けること
        push    ds
        pop     es
        mov     si, e_renamed
        mov     di, fcb1 + 1
        mov     cx, 11
        rep     movsb
        mov     byte [fcb1], 0
        mov     dx, fcb1
        mov     ah, 0x0F
        int     0x21
        test    al, al
        jnz     fail
        mov     dx, fcb1
        mov     ah, 0x10
        int     0x21
        jmp     pass

; ============================================================================
; 10. AH=13h FCB でファイルを消す
; ============================================================================
t_delete:
        mov     si, n_delete
        call    begin

        push    ds
        pop     es
        mov     si, e_renamed
        mov     di, fcb1 + 1
        mov     cx, 11
        rep     movsb
        mov     byte [fcb1], 0

        mov     dx, fcb1
        mov     ah, 0x13
        int     0x21
        test    al, al
        jnz     fail

        ; 消えていること
        mov     dx, fcb1
        mov     ah, 0x0F
        int     0x21
        cmp     al, 0xFF
        jne     fail
        jmp     pass

; ============================================================================
; 11. AH=32h DPB を取り出して BPB と辻褄が合うか見る
; ============================================================================
t_dpb:
        mov     si, n_dpb
        call    begin

        push    ds
        mov     dl, 1                   ; A:
        mov     ah, 0x32
        int     0x21
        test    al, al
        jnz     .fail_pop

        ; DS:BX = DPB
        cmp     byte [bx + 0x00], 0     ; ドライブ番号 0 = A:
        jne     .fail_pop
        cmp     word [bx + 0x02], 512   ; セクタあたりバイト数
        jne     .fail_pop
        cmp     byte [bx + 0x04], 0     ; クラスタ内の最大セクタ番号 (spc=1)
        jne     .fail_pop
        cmp     byte [bx + 0x08], 2     ; FAT の数
        jne     .fail_pop
        cmp     word [bx + 0x09], 224   ; ルートディレクトリのエントリ数
        jne     .fail_pop
        cmp     word [bx + 0x0F], 9     ; FAT のセクタ数
        jne     .fail_pop
        cmp     byte [bx + 0x17], 0xF0  ; メディアディスクリプタ
        jne     .fail_pop
        pop     ds
        jmp     pass
.fail_pop:
        pop     ds
        jmp     fail

; ============================================================================
; 12. AH=1Bh 割り当て情報
; ============================================================================
t_allocinfo:
        mov     si, n_alloc
        call    begin

        push    ds
        mov     ah, 0x1B
        int     0x21
        cmp     al, 1                   ; クラスタあたりセクタ数
        jne     .fail_pop
        cmp     cx, 512                 ; セクタあたりバイト数
        jne     .fail_pop
        test    dx, dx                  ; 総クラスタ数
        jz      .fail_pop
        mov     al, [bx]                ; DS:BX = メディアディスクリプタ
        cmp     al, 0xF0
        jne     .fail_pop
        pop     ds
        jmp     pass
.fail_pop:
        pop     ds
        jmp     fail

; ============================================================================
; 13. AH=5Bh 新規作成 (すでにあれば失敗する)
; ============================================================================
t_createnew:
        mov     si, n_createnew
        call    begin

        mov     dx, f_new
        xor     cx, cx
        mov     ah, 0x5B
        int     0x21
        jc      fail
        mov     bx, ax
        mov     ah, 0x3E
        int     0x21

        ; 2 回目は「すでにある」で失敗しなければならない
        mov     dx, f_new
        xor     cx, cx
        mov     ah, 0x5B
        int     0x21
        jnc     .unexpected
        cmp     ax, 80                  ; ERROR_FILE_EXISTS
        jne     fail
        jmp     pass
.unexpected:
        mov     bx, ax
        mov     ah, 0x3E
        int     0x21
        jmp     fail

; ============================================================================
; 14. AH=6Ch 拡張オープン
; ============================================================================
t_extopen:
        mov     si, n_extopen
        call    begin

        ; あれば開く (前のテストで作ってある)
        mov     si, f_new
        mov     bx, 0x0000              ; 読み取り
        xor     cx, cx
        mov     dx, 0x0001              ; あれば開く / なければ失敗
        mov     ax, 0x6C00
        int     0x21
        jc      fail
        cmp     cx, 1                   ; 1 = 開いた
        jne     .close_fail
        mov     bx, ax
        mov     ah, 0x3E
        int     0x21

        ; 無いファイルを「あれば開く」で開くと失敗するはず
        mov     si, f_absent
        mov     bx, 0x0000
        xor     cx, cx
        mov     dx, 0x0001
        mov     ax, 0x6C00
        int     0x21
        jnc     .unexpected

        ; 「なければ作る」を足すと作られるはず
        mov     si, f_absent
        mov     bx, 0x0002
        xor     cx, cx
        mov     dx, 0x0011              ; 上位 = 作る / 下位 = 開く
        mov     ax, 0x6C00
        int     0x21
        jc      fail
        cmp     cx, 2                   ; 2 = 作った
        jne     .close_fail
        mov     bx, ax
        mov     ah, 0x3E
        int     0x21

        ; 後片付け
        mov     dx, f_new
        mov     ah, 0x41
        int     0x21
        mov     dx, f_absent
        mov     ah, 0x41
        int     0x21
        jmp     pass

.close_fail:
        mov     bx, ax
        mov     ah, 0x3E
        int     0x21
        jmp     fail
.unexpected:
        mov     bx, ax
        mov     ah, 0x3E
        int     0x21
        jmp     fail

; ============================================================================
; 15. AH=60h パスの正規化 (TRUENAME)
; ============================================================================
t_truename:
        mov     si, n_truename
        call    begin

        ; 相対名 → "A:\README.TXT"
        push    ds
        pop     es
        mov     si, tn_rel
        mov     di, tn_out
        mov     ah, 0x60
        int     0x21
        jc      fail
        mov     si, tn_out
        mov     di, tn_exp1
        call    streq
        jne     .show

        ; ".." を含む絶対パスは畳まれるはず
        mov     si, tn_updown
        mov     di, tn_out
        mov     ah, 0x60
        int     0x21
        jc      fail
        mov     si, tn_out
        mov     di, tn_exp2
        call    streq
        jne     .show

        ; ルートそのものは "A:\"
        mov     si, tn_root
        mov     di, tn_out
        mov     ah, 0x60
        int     0x21
        jc      fail
        mov     si, tn_out
        mov     di, tn_exp3
        call    streq
        jne     .show
        jmp     pass

.show:
        mov     si, tn_dbg
        call    puts
        mov     si, tn_out
        call    puts
        call    newline
        jmp     fail

; DS:SI と DS:DI を比べる → ZF=1 なら一致
streq:
        push    ax
        push    si
        push    di
.loop:
        mov     al, [si]
        cmp     al, [di]
        jne     .no
        test    al, al
        jz      .yes
        inc     si
        inc     di
        jmp     .loop
.yes:
        xor     al, al
        jmp     .out
.no:
        or      al, 0xFF
.out:
        pop     di
        pop     si
        pop     ax
        ret

; ============================================================================
; 小道具
; ============================================================================

; fcb1 を "FCBDATA .DAT" で初期化する
setup_fcb1:
        push    ax
        push    cx
        push    si
        push    di
        push    es
        push    ds
        pop     es
        mov     di, fcb1
        mov     cx, 37
        xor     al, al
        rep     stosb
        mov     si, e_fcbdata
        mov     di, fcb1 + 1
        mov     cx, 11
        rep     movsb
        pop     es
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; fcb1 を "FCB?????.???" で初期化する (検索用)
setup_fcb_wild:
        push    ax
        push    cx
        push    si
        push    di
        push    es
        push    ds
        pop     es
        mov     di, fcb1
        mov     cx, 37
        xor     al, al
        rep     stosb
        mov     si, e_wildfcb
        mov     di, fcb1 + 1
        mov     cx, 11
        rep     movsb
        pop     es
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; fcb1 を "FCBTWO  .DAT" で初期化する (検索を 2 件にするための 2 本目)
setup_fcb_second:
        push    ax
        push    cx
        push    si
        push    di
        push    es
        push    ds
        pop     es
        mov     di, fcb1
        mov     cx, 37
        xor     al, al
        rep     stosb
        mov     si, e_fcbtwo
        mov     di, fcb1 + 1
        mov     cx, 11
        rep     movsb
        pop     es
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; ============================================================================
; 既定の DTA は「このプログラムの PSP + 80h」であること
;
; DOS は EXEC のたびに DTA を新しい PSP:0080 に向け直す。プログラムが
; AH=1Ah を呼ばずに検索を使うと、そこへ結果が書かれる。ここを親のまま
; にしておくと、検索結果が親 (COMMAND.COM) の PSP に書き込まれ、
; 自分の PSP を読んだプログラムは「何も見つからなかった」と判断する。
; 当時のユーティリティはだいたい既定の DTA をそのまま使う。
; ============================================================================
t_default_dta:
        mov     si, n_defdta
        call    begin
        cmp     byte [defdta_ok], 0
        je      fail
        jmp     pass

; ============================================================================
; 拡張 FCB でボリュームラベルが引けること
;
; LABEL のようなプログラムは、属性 8 を立てた拡張 FCB を AH=11h に渡して
; ラベルを探す。ディレクトリの走査でラベルを無条件に飛ばしていると
; 見つからない。
; ============================================================================
t_vollabel:
        mov     si, n_vollabel
        call    begin
        push    ds
        pop     es
        mov     ah, 0x1A                ; DTA を自前の場所に向ける
        mov     dx, vol_dta
        int     0x21

        mov     ah, 0x11
        mov     dx, vol_fcb
        int     0x21
        cmp     al, 0
        jne     .restore_fail

        ; 拡張 FCB の結果: +00 FFh, +06 属性, +07 ドライブ, +08 名前
        cmp     byte [vol_dta], 0xFF
        jne     .restore_fail
        cmp     byte [vol_dta + 6], ATTR_VOLUME
        jne     .restore_fail

        ; 名前が空白だけではないこと
        mov     si, vol_dta + 8
        mov     cx, 11
.scan:
        lodsb
        cmp     al, ' '
        jne     .have
        loop    .scan
        jmp     .restore_fail
.have:
        mov     ah, 0x1A                ; DTA を既定に戻す
        mov     dx, 0x80
        int     0x21
        jmp     pass
.restore_fail:
        push    ax
        mov     ah, 0x1A
        mov     dx, 0x80
        int     0x21
        pop     ax
        jmp     fail

; ---------------------------------------------------------------------------
; FCB の先頭 1 バイト (ドライブ番号) が効いているか
;
; FCB は 0 = カレントドライブ、1 = A:、2 = B: という数え方をする。
; ここを見ない実装だと、どのドライブを指しても常にカレントドライブを
; 開いてしまい、それでもファイルは見つかるので気づけない。
; A: にしか無いもの / C: にしか無いものを両方から引いて、当たり外れが
; 逆転することで確かめる。
;
;   HELLO.COM    A: にある / C: には無い
;   ENVTEST.COM  A: にも C: にもある  ← ドライブ判定には使えない
; ---------------------------------------------------------------------------
t_fcb_drive:
        mov     si, n_fcbdrv
        call    begin

        ; C: が生えていない環境ではこの試験は成立しない。素通しにする。
        mov     ah, 0x36                ; 空き容量 (AX=FFFFh なら無いドライブ)
        mov     dl, 3                   ; 1 = A:, 3 = C:
        int     0x21
        cmp     ax, 0xFFFF
        je      pass

        ; (1) ドライブ欄 = 1 (A:) で HELLO.COM → 開けるはず
        mov     si, f_hello_a
        call    fcb_try_open
        jc      fail

        ; (2) ドライブ欄 = 3 (C:) で HELLO.COM → 開けないはず
        mov     si, f_hello_c
        call    fcb_try_open
        jnc     fail

        ; (3) ドライブ欄 = 3 (C:) で ENVTEST.COM → 開けるはず
        mov     si, f_env_c
        call    fcb_try_open
        jc      fail

        ; (4) ドライブ欄 = 0 (カレント = A:) で HELLO.COM → 開けるはず
        mov     si, f_hello_0
        call    fcb_try_open
        jc      fail

        jmp     pass

; DS:SI = 37 バイトの FCB の雛形。作業用にコピーしてから開き、閉じる。
;   出力: CF=0 なら開けた
fcb_try_open:
        push    ax
        push    cx
        push    di
        push    es
        push    ds
        pop     es
        mov     di, drv_fcb
        mov     cx, 37
        rep     movsb
        mov     dx, drv_fcb
        mov     ah, 0x0F                ; FCB オープン
        int     0x21
        cmp     al, 0
        jne     .no
        mov     dx, drv_fcb
        mov     ah, 0x10                ; 開けたなら閉じる
        int     0x21
        clc
        jmp     .out
.no:
        stc
.out:
        pop     es
        pop     di
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; FCB で文字デバイスを開けるか
;
; ハンドルが入る前の DOS 1.x では、これが唯一のデバイスの開き方だった。
; 当時の作法を引きずったプログラムは今でもここを通ってくる。
; FreeDOS の LABEL は起動直後に FCB で CON を開き、失敗すると
; 「Not a valid drive」と言って何もせずに終わる。
; ---------------------------------------------------------------------------
t_fcb_device:
        mov     si, n_fcbdev
        call    begin

        mov     dx, con_fcb
        mov     ah, 0x0F
        int     0x21
        cmp     al, 0
        jne     fail

        mov     dx, con_fcb
        mov     ah, 0x10                ; 閉じる
        int     0x21
        jmp     pass

; ---------------------------------------------------------------------------
; 拡張 FCB の属性が、削除と作成で効いているか
;
; 属性 8 (ボリュームラベル) を立てた拡張 FCB に '???????????' を入れて
; AH=13h を呼ぶ、というのが LABEL のラベル張り替えの手順。属性を見ずに
; 名前だけで消すと、ディスクの中身が全部消える。実際そうなっていた。
; ここでは「ラベルを消す → 普通のファイルが残っている → ラベルを作り直す」
; までを通しで確かめる。
; ---------------------------------------------------------------------------
t_fcb_label:
        mov     si, n_fcblbl
        call    begin

        ; (1) 属性 8 の拡張 FCB でワイルドカード削除
        mov     dx, del_lbl_fcb
        mov     ah, 0x13
        int     0x21
        cmp     al, 0
        jne     fail                    ; ラベルが消せていない

        ; (2) 普通のファイルは生き残っていること
        mov     si, f_readme
        call    fcb_try_open
        jc      fail

        ; (3) 属性 8 の拡張 FCB でラベルを作り直す
        mov     dx, new_lbl_fcb
        mov     ah, 0x16
        int     0x21
        cmp     al, 0
        jne     fail
        mov     dx, new_lbl_fcb
        mov     ah, 0x10
        int     0x21

        ; (4) 作ったものがちゃんと属性 8 になっていること
        push    ds
        pop     es
        mov     ah, 0x1A
        mov     dx, vol_dta
        int     0x21
        mov     ah, 0x11
        mov     dx, vol_fcb
        int     0x21
        push    ax
        mov     ah, 0x1A                ; DTA を戻す
        mov     dx, dta_buf
        int     0x21
        pop     ax
        cmp     al, 0
        jne     fail
        cmp     byte [vol_dta + 6], ATTR_VOLUME
        jne     fail
        jmp     pass

begin:
        push    si
        mov     si, str_indent
        call    puts
        pop     si
        mov     [test_name], si
        ret

pass:
        inc     word [pass_count]
        mov     si, str_pass
        call    puts
        mov     si, [test_name]
        call    puts
        call    newline
        ret

fail:
        inc     word [fail_count]
        mov     si, str_fail
        call    puts
        mov     si, [test_name]
        call    puts
        call    newline
        ret

putc:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     [char_buf], al
        mov     dx, char_buf
        mov     cx, 1
        mov     bx, 1
        mov     ah, 0x40
        int     0x21
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

newline:
        push    ax
        mov     al, 13
        call    putc
        mov     al, 10
        call    putc
        pop     ax
        ret

puts:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        mov     dx, si
        xor     cx, cx
.len:
        lodsb
        test    al, al
        jz      .go
        inc     cx
        jmp     .len
.go:
        jcxz    .done
        mov     bx, 1
        mov     ah, 0x40
        int     0x21
.done:
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

put_dec:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     bx, 10
        xor     cx, cx
.split:
        xor     dx, dx
        div     bx
        push    dx
        inc     cx
        test    ax, ax
        jnz     .split
.emit:
        pop     ax
        add     al, '0'
        call    putc
        loop    .emit
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ============================================================================
; データ
; ============================================================================
msg_head:    db 13, 10, '=== MYDOS FCB / DOS 6.22 function test ===', 13, 10, 13, 10, 0
msg_result:  db '### RESULT pass=', 0
msg_result2: db ' fail=', 0
msg_end:     db '###TEST-END###', 13, 10, 0

str_indent:  db '  ', 0
str_pass:    db '[PASS] ', 0
str_fail:    db '[FAIL] ', 0

n_psp:       db 'PSP:5C / PSP:6C  command tail parsed into FCBs by EXEC', 0
n_parse:     db 'AH=29h  parse filename', 0
n_create:    db 'AH=16h/15h/10h  FCB create and sequential write', 0
n_read:      db 'AH=0Fh/14h  FCB open and sequential read', 0
n_size:      db 'AH=23h  FCB file size in records', 0
n_random:    db 'AH=21h/22h/24h  FCB random read, write, set record', 0
n_block:     db 'AH=27h  FCB random block read', 0
n_find:      db 'AH=11h/12h  FCB find first and next', 0
n_rename:    db 'AH=17h  FCB rename', 0
n_defdta:    db 'the default DTA is this program PSP:0080, not the parent', 0
n_vollabel:  db 'AH=11h  an extended FCB finds the volume label', 0
pat_all:     db '*.*', 0
defdta_ok:   db 0
             align 2
n_fcbdrv:    db 'FCB[0] selects the drive, 1 = A:', 0
n_fcbdev:    db 'AH=0Fh  an FCB can open a character device (CON)', 0
n_fcblbl:    db 'AH=13h/16h  an extended FCB acts on the attribute it names', 0
con_fcb:     db 0, 'CON        '
             times 25 db 0
f_readme:    db 0, 'README  TXT'
             times 25 db 0
; 属性 8 + 名前は全部ワイルドカード = 「ボリュームラベルを消せ」
del_lbl_fcb: db 0xFF, 0, 0, 0, 0, 0
             db ATTR_VOLUME
             db 0, '???????????'
             times 25 db 0
new_lbl_fcb: db 0xFF, 0, 0, 0, 0, 0
             db ATTR_VOLUME
             db 0, 'MYDOS      '
             times 25 db 0
f_hello_a:   db 1, 'HELLO   COM'
             times 25 db 0
f_hello_c:   db 3, 'HELLO   COM'
             times 25 db 0
f_hello_0:   db 0, 'HELLO   COM'
             times 25 db 0
f_env_c:     db 3, 'ENVTEST COM'
             times 25 db 0
drv_fcb:     times 37 db 0
             align 2
vol_fcb:     db 0xFF, 0, 0, 0, 0, 0
             db ATTR_VOLUME
             db 0
             db '???????????'
             times 25 db 0
vol_dta:     times 64 db 0
n_delete:    db 'AH=13h  FCB delete', 0
n_dpb:       db 'AH=32h  drive parameter block matches the BPB', 0
n_alloc:     db 'AH=1Bh  allocation info', 0
n_createnew: db 'AH=5Bh  create new file, fail if it exists', 0
n_extopen:   db 'AH=6Ch  extended open and create', 0
n_truename:  db 'AH=60h  truename canonicalises paths', 0

; 8.3 形式の期待値 (空白詰め 11 バイト)
e_fcbdata:   db 'FCBDATA DAT'
e_renamed:   db 'FCBREN  DAT'
e_wildfcb:   db 'FCB?????DAT'   ; FCBTEST.COM を巻き込まない形
e_fcbtwo:    db 'FCBTWO  DAT'
e_alpha:     db 'ALPHA      '
e_beta:      db 'BETA       '
e_readme:    db 'README  TXT'
e_wild:      db 'DATA    ???'

p_input:     db ' README.TXT', 0
p_wild:      db 'DATA.*', 0

f_new:       db 'FCBNEW.TMP', 0
f_absent:    db 'FCBABS.TMP', 0

tn_rel:      db 'readme.txt', 0
tn_exp1:     db 'A:\README.TXT', 0
tn_updown:   db '\SUB\..\FOO.TXT', 0
tn_exp2:     db 'A:\FOO.TXT', 0
tn_root:     db '\', 0
tn_exp3:     db 'A:\', 0

; --- 変数 ------------------------------------------------------------------
test_name:   dw 0
pass_count:  dw 0
fail_count:  dw 0
found_size:  dw 0
tn_dbg:      db '    got: ', 0
tn_out:      times 128 db 0
char_buf:    db 0
fcb1:        times 48 db 0
dta_buf:     times 48 db 0
io_buf:      times RECSIZE db 0
big_buf:     times RECSIZE * NRECS db 0
verify_buf:  times RECSIZE * NRECS db 0

             times 512 db 0
stack_top:
