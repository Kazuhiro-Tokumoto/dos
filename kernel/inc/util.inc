; ============================================================================
; util.inc  -  文字列とファイル名まわりの小道具
;
; ここの関数はすべて DF=0 (前進) を前提にしている。INT 21h の入口で cld を
; 実行しているので、呼び出し元のプログラムが DF=1 のまま DOS を呼んでも
; 巻き込まれない。
; ============================================================================

; ---------------------------------------------------------------------------
; to_upper - AL を大文字にする
;
; DOS はファイル名を大文字に畳んで扱う。ここでは ASCII の a-z だけを対象に
; する。0x80 以上はシフト JIS の 1 バイト目でありうるので触らない。
; ---------------------------------------------------------------------------
to_upper:
        cmp     al, 'a'
        jb      .done
        cmp     al, 'z'
        ja      .done
        sub     al, 'a' - 'A'
.done:
        ret

; ---------------------------------------------------------------------------
; name_to_83 - "foo.txt" を 11 バイトの 8.3 形式 ("FOO     TXT") に変換する
;   入力: DS:SI = 名前 (0 / '\' / '/' で終わる), ES:DI = 11 バイトの書き込み先
;   出力: SI は区切り文字の位置を指す。DI は変化しない
;
; ワイルドカード '*' はそこから先を '?' で埋める。FindFirst がパターンを
; 8.3 に畳んでから比較する本物の挙動に合わせている ("*.TXT" が
; "????????TXT" になる)。
; ---------------------------------------------------------------------------
name_to_83:
        push    ax
        push    cx
        push    di

        ; まず 11 バイトすべてを空白で埋めておく
        push    di
        mov     al, ' '
        mov     cx, 11
        rep     stosb
        pop     di

        ; 先頭が '.' なら "." か ".." のディレクトリ指定
        cmp     byte [si], '.'
        jne     .base_start

        movsb
        cmp     byte [si], '.'
        jne     .skip_rest
        movsb
        jmp     .skip_rest

.base_start:
        mov     cx, 8
.base:
        mov     al, [si]
        test    al, al
        jz      .fill_base
        cmp     al, '\'
        je      .fill_base
        cmp     al, '/'
        je      .fill_base
        cmp     al, '.'
        je      .to_ext
        inc     si
        cmp     al, '*'
        je      .star_base
        jcxz    .base                   ; 8 文字を超えた分は捨てる
        call    to_upper
        stosb
        dec     cx
        jmp     .base

.star_base:
        jcxz    .base
        mov     al, '?'
        rep     stosb                   ; 残りの枠を '?' で埋める
        jmp     .base

.fill_base:
        ; 拡張子なしで終わった
        jmp     .done

.to_ext:
        inc     si                      ; '.' を飛ばす
        add     di, cx                  ; 基本名の余りは空白のまま飛ばす
        mov     cx, 3
.ext:
        mov     al, [si]
        test    al, al
        jz      .done
        cmp     al, '\'
        je      .done
        cmp     al, '/'
        je      .done
        inc     si
        cmp     al, '*'
        je      .star_ext
        jcxz    .ext
        call    to_upper
        stosb
        dec     cx
        jmp     .ext

.star_ext:
        jcxz    .ext
        mov     al, '?'
        rep     stosb
        jmp     .ext

.skip_rest:
        mov     al, [si]
        test    al, al
        jz      .done
        cmp     al, '\'
        je      .done
        cmp     al, '/'
        je      .done
        inc     si
        jmp     .skip_rest

.done:
        pop     di
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; name_from_83 - 11 バイトの 8.3 形式を "FOO.TXT" に戻す
;   入力: DS:SI = 11 バイト, ES:DI = 書き込み先 (最大 13 バイト)
;   出力: ES:DI の内容は 0 終端。DI は終端の 0 を指す。SI は保存される
; ---------------------------------------------------------------------------
name_from_83:
        push    ax
        push    cx
        push    si

        mov     cx, 8
.base:
        lodsb
        cmp     al, ' '
        je      .base_end
        stosb
        loop    .base
        jmp     .ext_start              ; 8 文字きっちり使っていた

.base_end:
        ; 空白に当たった。残りの基本名を読み飛ばして SI を +8 に揃える
        dec     cx
        add     si, cx

.ext_start:
        cmp     byte [si], ' '
        je      .terminate              ; 拡張子なし
        mov     al, '.'
        stosb
        mov     cx, 3
.ext:
        lodsb
        cmp     al, ' '
        je      .terminate
        stosb
        loop    .ext

.terminate:
        xor     al, al
        stosb
        dec     di                      ; 終端の 0 を指した状態で返す

        pop     si
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; name_match - 8.3 名がパターンに一致するか調べる
;   入力: DS:SI = パターン (11 バイト, '?' は任意 1 文字), ES:DI = 名前 (11 バイト)
;   出力: ZF=1 なら一致
; ---------------------------------------------------------------------------
name_match:
        push    ax
        push    cx
        push    si
        push    di

        mov     cx, 11
.loop:
        mov     al, [si]
        cmp     al, '?'
        je      .next                   ; '?' は何にでも一致する
        cmp     al, [es:di]
        jne     .no
.next:
        inc     si
        inc     di
        loop    .loop
        xor     al, al                  ; ZF=1
        jmp     .out
.no:
        or      al, 0xFF                ; ZF=0
.out:
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; strlen_z - 0 終端文字列の長さを数える
;   入力: DS:SI
;   出力: CX = 長さ。SI は保存される
; ---------------------------------------------------------------------------
strlen_z:
        push    ax
        push    si
        xor     cx, cx
.loop:
        lodsb
        test    al, al
        jz      .done
        inc     cx
        jmp     .loop
.done:
        pop     si
        pop     ax
        ret

; ---------------------------------------------------------------------------
; put_dec16 - AX を 10 進で表示する (カーネル内のメッセージ用)
; ---------------------------------------------------------------------------
put_dec16:
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
        call    con_out
        loop    .emit
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; put_hex16 - AX を 16 進 4 桁で表示する
; ---------------------------------------------------------------------------
put_hex16:
        push    ax
        push    cx
        push    dx
        mov     dx, ax
        mov     cx, 4
.loop:
        rol     dx, 4
        mov     al, dl
        and     al, 0x0F
        add     al, '0'
        cmp     al, '9'
        jbe     .emit
        add     al, 'A' - '0' - 10
.emit:
        call    con_out
        loop    .loop
        pop     dx
        pop     cx
        pop     ax
        ret
