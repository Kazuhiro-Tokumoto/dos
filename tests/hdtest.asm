; ============================================================================
; hdtest.asm  -  複数ドライブ・ハードディスク・FAT16 を確かめる
;
; フロッピー (A:) から起動した状態で、MBR のパーティションテーブルから
; 生えたハードディスクのドライブ (C: と D:) を触る。
;
; ここで見たいのは 3 つ。
;   ・パーティションの先頭 LBA が足されているか
;     (足し忘れると MBR のあたりを読み書きしてディスクを壊す)
;   ・FAT16 のエントリをバッファ越しに正しく引けているか
;     (FAT12 の 12bit の取り出しをそのまま使うと 1 クラスタ目から壊れる)
;   ・クラスタ→LBA の計算が 32bit になっているか
;     (16bit のままだと 32MB を超えた途端に先頭付近を上書きする)
;
; 最後のものは、パーティションの後ろのほうにファイルを作って読み返せば
; 分かる。32bit で計算できていなければ、書いたはずの内容が返ってこない。
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

        call    t_present
        call    t_select
        call    t_fat16
        call    t_roundtrip
        call    t_far_cluster
        call    t_two_drives
        call    t_absolute

        ; 後始末: カレントドライブを A: に戻す
        mov     ah, 0x0E
        mov     dl, 0
        int     0x21

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
; 1. ドライブが生えているか
;
; AH=19h でカレントが A: であること、AH=0Eh が返す「ドライブの数」に
; C: と D: が含まれていることを見る。
; ============================================================================
t_present:
        mov     si, n_present
        call    begin

        mov     ah, 0x19                ; カレントドライブ
        int     0x21
        cmp     al, 0                   ; A:
        jne     fail

        mov     ah, 0x0E                ; ドライブ選択 (AL は今のまま)
        mov     dl, 0
        int     0x21
        cmp     al, 4                   ; A: B: C: D: の 4 台以上
        jb      fail
        jmp     pass

; ============================================================================
; 2. C: に切り替えられるか
; ============================================================================
t_select:
        mov     si, n_select
        call    begin

        mov     ah, 0x0E
        mov     dl, 2                   ; C:
        int     0x21
        mov     ah, 0x19
        int     0x21
        cmp     al, 2
        jne     fail

        ; カレントディレクトリはルート
        mov     ah, 0x47
        mov     dl, 0                   ; カレントドライブ
        mov     si, path_buf
        int     0x21
        jc      fail
        cmp     byte [path_buf], 0      ; ルートなら空文字列
        jne     fail
        jmp     pass

; ============================================================================
; 3. FAT16 として認識されているか
;
; AH=36h が返す総クラスタ数を見る。FAT12 の上限 (4085) を超えていれば
; FAT16 として扱えていることになる。クラスタあたりセクタ数と
; セクタあたりバイト数も、mformat が作ったとおりの値かを確かめる。
; ============================================================================
t_fat16:
        mov     si, n_fat16
        call    begin

        mov     ah, 0x36
        mov     dl, 3                   ; C: (1 = A:)
        int     0x21
        cmp     ax, 0xFFFF
        je      fail
        cmp     ax, 4                   ; 1 クラスタ = 4 セクタ
        jne     fail
        cmp     cx, 512                 ; 1 セクタ = 512 バイト
        jne     fail
        cmp     dx, 4085                ; FAT12 の上限を超えている
        jbe     fail
        test    bx, bx                  ; 空きクラスタがある
        jz      fail
        jmp     pass

; ============================================================================
; 4. C: でファイルを作って読み返す
;
; パーティションの先頭 LBA を足し忘れていると、ここでフロッピー側の
; レイアウトのつもりで書き込むことになり、読み返しは必ず食い違う。
; ============================================================================
t_roundtrip:
        mov     si, n_round
        call    begin

        mov     ah, 0x3C
        xor     cx, cx
        mov     dx, f_hdfile
        int     0x21
        jc      fail
        mov     [handle], ax

        mov     bx, ax
        mov     ah, 0x40
        mov     cx, payload_len
        mov     dx, payload
        int     0x21
        jc      fail
        cmp     ax, payload_len
        jne     fail

        mov     bx, [handle]
        mov     ah, 0x3E
        int     0x21

        ; 読み返す
        mov     ax, 0x3D00
        mov     dx, f_hdfile
        int     0x21
        jc      fail
        mov     [handle], ax
        mov     bx, ax
        mov     ah, 0x3F
        mov     cx, 128
        mov     dx, read_buf
        int     0x21
        jc      fail
        cmp     ax, payload_len
        jne     fail
        mov     bx, [handle]
        mov     ah, 0x3E
        int     0x21

        mov     si, payload
        mov     di, read_buf
        mov     cx, payload_len
        push    ds
        pop     es
        repe    cmpsb
        jne     fail
        jmp     pass

; ============================================================================
; 5. パーティションの後ろのほうまで届くか
;
; 32MB を超えた位置にデータが載るよう、大きなファイルを作ってから
; その末尾に書き込む。クラスタ→LBA が 16bit のままだと、この書き込みは
; パーティションの先頭付近へ回り込み、読み返した内容が食い違う。
;
; ファイル全体を書くと時間がかかるので、AH=42h で位置だけ飛ばしてから
; 末尾に書く。間は穴になるが、FAT の連鎖は最後まで伸びる。
; ============================================================================
t_far_cluster:
        mov     si, n_far
        call    begin

        ; C: に切り替わっていなければ、この試験はやらない。
        ; 36MB のファイルをフロッピーに作ろうとして中身を壊してしまう。
        mov     byte [step], 1
        mov     ah, 0x19
        int     0x21
        cmp     al, 2
        jne     failx

        mov     byte [step], 2
        mov     ah, 0x3C
        xor     cx, cx
        mov     dx, f_bigfile
        int     0x21
        jc      failx
        mov     [handle], ax

        ; 36MB の位置へ飛ぶ (32MB = 65536 セクタ の壁の向こう側)
        mov     byte [step], 3
        mov     bx, [handle]
        mov     ax, 0x4200
        mov     cx, 0x0240              ; CX:DX = 0x02400000 = 36MB
        xor     dx, dx
        int     0x21
        jc      failx
        cmp     dx, 0x0240              ; DX:AX に同じ位置が返るはず
        jne     failx
        test    ax, ax
        jnz     failx

        mov     byte [step], 4
        mov     bx, [handle]
        mov     ah, 0x40
        mov     cx, payload_len
        mov     dx, payload
        int     0x21
        jc      .closefail
        cmp     ax, payload_len
        jne     .closefail

        mov     bx, [handle]
        mov     ah, 0x3E
        int     0x21

        ; 読み返す
        mov     byte [step], 5
        mov     ax, 0x3D00
        mov     dx, f_bigfile
        int     0x21
        jc      failx
        mov     [handle], ax

        mov     byte [step], 6
        mov     bx, [handle]
        mov     ax, 0x4200
        mov     cx, 0x0240
        xor     dx, dx
        int     0x21
        jc      failx

        mov     byte [step], 7
        mov     bx, [handle]
        mov     ah, 0x3F
        mov     cx, 128
        mov     dx, read_buf
        int     0x21
        jc      .closefail
        cmp     ax, payload_len
        jne     .closefail
        mov     bx, [handle]
        mov     ah, 0x3E
        int     0x21

        mov     byte [step], 8
        mov     si, payload
        mov     di, read_buf
        mov     cx, payload_len
        push    ds
        pop     es
        repe    cmpsb
        jne     failx

        ; 使い終わったので消す (次の実行でも同じ条件になるように)
        mov     ah, 0x41
        mov     dx, f_bigfile
        int     0x21
        jmp     pass
.closefail:
        push    ax
        mov     bx, [handle]
        mov     ah, 0x3E
        int     0x21
        pop     ax
        jmp     failx

; ============================================================================
; 6. C: と D: を同時に扱えるか
;
; D: にファイルを作ってから、C: のファイルをドライブ名指しで開く。
; ドライブごとのレイアウトを取り違えていれば、どちらかが失敗する。
; ============================================================================
t_two_drives:
        mov     si, n_two
        call    begin

        mov     byte [step], 1
        mov     ah, 0x3C
        xor     cx, cx
        mov     dx, f_dfile             ; "D:\TWO.DAT"
        int     0x21
        jc      failx
        mov     [handle], ax

        mov     byte [step], 2
        mov     bx, ax
        mov     ah, 0x40
        mov     cx, payload_len
        mov     dx, payload
        int     0x21
        pushf
        mov     [read_len], ax
        mov     bx, [handle]
        mov     ah, 0x3E
        int     0x21
        popf
        jc      failx
        mov     ax, [read_len]
        cmp     ax, payload_len
        jne     failx

        ; カレントは C: のまま、D: のファイルが開けること
        mov     byte [step], 3
        mov     ax, 0x3D00
        mov     dx, f_dfile
        int     0x21
        jc      failx
        mov     [handle], ax

        mov     byte [step], 4
        mov     bx, ax
        mov     ah, 0x3F
        mov     cx, 128
        mov     dx, read_buf
        int     0x21
        pushf
        mov     [read_len], ax
        mov     bx, [handle]
        mov     ah, 0x3E
        int     0x21
        popf
        jc      failx
        mov     ax, [read_len]
        cmp     ax, payload_len
        jne     failx

        mov     byte [step], 5
        mov     si, payload
        mov     di, read_buf
        mov     cx, payload_len
        push    ds
        pop     es
        repe    cmpsb
        jne     failx

        ; 続けて C: のファイルも開ける
        mov     byte [step], 6
        mov     ax, 0x3D00
        mov     dx, f_hdfile            ; "C:\HDFILE.DAT"
        int     0x21
        jc      failx
        mov     bx, ax
        mov     ah, 0x3E
        int     0x21

        ; D: の空き容量も引ける
        mov     byte [step], 7
        mov     ah, 0x36
        mov     dl, 4                   ; D:
        int     0x21
        cmp     ax, 0xFFFF
        je      failx
        jmp     pass

; ============================================================================
; 7. AH=32h の DPB がパーティションの形を映しているか
;
; 当時のディスクツールはここを読む。ドライブ番号・メディアディスクリプタ・
; ルートディレクトリの開始セクタが、パーティションの BPB と揃っているか。
; ============================================================================
t_absolute:
        mov     si, n_dpb
        call    begin

        ; AH=32h は DS:BX で返す。DS が書き換わるので、拾ったら
        ; すぐ ES に移して自分の DS を戻す。ここを忘れると、以降の
        ; 文字列がカーネルのメモリから読まれて画面が化ける。
        push    ds
        mov     ah, 0x32
        mov     dl, 3                   ; C:
        int     0x21
        mov     cx, ds
        pop     ds
        mov     es, cx

        cmp     al, 0
        jne     fail

        cmp     byte [es:bx + DPB_DRIVE], 2     ; C:
        jne     fail
        cmp     word [es:bx + DPB_SECSIZE], 512
        jne     fail
        cmp     byte [es:bx + DPB_MEDIA], 0xF8  ; ハードディスク
        jne     fail
        cmp     byte [es:bx + DPB_NUMFATS], 2
        jne     fail
        ; ルートディレクトリは 予約セクタ + FAT 2 本 のすぐ後ろ
        mov     ax, [es:bx + DPB_RESERVED]
        mov     cx, [es:bx + DPB_SECPERFAT]
        add     ax, cx
        add     ax, cx
        cmp     ax, [es:bx + DPB_DIRSEC]
        jne     fail
        ; データ領域はルートの後ろ
        cmp     ax, [es:bx + DPB_DATASEC]
        jae     fail
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

; --- DPB のオフセット ---
DPB_DRIVE       equ 0x00
DPB_SECSIZE     equ 0x02
DPB_RESERVED    equ 0x06
DPB_NUMFATS     equ 0x08
DPB_DATASEC     equ 0x0B
DPB_SECPERFAT   equ 0x0F
DPB_DIRSEC      equ 0x11
DPB_MEDIA       equ 0x17

; ============================================================================
; データ
; ============================================================================
msg_head:    db 13, 10, '=== MYDOS hard disk / FAT16 / multi-drive test ===', 13, 10, 13, 10, 0
msg_result:  db '### RESULT pass=', 0
msg_result2: db ' fail=', 0
msg_end:     db '###TEST-END###', 13, 10, 0

str_indent:  db '  ', 0
str_pass:    db '[PASS] ', 0
str_fail:    db '[FAIL] ', 0
str_step:    db '  (step=', 0
str_ax:      db ' ax=', 0

n_present:   db 'C: and D: appeared from the partition table', 0
n_select:    db 'AH=0Eh/19h  switch the current drive to C:', 0
n_fat16:     db 'AH=36h  C: is FAT16 (more than 4085 clusters)', 0
n_round:     db 'file round trip on C: (partition offset applied)', 0
n_far:       db 'write past the 32MB mark (32-bit cluster to LBA)', 0
n_two:       db 'C: and D: are usable at the same time', 0
n_dpb:       db 'AH=32h  DPB of C: matches its BPB', 0

f_hdfile:    db 'C:\HDFILE.DAT', 0
f_bigfile:   db 'C:\BIGFILE.DAT', 0
f_dfile:     db 'D:\TWO.DAT', 0

payload:     db 'MYDOS phase D payload: partition + FAT16 + 32-bit LBA', 13, 10
payload_len  equ $ - payload

; --- 変数 ------------------------------------------------------------------
test_name:   dw 0
pass_count:  dw 0
fail_count:  dw 0
handle:      dw 0
step:        db 0
read_len:    dw 0
char_buf:    db 0
path_buf:    times 80 db 0
read_buf:    times 160 db 0

             times 512 db 0
stack_top:
