; ============================================================================
; time.inc  -  日時 (INT 1Ah 経由の RTC)
;
; DOS のディレクトリエントリに入る日時は 16bit に詰め込まれた独自形式で、
; 当時のツール (DIR の表示、アーカイバのタイムスタンプ) がこの形をそのまま
; 読む。ビット配置を間違えると「1980 年のファイル」だらけになる。
;
;   時刻: hhhhhmmm mmmsssss   秒は 2 秒単位 (0-29)
;   日付: yyyyyyym mmmddddd   年は 1980 からの経過年
;
; BIOS から返ってくる値は BCD なので、必ず 2 進に直してから使う。
; ============================================================================

; ---------------------------------------------------------------------------
; bcd_to_bin - BCD 1 バイトを 2 進に直す
;   入力: AL = BCD
;   出力: AL = 2 進 (CX は保存される)
; ---------------------------------------------------------------------------
bcd_to_bin:
        push    cx
        mov     cl, al
        and     cl, 0x0F                ; 下位桁
        shr     al, 4                   ; 上位桁
        mov     ch, 10
        mul     ch                      ; AX = 上位桁 * 10
        add     al, cl
        pop     cx
        ret

; ---------------------------------------------------------------------------
; rtc_read - RTC を 1 回だけ読んで、値を変数に落とす
;   出力: CF=1 なら時計が読めなかった
;
; 時・分・秒・年・月・日をそれぞれ別々に INT 1Ah で取りに行くと、
; その間に日付が変わって辻褄の合わない値になることがある。
; まとめて読んで保存する。
; ---------------------------------------------------------------------------
rtc_read:
        push    ax
        push    cx
        push    dx

        mov     ah, 0x02
        int     0x1A                    ; CH=時 CL=分 DH=秒 (BCD)
        jc      .fail
        mov     al, ch
        call    bcd_to_bin
        mov     [rtc_hour], al
        mov     al, cl
        call    bcd_to_bin
        mov     [rtc_min], al
        mov     al, dh
        call    bcd_to_bin
        mov     [rtc_sec], al

        mov     ah, 0x04
        int     0x1A                    ; CH=世紀 CL=年 DH=月 DL=日 (BCD)
        jc      .fail
        push    dx
        mov     al, ch
        call    bcd_to_bin
        mov     ah, 100
        mul     ah                      ; AX = 世紀 * 100
        mov     dx, ax
        mov     al, cl
        call    bcd_to_bin
        mov     ah, 0
        add     ax, dx
        mov     [rtc_year], ax
        pop     dx
        mov     al, dh
        call    bcd_to_bin
        mov     [rtc_month], al
        mov     al, dl
        call    bcd_to_bin
        mov     [rtc_day], al

        clc
        jmp     .out
.fail:
        ; 時計が使えない環境では 1980-01-01 00:00:00 にしておく
        mov     byte [rtc_hour], 0
        mov     byte [rtc_min], 0
        mov     byte [rtc_sec], 0
        mov     word [rtc_year], 1980
        mov     byte [rtc_month], 1
        mov     byte [rtc_day], 1
        stc
.out:
        pop     dx
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; get_dos_time - 現在時刻を DOS のディレクトリエントリ形式で返す
;   出力: AX = (時 << 11) | (分 << 5) | (秒 / 2)
; ---------------------------------------------------------------------------
get_dos_time:
        push    bx
        push    cx
        call    rtc_read

        movzx   bx, byte [rtc_hour]
        mov     cl, 11
        shl     bx, cl

        movzx   ax, byte [rtc_min]
        mov     cl, 5
        shl     ax, cl
        or      bx, ax

        movzx   ax, byte [rtc_sec]
        shr     ax, 1                   ; 秒は 2 秒単位
        or      bx, ax

        mov     ax, bx
        pop     cx
        pop     bx
        ret

; ---------------------------------------------------------------------------
; get_dos_date - 現在の日付を DOS のディレクトリエントリ形式で返す
;   出力: AX = ((年 - 1980) << 9) | (月 << 5) | 日
; ---------------------------------------------------------------------------
get_dos_date:
        push    bx
        push    cx
        call    rtc_read

        mov     bx, [rtc_year]
        sub     bx, 1980
        cmp     bx, 127
        jbe     .year_ok
        xor     bx, bx                  ; 範囲外は 1980 年にしておく
.year_ok:
        mov     cl, 9
        shl     bx, cl

        movzx   ax, byte [rtc_month]
        mov     cl, 5
        shl     ax, cl
        or      bx, ax

        movzx   ax, byte [rtc_day]
        or      bx, ax

        mov     ax, bx
        pop     cx
        pop     bx
        ret

; ---------------------------------------------------------------------------
; day_of_week - 曜日を求める (Sakamoto の方法)
;   入力: [rtc_year] / [rtc_month] / [rtc_day] が設定済みであること
;   出力: AL = 曜日 (0 = 日曜)
;
;   月ごとの補正値を引き、3 月より前は前年として数え、
;   (y + y/4 - y/100 + y/400 + t[m-1] + d) mod 7 を取る。
; ---------------------------------------------------------------------------
day_of_week:
        push    bx
        push    cx
        push    dx

        movzx   bx, byte [rtc_month]
        dec     bx
        cmp     bx, 12
        jb      .month_ok
        xor     bx, bx
.month_ok:
        movzx   cx, byte [dow_table + bx]        ; CX = 月の補正値

        mov     bx, [rtc_year]
        cmp     byte [rtc_month], 3
        jae     .no_shift
        dec     bx                      ; 1-2 月は前年として数える
.no_shift:

        mov     ax, bx                  ; AX = y
        push    bx
        shr     bx, 2
        add     ax, bx                  ; + y/4
        pop     bx

        push    ax
        mov     ax, bx
        xor     dx, dx
        push    bx
        mov     bx, 100
        div     bx                      ; AX = y/100
        pop     bx
        mov     dx, ax
        pop     ax
        sub     ax, dx                  ; - y/100

        push    ax
        mov     ax, bx
        xor     dx, dx
        push    bx
        mov     bx, 400
        div     bx                      ; AX = y/400
        pop     bx
        mov     dx, ax
        pop     ax
        add     ax, dx                  ; + y/400

        add     ax, cx                  ; + t[m-1]
        movzx   cx, byte [rtc_day]
        add     ax, cx                  ; + d

        xor     dx, dx
        mov     cx, 7
        div     cx                      ; DX = 余り
        mov     ax, dx

        pop     dx
        pop     cx
        pop     bx
        ret

dow_table:      db 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4

; --- 変数 ------------------------------------------------------------------
rtc_hour:       db 0
rtc_min:        db 0
rtc_sec:        db 0
rtc_month:      db 0
rtc_day:        db 0
rtc_year:       dw 1980
