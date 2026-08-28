; ============================================================================
; device.inc  -  デバイスドライバの連鎖と、組み込みドライバの本体
;
; DOS は入出力をすべて「デバイスドライバ」という同じ形の部品に通す。
; カーネルに埋め込まれたものも、CONFIG.SYS の DEVICE= で後から読み込んだ
; ものも、区別なく 1 本の連鎖にぶら下がる:
;
;   NUL → CON → AUX → PRN → CLOCK$ → ブロックデバイス(A:) → ...
;
; 連鎖の先頭 (NUL) は List of Lists の中に埋め込まれている。これも昔からの
; 約束で、当時のツールはここを起点にドライバを列挙する。
;
; 後から読み込んだドライバは NUL の直後に差し込まれる。名前で探すときは
; 先頭から順に見て最初に一致したものを使うので、あとから入れたほうが勝つ。
; ANSI.SYS が CON を乗っ取れるのはこの仕組みによる。
;
; 呼び出しは 2 段階:
;   STRATEGY  要求ヘッダの場所 (ES:BX) を受け取って覚えるだけ
;   INTERRUPT 覚えた場所を見て実際に処理する
; 大昔のマルチタスク構想の名残で、実際にはどのドライバも
; STRATEGY で覚えて INTERRUPT ですぐ処理する。
; ============================================================================

; ---------------------------------------------------------------------------
; dev_call - デバイスドライバを呼ぶ
;   入力: ES:SI = デバイスヘッダ、req_packet に要求が組み立ててあること
;   出力: AX = 要求ヘッダの status。CF=1 ならエラービットが立っていた
;
; ドライバは自分のセグメントで動くので far call で呼ぶ。デバイスの
; セグメントは連鎖を辿るときに DEV_NEXT の far ポインタから分かるので、
; ヘッダに独自の項目を足す必要はない (本物の 18 バイトのまま扱える)。
; ---------------------------------------------------------------------------
dev_call:
        push    bx
        push    cx
        push    dx
        push    di
        push    bp
        push    ds
        push    es

        mov     ax, [es:si + DEV_STRATEGY]
        mov     [cs:.entry], ax
        mov     [cs:.entry + 2], es
        mov     ax, [es:si + DEV_INTERRUPT]
        mov     [cs:.entry2], ax
        mov     [cs:.entry2 + 2], es

        push    cs
        pop     ds
        push    cs
        pop     es
        mov     bx, req_packet
        call    far [.entry]            ; STRATEGY: 場所を覚えさせる

        push    cs
        pop     ds
        call    far [.entry2]           ; INTERRUPT: 実行させる

        push    cs
        pop     ds
        mov     ax, [req_packet + REQ_STATUS]
        test    ax, DEVS_ERROR
        jnz     .error

        pop     es
        pop     ds
        pop     bp
        pop     di
        pop     dx
        pop     cx
        pop     bx
        clc
        ret
.error:
        pop     es
        pop     ds
        pop     bp
        pop     di
        pop     dx
        pop     cx
        pop     bx
        stc
        ret
.entry:  dd 0
.entry2: dd 0

; ---------------------------------------------------------------------------
; dev_char_io - 文字デバイスに読み書きさせる
;   入力: ES:SI = デバイスヘッダ, AL = DEVC_READ か DEVC_WRITE,
;         CX = バイト数, DX:DI = バッファ
;   出力: CF=0 なら AX = 実際に扱えたバイト数
; ---------------------------------------------------------------------------
dev_char_io:
        push    bx
        push    cx
        push    dx

        mov     byte [cs:req_packet + REQ_LENGTH], 22
        mov     byte [cs:req_packet + REQ_UNIT], 0
        mov     [cs:req_packet + REQ_CMD], al
        mov     word [cs:req_packet + REQ_STATUS], 0
        mov     [cs:req_packet + REQ_BUFFER], di
        mov     [cs:req_packet + REQ_BUFFER + 2], dx
        mov     [cs:req_packet + REQ_COUNT], cx

        call    dev_call
        jc      .fail
        mov     ax, [cs:req_packet + REQ_COUNT] ; 実際に扱えた数
        clc
        jmp     .out
.fail:
        xor     ax, ax
        stc
.out:
        pop     dx
        pop     cx
        pop     bx
        ret

; ---------------------------------------------------------------------------
; dev_block_io - ブロックデバイスに読み書きさせる
;   入力: ES:SI = デバイスヘッダ, AL = コマンド, BL = ユニット番号,
;         CX = セクタ数, DX = 開始セクタ, BP:DI = バッファ
;   出力: CF=1 なら失敗
; ---------------------------------------------------------------------------
dev_block_io:
        push    bx
        push    cx
        push    dx

        mov     byte [cs:req_packet + REQ_LENGTH], 30
        mov     [cs:req_packet + REQ_UNIT], bl
        mov     [cs:req_packet + REQ_CMD], al
        mov     word [cs:req_packet + REQ_STATUS], 0
        mov     [cs:req_packet + REQ_BUFFER], di
        mov     [cs:req_packet + REQ_BUFFER + 2], bp
        mov     [cs:req_packet + REQ_COUNT], cx
        mov     [cs:req_packet + REQ_START], dx
        mov     [cs:req_packet + REQ_STARTL], dx
        mov     word [cs:req_packet + REQ_STARTL + 2], 0

        call    dev_call

        pop     dx
        pop     cx
        pop     bx
        ret

; ---------------------------------------------------------------------------
; dev_find - 名前でデバイスを探す
;   入力: DS:SI = 8 バイトの名前 (空白詰め)
;   出力: CF=0 なら ES:DI = デバイスヘッダ
;
; 連鎖の先頭 (NUL) から順に見て、最初に一致したものを返す。あとから
; 差し込まれたドライバのほうが先に見つかるので、DEVICE= で入れた
; ANSI.SYS が組み込みの CON より優先される。
; ---------------------------------------------------------------------------
dev_find:
        push    ax
        push    bx
        push    cx
        push    si

        push    cs
        pop     es
        mov     di, dev_chain_head
.loop:
        test    word [es:di + DEV_ATTR], DEVA_CHAR
        jz      .next                   ; ブロックデバイスは名前を持たない

        push    si
        push    di
        add     di, DEV_NAME
        mov     cx, 8
.cmp:
        mov     al, [si]
        cmp     al, [es:di]
        jne     .no_match
        inc     si
        inc     di
        loop    .cmp
        pop     di
        pop     si
        jmp     .found
.no_match:
        pop     di
        pop     si
.next:
        mov     bx, [es:di + DEV_NEXT]
        mov     ax, [es:di + DEV_NEXT + 2]
        cmp     bx, 0xFFFF
        je      .not_found
        mov     es, ax
        mov     di, bx
        jmp     .loop

.not_found:
        pop     si
        pop     cx
        pop     bx
        pop     ax
        stc
        ret
.found:
        pop     si
        pop     cx
        pop     bx
        pop     ax
        clc
        ret

; ---------------------------------------------------------------------------
; dev_install - 読み込んだドライバを連鎖の先頭 (NUL の直後) に差し込む
;   入力: DX:AX = デバイスヘッダの far アドレス
; ---------------------------------------------------------------------------
dev_install:
        push    bx
        push    cx
        push    es

        ; いま NUL が指している先を、新しいドライバの次に付け替える
        mov     es, dx
        mov     bx, ax
        mov     cx, [cs:dev_chain_head + DEV_NEXT]
        mov     [es:bx + DEV_NEXT], cx
        mov     cx, [cs:dev_chain_head + DEV_NEXT + 2]
        mov     [es:bx + DEV_NEXT + 2], cx

        ; NUL の次を新しいドライバにする
        mov     [cs:dev_chain_head + DEV_NEXT], ax
        mov     [cs:dev_chain_head + DEV_NEXT + 2], dx

        pop     es
        pop     cx
        pop     bx
        ret

; ============================================================================
; 組み込みドライバ
;
; どれも「STRATEGY で req_ptr に覚え、INTERRUPT で処理する」形。
; カーネル内にあるので far ret で戻る点だけ外部ドライバと同じにしてある。
; ============================================================================

; --- 共通の STRATEGY: ES:BX を覚えるだけ ---
dev_strategy:
        mov     [cs:req_ptr], bx
        mov     [cs:req_ptr + 2], es
        retf

; --- 要求ヘッダを DS:BX で参照できるようにする (各 INTERRUPT の入口で使う) ---
%macro DEV_ENTER 0
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    ds
        push    es
        push    cs
        pop     ds
        les     bx, [req_ptr]           ; ES:BX = 要求ヘッダ
%endmacro

%macro DEV_LEAVE 0
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        retf
%endmacro

; ---------------------------------------------------------------------------
; NUL - 読めば即 EOF、書けば捨てる
; ---------------------------------------------------------------------------
nul_interrupt:
        DEV_ENTER
        mov     al, [es:bx + REQ_CMD]
        cmp     al, DEVC_READ
        jne     .not_read
        mov     word [es:bx + REQ_COUNT], 0     ; 1 バイトも読めない
.not_read:
        ; 書き込みは要求どおり全部受け取ったことにして捨てる
        mov     word [es:bx + REQ_STATUS], DEVS_DONE
        DEV_LEAVE

; ---------------------------------------------------------------------------
; CON - 画面とキーボード
; ---------------------------------------------------------------------------
con_interrupt:
        DEV_ENTER
        mov     al, [es:bx + REQ_CMD]

        cmp     al, DEVC_READ
        je      .read
        cmp     al, DEVC_WRITE
        je      .write
        cmp     al, DEVC_WRITEVFY
        je      .write
        cmp     al, DEVC_PEEK
        je      .peek
        cmp     al, DEVC_INSTATUS
        je      .instatus
        cmp     al, DEVC_INFLUSH
        je      .inflush
        cmp     al, DEVC_OUTSTATUS
        je      .ok                     ; 画面はいつでも書ける
        cmp     al, DEVC_OUTFLUSH
        je      .ok
        cmp     al, DEVC_INIT
        je      .ok
        cmp     al, DEVC_OPEN
        je      .ok
        cmp     al, DEVC_CLOSE
        je      .ok
        jmp     .unknown

.read:
        ; 要求されたバイト数だけキーを読む。DOS のハンドル 0 は行単位なので、
        ; CR まで来たらそこで打ち切る。行編集は上位 (con_read_block) の担当。
        mov     cx, [es:bx + REQ_COUNT]
        push    es
        push    bx
        les     di, [es:bx + REQ_BUFFER]
        xor     dx, dx                  ; DX = 実際に読んだ数
        jcxz    .read_done
.read_loop:
        call    con_in
        test    al, al
        jz      .read_loop              ; 拡張キーは読み飛ばす
        stosb
        inc     dx
        cmp     al, 13                  ; CR まで読んだら 1 行ぶん
        je      .read_done
        loop    .read_loop
.read_done:
        pop     bx
        pop     es
        mov     [es:bx + REQ_COUNT], dx
        jmp     .ok

.write:
        mov     cx, [es:bx + REQ_COUNT]
        push    ds
        push    si
        lds     si, [es:bx + REQ_BUFFER]
        jcxz    .write_done
.write_loop:
        lodsb
        call    con_out                 ; con_out は DS に依存しない
        loop    .write_loop
.write_done:
        pop     si
        pop     ds
        jmp     .ok

.peek:
        call    con_status
        jz      .peek_none
        call    con_peek                ; 消費せずに 1 文字覗く
        mov     [es:bx + REQ_MEDIA], al
        jmp     .ok
.peek_none:
        mov     word [es:bx + REQ_STATUS], DEVS_DONE | DEVS_BUSY
        DEV_LEAVE

.instatus:
        call    con_status
        jnz     .ok                     ; 入力あり → BUSY を立てない
        mov     word [es:bx + REQ_STATUS], DEVS_DONE | DEVS_BUSY
        DEV_LEAVE

.inflush:
        call    con_status
        jz      .ok
        call    con_in
        jmp     .inflush

.ok:
        mov     word [es:bx + REQ_STATUS], DEVS_DONE
        DEV_LEAVE
.unknown:
        mov     word [es:bx + REQ_STATUS], DEVS_DONE | DEVS_ERROR | DEVE_UNKNOWNCMD
        DEV_LEAVE

; ---------------------------------------------------------------------------
; AUX - シリアル。実機が無いときのために読みは即 EOF にしておく
; ---------------------------------------------------------------------------
aux_interrupt:
        DEV_ENTER
        mov     al, [es:bx + REQ_CMD]
        cmp     al, DEVC_READ
        jne     .not_read
        mov     word [es:bx + REQ_COUNT], 0
.not_read:
        mov     word [es:bx + REQ_STATUS], DEVS_DONE
        DEV_LEAVE

; ---------------------------------------------------------------------------
; PRN - プリンタ。書かれた内容は捨てるが、要求は成功で返す
; ---------------------------------------------------------------------------
prn_interrupt:
        DEV_ENTER
        mov     word [es:bx + REQ_STATUS], DEVS_DONE
        DEV_LEAVE

; ---------------------------------------------------------------------------
; CLOCK$ - 日時
;
; 読み書きするのは 6 バイトの決まった形:
;   +0 word  1980-01-01 からの日数
;   +2 byte  分
;   +3 byte  時
;   +4 byte  1/100 秒
;   +5 byte  秒
; DATE と TIME コマンドはこのドライバ越しに時計を読む。
; ---------------------------------------------------------------------------
clock_interrupt:
        DEV_ENTER
        mov     al, [es:bx + REQ_CMD]
        cmp     al, DEVC_READ
        je      .read
        cmp     al, DEVC_WRITE
        je      .ok                     ; 時計合わせは受け付けたことにする
        cmp     al, DEVC_WRITEVFY
        je      .ok
        cmp     al, DEVC_INIT
        je      .ok
        jmp     .unknown

.read:
        call    rtc_read
        les     di, [es:bx + REQ_BUFFER]
        call    days_since_1980
        stosw                           ; 日数
        mov     al, [rtc_min]
        stosb
        mov     al, [rtc_hour]
        stosb
        xor     al, al
        stosb                           ; 1/100 秒
        mov     al, [rtc_sec]
        stosb

        push    ds
        push    cs
        pop     ds
        les     bx, [req_ptr]
        pop     ds
        mov     word [es:bx + REQ_COUNT], 6
.ok:
        mov     word [es:bx + REQ_STATUS], DEVS_DONE
        DEV_LEAVE
.unknown:
        mov     word [es:bx + REQ_STATUS], DEVS_DONE | DEVS_ERROR | DEVE_UNKNOWNCMD
        DEV_LEAVE

; ---------------------------------------------------------------------------
; days_since_1980 - rtc_* の日付を 1980-01-01 からの日数に直す
;   出力: AX = 日数
; ---------------------------------------------------------------------------
days_since_1980:
        push    bx
        push    cx
        push    dx

        xor     ax, ax                  ; AX = 積み上げる日数
        mov     cx, 1980
.years:
        cmp     cx, [rtc_year]
        jae     .months
        push    ax
        mov     ax, cx
        call    is_leap_year            ; CF=1 ならうるう年
        pop     ax
        jc      .leap
        add     ax, 365
        jmp     .next_year
.leap:
        add     ax, 366
.next_year:
        inc     cx
        jmp     .years

.months:
        movzx   bx, byte [rtc_month]
        dec     bx
        jz      .days
        xor     cx, cx
.month_loop:
        cmp     cx, bx
        jae     .month_done
        push    bx
        mov     bx, cx
        movzx   dx, byte [month_days + bx]
        add     ax, dx
        cmp     bx, 1                   ; 2 月ならうるう年の分を足す
        jne     .no_feb
        push    ax
        mov     ax, [rtc_year]
        call    is_leap_year
        pop     ax
        jnc     .no_feb
        inc     ax
.no_feb:
        pop     bx
        inc     cx
        jmp     .month_loop
.month_done:

.days:
        movzx   dx, byte [rtc_day]
        add     ax, dx
        dec     ax                      ; 1 日は 0 日目

        pop     dx
        pop     cx
        pop     bx
        ret

; AX = 西暦 → CF=1 ならうるう年
is_leap_year:
        push    ax
        push    bx
        push    dx
        mov     bx, 4
        xor     dx, dx
        div     bx
        test    dx, dx
        jnz     .no
        pop     dx
        pop     bx
        pop     ax
        push    ax
        push    bx
        push    dx
        mov     bx, 100
        xor     dx, dx
        div     bx
        test    dx, dx
        jnz     .yes                    ; 100 で割れないなら閏年
        pop     dx
        pop     bx
        pop     ax
        push    ax
        push    bx
        push    dx
        mov     bx, 400
        xor     dx, dx
        div     bx
        test    dx, dx
        jz      .yes                    ; 400 で割れるなら閏年
.no:
        pop     dx
        pop     bx
        pop     ax
        clc
        ret
.yes:
        pop     dx
        pop     bx
        pop     ax
        stc
        ret

month_days:     db 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31

; ---------------------------------------------------------------------------
; ブロックデバイス - ディスク
;
; DOS のディスクアクセスはすべてここを通る。CONFIG.SYS で別のブロック
; デバイスを足したときも、同じ形で連鎖に加わる。
; ---------------------------------------------------------------------------
blk_interrupt:
        DEV_ENTER
        mov     al, [es:bx + REQ_CMD]

        cmp     al, DEVC_READ
        je      .read
        cmp     al, DEVC_WRITE
        je      .write
        cmp     al, DEVC_WRITEVFY
        je      .write
        cmp     al, DEVC_MEDIACHECK
        je      .mediacheck
        cmp     al, DEVC_BUILDBPB
        je      .buildbpb
        cmp     al, DEVC_REMOVABLE
        je      .removable
        cmp     al, DEVC_INIT
        je      .ok
        jmp     .unknown

.read:
        mov     al, [es:bx + REQ_UNIT]
        mov     [cs:blk_unit], al
        mov     cx, [es:bx + REQ_COUNT]
        mov     dx, [es:bx + REQ_START]
        push    es
        push    bx
        les     di, [es:bx + REQ_BUFFER]
        mov     bx, di
        movzx   eax, dx
        call    disk_read_unit
        pop     bx
        pop     es
        jc      .read_error
        jmp     .ok
.read_error:
        mov     word [es:bx + REQ_STATUS], DEVS_DONE | DEVS_ERROR | DEVE_READFAULT
        DEV_LEAVE

.write:
        mov     al, [es:bx + REQ_UNIT]
        mov     [cs:blk_unit], al
        mov     cx, [es:bx + REQ_COUNT]
        mov     dx, [es:bx + REQ_START]
        push    es
        push    bx
        les     di, [es:bx + REQ_BUFFER]
        mov     bx, di
        movzx   eax, dx
        call    disk_write_unit
        pop     bx
        pop     es
        jc      .write_error
        jmp     .ok
.write_error:
        mov     word [es:bx + REQ_STATUS], DEVS_DONE | DEVS_ERROR | DEVE_WRITEFAULT
        DEV_LEAVE

.mediacheck:
        ; 1 = メディアは変わっていない。フロッピーの入れ替えは見ていない。
        mov     byte [es:bx + REQ_MEDIA + 1], 1
        jmp     .ok

.buildbpb:
        ; そのユニットの BPB を組み立ててから、その位置を返す
        push    ax
        mov     al, [es:bx + REQ_UNIT]
        call    bpb_image_build
        pop     ax
        mov     word [es:bx + REQ_BUFFER], bpb_image
        mov     ax, cs
        mov     [es:bx + REQ_BUFFER + 2], ax
        jmp     .ok

.removable:
        ; ビジー状態で「取り外し可能」を表す (フロッピーなので可)
        mov     word [es:bx + REQ_STATUS], DEVS_DONE
        DEV_LEAVE

.ok:
        mov     word [es:bx + REQ_STATUS], DEVS_DONE
        DEV_LEAVE
.unknown:
        mov     word [es:bx + REQ_STATUS], DEVS_DONE | DEVS_ERROR | DEVE_UNKNOWNCMD
        DEV_LEAVE

; ============================================================================
; デバイスヘッダの並び
;
; NUL は List of Lists の中に埋め込まれているので、int21.inc の
; dos_lol の中で定義してある。ここはその続き。
;
; 各ヘッダはきっちり 18 バイト。デバイスのセグメントは、連鎖を辿るときに
; 直前のヘッダの DEV_NEXT (far ポインタ) から分かるので、独自の項目を
; 足す必要はない。
; ============================================================================

dev_con:
        dw      dev_aux, 0              ; 次 (セグメントは初期化時に入れる)
        dw      DEVA_CHAR | DEVA_STDIN | DEVA_STDOUT | DEVA_IOCTL
        dw      dev_strategy
        dw      con_interrupt
        db      'CON     '

dev_aux:
        dw      dev_prn, 0
        dw      DEVA_CHAR
        dw      dev_strategy
        dw      aux_interrupt
        db      'AUX     '

dev_prn:
        dw      dev_clock, 0
        dw      DEVA_CHAR | DEVA_OUTBUSY
        dw      dev_strategy
        dw      prn_interrupt
        db      'PRN     '

dev_clock:
        dw      dev_block, 0
        dw      DEVA_CHAR | DEVA_CLOCK
        dw      dev_strategy
        dw      clock_interrupt
        db      'CLOCK$  '

dev_block:
        dw      0xFFFF, 0xFFFF          ; 連鎖の最後
        dw      0                       ; ブロックデバイス (属性 0)
        dw      dev_strategy
        dw      blk_interrupt
        db      1                       ; ユニット数
        db      0, 0, 0, 0, 0, 0, 0

; ---------------------------------------------------------------------------
; dev_init - 連鎖の各ヘッダにセグメントを埋める
;
; アセンブル時にはカーネルがどのセグメントに載るか決まらない (再配置する)
; ので、起動時に自分の CS を書き込んで回る。
; ---------------------------------------------------------------------------
dev_init:
        push    ax
        push    si

        mov     ax, cs
        mov     si, dev_chain_head
.loop:
        cmp     word [si + DEV_NEXT], 0xFFFF
        je      .done
        mov     [si + DEV_NEXT + 2], ax         ; 次のデバイスのセグメント
        mov     si, [si + DEV_NEXT]
        jmp     .loop
.done:

        ; いま使う CON と CLOCK$ を控えておく (DEVICE= で差し替えられる)
        mov     word [dev_con_ptr], dev_con
        mov     [dev_con_ptr + 2], ax
        mov     word [dev_clock_ptr], dev_clock
        mov     [dev_clock_ptr + 2], ax

        pop     si
        pop     ax
        ret

; ============================================================================
; List of Lists  (AH=52h が ES:BX で返すもの)
;
; DOS の内部構造への入口。当時のメモリマネージャや常駐ソフトはここを起点に
; DPB 連鎖・SFT 連鎖・デバイス連鎖・CDS 配列を辿る。
;
; 手前 2 バイトに最初の MCB のセグメントが入っているのと、+22h に NUL の
; デバイスヘッダがそのまま埋め込まれているのが昔からの約束。NUL を
; 別の場所に置くと、連鎖を +22h から辿るツールが動かなくなる。
; ============================================================================
                dw 0                            ; -4: 予約
lol_first_mcb:  dw 0                            ; -2: 最初の MCB
dos_lol:
lol_first_dpb:  dd 0                            ; +00
lol_first_sft:  dd 0                            ; +04
lol_clock:      dd 0                            ; +08
lol_con:        dd 0                            ; +0C
                dw SECTOR_SIZE                  ; +10 最大セクタサイズ
lol_buffers:    dd 0                            ; +12 ディスクバッファ連鎖
lol_cds:        dd 0                            ; +16 CDS 配列
lol_fcbtable:   dd 0                            ; +1A
                dw 0                            ; +1E 保護される FCB 数
lol_numblock:   db 1                            ; +20 ブロックデバイスの数
lol_lastdrive:  db DEFAULT_LASTDRIVE            ; +21 LASTDRIVE

; +22h: NUL デバイスのヘッダ。ここが連鎖の先頭でもある。
dev_chain_head:
                dw dev_con, 0                   ; 次 (セグメントは dev_init が入れる)
                dw DEVA_CHAR | DEVA_NUL
                dw dev_strategy
                dw nul_interrupt
                db 'NUL     '
lol_numjoin:    db 0                            ; +34 JOIN されたドライブ数

; ---------------------------------------------------------------------------
; lol_init - List of Lists の各ポインタを実体に向ける
; ---------------------------------------------------------------------------
lol_init:
        push    ax

        ; DPB の連鎖の先頭。実在する最初のドライブのものを指す。
        push    bx
        push    cx
        xor     cl, cl
.find_dpb:
        mov     al, cl
        call    drive_entry
        jnc     .got_dpb
        inc     cl
        cmp     cl, MAX_DRIVES
        jb      .find_dpb
        mov     bx, drive_tab + DRV_DPB         ; 1 台も無いときの逃げ道
        jmp     .set_dpb
.got_dpb:
        mov     al, cl
        call    dpb_for_drive
.set_dpb:
        pop     cx
        mov     word [lol_first_dpb], bx
        pop     bx

        mov     ax, cs
        mov     [lol_first_dpb + 2], ax
        mov     word [lol_first_sft], sft_header
        mov     [lol_first_sft + 2], ax
        mov     word [lol_clock], dev_clock
        mov     [lol_clock + 2], ax
        mov     word [lol_con], dev_con
        mov     [lol_con + 2], ax
        mov     word [lol_buffers], buf_first
        mov     [lol_buffers + 2], ax
        mov     word [lol_cds], cds_table
        mov     [lol_cds + 2], ax
        mov     word [lol_fcbtable], sft_header
        mov     [lol_fcbtable + 2], ax

        pop     ax
        ret

; --- 変数 ------------------------------------------------------------------
req_ptr:        dd 0            ; STRATEGY が覚えた要求ヘッダの位置
req_packet:     times REQ_PACKET_SIZE db 0  ; カーネルが組み立てる要求ヘッダ
blk_unit:       db 0            ; ブロックデバイスの対象ユニット
dev_con_ptr:    dd 0            ; いま有効な CON デバイス (far)
dev_clock_ptr:  dd 0            ; いま有効な CLOCK$ デバイス (far)

; ブロックデバイスが BUILDBPB で返す BPB (25 バイト)
bpb_image:      times 25 db 0
