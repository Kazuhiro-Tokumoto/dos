; ============================================================================
; xms.inc  -  A20、HMA、XMS (HIMEM.SYS 相当)
;
; 1MB の壁の話。8086 のアドレス線は 20 本しかないので、セグメント:オフセット
; が 1MB を超えると 0 番地へ回り込んだ。286 以降はアドレス線が増えたが、
; その回り込みを当てにしたプログラムが山ほどあったため、20 本目より上を
; 「A20 ゲート」で殺しておく互換動作が残った。
;
; ここを開けると、リアルモードのままでも FFFF:0010 から FFFF:FFFF までの
; 64KB−16 バイトに手が届く。これが HMA (High Memory Area)。DOS 5 以降の
; DOS=HIGH は、自分自身をそこへ移して常駐メモリを空ける。
;
; その上 (1MB+64KB 以降) がいわゆる拡張メモリ。リアルモードからは直接
; 触れないので、XMS という約束ごしにやり取りする。窓口は INT 2Fh AX=4300h
; で「あるか」を聞き、AX=4310h で入口の far アドレスを貰う形。
; 当時のプログラムはこの手順しか知らないので、そのとおりに用意する。
;
; 拡張メモリの読み書きには INT 15h AH=87h (BIOS のブロック転送) を使う。
; 286 の頃の HIMEM.SYS と同じやり方で、自前でプロテクトモードへ出入りする
; 必要がない。
; ============================================================================

XMS_VERSION     equ 0x0300      ; XMS 3.0
XMS_DRIVER_VER  equ 0x0001

HMA_SEG         equ 0xFFFF      ; HMA のセグメント (オフセット 10h から)
HMA_MIN_OFFSET  equ 0x0010
HMA_SIZE        equ 0xFFF0      ; 65520 バイト

XMS_MAX_HANDLES equ 32

; エラーコード (XMS の仕様書のもの)
XMSE_NOTIMPL    equ 0x80
XMSE_A20        equ 0x82
XMSE_HMANOTEXIST equ 0x90
XMSE_HMAINUSE   equ 0x91
XMSE_HMATOOSMALL equ 0x92
XMSE_HMANOTALLOC equ 0x93
XMSE_A20STILLON equ 0x94
XMSE_NOMEMLEFT  equ 0xA0
XMSE_NOHANDLES  equ 0xA1
XMSE_BADHANDLE  equ 0xA2
XMSE_BADSRC     equ 0xA3
XMSE_BADSRCOFF  equ 0xA4
XMSE_BADDEST    equ 0xA5
XMSE_BADDESTOFF equ 0xA6
XMSE_BADLENGTH  equ 0xA7
XMSE_BADOVERLAP equ 0xA8
XMSE_LOCKED     equ 0xAB
XMSE_NOUMB      equ 0xB1
XMSE_SMALLERUMB equ 0xB0

; ハンドル表の 1 エントリ
XH_USED         equ 0x00        ; byte: 0 = 空き
XH_LOCKS        equ 0x01        ; byte: ロック回数
XH_BASE         equ 0x02        ; dword: 先頭の物理アドレス
XH_SIZE         equ 0x06        ; dword: 大きさ (KB)
XH_ENTSIZE      equ 10

; ---------------------------------------------------------------------------
; a20_enable - A20 ゲートを開ける
;   出力: CF=0 なら開いた
;
; 開け方は 1 つではない。まず BIOS (INT 15h AX=2401h) に頼み、
; だめならキーボードコントローラ経由、それもだめなら「ファスト A20」
; (ポート 92h) を試す。当時のマシンの差がそのまま残っているところ。
; ---------------------------------------------------------------------------
a20_enable:
        push    ax
        call    a20_query
        jnz     .already

        ; (1) BIOS に頼む
        mov     ax, 0x2401
        int     0x15
        call    a20_query
        jnz     .done

        ; (2) キーボードコントローラ
        call    a20_kbd
        call    a20_query
        jnz     .done

        ; (3) ファスト A20 (ポート 92h)
        in      al, 0x92
        test    al, 2
        jnz     .fast_done              ; すでに立っている
        or      al, 2
        and     al, 0xFE                ; bit0 は「リセット」なので落とす
        out     0x92, al
.fast_done:
        call    a20_query
        jnz     .done

        pop     ax
        stc
        ret
.already:
.done:
        pop     ax
        clc
        ret

; ---------------------------------------------------------------------------
; a20_disable - A20 ゲートを閉じる
;
; ---------------------------------------------------------------------------
a20_disable:
        push    ax
        mov     ax, 0x2400
        int     0x15
        call    a20_query
        jz      .done

        call    a20_kbd_off
.done:
        pop     ax
        ret

; ---------------------------------------------------------------------------
; a20_query - A20 が開いているか調べる
;   出力: ZF=0 なら開いている
;
; 0000:0500 と FFFF:0510 は、A20 が閉じていれば同じ物理番地を指す
; (FFFF:0510 = 0x100500 で、20 本目のアドレス線が死んでいれば 0x000500 に
; 回り込む)。片方に 1234h、もう片方に 5678h を書いてから片方を読み直し、
; 1234h のままなら別々の番地 — つまり A20 は開いている。
; 元の値は必ず書き戻す。0500h には BIOS が何か置いていることがある。
; ---------------------------------------------------------------------------
a20_query:
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

        mov     cx, [si]                ; 元の値を控える
        mov     ax, [es:di]
        push    cx
        push    ax

        mov     word [si], 0x1234
        mov     word [es:di], 0x5678
        mov     cx, [si]

        pop     ax
        mov     [es:di], ax             ; 書き戻す
        pop     ax
        mov     [si], ax

        cmp     cx, 0x1234
        jne     .closed                 ; 5678h に化けた = 回り込んでいる
        mov     al, 1                   ; 開いている → ZF=0 で返す
        or      al, al
        jmp     .out
.closed:
        xor     al, al                  ; ZF=1
.out:
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; --- キーボードコントローラ経由で A20 を開ける ---
a20_kbd:
        push    ax
        call    a20_wait_in
        mov     al, 0xAD                ; キーボードを止める
        out     0x64, al
        call    a20_wait_in
        mov     al, 0xD0                ; 出力ポートを読む
        out     0x64, al
        call    a20_wait_out
        in      al, 0x60
        push    ax
        call    a20_wait_in
        mov     al, 0xD1                ; 出力ポートを書く
        out     0x64, al
        call    a20_wait_in
        pop     ax
        or      al, 2                   ; bit1 = A20
        or      al, 1                   ; bit0 は CPU のリセット線。0 を書くと
                                        ; 機械が再起動する。読めた値をそのまま
                                        ; 書き戻す作りだと、たまたま 0 が
                                        ; 返ってきた瞬間に落ちる。
        out     0x60, al
        call    a20_wait_in
        mov     al, 0xAE                ; キーボードを戻す
        out     0x64, al
        call    a20_wait_in
        pop     ax
        ret

a20_kbd_off:
        push    ax
        call    a20_wait_in
        mov     al, 0xD0
        out     0x64, al
        call    a20_wait_out
        in      al, 0x60
        push    ax
        call    a20_wait_in
        mov     al, 0xD1
        out     0x64, al
        call    a20_wait_in
        pop     ax
        and     al, ~2 & 0xFF
        or      al, 1                   ; bit0 (リセット線) は必ず立てておく
        out     0x60, al
        call    a20_wait_in
        pop     ax
        ret

; キーボードコントローラは遅い。コマンドを送る前にバッファが空くのを待つ。
a20_wait_in:
        push    ax
        push    cx
        mov     cx, 0xFFFF
.wi:
        in      al, 0x64
        test    al, 2                   ; 入力バッファが空くまで
        jz      .wi_done
        loop    .wi
.wi_done:
        pop     cx
        pop     ax
        ret
a20_wait_out:
        push    ax
        push    cx
        mov     cx, 0xFFFF
.wo:
        in      al, 0x64
        test    al, 1                   ; 出力バッファに何か来るまで
        jnz     .wo_done
        loop    .wo
.wo_done:
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; xms_init - 拡張メモリの大きさを調べて XMS の窓口を立てる
;
; INT 15h AH=88h が 1MB より上の KB 数を返す。そこから HMA の 64KB を
; 差し引いた残りを、XMS のブロックとして配る。
; ---------------------------------------------------------------------------
xms_init:
        push    ax
        push    bx
        push    cx
        push    dx
        push    di
        push    es

        ; ハンドル表を空にする
        push    ds
        pop     es
        mov     di, xms_handles
        mov     cx, XMS_MAX_HANDLES * XH_ENTSIZE
        xor     al, al
        rep     stosb

        mov     byte [xms_hma_used], 0
        mov     word [xms_a20_count], 0
        mov     dword [xms_total_kb], 0
        mov     byte [xms_present], 0

        ; A20 が開けないなら XMS は名乗らない
        call    a20_enable
        jc      .none

        ; --- 拡張メモリの大きさ ---
        ; AX=E801h が新しいほうの聞き方 (16MB までの KB 数と、その上の
        ; 64KB ブロック数を別々に返す)。古い BIOS には無いので、
        ; 答えが返らなければ AH=88h に落とす。
        push    bx
        push    cx
        push    dx
        xor     eax, eax
        mov     ax, 0xE801
        xor     bx, bx
        xor     cx, cx
        xor     dx, dx
        int     0x15
        jc      .try_88
        test    cx, cx
        jnz     .use_cx
        mov     cx, ax                  ; CX が 0 なら AX/BX に入っている
        mov     dx, bx
.use_cx:
        movzx   eax, cx                 ; 1MB〜16MB の KB 数
        movzx   ebx, dx
        shl     ebx, 6                  ; 64KB ブロック → KB
        add     eax, ebx
        test    eax, eax
        jnz     .got_size
.try_88:
        xor     eax, eax
        mov     ah, 0x88
        int     0x15
        movzx   eax, ax
.got_size:
        pop     dx
        pop     cx
        pop     bx

        cmp     eax, 128
        jbe     .none                   ; HMA を取ったら何も残らない
        sub     eax, 64                 ; HMA の 64KB は別扱い
        mov     [xms_total_kb], eax

        mov     byte [xms_present], 1
        mov     byte [xms_hma_exists], 1

        ; 拡張メモリの配り始めは 1MB + 64KB (HMA の直後)
        mov     dword [xms_base], 0x00110000
        mov     dword [xms_next], 0x00110000

        ; A20 は要求されるまで閉じておく (当時の作法)
        call    a20_disable
        jmp     .done
.none:
        mov     byte [xms_present], 0
        mov     byte [xms_hma_exists], 0
.done:
        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; xms_int2f - INT 2Fh AX=43xx (XMS の窓口)
;   AX=4300h  導入されているか → AL=80h なら「いる」
;   AX=4310h  入口の far アドレスを ES:BX で返す
; ---------------------------------------------------------------------------
xms_int2f:
        cmp     al, 0x00
        je      .installed
        cmp     al, 0x10
        je      .entry
        ret
.installed:
        cmp     byte [cs:xms_present], 0
        je      .no
        mov     al, 0x80
        ret
.no:
        xor     al, al
        ret
.entry:
        cmp     byte [cs:xms_present], 0
        je      .no_entry
        push    cs
        pop     es
        mov     bx, xms_entry
        ret
.no_entry:
        xor     bx, bx
        mov     es, bx
        ret

; ---------------------------------------------------------------------------
; xms_entry - XMS の本体。プログラムは far call で入ってくる。
;   AH = 機能番号。返り値は機能ごと。AX=0 が失敗で、そのとき BL にエラー。
; ---------------------------------------------------------------------------
xms_entry:
%ifdef XMS_TRACE
        pushf
        pusha
        push    ds
        mov     bl, ah                  ; 機能番号を控える
        push    cs
        pop     ds
        mov     al, 'X'
        call    serial_putc_dbg
        mov     al, bl
        shr     al, 4
        call    hexd_dbg
        mov     al, bl
        call    hexd_dbg
        mov     al, ' '
        call    serial_putc_dbg
        pop     ds
        popa
        popf
%endif
        cmp     ah, 0x00
        je      xms_f00
        cmp     ah, 0x01
        je      xms_f01
        cmp     ah, 0x02
        je      xms_f02
        cmp     ah, 0x03
        je      xms_f03
        cmp     ah, 0x04
        je      xms_f04
        cmp     ah, 0x05
        je      xms_f05
        cmp     ah, 0x06
        je      xms_f06
        cmp     ah, 0x07
        je      xms_f07
        cmp     ah, 0x08
        je      xms_f08
        cmp     ah, 0x09
        je      xms_f09
        cmp     ah, 0x0A
        je      xms_f0A
        cmp     ah, 0x0B
        je      xms_f0B
        cmp     ah, 0x0C
        je      xms_f0C
        cmp     ah, 0x0D
        je      xms_f0D
        cmp     ah, 0x0E
        je      xms_f0E
        cmp     ah, 0x0F
        je      xms_f0F
        cmp     ah, 0x10
        je      xms_f10
        cmp     ah, 0x11
        je      xms_f11
        cmp     ah, 0x88
        je      xms_f88
        cmp     ah, 0x89
        je      xms_f89
        cmp     ah, 0x8E
        je      xms_f8E
        cmp     ah, 0x8F
        je      xms_f8F
        ; 知らない機能
        xor     ax, ax
        mov     bl, XMSE_NOTIMPL
        retf

; --- 00h: バージョンを返す ---
;   AX = XMS のバージョン, BX = ドライバのバージョン, DX = HMA があれば 1
xms_f00:
        mov     ax, XMS_VERSION
        mov     bx, XMS_DRIVER_VER
        movzx   dx, byte [cs:xms_hma_exists]
        retf

; --- 01h: HMA を要求する ---
;   DX = 必要なバイト数 (FFFFh はドライバからの要求)
xms_f01:
        cmp     byte [cs:xms_hma_exists], 0
        je      .no_hma
        cmp     byte [cs:xms_hma_used], 0
        jne     .in_use
        ; DX=FFFFh は「全部使う」の意味 (ドライバからの要求)
        cmp     dx, 0xFFFF
        je      .take_all
        cmp     dx, HMA_SIZE
        ja      .too_small
.take_all:
        mov     byte [cs:xms_hma_used], 1
        call    a20_enable
        mov     ax, 1
        retf
.no_hma:
        xor     ax, ax
        mov     bl, XMSE_HMANOTEXIST
        retf
.in_use:
        xor     ax, ax
        mov     bl, XMSE_HMAINUSE
        retf
.too_small:
        xor     ax, ax
        mov     bl, XMSE_HMATOOSMALL
        retf

; --- 02h: HMA を返す ---
xms_f02:
        cmp     byte [cs:xms_hma_used], 0
        je      .not_alloc
        mov     byte [cs:xms_hma_used], 0
        mov     ax, 1
        retf
.not_alloc:
        xor     ax, ax
        mov     bl, XMSE_HMANOTALLOC
        retf

; --- 03h / 04h: A20 を開ける / 閉じる (グローバル) ---
xms_f03:
        call    a20_enable
        jc      .fail
        mov     ax, 1
        retf
.fail:
        xor     ax, ax
        mov     bl, XMSE_A20
        retf

xms_f04:
        cmp     word [cs:xms_a20_count], 0
        jne     .still_on               ; ローカルで開けている人がいる
        call    a20_disable
        mov     ax, 1
        retf
.still_on:
        xor     ax, ax
        mov     bl, XMSE_A20STILLON
        retf

; --- 05h / 06h: A20 を開ける / 閉じる (ローカル。回数を数える) ---
xms_f05:
        call    a20_enable
        jc      .fail
        inc     word [cs:xms_a20_count]
        mov     ax, 1
        retf
.fail:
        xor     ax, ax
        mov     bl, XMSE_A20
        retf

xms_f06:
        cmp     word [cs:xms_a20_count], 0
        je      .not_on
        dec     word [cs:xms_a20_count]
        cmp     word [cs:xms_a20_count], 0
        jne     .still
        call    a20_disable
.still:
        mov     ax, 1
        retf
.not_on:
        xor     ax, ax
        mov     bl, XMSE_A20STILLON
        retf

; --- 07h: A20 の状態 ---
xms_f07:
        call    a20_query
        jz      .off
        mov     ax, 1
        xor     bl, bl
        retf
.off:
        xor     ax, ax
        xor     bl, bl
        retf

; --- 08h: 空きを聞く ---
;   AX = 一番大きく取れるブロック (KB), DX = 全部の空き (KB)
xms_f08:
        push    cx
        push    si
        call    xms_free_kb             ; EAX = 空き KB
        cmp     eax, 0xFFFF
        jbe     .fits
        mov     eax, 0xFFFF
.fits:
        mov     dx, ax
        pop     si
        pop     cx
        test    ax, ax
        jnz     .ok
        mov     bl, XMSE_NOMEMLEFT
.ok:
        retf

; --- 09h: 拡張メモリを確保する ---
;   DX = KB → DX = ハンドル
xms_f09:
        push    cx
        push    si
        push    di

        test    dx, dx
        jz      .zero
        movzx   ecx, dx                 ; ECX = 欲しい KB

        call    xms_free_kb
        cmp     eax, ecx
        jb      .no_mem

        ; 空いているハンドルを探す
        call    xms_handle_alloc
        jc      .no_handles

        ; 割り当ては単純に後ろへ伸ばすだけ。DOS のプログラムは
        ; 「起動時に確保して終了時に解放」しか実質しないので、
        ; これで十分に用が足りる。
        mov     eax, [cs:xms_next]
        mov     [cs:si + XH_BASE], eax
        mov     [cs:si + XH_SIZE], ecx
        mov     byte [cs:si + XH_USED], 1
        mov     byte [cs:si + XH_LOCKS], 0

        push    eax
        mov     eax, ecx
        shl     eax, 10                 ; KB → バイト
        add     [cs:xms_next], eax
        pop     eax

        mov     dx, di                  ; DI = ハンドル番号
        mov     ax, 1
        pop     di
        pop     si
        pop     cx
        retf
.zero:
        xor     ax, ax
        mov     bl, XMSE_BADLENGTH
        pop     di
        pop     si
        pop     cx
        retf
.no_mem:
        xor     ax, ax
        mov     bl, XMSE_NOMEMLEFT
        pop     di
        pop     si
        pop     cx
        retf
.no_handles:
        xor     ax, ax
        mov     bl, XMSE_NOHANDLES
        pop     di
        pop     si
        pop     cx
        retf

; --- 0Ah: 拡張メモリを返す ---
xms_f0A:
        push    si
        mov     ax, dx
        call    xms_handle_addr         ; SI = エントリ
        jc      .bad
        cmp     byte [cs:si + XH_LOCKS], 0
        jne     .locked
        mov     byte [cs:si + XH_USED], 0
        mov     ax, 1
        pop     si
        retf
.bad:
        xor     ax, ax
        mov     bl, XMSE_BADHANDLE
        pop     si
        retf
.locked:
        xor     ax, ax
        mov     bl, XMSE_LOCKED
        pop     si
        retf

; --- 0Bh: ブロックを転送する ---
;   DS:SI = 転送を指示する構造体
;     +0  dword バイト数 (偶数)
;     +4  word  転送元ハンドル (0 なら通常メモリで +6 が far ポインタ)
;     +6  dword 転送元オフセット
;     +10 word  転送先ハンドル
;     +12 dword 転送先オフセット
xms_f0B:
        push    cx
        push    dx
        push    si
        push    di
        push    es
        push    ds

        push    cs
        pop     es                      ; ES = カーネル (作業用の変数)

        mov     eax, [si + 0]
        test    eax, 1
        jnz     .bad_len
        mov     [es:xms_move_len], eax
        test    eax, eax
        jz      .done_ok                ; 0 バイトは成功扱い

        ; 転送元の物理アドレスを求める
        mov     ax, [si + 4]
        mov     ecx, [si + 6]
        call    xms_linear
        jc      .bad_src
        mov     [es:xms_move_src], eax

        ; 転送先
        mov     ax, [si + 10]
        mov     ecx, [si + 12]
        call    xms_linear
        jc      .bad_dst
        mov     [es:xms_move_dst], eax

        push    cs
        pop     ds                      ; ここから先はカーネルの変数を使う
        call    xms_block_move
        jc      .move_fail

.done_ok:
        pop     ds
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        mov     ax, 1
        retf
.bad_len:
        mov     bl, XMSE_BADLENGTH
        jmp     .fail
.bad_src:
        mov     bl, XMSE_BADSRC
        jmp     .fail
.bad_dst:
        mov     bl, XMSE_BADDEST
        jmp     .fail
.move_fail:
        mov     bl, XMSE_BADLENGTH
.fail:
        pop     ds
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        xor     ax, ax
        retf

; --- 0Ch / 0Dh: ロック / 解除 ---
;   ロックすると、そのブロックの物理アドレスが DX:BX で返る。
xms_f0C:
        push    si
        mov     ax, dx
        call    xms_handle_addr
        jc      .bad
        inc     byte [cs:si + XH_LOCKS]
        mov     eax, [cs:si + XH_BASE]
        mov     bx, ax
        shr     eax, 16
        mov     dx, ax
        mov     ax, 1
        pop     si
        retf
.bad:
        xor     ax, ax
        mov     bl, XMSE_BADHANDLE
        pop     si
        retf

xms_f0D:
        push    si
        mov     ax, dx
        call    xms_handle_addr
        jc      .bad
        cmp     byte [cs:si + XH_LOCKS], 0
        je      .not_locked
        dec     byte [cs:si + XH_LOCKS]
        mov     ax, 1
        pop     si
        retf
.bad:
.not_locked:
        xor     ax, ax
        mov     bl, XMSE_BADHANDLE
        pop     si
        retf

; --- 0Eh: ハンドルの情報 ---
;   BH = ロック回数, BL = 空きハンドル数, DX = 大きさ (KB)
xms_f0E:
        push    si
        push    cx
        mov     ax, dx
        call    xms_handle_addr
        jc      .bad
        mov     bh, [cs:si + XH_LOCKS]
        mov     edx, [cs:si + XH_SIZE]
        push    dx
        call    xms_handle_free_count
        mov     bl, al
        pop     dx
        mov     ax, 1
        pop     cx
        pop     si
        retf
.bad:
        xor     ax, ax
        mov     bl, XMSE_BADHANDLE
        pop     cx
        pop     si
        retf

; --- 0Fh: 大きさを変える ---
;   縮めるだけ受け付ける。伸ばすのは後ろが空いているとは限らないので断る。
xms_f0F:
        push    si
        push    cx
        mov     ax, dx
        call    xms_handle_addr
        jc      .bad
        cmp     byte [cs:si + XH_LOCKS], 0
        jne     .locked
        movzx   ecx, bx
        cmp     ecx, [cs:si + XH_SIZE]
        ja      .no_mem
        mov     [cs:si + XH_SIZE], ecx
        mov     ax, 1
        pop     cx
        pop     si
        retf
.bad:
        xor     ax, ax
        mov     bl, XMSE_BADHANDLE
        pop     cx
        pop     si
        retf
.locked:
        xor     ax, ax
        mov     bl, XMSE_LOCKED
        pop     cx
        pop     si
        retf
.no_mem:
        xor     ax, ax
        mov     bl, XMSE_NOMEMLEFT
        pop     cx
        pop     si
        retf

; --- 10h / 11h: UMB の要求と返却 ---
;
; UMB は 640KB と 1MB の間の「穴」に RAM を貼って作る。貼るには
; V86 モードとページングが要り、それをやるのが EMM386。ここにはまだ
; それが無いので、正直に「無い」と答える。DOS=UMB だけ書いて EMM386 を
; 入れなかったときの本物の振る舞いと同じ。
xms_f10:
        xor     ax, ax
        mov     bl, XMSE_NOUMB
        xor     dx, dx
        retf

xms_f11:
        xor     ax, ax
        mov     bl, XMSE_NOUMB
        retf

; --- 88h: 32bit で空きを聞く (XMS 3.0) ---
xms_f88:
        push    cx
        push    si
        call    xms_free_kb
        mov     edx, eax                ; 全部の空き
        mov     ecx, [cs:xms_last_addr]
        pop     si
        pop     cx
        xor     bl, bl
        retf

; --- 89h: 拡張メモリを確保する (32bit 版) ---
;   EDX = 欲しい KB → DX = ハンドル
;
; 09h の 32bit 版。16bit の DX では 64MB までしか頼めないので、XMS 3.0 で
; 足された。DJGPP の DPMI ホスト (CWSDPMI) はこちらしか呼ばない。
; 実装していないと「確保できなかった」ではなく「そんな機能はない」が
; 返り、呼び出し側がそれを見落とすと、掴んでもいないメモリを自分の
; ものとして配り始める。実際 CWSDPMI は物理 0 番地から配り出して、
; DOS のカーネルを上書きした。
xms_f89:
        push    ecx
        push    si
        push    di

        mov     ecx, edx                ; ECX = 欲しい KB
        test    ecx, ecx
        jz      .zero

        call    xms_free_kb             ; EAX = 空き KB
        cmp     eax, ecx
        jb      .no_mem

        call    xms_handle_alloc
        jc      .no_handles

        mov     eax, [cs:xms_next]
        mov     [cs:si + XH_BASE], eax
        mov     [cs:si + XH_SIZE], ecx
        mov     byte [cs:si + XH_USED], 1
        mov     byte [cs:si + XH_LOCKS], 0

        mov     eax, ecx
        shl     eax, 10                 ; KB → バイト
        add     [cs:xms_next], eax

        mov     dx, di                  ; DI = ハンドル番号
        mov     ax, 1
        xor     bl, bl
        pop     di
        pop     si
        pop     ecx
        retf
.zero:
        xor     ax, ax
        mov     bl, XMSE_BADLENGTH
        pop     di
        pop     si
        pop     ecx
        retf
.no_mem:
        xor     ax, ax
        mov     bl, XMSE_NOMEMLEFT
        pop     di
        pop     si
        pop     ecx
        retf
.no_handles:
        xor     ax, ax
        mov     bl, XMSE_NOHANDLES
        pop     di
        pop     si
        pop     ecx
        retf

; --- 8Eh: ハンドルの情報を返す (32bit 版) ---
;   DX = ハンドル → BH = ロック回数, BL = 空きハンドル数, EDX = 大きさ KB
xms_f8E:
        push    si
        mov     ax, dx
        call    xms_handle_addr
        jc      .bad
        mov     bh, [cs:si + XH_LOCKS]
        push    bx
        call    xms_handle_free_count   ; AL = 空きハンドル数
        mov     ah, al
        pop     bx
        mov     bl, ah
        mov     edx, [cs:si + XH_SIZE]
        mov     ax, 1
        pop     si
        retf
.bad:
        xor     ax, ax
        mov     bl, XMSE_BADHANDLE
        pop     si
        retf

; --- 8Fh: 確保した大きさを変える (32bit 版) ---
;   EBX = 新しい大きさ KB, DX = ハンドル
;
; 09h/89h の割り当ては後ろへ伸ばすだけなので、縮めるのは常にでき、
; 伸ばせるのは「いちばん後ろに取ったブロック」のときだけ。
xms_f8F:
        push    ecx
        push    edx
        push    si

        mov     ax, dx
        call    xms_handle_addr
        jc      .bad

        mov     ecx, [cs:si + XH_SIZE]
        cmp     ebx, ecx
        je      .done                   ; 変わらない
        jb      .shrink

        ; 伸ばす: このブロックが末尾でなければ動かせない
        mov     eax, [cs:si + XH_BASE]
        mov     edx, ecx
        shl     edx, 10
        add     eax, edx
        cmp     eax, [cs:xms_next]
        jne     .no_mem

        mov     eax, ebx
        sub     eax, ecx                ; 増やす KB
        push    eax
        call    xms_free_kb
        pop     edx
        cmp     eax, edx
        jb      .no_mem
        mov     eax, edx
        shl     eax, 10
        add     [cs:xms_next], eax
        mov     [cs:si + XH_SIZE], ebx
        jmp     .done

.shrink:
        mov     eax, ecx
        sub     eax, ebx
        shl     eax, 10
        ; 末尾なら本当に返す
        push    eax
        mov     eax, [cs:si + XH_BASE]
        mov     edx, ecx
        shl     edx, 10
        add     eax, edx
        cmp     eax, [cs:xms_next]
        pop     eax
        jne     .just_note
        sub     [cs:xms_next], eax
.just_note:
        mov     [cs:si + XH_SIZE], ebx
.done:
        mov     ax, 1
        xor     bl, bl
        pop     si
        pop     edx
        pop     ecx
        retf
.no_mem:
        xor     ax, ax
        mov     bl, XMSE_NOMEMLEFT
        pop     si
        pop     edx
        pop     ecx
        retf
.bad:
        xor     ax, ax
        mov     bl, XMSE_BADHANDLE
        pop     si
        pop     edx
        pop     ecx
        retf

; ============================================================================
; 中で使う道具
; ============================================================================

; ---------------------------------------------------------------------------
; xms_handle_alloc - 空きハンドルを 1 つ取る
;   出力: CF=0 なら SI = エントリ、DI = ハンドル番号 (1 起算)
; ---------------------------------------------------------------------------
xms_handle_alloc:
        push    cx
        mov     si, xms_handles
        xor     di, di
.loop:
        inc     di
        cmp     byte [cs:si + XH_USED], 0
        je      .found
        add     si, XH_ENTSIZE
        cmp     di, XMS_MAX_HANDLES
        jb      .loop
        pop     cx
        stc
        ret
.found:
        pop     cx
        clc
        ret

; ---------------------------------------------------------------------------
; xms_handle_addr - ハンドル番号からエントリを求める
;   入力: AX = ハンドル番号 (1 起算)
;   出力: CF=0 なら SI = エントリ
; ---------------------------------------------------------------------------
xms_handle_addr:
        push    dx
        test    ax, ax
        jz      .bad
        cmp     ax, XMS_MAX_HANDLES
        ja      .bad
        push    ax
        dec     ax
        mov     dx, XH_ENTSIZE
        mul     dx
        mov     si, xms_handles
        add     si, ax
        pop     ax
        cmp     byte [cs:si + XH_USED], 0
        je      .bad
        pop     dx
        clc
        ret
.bad:
        pop     dx
        stc
        ret

; ---------------------------------------------------------------------------
; xms_handle_free_count - 空いているハンドルの数
;   出力: AL = 数
; ---------------------------------------------------------------------------
xms_handle_free_count:
        push    cx
        push    si
        xor     al, al
        mov     cx, XMS_MAX_HANDLES
        mov     si, xms_handles
.loop:
        cmp     byte [cs:si + XH_USED], 0
        jne     .next
        inc     al
.next:
        add     si, XH_ENTSIZE
        loop    .loop
        pop     si
        pop     cx
        ret

; ---------------------------------------------------------------------------
; xms_free_kb - まだ配っていない拡張メモリの KB 数
;   出力: EAX = KB
; ---------------------------------------------------------------------------
xms_free_kb:
        push    ecx
        mov     eax, [cs:xms_next]
        sub     eax, [cs:xms_base]
        shr     eax, 10                 ; 配った KB
        mov     ecx, [cs:xms_total_kb]
        cmp     eax, ecx
        jae     .none
        sub     ecx, eax
        mov     eax, ecx
        pop     ecx
        ret
.none:
        xor     eax, eax
        pop     ecx
        ret

; ---------------------------------------------------------------------------
; xms_linear - 転送の指定を物理アドレスに直す
;   入力: AX = ハンドル (0 なら通常メモリ), ECX = オフセットまたは far ポインタ
;   出力: CF=0 なら EAX = 物理アドレス
; ---------------------------------------------------------------------------
xms_linear:
        push    si
        push    dx
        test    ax, ax
        jnz     .handle

        ; 通常メモリ。ECX の上位がセグメント、下位がオフセット。
        mov     eax, ecx
        shr     eax, 16                 ; セグメント
        shl     eax, 4
        movzx   edx, cx
        add     eax, edx
        pop     dx
        pop     si
        clc
        ret
.handle:
        push    cx
        call    xms_handle_addr
        pop     cx
        jc      .bad
        mov     eax, [cs:si + XH_BASE]
        add     eax, ecx
        pop     dx
        pop     si
        clc
        ret
.bad:
        pop     dx
        pop     si
        stc
        ret

; ---------------------------------------------------------------------------
; xms_block_move - 物理アドレス同士でブロックを転送する
;   入力: [xms_move_src] / [xms_move_dst] / [xms_move_len]
;
; リアルモードのセグメントは 64KB までしか届かない。届かせる方法は 2 つ。
;
;   (1) BIOS の INT 15h AH=87h に頼む。286 の頃の HIMEM.SYS はこれだった。
;       ただし BIOS がプロテクトモードへ出入りするので、戻ってきたときの
;       機械の状態が読みにくい。
;   (2) 自分でいったんプロテクトモードに入り、4GB のリミットを
;       セグメントレジスタの隠し部分に焼き付けてからリアルモードへ戻る。
;       戻ったあともその隠しリミットは残るので、リアルモードのまま
;       32bit のオフセットで 1MB より上に手が届く。俗にアンリアルモード。
;       386 以降の HIMEM.SYS はこちらを使った。
;
; ここは (2)。焼き付ける先を FS と GS にしてあるのは、DS/ES/SS を
; 触らずに済ませるため。リアルモードでセグメントレジスタに値を入れ直すと
; 隠しリミットが 64KB に戻ってしまうので、転送の間はこの 2 本以外を
; 触らないようにしている。同じ理由で割り込みも止めておく
; (割り込みハンドラが FS/GS を作り直すと台無しになる)。
; ---------------------------------------------------------------------------
xms_block_move:
        pushad
        push    ds
        push    es

        ; A20 の状態を控えておく。転送のために開けるが、閉じたまま
        ; 使っていたプログラムにとっては、開きっぱなしで返されると
        ; 1MB を越えたところが回り込まなくなる。HIMEM.SYS も
        ; ブロック転送の前後で元に戻す。
        call    a20_query
        mov     [xms_a20_saved], al

        ; 1MB より上に触るので A20 は開けておく
        call    a20_enable
        jc      .fail

        ; GDT の位置を線形アドレスで書き込む (アセンブル時には決まらない)
        push    eax
        xor     eax, eax
        mov     ax, cs
        shl     eax, 4
        add     eax, flat_gdt
        mov     [flat_gdtr + 2], eax
        pop     eax

        cli
        ; いま入っている GDTR を控えてから自前のものに差し替える。
        ; 戻さずに帰ると、この先 BIOS や常駐ソフトがプロテクトモードへ
        ; 出入りするときに、自分が置いたつもりの GDT ではなく MYDOS の
        ; 2 エントリしかない表を見ることになる。
        sgdt    [saved_gdtr]
        lgdt    [flat_gdtr]

        mov     eax, cr0
        or      al, 1                   ; PE を立てる = プロテクトモード
        mov     cr0, eax
        jmp     short $+2               ; 先読みしたものを捨てる

        mov     bx, 0x08                ; フラットなデータセグメント
        mov     fs, bx
        mov     gs, bx

        mov     eax, cr0
        and     al, 0xFE                ; PE を倒す = リアルモードへ戻る
        mov     cr0, eax
        jmp     short $+2

        ; ここからリアルモード。ただし FS と GS の隠しリミットは 4GB のまま。
        mov     esi, [xms_move_src]
        mov     edi, [xms_move_dst]
        mov     ecx, [xms_move_len]

        mov     edx, ecx
        shr     ecx, 2                  ; まず 4 バイトずつ
        jecxz   .tail
.copy:
        mov     eax, [gs:esi]
        mov     [fs:edi], eax
        add     esi, 4
        add     edi, 4
        dec     ecx
        jnz     .copy
.tail:
        and     edx, 3                  ; 余りは 1 バイトずつ
        jz      .copied
.copy_tail:
        mov     al, [gs:esi]
        mov     [fs:edi], al
        inc     esi
        inc     edi
        dec     edx
        jnz     .copy_tail
.copied:
        ; FS / GS を元に戻す (隠しリミットも 64KB に戻る)
        xor     bx, bx
        mov     fs, bx
        mov     gs, bx
        lgdt    [saved_gdtr]            ; GDTR も元に戻す
        sti

        ; A20 も元の状態に戻す
        cmp     byte [xms_a20_saved], 0
        jne     .keep_a20
        call    a20_disable
.keep_a20:

        pop     es
        pop     ds
        popad
        clc
        ret
.fail:
        pop     es
        pop     ds
        popad
        stc
        ret

; --- アンリアルモードに入るための最小の GDT -------------------------------
; 0 番は使えない決まりなので空き。8 番だけ「基点 0、上限 4GB、読み書き
; できるデータ」を作る。ここに載せた形が、リアルモードへ戻ったあとも
; セグメントレジスタの隠し部分に残る。
                align 4
flat_gdt:
                dd 0, 0                 ; 0番: 空き
                dw 0xFFFF               ; 上限 (下位 16bit)
                dw 0x0000               ; 基点 (下位 16bit)
                db 0x00                 ; 基点 (bit16-23)
                db 0x92                 ; 読み書きできるデータ、特権 0
                db 0xCF                 ; G=1 D/B=1, 上限 (bit16-19)
                db 0x00                 ; 基点 (bit24-31)

flat_gdtr:
                dw 15                   ; GDT の大きさ - 1
                dd 0                    ; 位置 (起動時に埋める)

; 差し替える前の GDTR を控える場所
saved_gdtr:     dw 0
                dd 0
xms_a20_saved:  db 0                    ; 転送前の A20 の状態

; --- 変数 ------------------------------------------------------------------
xms_present:    db 0            ; XMS を名乗るか
xms_hma_exists: db 0            ; HMA が使えるか
xms_hma_used:   db 0            ; HMA を誰かが取っているか
xms_a20_count:  dw 0            ; ローカルで A20 を開けている数
xms_total_kb:   dd 0            ; 配れる拡張メモリ (KB)
xms_base:       dd 0x00110000   ; 配り始めの物理アドレス
xms_next:       dd 0x00110000   ; 次に配る物理アドレス
xms_last_addr:  dd 0
xms_move_src:   dd 0
xms_move_dst:   dd 0
xms_move_len:   dd 0
xms_handles:    times XMS_MAX_HANDLES * XH_ENTSIZE db 0

%ifdef XMS_TRACE
; --- 追跡用のシリアル出力 (XMS_TRACE を定義したときだけ) ---
serial_putc_dbg:
        push    ax
        push    dx
        mov     ah, al
.wait:
        mov     dx, 0x3FD
        in      al, dx
        test    al, 0x20
        jz      .wait
        mov     dx, 0x3F8
        mov     al, ah
        out     dx, al
        pop     dx
        pop     ax
        ret

hexd_dbg:
        push    ax
        and     al, 0x0F
        cmp     al, 10
        jb      .d
        add     al, 'A' - 10 - '0'
.d:     add     al, '0'
        call    serial_putc_dbg
        pop     ax
        ret
%endif
