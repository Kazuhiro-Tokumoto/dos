; ============================================================================
; emstest.asm  -  EMS (INT 67h) を確かめる
;
; EMS は 1MB の中に置いた 64KB の「窓」を 16KB ずつ 4 枚に区切り、その裏に
; 何百枚も用意したページを差し替えて使う仕組み。ここで見たいのは 4 つ。
;
;   ・EMS が居ることを、当時の正しい手順で確かめられるか
;     ("EMMXXXX0" という名前のデバイスが開けるか)
;   ・窓に出したページに書いた内容が、追い出して呼び戻しても残っているか
;     (窓の張り替えが「見せかけ」になっていないか)
;   ・4 枚の窓が互いに独立しているか
;   ・ハンドルを返したあと、そのページが空きに戻っているか
;
; 2 番目がいちばん大事で、ここを外していると「動いているように見えて
; 書いたものが消える」という、いちばん見つけにくい壊れ方になる。
; ============================================================================
        cpu     386
        bits    16
        org     0x100

EMS_PAGE_SIZE   equ 16384

start:
        mov     sp, stack_top
        mov     bx, stack_top
        add     bx, 15
        mov     cl, 4
        shr     bx, cl
        mov     ah, 0x4A
        int     0x21

        mov     si, msg_head
        call    puts

        call    t_present
        call    t_version
        call    t_counts
        call    t_alloc
        call    t_map
        call    t_persist
        call    t_four_windows
        call    t_savemap
        call    t_free

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
; 1. EMS が居ること
;
; 正式な手順は「EMMXXXX0 という名前のデバイスを開いてみる」。EMS ドライバは
; 文字デバイスとして自分を名乗っているので、居なければ開けない。
; ============================================================================
t_present:
        mov     si, n_present
        call    begin

        mov     dx, ems_name
        mov     ax, 0x3D00
        int     0x21
        jc      fail
        mov     bx, ax
        mov     ah, 0x3E
        int     0x21
        jmp     pass

; ============================================================================
; 2. 版数
; ============================================================================
t_version:
        mov     si, n_version
        call    begin
        mov     ah, 0x46
        int     0x67
        test    ah, ah
        jnz     fail
        cmp     al, 0x40                ; 4.0
        jne     fail
        jmp     pass

; ============================================================================
; 3. ページ数と窓の場所
; ============================================================================
t_counts:
        mov     si, n_counts
        call    begin

        mov     ah, 0x42
        int     0x67
        test    ah, ah
        jnz     fail
        test    dx, dx
        jz      fail                    ; 総ページ数が 0
        cmp     bx, dx
        ja      fail                    ; 空きが総数を超えている
        mov     [total_pages], dx
        mov     [free_before], bx

        ; 窓の場所。16KB 境界に載っていること。
        mov     ah, 0x41
        int     0x67
        test    ah, ah
        jnz     fail
        test    bx, bx
        jz      fail
        test    bx, 0x03FF              ; 0x400 段落 = 16KB の倍数か
        jnz     fail
        mov     [frame_seg], bx
        jmp     pass

; ============================================================================
; 4. ページを確保する
; ============================================================================
t_alloc:
        mov     si, n_alloc
        call    begin

        mov     ah, 0x43
        mov     bx, 6                   ; 6 ページ = 96KB
        int     0x67
        test    ah, ah
        jnz     fail
        mov     [handle], dx

        ; 空きが 6 減っていること
        mov     ah, 0x42
        int     0x67
        mov     ax, [free_before]
        sub     ax, 6
        cmp     ax, bx
        jne     fail

        ; そのハンドルが 6 ページ持っていること
        mov     ah, 0x4C
        mov     dx, [handle]
        int     0x67
        test    ah, ah
        jnz     fail
        cmp     bx, 6
        jne     fail
        jmp     pass

; ============================================================================
; 5. 窓に出して書く
; ============================================================================
t_map:
        mov     si, n_map
        call    begin

        mov     ah, 0x44
        mov     al, 0                   ; 窓 0
        mov     bx, 0                   ; 論理ページ 0
        mov     dx, [handle]
        int     0x67
        test    ah, ah
        jnz     fail

        ; 窓 0 の先頭に印を書く
        mov     es, [frame_seg]
        xor     di, di
        mov     word [es:di], 0x1234
        mov     word [es:di + EMS_PAGE_SIZE - 2], 0x5678
        cmp     word [es:di], 0x1234
        jne     fail

        ; 範囲外の窓と論理ページは断られること
        mov     ah, 0x44
        mov     al, 4                   ; 窓は 0-3 しかない
        mov     bx, 0
        mov     dx, [handle]
        int     0x67
        cmp     ah, 0x8B
        jne     fail

        mov     ah, 0x44
        mov     al, 0
        mov     bx, 6                   ; 6 ページしか持っていない (0-5)
        mov     dx, [handle]
        int     0x67
        cmp     ah, 0x8A
        jne     fail

        ; 知らないハンドルも断られること
        mov     ah, 0x44
        mov     al, 0
        mov     bx, 0
        mov     dx, 0x7FFF
        int     0x67
        cmp     ah, 0x83
        jne     fail
        jmp     pass

; ============================================================================
; 6. 追い出して呼び戻しても内容が残っているか  ← ここが本題
;
; 窓 0 に論理ページ 0 を出して書いた。そこへ論理ページ 1 を出せば、
; ページ 0 は窓から追い出される。もう一度ページ 0 を呼び戻したときに
; さっき書いた値が返ってこなければ、追い出すときの書き戻しをしていない。
; ============================================================================
t_persist:
        mov     si, n_persist
        call    begin

        ; 窓 0 に論理ページ 1 を出して、違う印を書く
        mov     ah, 0x44
        mov     al, 0
        mov     bx, 1
        mov     dx, [handle]
        int     0x67
        test    ah, ah
        jnz     fail

        mov     es, [frame_seg]
        xor     di, di
        cmp     word [es:di], 0x1234
        je      fail                    ; ページ 1 に 0 の内容が見えている
        mov     word [es:di], 0xABCD

        ; 論理ページ 0 を呼び戻す
        mov     ah, 0x44
        mov     al, 0
        mov     bx, 0
        mov     dx, [handle]
        int     0x67
        test    ah, ah
        jnz     fail

        mov     es, [frame_seg]
        xor     di, di
        cmp     word [es:di], 0x1234
        jne     fail                    ; 書いたものが消えている
        cmp     word [es:di + EMS_PAGE_SIZE - 2], 0x5678
        jne     fail                    ; ページの後ろ half が壊れている

        ; ページ 1 のほうも残っているか
        mov     ah, 0x44
        mov     al, 0
        mov     bx, 1
        mov     dx, [handle]
        int     0x67
        mov     es, [frame_seg]
        xor     di, di
        cmp     word [es:di], 0xABCD
        jne     fail
        jmp     pass

; ============================================================================
; 7. 4 枚の窓が互いに独立していること
; ============================================================================
t_four_windows:
        mov     si, n_four
        call    begin

        ; 窓 0-3 に論理ページ 2-5 を出して、それぞれ別の値を書く
        xor     cx, cx
.write:
        mov     ah, 0x44
        mov     al, cl
        mov     bx, cx
        add     bx, 2
        mov     dx, [handle]
        int     0x67
        test    ah, ah
        jnz     fail

        mov     ax, [frame_seg]
        push    cx
        mov     bx, cx
        mov     cl, 10
        shl     bx, cl                  ; 窓の番号 * 0x400 段落
        pop     cx
        add     ax, bx
        mov     es, ax
        xor     di, di
        mov     ax, cx
        add     ax, 0x1000
        mov     [es:di], ax

        inc     cx
        cmp     cx, 4
        jb      .write

        ; 読み返す
        xor     cx, cx
.read:
        mov     ax, [frame_seg]
        push    cx
        mov     bx, cx
        mov     cl, 10
        shl     bx, cl
        pop     cx
        add     ax, bx
        mov     es, ax
        xor     di, di
        mov     ax, cx
        add     ax, 0x1000
        cmp     [es:di], ax
        jne     fail
        inc     cx
        cmp     cx, 4
        jb      .read
        jmp     pass

; ============================================================================
; 8. 窓の状態を退避・復元する (47h / 48h)
; ============================================================================
t_savemap:
        mov     si, n_savemap
        call    begin

        mov     byte [step], 1
        mov     ah, 0x47                ; いまの並びを控える
        mov     dx, [handle]
        int     0x67
        test    ah, ah
        jnz     fail

        ; 二重には控えられない
        mov     byte [step], 2
        mov     ah, 0x47
        mov     dx, [handle]
        int     0x67
        cmp     ah, 0x8D
        jne     fail

        ; 窓 0 を別のページに差し替える
        mov     byte [step], 3
        mov     ah, 0x44
        mov     al, 0
        mov     bx, 0
        mov     dx, [handle]
        int     0x67
        mov     es, [frame_seg]
        xor     di, di
        cmp     word [es:di], 0x1234
        jne     fail

        ; 復元すると窓 0 は論理ページ 2 に戻るはず
        mov     byte [step], 4
        mov     ah, 0x48
        mov     dx, [handle]
        int     0x67
        test    ah, ah
        jnz     fail
        mov     byte [step], 5
        mov     es, [frame_seg]
        xor     di, di
        cmp     word [es:di], 0x1000
        jne     fail

        ; 控えは使い切ったので、もう復元できない
        mov     byte [step], 6
        mov     ah, 0x48
        mov     dx, [handle]
        int     0x67
        cmp     ah, 0x8C
        jne     fail
        jmp     pass

; ============================================================================
; 9. ハンドルを返すと空きが戻ること
; ============================================================================
t_free:
        mov     si, n_free
        call    begin

        mov     ah, 0x45
        mov     dx, [handle]
        int     0x67
        test    ah, ah
        jnz     fail

        ; 空きが元に戻っていること
        mov     ah, 0x42
        int     0x67
        cmp     bx, [free_before]
        jne     fail

        ; 返したハンドルはもう使えない
        mov     ah, 0x4C
        mov     dx, [handle]
        int     0x67
        cmp     ah, 0x83
        jne     fail
        jmp     pass

; ============================================================================
; 出力まわり
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
        mov     si, str_step
        call    puts
        movzx   ax, byte [step]
        call    put_dec
        call    newline
        mov     byte [step], 0
        ret

puts:
        push    ax
        push    bx
        push    si
.loop:
        lodsb
        test    al, al
        jz      .done
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        jmp     .loop
.done:
        pop     si
        pop     bx
        pop     ax
        ret

newline:
        push    ax
        push    dx
        mov     dl, 13
        mov     ah, 0x02
        int     0x21
        mov     dl, 10
        mov     ah, 0x02
        int     0x21
        pop     dx
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
        pop     dx
        add     dl, '0'
        mov     ah, 0x02
        int     0x21
        loop    .emit
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ============================================================================
; データ
; ============================================================================
msg_head:    db 13, 10, '=== MYDOS EMS (INT 67h) test ===', 13, 10, 13, 10, 0
msg_result:  db '### RESULT pass=', 0
msg_result2: db ' fail=', 0
msg_end:     db '###TEST-END###', 13, 10, 0
str_indent:  db '  ', 0
str_pass:    db '[PASS] ', 0
str_fail:    db '[FAIL] ', 0
str_step:    db '  step=', 0
step:        db 0

n_present:   db 'the EMMXXXX0 device is there', 0
n_version:   db 'AH=46h  EMS version 4.0', 0
n_counts:    db 'AH=41h/42h  page frame is 16KB aligned, pages are counted', 0
n_alloc:     db 'AH=43h/4Ch  allocate six pages', 0
n_map:       db 'AH=44h  map a page, and refuse the bad arguments', 0
n_persist:   db 'AH=44h  what was written survives being swapped out', 0
n_four:      db 'AH=44h  the four windows are independent', 0
n_savemap:   db 'AH=47h/48h  save and restore the page map', 0
n_free:      db 'AH=45h  freeing the handle gives the pages back', 0

ems_name:    db 'EMMXXXX0', 0

test_name:   dw 0
pass_count:  dw 0
fail_count:  dw 0
total_pages: dw 0
free_before: dw 0
frame_seg:   dw 0
handle:      dw 0

             times 512 db 0
stack_top:
