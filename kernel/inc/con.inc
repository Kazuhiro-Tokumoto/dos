; ============================================================================
; con.inc  -  CON デバイス (画面とキーボード)
;
; 出力は BIOS の TTY 出力 (INT 10h AH=0Eh)、入力は INT 16h を使う。
; 自前で VRAM を叩かないのは、そのほうが機種差を BIOS に吸収してもらえるため。
; 当時のプログラムが速度目当てに 0xB8000 を直接書くのは自由で、
; リアルモードの実 OS なのでこちらが何もしなくてもそのまま動く。
;
; SERIAL_CONSOLE を定義してアセンブルすると、画面に出す文字を COM1 にも
; 流す。QEMU を -serial stdio で回したときにホスト側でログが取れるので、
; 自動テストがこれ一つで成立する。互換性には影響しない (DOS の API から
; 見える挙動は何も変わらない) が、既定では無効にしてある。
; ============================================================================

; ---------------------------------------------------------------------------
; con_out - 1 文字表示する
;   入力: AL = 文字
; ---------------------------------------------------------------------------
con_out:
        push    ax
        push    bx
        push    cx
        push    dx

        cmp     al, 9                   ; TAB は次の 8 桁境界まで空白で埋める
        je      .tab

        cmp     al, 10                  ; LF は CR+LF に展開する
        jne     .plain
        push    ax
        mov     al, 13
        call    .bios
        pop     ax
.plain:
        call    .bios
        jmp     .done

.tab:
        ; カーソル位置を読んで、次の 8 の倍数まで空白を出す
        mov     ah, 0x03
        xor     bh, bh
        int     0x10                    ; DL = 桁, DH = 行
        mov     al, dl
        and     al, 7
        mov     cl, 8
        sub     cl, al                  ; 1..8 個の空白
        xor     ch, ch
.tab_loop:
        mov     al, ' '
        call    .bios
        loop    .tab_loop

.done:
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

.bios:
        push    ax
        push    bx
        mov     ah, 0x0E
        mov     bx, 0x0007
        int     0x10
        pop     bx
        pop     ax
%ifdef SERIAL_CONSOLE
        call    serial_out
%endif
        ret

; ---------------------------------------------------------------------------
; con_puts - 0 終端の文字列を表示する
;   入力: DS:SI = 文字列
; ---------------------------------------------------------------------------
con_puts:
        push    ax
        push    si
.loop:
        lodsb
        test    al, al
        jz      .done
        call    con_out
        jmp     .loop
.done:
        pop     si
        pop     ax
        ret

; ---------------------------------------------------------------------------
; con_in - キーを 1 つ待って読む (エコーなし)
;   出力: AL = ASCII (0 なら AH = 拡張キーのスキャンコード)
;
; 拡張キー (矢印など) は INT 16h が AL=0 で返してくる。DOS は次の
; 呼び出しでスキャンコードを返す約束なので、ここで 1 文字分だけ溜めておく。
; ---------------------------------------------------------------------------
con_in:
        push    bx

        mov     al, [ext_pending]
        test    al, al
        jz      .fresh
        mov     byte [ext_pending], 0   ; 溜めておいた拡張キーを返す
        jmp     .out

.fresh:
        xor     ah, ah
        int     0x16                    ; AH = スキャンコード, AL = ASCII
        test    al, al
        jnz     .out
        mov     [ext_pending], ah       ; 次回に返す
        xor     al, al
.out:
        pop     bx
        ret

; ---------------------------------------------------------------------------
; con_peek - 次のキーを消費せずに覗く
;   出力: AL = ASCII (押されていなければ 0)
;
; デバイスドライバの「破壊しない読み取り」(コマンド 5) 用。
; プログラムが「何か押されたか」を見てから読むかどうか決められる。
; ---------------------------------------------------------------------------
con_peek:
        mov     al, [ext_pending]
        test    al, al
        jnz     .have                   ; 溜めてある拡張キーがある
        push    ax
        mov     ah, 0x01
        int     0x16                    ; 覗くだけ (バッファからは消えない)
        jz      .none
        pop     ax
        mov     ah, 0x01
        int     0x16
        ret
.none:
        pop     ax
        xor     al, al
.have:
        ret

; ---------------------------------------------------------------------------
; con_status - キーが押されているか調べる
;   出力: ZF=0 (かつ AL=0xFF) なら入力あり、ZF=1 (AL=0) ならなし
; ---------------------------------------------------------------------------
con_status:
        cmp     byte [ext_pending], 0
        jne     .ready
        push    ax
        mov     ah, 0x01
        int     0x16
        jz      .empty
        pop     ax
.ready:
        mov     al, 0xFF
        or      al, al                  ; ZF=0 にする
        ret
.empty:
        pop     ax
        xor     al, al                  ; ZF=1
        ret

; ---------------------------------------------------------------------------
; con_readline - 行入力 (AH=0Ah 用)
;   入力: ES:DI = DOS のバッファ
;           [DI+0] = 最大文字数 (終端の CR を含む)
;           [DI+1] = 実際に読めた文字数 (出力)
;           [DI+2] 以降が文字列 (CR で終わる)
;
; BackSpace で 1 文字消せる。DOS の仕様どおり、最大文字数に達したあとは
; CR と BackSpace 以外を受け付けず、ベルを鳴らす。
; ---------------------------------------------------------------------------
con_readline:
        push    ax
        push    bx
        push    cx
        push    dx

        mov     bl, [es:di]             ; 最大文字数
        xor     bh, bh
        test    bl, bl
        jz      .done
        dec     bx                      ; CR の分を引く
        xor     cx, cx                  ; CX = 今の文字数

.loop:
        call    con_in
        test    al, al
        jz      .loop                   ; 拡張キーは無視

        cmp     al, 13                  ; CR で確定
        je      .enter
        cmp     al, 8                   ; BackSpace
        je      .backspace
        cmp     al, 3                   ; Ctrl-C
        je      .ctrlc
        cmp     al, ' '
        jb      .loop                   ; その他の制御文字は捨てる

        cmp     cx, bx
        jae     .bell                   ; もう入らない

        push    di
        add     di, 2
        add     di, cx
        mov     [es:di], al
        pop     di
        inc     cx
        call    con_out
        jmp     .loop

.backspace:
        test    cx, cx
        jz      .loop
        dec     cx
        mov     al, 8                   ; カーソルを戻して空白で潰して戻す
        call    con_out
        mov     al, ' '
        call    con_out
        mov     al, 8
        call    con_out
        jmp     .loop

.bell:
        mov     al, 7
        call    con_out
        jmp     .loop

.ctrlc:
        ; DOS は Ctrl-C を見つけると INT 23h を呼ぶ。既定のハンドラは
        ; プログラムを終了させる。
        mov     al, '^'
        call    con_out
        mov     al, 'C'
        call    con_out
        mov     al, 13
        call    con_out
        mov     al, 10
        call    con_out
        int     INT_CTRLC
        ; ハンドラが戻ってきたら入力をやり直す
        xor     cx, cx
        jmp     .loop

.enter:
        push    di
        add     di, 2
        add     di, cx
        mov     byte [es:di], 13
        pop     di
        mov     [es:di+1], cl
        mov     al, 13
        call    con_out
        mov     al, 10
        call    con_out

.done:
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

%ifdef SERIAL_CONSOLE
; ---------------------------------------------------------------------------
; シリアル出力 (テストビルド専用)
; ---------------------------------------------------------------------------
serial_init:
        push    ax
        push    dx
        mov     dx, SERIAL_BASE + 1
        xor     al, al
        out     dx, al
        mov     dx, SERIAL_BASE + 3
        mov     al, 0x80                ; DLAB
        out     dx, al
        mov     dx, SERIAL_BASE + 0
        mov     al, 1                   ; 115200 bps
        out     dx, al
        mov     dx, SERIAL_BASE + 1
        xor     al, al
        out     dx, al
        mov     dx, SERIAL_BASE + 3
        mov     al, 0x03                ; 8N1
        out     dx, al
        mov     dx, SERIAL_BASE + 2
        mov     al, 0xC7                ; FIFO
        out     dx, al
        mov     dx, SERIAL_BASE + 4
        mov     al, 0x0B                ; DTR/RTS/OUT2
        out     dx, al
        pop     dx
        pop     ax
        ret

serial_out:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     cx, ax
        mov     dx, SERIAL_BASE + 5
        mov     bx, 0xFFFF
.wait:
        in      al, dx
        test    al, 0x20                ; 送信バッファが空くのを待つ
        jnz     .send
        dec     bx
        jnz     .wait
        jmp     .out                    ; シリアルが無い環境では諦める
.send:
        mov     dx, SERIAL_BASE
        mov     ax, cx
        out     dx, al
.out:
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret
%endif

; --- 変数 ------------------------------------------------------------------
ext_pending:    db 0                    ; 溜めてある拡張キーのスキャンコード
