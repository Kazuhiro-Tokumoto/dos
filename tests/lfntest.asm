; ============================================================================
; lfntest.asm  -  長いファイル名 (VFAT の LFN)
;
; FAT のディレクトリエントリは名前を 11 バイトしか持てない。8 文字 +
; 拡張子 3 文字という制限はここから来ている。VFAT は、本体のエントリの
; 手前に属性 0x0F のエントリを並べて、そこに長い名前を 13 文字ずつ
; 分けて入れる。属性 0x0F は昔の DOS が読み飛ばす組み合わせなので、
; 知らないシステムから見ても壊れて見えない。
;
; ここで見たいのは
;   ・長い名前でファイルを作れるか (断片が正しく並ぶか)
;   ・作ったものを長い名前で開き直せるか
;   ・短い名前 ("NAME~1.EXT") も同時に付いていて、そちらでも開けるか
;   ・AX=714Eh の検索が長い名前を返すか
;   ・消したあとに断片が取り残されないか
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

        call    t_volinfo
        call    t_create
        call    t_reopen
        call    t_shortname
        call    t_find
        call    t_subdir
        call    t_delete

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
; 1. 長い名前の窓口があること (AX=71A0h)
; ============================================================================
t_volinfo:
        mov     si, n_volinfo
        call    begin
        mov     byte [step], 1
        mov     ax, 0x71A0
        mov     di, volinfo_buf
        push    ds
        pop     es
        mov     cx, 64
        int     0x21
        jc      failx
        mov     byte [step], 2
        cmp     cx, 255                 ; 名前の最大長
        jne     failx
        jmp     pass

; ============================================================================
; 2. 長い名前でファイルを作る
; ============================================================================
t_create:
        mov     si, n_create
        call    begin

        mov     byte [step], 1
        mov     dx, long1
        xor     cx, cx
        mov     ah, 0x3C
        int     0x21
        jc      failx
        mov     [handle], ax

        mov     byte [step], 2
        mov     bx, [handle]
        mov     dx, payload
        mov     cx, payload_len
        mov     ah, 0x40
        int     0x21
        jc      failx
        cmp     ax, payload_len
        jne     failx

        mov     bx, [handle]
        mov     ah, 0x3E
        int     0x21
        jmp     pass

; ============================================================================
; 3. 長い名前で開き直せる
; ============================================================================
t_reopen:
        mov     si, n_reopen
        call    begin

        mov     byte [step], 1
        mov     dx, long1
        mov     ax, 0x3D00
        int     0x21
        jc      failx
        mov     [handle], ax

        mov     byte [step], 2
        mov     bx, [handle]
        mov     dx, read_buf
        mov     cx, 64
        mov     ah, 0x3F
        int     0x21
        pushf
        push    ax
        mov     bx, [handle]
        mov     ah, 0x3E
        int     0x21
        pop     ax
        popf
        jc      failx
        cmp     ax, payload_len
        jne     failx

        mov     byte [step], 3
        mov     si, read_buf
        mov     di, payload
        mov     cx, payload_len
        call    cmp_mem
        jc      failx
        jmp     pass

; ============================================================================
; 4. 短い名前も付いていて、そちらでも開ける
;
; 長い名前を知らないプログラムからも触れなければ意味がない。
; ============================================================================
t_shortname:
        mov     si, n_shortname
        call    begin

        mov     byte [step], 1
        mov     dx, short1
        mov     ax, 0x3D00
        int     0x21
        jc      failx
        mov     bx, ax
        mov     ah, 0x3E
        int     0x21
        jmp     pass

; ============================================================================
; 5. AX=714Eh の検索が長い名前を返す
; ============================================================================
t_find:
        mov     si, n_find
        call    begin

        mov     byte [step], 1
        push    ds
        pop     es
        mov     di, find_buf
        mov     dx, long1
        xor     cx, cx
        mov     si, 1
        mov     ax, 0x714E
        int     0x21
        jc      failx
        mov     [search], ax

        ; 長い名前がそのまま返っているか
        mov     byte [step], 2
        mov     si, find_buf + 0x2C
        mov     di, long1
        call    cmp_z
        pushf
        mov     bx, [search]
        mov     ax, 0x71A1
        int     0x21
        popf
        jc      failx
        jmp     pass

; ============================================================================
; 6. 長い名前のディレクトリ
; ============================================================================
t_subdir:
        mov     si, n_subdir
        call    begin

        mov     byte [step], 1
        mov     dx, longdir
        mov     ah, 0x39                ; ディレクトリを作る
        int     0x21
        jc      failx

        mov     byte [step], 2
        mov     dx, longdir
        mov     ah, 0x3B                ; そこへ移る
        int     0x21
        jc      failx

        mov     byte [step], 3
        mov     dx, str_root
        mov     ah, 0x3B
        int     0x21
        jc      failx

        mov     byte [step], 4
        mov     dx, longdir
        mov     ah, 0x3A                ; 消す
        int     0x21
        jc      failx
        jmp     pass

; ============================================================================
; 7. 消したあとに断片が残らない
;
; 断片が取り残されると、次にそこへ作られたファイルに前の名前が付いて
; 見える。消したあと同じ場所に別の名前で作り直して確かめる。
; ============================================================================
t_delete:
        mov     si, n_delete
        call    begin

        mov     byte [step], 1
        mov     dx, long1
        mov     ah, 0x41
        int     0x21
        jc      failx

        ; 消えていること
        mov     byte [step], 2
        mov     dx, long1
        mov     ax, 0x3D00
        int     0x21
        jnc     failx

        ; 短い名前でも開けないこと
        mov     byte [step], 3
        mov     dx, short1
        mov     ax, 0x3D00
        int     0x21
        jnc     failx

        ; 別の長い名前で作り直して、その名前で見つかること
        mov     byte [step], 4
        mov     dx, long2
        xor     cx, cx
        mov     ah, 0x3C
        int     0x21
        jc      failx
        mov     bx, ax
        mov     ah, 0x3E
        int     0x21

        mov     byte [step], 5
        push    ds
        pop     es
        mov     di, find_buf
        mov     dx, long2
        xor     cx, cx
        mov     si, 1
        mov     ax, 0x714E
        int     0x21
        jc      failx
        mov     [search], ax
        mov     si, find_buf + 0x2C
        mov     di, long2
        call    cmp_z
        pushf
        mov     bx, [search]
        mov     ax, 0x71A1
        int     0x21
        popf
        jc      failx

        ; 後始末
        mov     dx, long2
        mov     ah, 0x41
        int     0x21
        jmp     pass

; ============================================================================
; 小道具
; ============================================================================

; cmp_mem - DS:SI と DS:DI を CX バイト比べる。CF=0 なら同じ。
cmp_mem:
        push    ax
        push    cx
        push    si
        push    di
.loop:
        jcxz    .same
        mov     al, [si]
        cmp     al, [di]
        jne     .diff
        inc     si
        inc     di
        dec     cx
        jmp     .loop
.same:
        pop     di
        pop     si
        pop     cx
        pop     ax
        clc
        ret
.diff:
        pop     di
        pop     si
        pop     cx
        pop     ax
        stc
        ret

; cmp_z - DS:SI と DS:DI の ASCIZ を比べる。CF=0 なら同じ。
cmp_z:
        push    ax
        push    si
        push    di
.loop:
        mov     al, [si]
        cmp     al, [di]
        jne     .diff
        test    al, al
        jz      .same
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
msg_head:    db 13, 10, '=== MYDOS long file name (VFAT LFN) test ===', 13, 10, 13, 10, 0
msg_result:  db '### RESULT pass=', 0
msg_result2: db ' fail=', 0
msg_end:     db '###TEST-END###', 13, 10, 0

str_indent:  db '  ', 0
str_pass:    db '[PASS] ', 0
str_fail:    db '[FAIL] ', 0
str_step:    db '  (step=', 0
str_ax:      db ' ax=', 0

n_volinfo:   db 'AX=71A0h  the long name interface is there', 0
n_create:    db 'AH=3Ch    create a file whose name does not fit 8.3', 0
n_reopen:    db 'AH=3Dh    open it again by its long name', 0
n_shortname: db 'AH=3Dh    the generated 8.3 alias opens the same file', 0
n_find:      db 'AX=714Eh  the search returns the long name', 0
n_subdir:    db 'AH=39h/3Bh/3Ah  a directory with a long name', 0
n_delete:    db 'AH=41h    no orphaned fragments are left behind', 0

long1:       db 'a very long file name for testing.data', 0
short1:      db 'AVERYL~1.DAT', 0
long2:       db 'another long name entirely.txt', 0
longdir:     db 'a directory with a long name', 0
str_root:    db '\', 0

payload:     db 'long file names work'
payload_len  equ $ - payload

handle:      dw 0
search:      dw 0

test_name:   dw 0
pass_count:  dw 0
fail_count:  dw 0
step:        db 0
char_buf:    db 0
read_buf:    times 80 db 0
find_buf:    times 320 db 0
volinfo_buf: times 64 db 0

             times 512 db 0
stack_top:
