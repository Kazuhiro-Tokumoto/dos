; ============================================================================
; cds.inc  -  CDS (Current Directory Structure) — ドライブごとのカレントディレクトリ
;
; DOS は「いまどのディレクトリにいるか」をドライブごとに覚えている。
; A: のカレントが \DOS で C: のカレントが \TOOLS、という状態を同時に
; 保てるのはこの配列があるから。
;
; List of Lists の +16h がこの配列を指し、LASTDRIVE の数だけ並ぶ。
; 当時のファイラや SUBST/JOIN はここを直接読み書きする。
;
; カレントドライブぶんの情報 (cur_dir_clus / cur_dir_path) は、
; 毎回 CDS を引き直すのではなく作業用の変数に載せておき、
; 変わったときに cds_sync で書き戻す。ドライブを切り替えるときは
; cds_load で載せ替える。外から CDS を読んだときの内容は常に正しい。
; ============================================================================

; ---------------------------------------------------------------------------
; cds_init - CDS 配列を初期化する
;   入力: AL = ドライブの数 (LASTDRIVE)
; ---------------------------------------------------------------------------
cds_init:
        push    ax
        push    bx
        push    cx
        push    dx
        push    di
        push    es

        push    ds
        pop     es

        cmp     al, 1
        jae     .count_ok
        mov     al, 1
.count_ok:
        cmp     al, MAX_DRIVES
        jbe     .count_ok2
        mov     al, MAX_DRIVES
.count_ok2:
        mov     [lol_lastdrive], al
        movzx   cx, al

        ; 全体を 0 で埋める
        push    cx
        mov     di, cds_table
        mov     cx, MAX_DRIVES * CDS_ENTSIZE
        xor     al, al
        rep     stosb
        pop     cx

        ; ドライブごとに "X:\" を入れる
        xor     dx, dx                  ; DX = ドライブ番号
        mov     di, cds_table
.loop:
        mov     al, 'A'
        add     al, dl
        mov     [di + CDS_PATH], al
        mov     byte [di + CDS_PATH + 1], ':'
        mov     byte [di + CDS_PATH + 2], '\'
        mov     byte [di + CDS_PATH + 3], 0
        mov     word [di + CDS_ROOTLEN], 2      ; "X:" の直後がルート
        mov     word [di + CDS_CLUSTER], 0      ; ルート
        mov     word [di + CDS_UNKNOWN1], 0xFFFF

        ; 実在するドライブだけ「物理ドライブ」の印を付け、DPB に結ぶ。
        ; A: しか無いのに B: を名指しされたときに正しく断れるよう、
        ; 台数ではなくドライブ表を 1 台ずつ見る。
        push    ax
        push    di
        mov     al, dl
        call    drive_entry
        pop     di
        pop     ax
        jc      .not_present
        mov     word [di + CDS_FLAGS], CDSF_PHYSICAL
        push    ax
        mov     al, dl
        call    dpb_for_drive           ; BX = そのドライブの DPB
        mov     [di + CDS_DPB], bx
        mov     ax, cs
        mov     [di + CDS_DPB + 2], ax
        pop     ax
        jmp     .next
.not_present:
        mov     word [di + CDS_FLAGS], 0
.next:
        add     di, CDS_ENTSIZE
        inc     dx
        loop    .loop

        ; カレントドライブは起動したドライブ (disk_init が選んである)
        mov     al, [fs_drive]
        cmp     al, MAX_DRIVES
        jb      .have_cur
        xor     al, al
.have_cur:
        mov     [cur_drive], al
        call    cds_load

        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; cds_addr - ドライブ番号からその CDS の位置を求める
;   入力: AL = ドライブ番号 (0 = A:)
;   出力: CF=0 なら DI = CDS のオフセット、CF=1 なら範囲外
; ---------------------------------------------------------------------------
cds_addr:
        push    ax
        push    bx
        cmp     al, [lol_lastdrive]
        jae     .bad
        mov     bl, CDS_ENTSIZE
        mul     bl
        mov     di, cds_table
        add     di, ax
        pop     bx
        pop     ax
        clc
        ret
.bad:
        pop     bx
        pop     ax
        stc
        ret

; ---------------------------------------------------------------------------
; cds_current - カレントドライブの CDS を得る
;   出力: DI = CDS のオフセット
; ---------------------------------------------------------------------------
cds_current:
        push    ax
        mov     al, [cur_drive]
        call    cds_addr
        pop     ax
        ret

; ---------------------------------------------------------------------------
; cds_sync - 作業用の変数を CDS に書き戻す
;
; カレントディレクトリが変わったときに呼ぶ。これを忘れると、外から
; CDS を読んだツールに古い場所が見える。
; ---------------------------------------------------------------------------
cds_sync:
        push    ax
        push    cx
        push    si
        push    di
        push    es

        push    ds
        pop     es
        call    cds_current
        jc      .done

        mov     ax, [cur_dir_clus]
        mov     [di + CDS_CLUSTER], ax

        ; パスは "X:\" を付けた形で入れる
        mov     al, 'A'
        add     al, [cur_drive]
        mov     [di + CDS_PATH], al
        mov     byte [di + CDS_PATH + 1], ':'
        mov     byte [di + CDS_PATH + 2], '\'
        push    di
        add     di, CDS_PATH + 3
        mov     si, cur_dir_path
.copy:
        lodsb
        stosb
        test    al, al
        jnz     .copy
        pop     di
.done:
        pop     es
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; cds_load - CDS の内容を作業用の変数へ載せる (ドライブ切り替え時)
; ---------------------------------------------------------------------------
cds_load:
        push    ax
        push    cx
        push    si
        push    di
        push    es

        push    ds
        pop     es
        call    cds_current
        jc      .done

        mov     ax, [di + CDS_CLUSTER]
        mov     [cur_dir_clus], ax

        ; "X:\" の 3 文字を飛ばした残りが作業用のパス
        push    di
        mov     si, di
        add     si, CDS_PATH + 3
        mov     di, cur_dir_path
.copy:
        lodsb
        stosb
        test    al, al
        jnz     .copy
        pop     di
.done:
        pop     es
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; cds_set_drive - カレントドライブを変える
;   入力: AL = ドライブ番号
;   出力: CF=1 なら無効なドライブ
; ---------------------------------------------------------------------------
cds_set_drive:
        push    di
        cmp     al, [num_drives]
        jae     .bad
        call    cds_addr
        jc      .bad
        test    word [di + CDS_FLAGS], CDSF_PHYSICAL
        jz      .bad

        call    cds_sync                ; いまの場所を書き戻してから
        mov     [cur_drive], al
        call    cds_load                ; 新しいドライブの場所を載せる
        call    drive_select            ; ファイルシステムの諸元も載せ替える
        pop     di
        clc
        ret
.bad:
        pop     di
        stc
        ret

; --- 変数 ------------------------------------------------------------------
cur_drive:      db 0                    ; カレントドライブ (0 = A:)
num_drives:     db 1                    ; 実在するドライブの数
cds_table:      times MAX_DRIVES * CDS_ENTSIZE db 0
