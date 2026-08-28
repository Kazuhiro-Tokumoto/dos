; ============================================================================
; mydev.asm  -  インストール可能な文字デバイスドライバ (MYDEV)
;
; CONFIG.SYS に
;   DEVICE=\MYDEV.SYS hello
; と書くと組み込まれ、以降 "MYDEV" という名前でファイルのように開ける。
;
; DOS のデバイスドライバは、ヘッダが先頭に置かれただけのただのバイナリ。
; 実行ファイルではないので入口もリロケーションも無い。DOS は読み込んで
; INIT (コマンド 0) を一度呼び、ドライバが返した「常駐部分の末尾」まで
; メモリを残して、ヘッダを連鎖に差し込む。
;
; 呼び出しは 2 段階:
;   STRATEGY  要求ヘッダの場所 (ES:BX) を受け取って覚えるだけ
;   INTERRUPT 覚えた場所を見て実際に処理する
; どちらも far ret で戻る。
;
; このドライバの中身:
;   読むと決まった文字列を返し、書かれたバイト数を数えている。
;   IOCTL 読み取り (コマンド 3) で、その数と INIT に渡された引数を返す。
;   テストプログラムがそれを見て「本当に自分が呼ばれているか」を確かめる。
; ============================================================================
        cpu     386
        bits    16
        org     0                       ; .SYS はセグメントの先頭に載る

; --- 要求ヘッダのオフセット ---
REQ_LENGTH      equ 0x00
REQ_UNIT        equ 0x01
REQ_CMD         equ 0x02
REQ_STATUS      equ 0x03
REQ_MEDIA       equ 0x0D
REQ_BUFFER      equ 0x0E
REQ_COUNT       equ 0x12
REQ_INIT_UNITS  equ 0x0D
REQ_INIT_END    equ 0x0E
REQ_INIT_BPB    equ 0x12
REQ_INIT_DRIVE  equ 0x16

DEVS_DONE       equ 0x0100
DEVS_ERROR      equ 0x8000
DEVE_UNKNOWNCMD equ 0x03

DEVA_CHAR       equ 0x8000
DEVA_IOCTL      equ 0x4000

; ============================================================================
; デバイスヘッダ (18 バイト)
; ============================================================================
header:
        dw      0xFFFF, 0xFFFF          ; 次のドライバは無い
        dw      DEVA_CHAR | DEVA_IOCTL
        dw      strategy
        dw      interrupt
        db      'MYDEV   '              ; 8 文字。空白で埋める

; ============================================================================
; STRATEGY - 要求ヘッダの場所を覚えるだけ
; ============================================================================
strategy:
        mov     [cs:req_off], bx
        mov     [cs:req_seg], es
        retf

; ============================================================================
; INTERRUPT - 覚えた要求を処理する
; ============================================================================
interrupt:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    ds
        push    es
        pushf
        cld

        push    cs
        pop     ds
        les     bx, [req_ptr]           ; ES:BX = 要求ヘッダ

        mov     al, [es:bx + REQ_CMD]
        cmp     al, 0
        je      .init
        cmp     al, 3
        je      .ioctl_read
        cmp     al, 4
        je      .read
        cmp     al, 5
        je      .peek
        cmp     al, 6
        je      .instatus
        cmp     al, 7
        je      .ok                     ; 入力フラッシュ: 何もしない
        cmp     al, 8
        je      .write
        cmp     al, 9
        je      .write
        cmp     al, 10
        je      .ok                     ; 出力状態: いつでも空き
        cmp     al, 11
        je      .ok
        cmp     al, 13
        je      .ok                     ; オープン
        cmp     al, 14
        je      .ok                     ; クローズ
        jmp     .unknown

; --- INIT (0) ---------------------------------------------------------------
; DOS は +0Eh に「ここまで使ってよい」上限を、+12h に CONFIG.SYS の
; 引数の文字列を入れて渡してくる。こちらは +0Eh に常駐部分の末尾を返す。
.init:
        ; 引数を控えておく (テストプログラムが IOCTL で読みに来る)
        push    es
        push    bx
        lds     si, [es:bx + REQ_INIT_BPB]
        push    cs
        pop     es
        mov     di, init_args
        mov     cx, 24
.copy_args:
        lodsb
        test    al, al
        jz      .args_done
        stosb
        loop    .copy_args
.args_done:
        ; AH=09h は '$' で止まる。0 終端はその次に置く。
        mov     byte [es:di], '$'
        mov     byte [es:di + 1], 0
        push    cs
        pop     ds
        pop     bx
        pop     es

        ; 常駐部分の末尾 = このドライバの終わり
        mov     word [es:bx + REQ_INIT_END], resident_end
        mov     ax, cs
        mov     [es:bx + REQ_INIT_END + 2], ax
        mov     byte [es:bx + REQ_INIT_UNITS], 0

        ; 組み込まれたことを画面に出す (INIT の中では INT 21h の
        ; AH=01h..0Ch だけ使ってよい、というのが当時からの決まり)
        mov     dx, msg_hello
        mov     ah, 0x09
        int     0x21
        mov     dx, init_args
        mov     ah, 0x09
        int     0x21
        mov     dx, msg_crlf
        mov     ah, 0x09
        int     0x21
        jmp     .ok

; --- 読み取り (4) -----------------------------------------------------------
; 決まった文字列を、末尾まで行ったら先頭に戻りながら返す。
.read:
        mov     cx, [es:bx + REQ_COUNT]
        jcxz    .ok
        push    es
        push    bx
        les     di, [es:bx + REQ_BUFFER]
        mov     si, [read_pos]
.read_loop:
        cmp     si, payload_len
        jb      .read_have
        xor     si, si
.read_have:
        mov     al, [payload + si]
        stosb
        inc     si
        loop    .read_loop
        mov     [read_pos], si
        pop     bx
        pop     es
        jmp     .ok

; --- 破壊しない読み取り (5) -------------------------------------------------
.peek:
        mov     si, [read_pos]
        cmp     si, payload_len
        jb      .peek_have
        xor     si, si
.peek_have:
        mov     al, [payload + si]
        mov     [es:bx + REQ_MEDIA], al
        jmp     .ok

; --- 入力状態 (6) -----------------------------------------------------------
.instatus:
        ; いつでも読める (busy ビットは立てない)
        jmp     .ok

; --- 書き込み (8 / 9) -------------------------------------------------------
; 数えるだけ。最後の 1 バイトは覚えておく。
.write:
        mov     cx, [es:bx + REQ_COUNT]
        jcxz    .ok
        add     [write_count], cx
        push    es
        push    bx
        les     di, [es:bx + REQ_BUFFER]
        add     di, cx
        dec     di
        mov     al, [es:di]
        mov     [last_byte], al
        pop     bx
        pop     es
        jmp     .ok

; --- IOCTL 読み取り (3) -----------------------------------------------------
; 呼び出し側のバッファに 4 バイト返す:
;   +0 word  これまでに書き込まれたバイト数
;   +2 word  INIT に渡された引数の長さ
.ioctl_read:
        push    es
        push    bx
        les     di, [es:bx + REQ_BUFFER]
        mov     ax, [write_count]
        stosw
        mov     si, init_args
        xor     ax, ax
.arg_len:
        cmp     byte [si], '$'
        je      .arg_done
        inc     si
        inc     ax
        jmp     .arg_len
.arg_done:
        stosw
        pop     bx
        pop     es
        jmp     .ok

.unknown:
        mov     word [es:bx + REQ_STATUS], DEVS_DONE | DEVS_ERROR | DEVE_UNKNOWNCMD
        jmp     .leave
.ok:
        mov     word [es:bx + REQ_STATUS], DEVS_DONE
.leave:
        popf
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        retf

; --- 変数 ------------------------------------------------------------------
req_ptr:
req_off:        dw 0
req_seg:        dw 0

read_pos:       dw 0
write_count:    dw 0
last_byte:      db 0

payload:        db 'MYDEV says hello... '
payload_len     equ $ - payload

msg_hello:      db 'MYDEV.SYS installed, args=$'
msg_crlf:       db 13, 10, '$'
init_args:      times 26 db 0

resident_end:
