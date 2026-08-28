; ============================================================================
; envtest.asm  -  子プロセスに渡される環境ブロックを確かめる
;
; DOS は EXEC のたびに子プロセス用の環境ブロックを作り直す。中身は
;
;     "NAME=VALUE", 0 の並び
;     0                       ← 並びの終わり
;     word 1                  ← 続きがある印
;     "X:\PATH\PROG.EXE", 0   ← 起動したプログラムのフルパス
;
; という形で、最後の文字列が C の argv[0] になる。DOS エクステンダ
; (DOS/32A や DOS4GW) を被せたプログラムは、16bit のスタブが 32bit の
; 本体を読むために自分の EXE を開き直す。その場所をこの文字列だけから
; 決めるので、ここが親のパスのままだと COMMAND.COM を自分だと思って
; 開き、「exec ファイルが壊れている」と言って落ちる。
;
; 親の環境をそのまま渡す実装だと、この試験は全部落ちる。
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
        mov     ah, 0x4A                ; 自分のブロックを縮める
        int     0x21

        mov     si, msg_head
        call    puts

        call    t_have_env
        call    t_not_parent
        call    t_strings
        call    t_count
        call    t_argv0
        call    t_drive
        call    t_owner

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
        mov     ah, 0x4C
        int     0x21

; ============================================================================
; env_scan - 環境ブロックを歩いて、各部分の位置を控える
;   出力: [env_seg]  環境セグメント (0 なら無い)
;         [off_end]  並びの終わりの 0 の次 (= word 1 の位置)
;         [off_path] フルパスの先頭
;         CF=1 なら環境が無い
; ============================================================================
env_scan:
        push    ax
        push    si
        push    es

        mov     ax, [0x2C]
        mov     [env_seg], ax
        test    ax, ax
        jz      .none
        mov     es, ax

        xor     si, si
.loop:
        cmp     si, 0x7FF0              ; 壊れていても止まるように
        jae     .none
        cmp     byte [es:si], 0
        je      .end
.str:
        inc     si
        cmp     byte [es:si], 0
        jne     .str
        inc     si
        jmp     .loop
.end:
        inc     si                      ; 並びの終わりの 0 を飛ばす
        mov     [off_end], si
        add     si, 2                   ; word 1 を飛ばす
        mov     [off_path], si

        pop     es
        pop     si
        pop     ax
        clc
        ret
.none:
        pop     es
        pop     si
        pop     ax
        stc
        ret

; ============================================================================
; 試験
; ============================================================================

; --- 環境ブロックがあるか -------------------------------------------------
t_have_env:
        mov     si, name_have
        call    begin
        call    env_scan
        jc      .bad
        jmp     pass
.bad:
        jmp     fail

; --- 親のものをそのまま渡していないか -------------------------------------
;
; 親 (COMMAND.COM) の PSP は自分の PSP から辿れないので、代わりに
; 「自分の環境ブロックの持ち主が自分の PSP になっているか」を見る。
; 親のものを使い回していれば、持ち主は親の PSP のままになる。
t_not_parent:
        mov     si, name_notparent
        call    begin
        call    env_scan
        jc      .bad

        mov     ax, [env_seg]
        dec     ax
        mov     es, ax
        mov     ax, [es:1]              ; MCB +1 = 持ち主の PSP
        mov     bx, cs                  ; .COM なので CS = 自分の PSP
        cmp     ax, bx
        jne     .bad
        jmp     pass
.bad:
        jmp     fail

; --- 文字列の並びが壊れていないか -----------------------------------------
t_strings:
        mov     si, name_strings
        call    begin
        call    env_scan
        jc      .bad

        mov     es, [env_seg]
        xor     si, si
        mov     di, want_comspec
        call    cmp_far
        jc      .bad
        jmp     pass
.bad:
        jmp     fail

; --- 続きがある印 (word 1) ------------------------------------------------
t_count:
        mov     si, name_count
        call    begin
        call    env_scan
        jc      .bad

        mov     es, [env_seg]
        mov     si, [off_end]
        cmp     word [es:si], 1
        jne     .bad
        jmp     pass
.bad:
        jmp     fail

; --- argv[0] が自分のフルパスか -------------------------------------------
t_argv0:
        mov     si, name_argv0
        call    begin
        call    env_scan
        jc      .bad

        ; 末尾が "\ENVTEST.COM" で終わっているか
        mov     es, [env_seg]
        mov     si, [off_path]
        call    far_len                 ; CX = 長さ
        cmp     cx, want_tail_len
        jb      .bad
        add     si, cx
        sub     si, want_tail_len
        mov     di, want_tail
        call    cmp_far
        jc      .bad
        jmp     pass
.bad:
        jmp     fail

; --- argv[0] の頭がカレントドライブか --------------------------------------
;
; A: から起動すれば "A:\...", C: からなら "C:\..." になっていなければ
; ならない。ここが常に 'A' だと、他のドライブに置いたプログラムは
; 自分を見つけられない。
t_drive:
        mov     si, name_drive
        call    begin
        call    env_scan
        jc      .bad

        mov     ah, 0x19                ; カレントドライブ
        int     0x21
        add     al, 'A'
        mov     ah, al

        mov     es, [env_seg]
        mov     si, [off_path]
        mov     al, [es:si]
        cmp     al, ah
        jne     .bad
        cmp     byte [es:si + 1], ':'
        jne     .bad
        cmp     byte [es:si + 2], '\'
        jne     .bad
        jmp     pass
.bad:
        jmp     fail

; --- 環境ブロックが本体と重なっていないか ---------------------------------
;
; 環境ブロックを本体より後に確保していると、.COM が空きを全部持って
; いったあとには場所が残らない。持ち主を 0 にしてしまうと MCB からは
; 「空き」に見えて、本体の確保に食われる。どちらの間違いでも、環境と
; 自分のコードが重なる。
t_owner:
        mov     si, name_owner
        call    begin
        call    env_scan
        jc      .bad

        mov     ax, [env_seg]
        mov     bx, cs
        cmp     ax, bx
        je      .bad                    ; 自分と同じセグメント = 重なっている
        jmp     pass
.bad:
        jmp     fail

; ============================================================================
; 小道具
; ============================================================================

; cmp_far - ES:SI の文字列が DS:DI で始まるか。CF=0 なら一致。
cmp_far:
        push    ax
        push    si
        push    di
.loop:
        mov     al, [di]
        test    al, al
        jz      .same
        cmp     al, [es:si]
        jne     .diff
        inc     si
        inc     di
        jmp     .loop
.same:
        pop     di
        pop     si
        pop     ax
        clc
        ret
.diff:
        pop     di
        pop     si
        pop     ax
        stc
        ret

; far_len - ES:SI の ASCIZ の長さを CX に返す
far_len:
        push    si
        xor     cx, cx
.loop:
        cmp     byte [es:si], 0
        je      .done
        inc     si
        inc     cx
        cmp     cx, 0x7FF0
        jb      .loop
.done:
        pop     si
        ret

; ============================================================================
; 出力まわり (他の試験と同じもの)
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

; failx - どの段で落ちたかと、そのときの AX を添えて出す。
; 中で何が起きたかを、もう一度エミュレータを回さずに追えるようにするため。
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
msg_head:    db 13, 10, '=== MYDOS environment block / argv[0] test ===', 13, 10, 13, 10, 0
msg_result:  db '### RESULT pass=', 0
msg_result2: db ' fail=', 0
msg_end:     db '###TEST-END###', 13, 10, 0

str_indent:  db '  ', 0
str_pass:    db '[PASS] ', 0
str_fail:    db '[FAIL] ', 0
str_step:    db '  (step=', 0
str_ax:      db ' ax=', 0

name_have:       db 'PSP:2Ch  the child gets an environment block', 0
name_notparent:  db 'the block belongs to this PSP, not the parent', 0
name_strings:    db 'the inherited strings survived the copy', 0
name_count:      db 'the word after the strings is 1', 0
name_argv0:      db 'argv[0] is this program, not the parent', 0
name_drive:      db 'argv[0] carries the drive it was started from', 0
name_owner:      db 'the block is not overlapping the program itself', 0

want_comspec:    db 'COMSPEC=', 0
want_tail:       db '\ENVTEST.COM', 0
want_tail_len    equ 12

env_seg:     dw 0
off_end:     dw 0
off_path:    dw 0

test_name:   dw 0
pass_count:  dw 0
fail_count:  dw 0
step:        db 0
char_buf:    db 0

             times 512 db 0
stack_top:
