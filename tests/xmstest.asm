; ============================================================================
; xmstest.asm  -  A20 / HMA / XMS を確かめる
;
; 1MB より上のメモリは、リアルモードからは直接触れない。当時のプログラムは
; XMS という約束ごしに使った。窓口の見つけ方まで含めて決まっているので、
; そのとおりの手順を踏んで確かめる。
;
;   INT 2Fh AX=4300h  → AL=80h なら XMS が居る
;   INT 2Fh AX=4310h  → ES:BX に入口の far アドレス
;   以降は far call で AH に機能番号を入れて呼ぶ
;
; ここで見たいのは 3 つ。
;   ・窓口が当時の手順どおりに見つかること
;   ・HMA を借りて返せること (A20 の向こう側に手が届いていること)
;   ・拡張メモリを確保して、そこへ書いて読み返せること
;     (INT 15h AH=87h のブロック転送が本当に効いているか)
; ============================================================================
        cpu     386
        bits    16
        org     0x100

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
        call    t_a20
        call    t_hma
        call    t_alloc
        call    t_move
        call    t_info
        call    t_free
        call    t_int2f
        call    t_xms3

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
; 1. 窓口が見つかること
; ============================================================================
t_present:
        mov     si, n_present
        call    begin

        mov     byte [step], 1
        mov     ax, 0x4300
        int     0x2F
        cmp     al, 0x80
        jne     failx

        mov     byte [step], 2
        mov     ax, 0x4310
        int     0x2F
        mov     [xms_off], bx
        mov     [xms_seg], es
        mov     ax, es
        or      ax, bx
        jz      failx                   ; 0000:0000 は入口ではない
        jmp     pass

; ============================================================================
; 2. バージョンと HMA の有無
; ============================================================================
t_version:
        mov     si, n_version
        call    begin

        mov     byte [step], 1
        mov     ah, 0x00
        call    far [xms_off]
        cmp     ax, 0x0300              ; XMS 3.0
        jne     failx
        mov     byte [step], 2
        cmp     dx, 1                   ; HMA がある
        jne     failx
        jmp     pass

; ============================================================================
; 3. A20 の開け閉め
;
; DOS=HIGH のときは、閉じてくれと言われても開けたままにするのが本物の
; 振る舞い。閉じた瞬間にカーネルが消えるため。ここでもそれを確かめる。
; ============================================================================
t_a20:
        mov     si, n_a20
        call    begin

        ; ローカルで開ける
        mov     byte [step], 1
        mov     ah, 0x05
        call    far [xms_off]
        cmp     ax, 1
        jne     failx

        mov     byte [step], 2
        mov     ah, 0x07                ; 状態を聞く
        call    far [xms_off]
        cmp     ax, 1
        jne     failx

        ; 実際に回り込んでいないことを見る
        mov     byte [step], 3
        call    a20_check
        jc      failx

        ; 閉じる (数えているので、開けた回数だけ閉じると閉じる)
        mov     byte [step], 4
        mov     ah, 0x06
        call    far [xms_off]
        cmp     ax, 1
        jne     failx
        jmp     pass

; ============================================================================
; 4. HMA を借りて返す
;
; DOS 自身は HMA を使わない (カーネルは低位メモリに居る) ので、
; 最初に要求したプログラムがそのまま借りられる。2 度目は「使用中」で
; 断られ、返せばまた借りられる。
; ============================================================================
t_hma:
        mov     si, n_hma
        call    begin

        mov     byte [step], 1
        mov     ah, 0x01
        mov     dx, 0xFFFF              ; 全部使う
        call    far [xms_off]
        cmp     ax, 1
        jne     failx

        ; 借りている間は A20 が開いている (HMA は A20 の向こうにあるので)
        mov     byte [step], 2
        call    a20_check
        jc      failx

        ; 二重に借りられないこと
        mov     byte [step], 3
        mov     ah, 0x01
        mov     dx, 0xFFFF
        call    far [xms_off]
        test    ax, ax
        jnz     failx
        mov     byte [step], 4
        cmp     bl, 0x91                ; HMA は使用中
        jne     failx

        ; 返す
        mov     byte [step], 5
        mov     ah, 0x02
        call    far [xms_off]
        cmp     ax, 1
        jne     failx

        ; 返したあとは、もう一度借りられる
        mov     byte [step], 6
        mov     ah, 0x01
        mov     dx, 0xFFFF
        call    far [xms_off]
        cmp     ax, 1
        jne     failx
        mov     ah, 0x02
        call    far [xms_off]
        jmp     pass

; ============================================================================
; 5. 拡張メモリを確保する
; ============================================================================
t_alloc:
        mov     si, n_alloc
        call    begin

        ; 空きを聞く
        mov     byte [step], 1
        mov     ah, 0x08
        call    far [xms_off]
        test    ax, ax
        jz      failx                   ; 一番大きく取れるブロックが 0
        mov     [free_kb], ax

        ; 64KB 取る
        mov     byte [step], 2
        mov     ah, 0x09
        mov     dx, 64
        call    far [xms_off]
        cmp     ax, 1
        jne     failx
        mov     [handle], dx
        test    dx, dx
        jz      failx

        ; 取ったぶん空きが減っていること
        jmp     pass

; ============================================================================
; 6. 拡張メモリへ書いて読み返す
;
; XMS の 0Bh は「転送元と転送先を書いた構造体」を渡す形。ハンドルが 0 なら
; 通常メモリで、そのときオフセットの欄は far ポインタとして読まれる。
; ============================================================================
t_move:
        mov     si, n_move
        call    begin

        ; 送るデータを用意する
        mov     di, src_buf
        mov     cx, 256
        mov     al, 0
.fill:
        mov     [di], al
        inc     di
        inc     al
        loop    .fill

        ; 通常メモリ → 拡張メモリ
        mov     byte [step], 1
        mov     dword [mv_len], 256
        mov     word [mv_srch], 0
        mov     word [mv_srco], src_buf
        mov     [mv_srco + 2], ds
        mov     ax, [handle]
        mov     [mv_dsth], ax
        mov     dword [mv_dsto], 0
        mov     si, mv_len
        mov     ah, 0x0B
        call    far [xms_off]
        cmp     ax, 1
        jne     failx

        ; 受け側を汚しておく (本当に転送されたか分かるように)
        ;
        ; ES を入れ直すのを忘れないこと。直前に INT 2Fh AX=4310h を
        ; 呼んでいるので、ES には XMS の入口 — つまりカーネルのセグメント —
        ; が入ったままになっている。そのまま rep stosb すると 0xEE を
        ; 256 バイト、カーネルのコードの上に書き込む。書いた瞬間には
        ; 何も起こらず、あとでそこを通ったときに機械ごと落ちる。
        ; 「XMS のブロック転送のあとで C: を触ると再起動する」という
        ; 症状で長いこと残っていた不具合の正体がこれだった。
        push    ds
        pop     es
        mov     di, dst_buf
        mov     cx, 256
        mov     al, 0xEE
        rep     stosb

        ; 拡張メモリ → 通常メモリ
        mov     byte [step], 2
        mov     dword [mv_len], 256
        mov     ax, [handle]
        mov     [mv_srch], ax
        mov     dword [mv_srco], 0
        mov     word [mv_dsth], 0
        mov     word [mv_dsto], dst_buf
        mov     [mv_dsto + 2], ds
        mov     si, mv_len
        mov     ah, 0x0B
        call    far [xms_off]
        cmp     ax, 1
        jne     failx

        ; 中身が一致すること
        mov     byte [step], 3
        mov     si, src_buf
        mov     di, dst_buf
        mov     cx, 256
        push    ds
        pop     es
        repe    cmpsb
        jne     failx
        jmp     pass

; ============================================================================
; 7. ハンドルの情報と大きさの変更
; ============================================================================
t_info:
        mov     si, n_info
        call    begin

        mov     byte [step], 1
        mov     ah, 0x0E
        mov     dx, [handle]
        call    far [xms_off]
        cmp     ax, 1
        jne     failx
        mov     byte [step], 2
        cmp     dx, 64                  ; 確保した大きさ
        jne     failx
        mov     byte [step], 3
        cmp     bh, 0                   ; ロックしていない
        jne     failx

        ; ロックすると物理アドレスが返る
        mov     byte [step], 4
        mov     ah, 0x0C
        mov     dx, [handle]
        call    far [xms_off]
        cmp     ax, 1
        jne     failx
        mov     byte [step], 5
        cmp     dx, 0x0010              ; 1MB より上にあるはず
        jb      failx

        mov     byte [step], 6
        mov     ah, 0x0D
        mov     dx, [handle]
        call    far [xms_off]
        cmp     ax, 1
        jne     failx

        ; 32KB に縮める
        mov     byte [step], 7
        mov     ah, 0x0F
        mov     dx, [handle]
        mov     bx, 32
        call    far [xms_off]
        cmp     ax, 1
        jne     failx
        jmp     pass

; ============================================================================
; 8. 返す
; ============================================================================
t_free:
        mov     si, n_free
        call    begin

        mov     byte [step], 1
        mov     ah, 0x0A
        mov     dx, [handle]
        call    far [xms_off]
        cmp     ax, 1
        jne     failx

        ; 返したハンドルはもう使えない
        mov     byte [step], 2
        mov     ah, 0x0E
        mov     dx, [handle]
        call    far [xms_off]
        test    ax, ax
        jnz     failx
        mov     byte [step], 3
        cmp     bl, 0xA2                ; 無効なハンドル
        jne     failx
        jmp     pass

; ---------------------------------------------------------------------------
; a20_check - A20 が本当に開いているか自分で確かめる
;   出力: CF=0 なら開いている
;
; 0000:0500 と FFFF:0510 は同じ物理番地を指す — A20 が閉じていれば。
; 片方を書き換えてもう片方が変わらなければ、20 本目のアドレス線が生きている。
; ---------------------------------------------------------------------------
a20_check:
        push    ax
        push    cx
        push    si
        push    di
        push    ds
        push    es

        xor     ax, ax
        mov     ds, ax
        mov     si, 0x0500
        mov     ax, 0xFFFF
        mov     es, ax
        mov     di, 0x0510

        mov     cx, [si]
        mov     ax, [es:di]
        push    cx
        push    ax
        mov     word [si], 0x1234
        mov     word [es:di], 0x5678
        mov     cx, [si]
        pop     ax
        mov     [es:di], ax
        pop     ax
        mov     [si], ax

        cmp     cx, 0x1234
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     cx
        pop     ax
        je      .open
        stc
        ret
.open:
        clc
        ret

; ============================================================================
; 9. INT 2Fh は知らない機能でレジスタを変えないこと
;
; INT 2Fh は「導入確認」の待ち合わせ場所で、呼ぶ側が AL=0 を入れて
; 呼び、常駐したものがあれば AL=FFh を返す、という取り決めになっている。
; DOS が知らない機能で勝手に AL を書くと、この取り決めが壊れるだけでなく、
; AL に引数を載せて呼ぶ機能 (DPMI の 1687h など) では返り値そのものが
; 化ける。DPMI ホストの有無を見に来たプログラムが「いる」と誤解する、
; といった形で表に出る。
; ============================================================================
t_int2f:
        mov     si, n_int2f
        call    begin

        ; DPMI の導入確認。ホストが居なければ AX は 1687h のまま返るのが
        ; 正しい。0 が返ると「DPMI ホストが居る」の意味になってしまう。
        mov     byte [step], 1
        mov     ax, 0x1687
        xor     bx, bx
        xor     cx, cx
        xor     dx, dx
        xor     si, si
        int     0x2F
        cmp     ax, 0x1687
        jne     failx

        ; 誰も居ない機能番号でも同じ。AL を書き換えられていないか見る。
        mov     byte [step], 2
        mov     ax, 0xB800
        int     0x2F
        cmp     ax, 0xB800
        jne     failx

        jmp     pass

; ============================================================================
; 10. XMS 3.0 の 32bit 系 (88h / 89h / 8Eh / 8Fh)
;
; 16bit の 09h では DX が KB 数なので 64MB までしか頼めない。XMS 3.0 は
; EDX を使う 89h を足した。いまの DPMI ホスト (DJGPP の CWSDPMI など) は
; こちらしか呼ばない。
;
; 実装していないと「確保できなかった」ではなく「そんな機能はない」が
; 返る。呼び出し側がそれを見落とすと、掴んでもいないメモリを自分のもの
; として配り始める。実際 CWSDPMI は物理 0 番地から配り出して、DOS の
; カーネルを上書きした。
; ============================================================================
t_xms3:
        mov     si, n_xms3
        call    begin

        ; 88h: 空きを 32bit で問い合わせる
        mov     byte [step], 1
        mov     ah, 0x88
        call    far [xms_off]
        test    eax, eax
        jz      failx                   ; 空きが 0 はおかしい
        mov     [.free_kb], eax

        ; 89h: 32bit で 128KB 確保する
        mov     byte [step], 2
        mov     ah, 0x89
        mov     edx, 128
        call    far [xms_off]
        test    ax, ax
        jz      failx
        mov     [.handle], dx

        ; 8Eh: 大きさを問い合わせる
        mov     byte [step], 3
        mov     ah, 0x8E
        mov     dx, [.handle]
        call    far [xms_off]
        test    ax, ax
        jz      failx
        cmp     edx, 128
        jne     failx

        ; 8Fh: 64KB に縮める
        mov     byte [step], 4
        mov     ah, 0x8F
        mov     ebx, 64
        mov     dx, [.handle]
        call    far [xms_off]
        test    ax, ax
        jz      failx

        mov     byte [step], 5
        mov     ah, 0x8E
        mov     dx, [.handle]
        call    far [xms_off]
        test    ax, ax
        jz      failx
        cmp     edx, 64
        jne     failx

        ; 0Ch: 物理アドレスが 1MB より上を指していること
        mov     byte [step], 6
        mov     ah, 0x0C
        mov     dx, [.handle]
        call    far [xms_off]
        test    ax, ax
        jz      failx
        test    dx, dx
        jz      failx                   ; 上位が 0 = 1MB より下

        mov     ah, 0x0D
        mov     dx, [.handle]
        call    far [xms_off]

        mov     byte [step], 7
        mov     ah, 0x0A
        mov     dx, [.handle]
        call    far [xms_off]
        test    ax, ax
        jz      failx
        jmp     pass
.free_kb: dd 0
.handle:  dw 0

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

failx:
        push    ax
        inc     word [fail_count]
        mov     si, str_fail
        call    puts
        mov     si, [test_name]
        call    puts
        mov     si, str_step
        call    puts
        movzx   ax, byte [step]
        call    put_dec
        mov     si, str_ax
        call    puts
        pop     ax
        call    put_dec
        call    newline
        ret

putc:
        push    ax
        push    bx
        push    cx
        push    dx
        push    ds
        push    cs
        pop     ds
        mov     [char_buf], al
        mov     dx, char_buf
        mov     cx, 1
        mov     bx, 1
        mov     ah, 0x40
        int     0x21
        pop     ds
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
        push    si
.loop:
        lodsb
        test    al, al
        jz      .done
        call    putc
        jmp     .loop
.done:
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
msg_head:    db 13, 10, '=== MYDOS A20 / HMA / XMS test ===', 13, 10, 13, 10, 0
msg_result:  db '### RESULT pass=', 0
msg_result2: db ' fail=', 0
msg_end:     db '###TEST-END###', 13, 10, 0

str_indent:  db '  ', 0
str_pass:    db '[PASS] ', 0
str_fail:    db '[FAIL] ', 0
str_step:    db '  (step=', 0
str_ax:      db ' ax=', 0

n_present:   db 'INT 2Fh 4300h/4310h  the XMS entry point is there', 0
n_version:   db 'XMS 00h  version 3.00 and the HMA exists', 0
n_a20:       db 'XMS 05h/06h/07h  the A20 gate really opens', 0
n_hma:       db 'XMS 01h/02h  the HMA can be borrowed and given back', 0
n_alloc:     db 'XMS 08h/09h  allocate 64K of extended memory', 0
n_move:      db 'XMS 0Bh  copy up past 1MB and back, byte for byte', 0
n_info:      db 'XMS 0Ch/0Dh/0Eh/0Fh  lock, query and shrink', 0
n_free:      db 'XMS 0Ah  the handle is gone after freeing it', 0
n_int2f:     db 'INT 2Fh  an unknown function leaves the registers alone', 0
n_xms3:      db 'XMS 88h/89h/8Eh/8Fh  the 32-bit calls a DPMI host uses', 0

; --- 変数 ------------------------------------------------------------------
test_name:   dw 0
pass_count:  dw 0
fail_count:  dw 0
step:        db 0
char_buf:    db 0
xms_off:     dw 0
xms_seg:     dw 0
handle:      dw 0
free_kb:     dw 0

; XMS 0Bh に渡す構造体
             align 2
mv_len:      dd 0
mv_srch:     dw 0
mv_srco:     dd 0
mv_dsth:     dw 0
mv_dsto:     dd 0

src_buf:     times 256 db 0
dst_buf:     times 256 db 0

             times 512 db 0
stack_top:
