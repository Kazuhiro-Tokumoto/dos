; ============================================================================
; dostest.asm  -  INT 21h の自動テスト
;
; MYDOS 上で動かして、DOS の各機能が仕様どおりかを確かめる。
; 出力は 1 行 1 項目で、最後に集計とテスト終了の目印を出す。
; tools/runtest.py がシリアル経由でこれを読み、合否を判定する。
;
; 3 番目の「クラスタをまたぐ読み書き」は、参考にした JS 実装で見つかった
; 不具合 (クラスタを確保した直後に FAT へ印を付けていないため、512 バイトを
; 超えるファイルが 1 クラスタおきに壊れる) と同じ形の欠陥を検出する。
; 1 バイトずつ突き合わせているので、内容が 1 クラスタずれても見逃さない。
; ============================================================================
        cpu     386
        bits    16
        org     0x100

BIGSIZE equ 3000                        ; 512 の倍数でない、複数クラスタにまたがる長さ

start:
        ; .COM は起動時にメモリを全部渡されるので、自分に必要な分だけ残して
        ; 返す。これをやらないと AH=48h で 1 バイトも確保できない。
        ; (メモリのテストをする前に、まずこの作法自体を通しておく)
        mov     sp, stack_top
        mov     bx, stack_top
        add     bx, 15
        mov     cl, 4
        shr     bx, cl
        mov     ah, 0x4A
        int     0x21

        mov     dx, dta_buf             ; DTA を PSP の外に置く
        mov     ah, 0x1A
        int     0x21

        mov     si, msg_head
        call    puts

        call    t_version
        call    t_small_file
        call    t_big_file
        call    t_seek
        call    t_find
        call    t_directory
        call    t_memory
        call    t_rename
        call    t_delete
        call    t_diskfree

        ; --- 集計 ---
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

        ; 失敗があれば終了コードに載せる
        mov     ax, [fail_count]
        test    ax, ax
        jz      .clean
        mov     ax, 0x4C01
        int     0x21
.clean:
        mov     ax, 0x4C00
        int     0x21

; ============================================================================
; 1. DOS のバージョン
; ============================================================================
t_version:
        mov     si, n_version
        call    begin
        mov     ah, 0x30
        int     0x21
        cmp     al, 6                   ; MYDOS は 6.22 を名乗る
        jne     .fail
        cmp     ah, 22
        jne     .fail
        jmp     pass
.fail:
        jmp     fail

; ============================================================================
; 2. 小さいファイルの作成・書き込み・読み戻し
; ============================================================================
t_small_file:
        mov     si, n_small
        call    begin

        mov     dx, f_small
        xor     cx, cx
        mov     ah, 0x3C                ; 作成
        int     0x21
        jc      fail
        mov     [h1], ax

        mov     bx, [h1]
        mov     cx, small_len
        mov     dx, small_data
        mov     ah, 0x40                ; 書き込み
        int     0x21
        jc      fail
        cmp     ax, small_len
        jne     fail

        mov     bx, [h1]
        mov     ah, 0x3E                ; 閉じる
        int     0x21
        jc      fail

        ; 読み戻して 1 バイトずつ突き合わせる
        mov     dx, f_small
        mov     ax, 0x3D00
        int     0x21
        jc      fail
        mov     [h1], ax

        mov     bx, [h1]
        mov     cx, small_len + 16      ; わざと多めに要求する
        mov     dx, io_buf
        mov     ah, 0x3F
        int     0x21
        jc      fail
        cmp     ax, small_len           ; 実際のサイズ以上は読めないはず
        jne     .close_fail

        mov     si, small_data
        mov     di, io_buf
        mov     cx, small_len
        push    ds
        pop     es
        repe    cmpsb
        jne     .close_fail

        mov     bx, [h1]
        mov     ah, 0x3E
        int     0x21
        jmp     pass

.close_fail:
        mov     bx, [h1]
        mov     ah, 0x3E
        int     0x21
        jmp     fail

; ============================================================================
; 3. クラスタをまたぐファイル (これが一番壊れやすい)
; ============================================================================
t_big_file:
        mov     si, n_big
        call    begin

        ; 判別しやすい値でパターンを作る: byte[i] = (i * 7 + 3) & 0xFF
        mov     di, big_buf
        xor     cx, cx
        xor     al, al
        mov     bl, 3
.gen:
        mov     [di], bl
        inc     di
        add     bl, 7
        inc     cx
        cmp     cx, BIGSIZE
        jb      .gen

        mov     dx, f_big
        xor     cx, cx
        mov     ah, 0x3C
        int     0x21
        jc      fail
        mov     [h1], ax

        ; わざと 512 の倍数でない大きさで、2 回に分けて書く。
        ; セクタの途中から書き足す経路も通しておきたいため。
        mov     bx, [h1]
        mov     cx, 700
        mov     dx, big_buf
        mov     ah, 0x40
        int     0x21
        jc      fail
        cmp     ax, 700
        jne     fail

        mov     bx, [h1]
        mov     cx, BIGSIZE - 700
        mov     dx, big_buf + 700
        mov     ah, 0x40
        int     0x21
        jc      fail
        cmp     ax, BIGSIZE - 700
        jne     fail

        mov     bx, [h1]
        mov     ah, 0x3E
        int     0x21
        jc      fail

        ; --- 読み戻す ---
        mov     dx, f_big
        mov     ax, 0x3D00
        int     0x21
        jc      fail
        mov     [h1], ax

        ; 中身を消してから読む (前の内容が残っていて通ってしまうのを防ぐ)
        mov     di, verify_buf
        mov     cx, BIGSIZE
        xor     al, al
        push    ds
        pop     es
        rep     stosb

        mov     bx, [h1]
        mov     cx, BIGSIZE
        mov     dx, verify_buf
        mov     ah, 0x3F
        int     0x21
        jc      .close_fail
        cmp     ax, BIGSIZE
        jne     .close_fail             ; 読めた長さが違う = クラスタ連鎖が短い

        mov     si, big_buf
        mov     di, verify_buf
        mov     cx, BIGSIZE
        push    ds
        pop     es
        repe    cmpsb
        jne     .close_fail             ; 内容が 1 クラスタずれていても捕まる

        mov     bx, [h1]
        mov     ah, 0x3E
        int     0x21
        jmp     pass

.close_fail:
        mov     bx, [h1]
        mov     ah, 0x3E
        int     0x21
        jmp     fail

; ============================================================================
; 4. シーク (AH=42h)
; ============================================================================
t_seek:
        mov     si, n_seek
        call    begin

        mov     dx, f_big
        mov     ax, 0x3D00
        int     0x21
        jc      fail
        mov     [h1], ax

        ; 末尾へシークするとファイルサイズが返るはず
        mov     bx, [h1]
        mov     ax, 0x4202
        xor     cx, cx
        xor     dx, dx
        int     0x21
        jc      .close_fail
        cmp     ax, BIGSIZE
        jne     .close_fail
        test    dx, dx
        jnz     .close_fail

        ; 1000 バイト目へ移って 1 バイト読む
        mov     bx, [h1]
        mov     ax, 0x4200
        xor     cx, cx
        mov     dx, 1000
        int     0x21
        jc      .close_fail

        mov     bx, [h1]
        mov     cx, 1
        mov     dx, io_buf
        mov     ah, 0x3F
        int     0x21
        jc      .close_fail
        cmp     ax, 1
        jne     .close_fail

        mov     al, [io_buf]
        cmp     al, [big_buf + 1000]
        jne     .close_fail

        mov     bx, [h1]
        mov     ah, 0x3E
        int     0x21
        jmp     pass
.close_fail:
        mov     bx, [h1]
        mov     ah, 0x3E
        int     0x21
        jmp     fail

; ============================================================================
; 5. ファイル検索 (AH=4Eh / 4Fh)
; ============================================================================
t_find:
        mov     si, n_find
        call    begin

        ; さっき作った 2 つのファイルが "TEST*.*" で見つかるはず
        mov     dx, pat_test
        xor     cx, cx
        mov     ah, 0x4E
        int     0x21
        jc      fail

        xor     bx, bx                  ; BX = 見つかった数
.loop:
        inc     bx
        mov     ah, 0x4F
        int     0x21
        jnc     .loop

        cmp     bx, 2
        jne     fail
        jmp     pass

; ============================================================================
; 6. ディレクトリの作成・移動・取得・削除
; ============================================================================
t_directory:
        mov     si, n_dir
        call    begin

        mov     dx, d_sub
        mov     ah, 0x39                ; MKDIR
        int     0x21
        jc      fail

        mov     dx, d_sub
        mov     ah, 0x3B                ; CHDIR
        int     0x21
        jc      fail

        ; カレントディレクトリが期待どおりか
        mov     si, cwd_buf
        mov     dl, 0
        mov     ah, 0x47
        int     0x21
        jc      .back_fail

        mov     si, cwd_buf
        mov     di, d_sub
        call    streq
        jne     .back_fail

        ; サブディレクトリの中にファイルを作れるか
        mov     dx, f_inner
        xor     cx, cx
        mov     ah, 0x3C
        int     0x21
        jc      .back_fail
        mov     bx, ax
        mov     ah, 0x3E
        int     0x21

        mov     dx, f_inner
        mov     ah, 0x41                ; 消しておく
        int     0x21

        ; 親に戻る
        mov     dx, d_up
        mov     ah, 0x3B
        int     0x21
        jc      fail

        ; 空になったので消せるはず
        mov     dx, d_sub
        mov     ah, 0x3A                ; RMDIR
        int     0x21
        jc      fail
        jmp     pass

.back_fail:
        mov     dx, d_up
        mov     ah, 0x3B
        int     0x21
        jmp     fail

; ============================================================================
; 7. メモリの確保・拡大縮小・解放
; ============================================================================
t_memory:
        mov     si, n_mem
        call    begin

        ; 空き全部の大きさを聞く (0xFFFF を渡すと失敗して BX に最大値が入る)
        mov     bx, 0xFFFF
        mov     ah, 0x48
        int     0x21
        jnc     fail                    ; 全部は取れないはず (CF=1 が正しい)
        mov     [max_paras], bx
        test    bx, bx
        jz      fail

        ; 16 パラグラフ (256 バイト) だけ取る
        mov     bx, 16
        mov     ah, 0x48
        int     0x21
        jc      fail
        mov     [seg1], ax

        ; 取れた領域に書いて読めるか
        push    es
        mov     es, ax
        mov     word [es:0], 0x1234
        mov     word [es:254], 0x5678
        mov     ax, [es:0]
        mov     bx, [es:254]
        pop     es
        cmp     ax, 0x1234
        jne     .free_fail
        cmp     bx, 0x5678
        jne     .free_fail

        ; 縮める (必ず成功しなければならない)
        push    es
        mov     es, [seg1]
        mov     bx, 4
        mov     ah, 0x4A
        int     0x21
        pop     es
        jc      .free_fail

        ; 解放する
        push    es
        mov     es, [seg1]
        mov     ah, 0x49
        int     0x21
        pop     es
        jc      fail

        ; 解放したぶんが空きに戻っているか
        mov     bx, 0xFFFF
        mov     ah, 0x48
        int     0x21
        cmp     bx, [max_paras]
        jne     fail
        jmp     pass

.free_fail:
        push    es
        mov     es, [seg1]
        mov     ah, 0x49
        int     0x21
        pop     es
        jmp     fail

; ============================================================================
; 8. 名前の変更 (AH=56h)
; ============================================================================
t_rename:
        mov     si, n_rename
        call    begin

        push    ds
        pop     es
        mov     dx, f_small
        mov     di, f_renamed
        mov     ah, 0x56
        int     0x21
        jc      fail

        ; 新しい名前で開けること
        mov     dx, f_renamed
        mov     ax, 0x3D00
        int     0x21
        jc      fail
        mov     bx, ax
        mov     ah, 0x3E
        int     0x21

        ; 古い名前では開けないこと
        mov     dx, f_small
        mov     ax, 0x3D00
        int     0x21
        jnc     .still_there
        jmp     pass
.still_there:
        mov     bx, ax
        mov     ah, 0x3E
        int     0x21
        jmp     fail

; ============================================================================
; 9. ファイルの削除
; ============================================================================
t_delete:
        mov     si, n_delete
        call    begin

        mov     dx, f_renamed
        mov     ah, 0x41
        int     0x21
        jc      fail

        mov     dx, f_big
        mov     ah, 0x41
        int     0x21
        jc      fail

        ; 消えていること
        mov     dx, f_big
        mov     ax, 0x3D00
        int     0x21
        jnc     .still_there
        jmp     pass
.still_there:
        mov     bx, ax
        mov     ah, 0x3E
        int     0x21
        jmp     fail

; ============================================================================
; 10. 空き容量 (AH=36h)
; ============================================================================
t_diskfree:
        mov     si, n_free
        call    begin

        mov     ah, 0x36
        mov     dl, 0
        int     0x21
        cmp     ax, 0xFFFF              ; 0xFFFF は「無効なドライブ」
        je      fail
        test    ax, ax                  ; クラスタあたりセクタ数
        jz      fail
        cmp     cx, 512                 ; セクタあたりバイト数
        jne     fail
        test    bx, bx                  ; 空きクラスタ
        jz      fail
        test    dx, dx                  ; 総クラスタ
        jz      fail
        jmp     pass

; ============================================================================
; 判定と表示
; ============================================================================
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

; ============================================================================
; 小道具
; ============================================================================
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
msg_head:       db 13, 10, '=== MYDOS INT 21h self test ===', 13, 10, 13, 10, 0
msg_result:     db '### RESULT pass=', 0
msg_result2:    db ' fail=', 0
msg_end:        db '###TEST-END###', 13, 10, 0

str_indent:     db '  ', 0
str_pass:       db '[PASS] ', 0
str_fail:       db '[FAIL] ', 0

n_version:      db 'AH=30h  DOS version is 6.22', 0
n_small:        db 'AH=3Ch/40h/3Dh/3Fh  small file round trip', 0
n_big:          db 'AH=40h/3Fh  3000-byte file across clusters', 0
n_seek:         db 'AH=42h  seek from end and from start', 0
n_find:         db 'AH=4Eh/4Fh  FindFirst / FindNext', 0
n_dir:          db 'AH=39h/3Bh/47h/3Ah  directory create, enter, remove', 0
n_mem:          db 'AH=48h/49h/4Ah  memory allocate, resize, free', 0
n_rename:       db 'AH=56h  rename', 0
n_delete:       db 'AH=41h  delete', 0
n_free:         db 'AH=36h  free disk space', 0

f_small:        db 'TESTS.TXT', 0
f_renamed:      db 'TESTR.TXT', 0
f_big:          db 'TESTBIG.DAT', 0
f_inner:        db 'INNER.TXT', 0
pat_test:       db 'TEST*.*', 0
d_sub:          db 'TESTDIR', 0
d_up:           db '..', 0

small_data:     db 'The quick brown fox jumps over the lazy dog.', 13, 10
                db 'MYDOS small file test payload.', 13, 10
small_len       equ $ - small_data

; --- 変数 ------------------------------------------------------------------
test_name:      dw 0
pass_count:     dw 0
fail_count:     dw 0
h1:             dw 0
seg1:           dw 0
max_paras:      dw 0
char_buf:       db 0
cwd_buf:        times 68 db 0
dta_buf:        times 48 db 0
io_buf:         times 128 db 0
big_buf:        times BIGSIZE db 0
verify_buf:     times BIGSIZE db 0

                times 512 db 0
stack_top:
