; ============================================================================
; emm386.asm  -  EMS (LIM EMS 4.0) を提供する文字デバイスドライバ
;
; CONFIG.SYS に
;   DEVICE=\EMM386.SYS
; と書くと組み込まれ、INT 67h が生えて拡張メモリを EMS として配れるようになる。
;
; --- EMS とは何だったか ----------------------------------------------------
;
; 8086 が触れるのは 1MB まで。そのうち 640KB より上はビデオや ROM のために
; 予約されているので、プログラムが使えるのは 640KB しかない。これを越える
; ために作られたのが EMS で、考え方は banked memory そのもの。
;
;   ・1MB の中に 64KB の「窓」(ページフレーム) を 1 つ置く
;   ・窓は 16KB ずつ 4 枚に分かれている
;   ・その裏に何百枚もの 16KB のページを用意しておく
;   ・INT 67h AH=44h で「窓の N 枚目に、裏の M 枚目を出せ」と頼む
;
; プログラムから見ると、窓の中身が一瞬で差し替わる。物理的には EMS ボードの
; 中でアドレスデコーダを切り替えていた。
;
; --- 386 以降はどうやっていたか --------------------------------------------
;
; 386 が出ると、ボードを挿さずに同じことができるようになった。CPU を V86
; モードに落としてページングを有効にし、ページテーブルの書き換えで窓の
; 中身を差し替える。これをやるのが EMM386.EXE。
;
; MYDOS のこれは V86 モードを使わない。理由は 2 つある。
;
;   1. V86 モニタは DOS 本体と同じくらいの規模になる。割り込みの仮想化、
;      I/O の捕捉、例外の処理を全部書くことになり、EMS を提供するという
;      目的に対して見合わない。
;   2. ページングを使わずに 640KB より上に窓を置く方法が無い。チップセットの
;      シャドウ RAM を叩けば開く機械もあるが、機種ごとに全く違う
;      (当時のメモリマネージャが V86 を選んだのはまさにこれが理由)。
;      QEMU で調べたところ、C000-EFFF のうち書けるのは飛び飛びの 16KB だけで、
;      64KB 連続した窓は取れなかった。
;
; そこで、窓は通常メモリ (このドライバの直後) に置き、ページの実体は XMS に
; 置いて、AH=44h のたびに 16KB を読み書きしてすげ替える。EMS の API としては
; 完全に正しく動くが、
;
;   ・窓が 640KB の中にあるので、通常メモリを 64KB 消費する
;   ・ページの差し替えがアドレスデコーダの切り替えではなくコピーなので遅い
;
; という 2 点が本物と違う。プログラムは AH=41h で窓の場所を聞く決まりなので、
; 場所そのものは問題にならない。当時の「EMS エミュレータ」— EMS ボードの
; 無い機械で EMS を欲しがるソフトを動かすために配られていたもの —
; と同じ作りにしてある。
;
; --- 検出のされ方 ----------------------------------------------------------
;
; EMS があるかどうかは、"EMMXXXX0" という名前の文字デバイスが開けるかで
; 調べるのが正式な手順。INT 67h のベクタが指す先の +0Ah に置かれた 8 バイトを
; 直接見に行くプログラムも多いので、デバイスヘッダを割り込みベクタの
; 指す先に合わせてある (ヘッダの直後にハンドラを置くのではなく、
; ベクタをヘッダの先頭に向ける、という当時からの決まり)。
; ============================================================================
        cpu     386
        bits    16
        org     0

; --- 要求ヘッダ ---
REQ_CMD         equ 0x02
REQ_STATUS      equ 0x03
REQ_INIT_UNITS  equ 0x0D
REQ_INIT_END    equ 0x0E
REQ_INIT_BPB    equ 0x12

DEVS_DONE       equ 0x0100
DEVS_ERROR      equ 0x8000
DEVE_UNKNOWNCMD equ 0x03
DEVA_CHAR       equ 0x8000
DEVA_IOCTL      equ 0x4000

; --- EMS の諸元 ---
EMS_PAGE_SIZE   equ 16384               ; 1 ページ 16KB
EMS_PAGE_PARAS  equ EMS_PAGE_SIZE / 16  ; 0x400 段落
FRAME_PAGES     equ 4                   ; 窓は 16KB x 4 = 64KB
FRAME_PARAS     equ FRAME_PAGES * EMS_PAGE_PARAS
MAX_PAGES       equ 512                 ; 512 * 16KB = 8MB まで配れる
MAX_HANDLES     equ 64

; --- EMS のエラーコード ---
EMSE_OK         equ 0x00
EMSE_SOFTWARE   equ 0x80
EMSE_HARDWARE   equ 0x81
EMSE_BADHANDLE  equ 0x83
EMSE_BADFUNC    equ 0x84
EMSE_NOHANDLES  equ 0x85
EMSE_SAVEFULL   equ 0x86
EMSE_TOOFEW     equ 0x87                ; 総ページ数が足りない
EMSE_NOTENOUGH  equ 0x88                ; 空きページが足りない
EMSE_ZEROPAGES  equ 0x89
EMSE_BADLOGICAL equ 0x8A
EMSE_BADPHYS    equ 0x8B
EMSE_NOSAVED    equ 0x8C
EMSE_HAVESAVED  equ 0x8D
EMSE_NOSAVEDCTX equ 0x8E

; ============================================================================
; デバイスヘッダ (18 バイト)
;
; 名前は "EMMXXXX0"。EMS があるかどうかを調べるプログラムは、この名前で
; ファイルを開こうとするか、INT 67h のベクタが指すセグメントの +0Ah に
; この 8 文字が並んでいるかを見る。後者のために、INIT でベクタを
; 「このヘッダの先頭」ではなく「ヘッダの載っているセグメントの 0」に
; 向ける必要がある。ドライバはセグメントの先頭に読み込まれるので、
; org 0 のままヘッダを先頭に置けば条件を満たす。
; ============================================================================
header:
        dw      0xFFFF, 0xFFFF          ; 次のドライバは無い
        dw      DEVA_CHAR | DEVA_IOCTL
        dw      strategy
        dw      interrupt
        db      'EMMXXXX0'              ; 8 文字ちょうど

; ============================================================================
; STRATEGY / INTERRUPT
; ============================================================================
strategy:
        mov     [cs:req_off], bx
        mov     [cs:req_seg], es
        retf

interrupt:
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

        mov     es, [req_seg]
        mov     bx, [req_off]
        mov     al, [es:bx + REQ_CMD]

        test    al, al
        jz      .init
        cmp     al, 4                   ; 読み込み
        je      .nothing
        cmp     al, 8                   ; 書き込み
        je      .nothing
        cmp     al, 9
        je      .nothing
        cmp     al, 5                   ; 破壊しない読み取り
        je      .nothing
        cmp     al, 6                   ; 入力状態
        je      .nothing
        cmp     al, 7                   ; 入力バッファを捨てる
        je      .nothing
        cmp     al, 10                  ; 出力状態
        je      .nothing
        jmp     .unknown

; デバイスとして読み書きされても何もしない。EMS の入口は INT 67h であって、
; このデバイスは「EMS が居る」という目印としてだけ存在する。
.nothing:
        jmp     .ok

.unknown:
        mov     word [es:bx + REQ_STATUS], DEVS_DONE | DEVS_ERROR | DEVE_UNKNOWNCMD
        jmp     .out
.ok:
        mov     word [es:bx + REQ_STATUS], DEVS_DONE
.out:
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        retf

; ---------------------------------------------------------------------------
; INIT
; ---------------------------------------------------------------------------
.init:
        ; ems_init は INT 2Fh AX=4310h を呼ぶ。あれは ES:BX に XMS の入口を
        ; 返すので、要求ヘッダの位置がそのまま消える。控えてから呼ぶこと。
        ; ここを忘れると、INIT の結果を書き戻すつもりでカーネルの中を
        ; 書き潰す。ドライバは「組み込めた」と表示したあとで、次に
        ; プログラムを起動しようとした時点で全部おかしくなる。
        push    es
        push    bx
        call    ems_init
        pop     bx
        pop     es
        jc      .init_fail

        ; 常駐部分の末尾 = 窓の終わり
        mov     ax, [frame_seg]
        add     ax, FRAME_PARAS
        mov     word [es:bx + REQ_INIT_END], 0
        mov     [es:bx + REQ_INIT_END + 2], ax
        mov     byte [es:bx + REQ_INIT_UNITS], 0

        push    es
        push    bx
        mov     dx, msg_ok
        mov     ah, 0x09
        int     0x21
        pop     bx
        pop     es
        jmp     .ok

        ; 使えないときは 1 バイトも残さない。DOS はドライバを捨てる。
.init_fail:
        push    es
        push    bx
        mov     dx, [fail_msg]
        mov     ah, 0x09
        int     0x21
        pop     bx
        pop     es
        mov     word [es:bx + REQ_INIT_END], 0
        mov     ax, cs
        mov     [es:bx + REQ_INIT_END + 2], ax
        mov     byte [es:bx + REQ_INIT_UNITS], 0
        jmp     .ok

; ============================================================================
; ems_init - 窓と裏のページを用意し、INT 67h を乗っ取る
;   出力: CF=1 なら使えない
; ============================================================================
ems_init:
        ; --- XMS が居るか ---
        mov     word [fail_msg], msg_no_xms
        mov     ax, 0x4300
        int     0x2F
        cmp     al, 0x80
        jne     .fail
        mov     ax, 0x4310
        int     0x2F
        mov     [xms_entry], bx
        mov     [xms_entry + 2], es

        ; --- 窓の場所を決める ---
        ; ドライバの直後。ただし 16KB 境界に揃える。EMS の窓は 16KB 単位で
        ; 区切られているので、境界に載っていないと AH=41h で返した
        ; セグメントから 16KB ずつ数えた位置がページの頭にならない。
        mov     ax, resident_end + 15
        mov     cl, 4
        shr     ax, cl
        mov     dx, cs
        add     ax, dx
        add     ax, EMS_PAGE_PARAS - 1
        and     ax, ~(EMS_PAGE_PARAS - 1) & 0xFFFF
        mov     [frame_seg], ax

        ; --- 裏のページを XMS から取る ---
        mov     word [fail_msg], msg_no_mem
        mov     ah, 0x08                ; 一番大きく取れる塊 (KB)
        call    far [xms_entry]
        test    ax, ax
        jz      .fail

        ; ページ 1 枚 16KB。取れるだけ取るが MAX_PAGES で頭打ち。
        ; 全部取ってしまうと DPMI ホストなど他の利用者に何も残らないので、
        ; 半分だけもらう (EMM386 の NOEMS/RAM 指定に近い加減)。
        shr     ax, 1
        mov     cl, 4
        shr     ax, cl                  ; KB → 16KB 単位のページ数
        test    ax, ax
        jz      .fail
        cmp     ax, MAX_PAGES
        jbe     .cap_ok
        mov     ax, MAX_PAGES
.cap_ok:
        mov     [total_pages], ax
        mov     [free_pages], ax

        mov     dx, ax                  ; DX = KB 数
        mov     cl, 4
        shl     dx, cl
        mov     ah, 0x09                ; 拡張メモリを確保
        call    far [xms_entry]
        cmp     ax, 1
        jne     .fail
        mov     [xms_handle], dx

        ; --- 表を初期化する ---
        push    es
        push    cs
        pop     es
        mov     di, page_owner
        mov     cx, MAX_PAGES
        xor     ax, ax
        rep     stosw                   ; 0 = 空き
        mov     di, h_count
        mov     cx, MAX_HANDLES * 2
        rep     stosw                   ; h_count と h_base をまとめて 0
        mov     di, map_page
        mov     cx, FRAME_PAGES
        mov     ax, 0xFFFF
        rep     stosw                   ; どの窓にも何も出ていない
        push    cs
        pop     es
        mov     di, saved_valid
        mov     cx, MAX_HANDLES
        xor     al, al
        rep     stosb
        pop     es

        ; --- INT 67h を乗っ取る ---
        ; ベクタはこのセグメントの 0 番地 — つまりデバイスヘッダの先頭 —
        ; ではなく、ハンドラを指す。ヘッダの 8 文字を見に来るプログラムは
        ; ベクタのセグメント側だけを使うので、これで両方成り立つ。
        push    ds
        mov     dx, int67
        mov     ax, 0x2567
        int     0x21
        pop     ds

        ; 表示用にページ数を十進に直しておく
        mov     ax, [total_pages]
        mov     cl, 4
        shl     ax, cl                  ; ページ数 → KB
        mov     di, msg_kb
        call    put_dec

        clc
        ret
.fail:
        stc
        ret

; ============================================================================
; INT 67h - EMS の窓口
; ============================================================================
int67:
        sti
        cmp     ah, 0x40
        je      f_40
        cmp     ah, 0x41
        je      f_41
        cmp     ah, 0x42
        je      f_42
        cmp     ah, 0x43
        je      f_43
        cmp     ah, 0x44
        je      f_44
        cmp     ah, 0x45
        je      f_45
        cmp     ah, 0x46
        je      f_46
        cmp     ah, 0x47
        je      f_47
        cmp     ah, 0x48
        je      f_48
        cmp     ah, 0x4B
        je      f_4B
        cmp     ah, 0x4C
        je      f_4C
        cmp     ah, 0x4D
        je      f_4D
        cmp     ah, 0x4E
        je      f_4E
        cmp     ah, 0x51
        je      f_51
        cmp     ah, 0x58
        je      f_58
        cmp     ah, 0x59
        je      f_59
        mov     ah, EMSE_BADFUNC
        iret

; --- 40h: 状態を返す -------------------------------------------------------
f_40:
        mov     ah, EMSE_OK
        iret

; --- 41h: 窓の場所を返す ---------------------------------------------------
f_41:
        mov     bx, [cs:frame_seg]
        mov     ah, EMSE_OK
        iret

; --- 42h: 総ページ数と空きページ数 -----------------------------------------
f_42:
        mov     bx, [cs:free_pages]
        mov     dx, [cs:total_pages]
        mov     ah, EMSE_OK
        iret

; --- 43h: ページを確保する (BX = 枚数) -------------------------------------
f_43:
        push    cx
        push    si
        push    di
        push    ds
        push    cs
        pop     ds

        test    bx, bx
        jz      .zero
        cmp     bx, [total_pages]
        ja      .too_few
        cmp     bx, [free_pages]
        ja      .not_enough

        call    handle_alloc            ; DX = ハンドル
        jc      .no_handles

        call    pages_alloc             ; BX 枚ぶんの連続した並びを探す
        jc      .not_enough_free

        mov     ah, EMSE_OK
.out:
        pop     ds
        pop     di
        pop     si
        pop     cx
        iret
.zero:
        mov     ah, EMSE_ZEROPAGES
        jmp     .out
.too_few:
        mov     ah, EMSE_TOOFEW
        jmp     .out
.not_enough:
        mov     ah, EMSE_NOTENOUGH
        jmp     .out
.no_handles:
        mov     ah, EMSE_NOHANDLES
        jmp     .out
.not_enough_free:
        call    handle_free_slot
        mov     ah, EMSE_NOTENOUGH
        jmp     .out

; --- 44h: 窓にページを出す -------------------------------------------------
;   AL = 窓の番号 (0-3), BX = 論理ページ番号 (FFFFh なら外す), DX = ハンドル
f_44:
        push    bx
        push    cx
        push    si
        push    di
        push    ds
        push    cs
        pop     ds

        cmp     al, FRAME_PAGES
        jae     .bad_phys
        call    handle_check
        jc      .bad_handle

        cmp     bx, 0xFFFF
        je      .unmap
        cmp     bx, [h_count + si]      ; SI = ハンドル表の中の位置
        jae     .bad_logical

        call    map_page_in
        mov     ah, EMSE_OK
.out:
        pop     ds
        pop     di
        pop     si
        pop     cx
        pop     bx
        iret
.unmap:
        call    map_page_out
        mov     ah, EMSE_OK
        jmp     .out
.bad_phys:
        mov     ah, EMSE_BADPHYS
        jmp     .out
.bad_handle:
        mov     ah, EMSE_BADHANDLE
        jmp     .out
.bad_logical:
        mov     ah, EMSE_BADLOGICAL
        jmp     .out

; --- 45h: ハンドルを返す ---------------------------------------------------
f_45:
        push    ax
        push    bx
        push    cx
        push    si
        push    di
        push    ds
        push    cs
        pop     ds

        call    handle_check
        jc      .bad

        call    pages_free
        call    handle_free_slot
        pop     ds
        pop     di
        pop     si
        pop     cx
        pop     bx
        pop     ax
        mov     ah, EMSE_OK
        iret
.bad:
        pop     ds
        pop     di
        pop     si
        pop     cx
        pop     bx
        pop     ax
        mov     ah, EMSE_BADHANDLE
        iret

; --- 46h: EMS の版数 -------------------------------------------------------
f_46:
        mov     ax, 0x0040              ; AH=0, AL=40h → 4.0
        iret

; --- 47h / 48h: 窓の状態を退避・復元 ---------------------------------------
;
; ハンドルごとに 1 組だけ控えられる。同じハンドルで 2 度 47h を呼ぶと
; 「もう控えてある」(8Dh) で断るのが決まり。
; AX を退避してはいけない。戻り値の AH をここで作るので、最後に
; pop ax で戻すと呼び出し時の機能番号 (47h) がそのまま返る。
f_47:
        push    bx
        push    cx
        push    si
        push    di
        push    ds
        push    es
        push    cs
        pop     ds
        push    cs
        pop     es

        call    handle_check
        jc      .bad
        mov     di, dx                  ; DI = ハンドル番号
        cmp     byte [saved_valid + di], 0
        jne     .already
        mov     byte [saved_valid + di], 1

        call    saved_slot              ; DI = そのハンドルの控え場所
        mov     si, map_page
        mov     cx, FRAME_PAGES
        rep     movsw
        mov     ah, EMSE_OK
.out:
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     cx
        pop     bx
        iret
.bad:
        mov     ah, EMSE_BADHANDLE
        jmp     .out
.already:
        mov     ah, EMSE_HAVESAVED
        jmp     .out

f_48:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    ds
        push    es
        push    cs
        pop     ds
        push    cs
        pop     es

        call    handle_check
        jc      .bad
        mov     di, dx
        cmp     byte [saved_valid + di], 0
        je      .none
        mov     byte [saved_valid + di], 0

        call    saved_slot              ; DI = 控え場所
        mov     si, di
        mov     di, saved_tmp
        mov     cx, FRAME_PAGES
        rep     movsw

        call    remap_from_tmp
        mov     ah, EMSE_OK
.out:
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        iret
.bad:
        mov     ah, EMSE_BADHANDLE
        jmp     .out
.none:
        mov     ah, EMSE_NOSAVED
        jmp     .out

; --- saved_slot - ハンドル番号 DI から控え場所の位置を出す
;   出力: DI = saved_maps の中の位置
saved_slot:
        push    ax
        push    cx
        mov     ax, di
        mov     cl, FRAME_PAGES * 2     ; 1 ハンドルあたりのバイト数
        mul     cl
        mov     di, ax
        add     di, saved_maps
        pop     cx
        pop     ax
        ret

; --- remap_from_tmp - saved_tmp の並びどおりに窓を張り直す
remap_from_tmp:
        push    ax
        push    bx
        push    dx
        push    si
        xor     bx, bx
.loop:
        mov     si, bx
        shl     si, 1
        mov     dx, [saved_tmp + si]
        cmp     dx, 0xFFFF
        je      .skip
        mov     al, bl
        call    map_global_in
.skip:
        inc     bx
        cmp     bx, FRAME_PAGES
        jb      .loop
        pop     si
        pop     dx
        pop     bx
        pop     ax
        ret

; --- 4Bh: 使われているハンドルの数 -----------------------------------------
f_4B:
        push    cx
        push    si
        push    ds
        push    cs
        pop     ds
        xor     bx, bx
        xor     si, si
        mov     cx, MAX_HANDLES
.loop:
        cmp     word [h_used + si], 0
        je      .next
        inc     bx
.next:
        add     si, 2
        loop    .loop
        pop     ds
        pop     si
        pop     cx
        mov     ah, EMSE_OK
        iret

; --- 4Ch: そのハンドルが持っているページ数 ---------------------------------
f_4C:
        push    si
        push    ds
        push    cs
        pop     ds
        call    handle_check
        jc      .bad
        mov     bx, [h_count + si]
        pop     ds
        pop     si
        mov     ah, EMSE_OK
        iret
.bad:
        pop     ds
        pop     si
        mov     ah, EMSE_BADHANDLE
        iret

; --- 4Dh: 全ハンドルとページ数の一覧 (ES:DI へ) ----------------------------
f_4D:
        push    ax
        push    cx
        push    si
        push    di
        push    ds
        push    cs
        pop     ds
        xor     bx, bx
        xor     si, si
        mov     cx, MAX_HANDLES
.loop:
        cmp     word [h_used + si], 0
        je      .next
        mov     ax, si
        shr     ax, 1
        stosw                           ; ハンドル番号
        mov     ax, [h_count + si]
        stosw                           ; ページ数
        inc     bx
.next:
        add     si, 2
        loop    .loop
        pop     ds
        pop     di
        pop     si
        pop     cx
        pop     ax
        mov     ah, EMSE_OK
        iret

; --- 4Eh: 窓の状態をまるごと出し入れする -----------------------------------
;   AL=0 ES:DI へ書き出す / AL=1 DS:SI から読み込む / AL=2 両方 / AL=3 大きさ
f_4E:
        cmp     al, 0x03
        je      .size
        cmp     al, 0x00
        je      .get
        cmp     al, 0x01
        je      .set
        cmp     al, 0x02
        je      .both
        mov     ah, EMSE_BADFUNC
        iret
.size:
        mov     al, FRAME_PAGES * 2
        mov     ah, EMSE_OK
        iret
.get:
        call    ctx_save
        mov     ah, EMSE_OK
        iret
.set:
        call    ctx_load
        mov     ah, EMSE_OK
        iret
.both:
        push    ds
        push    si
        call    ctx_save
        pop     si
        pop     ds
        call    ctx_load
        mov     ah, EMSE_OK
        iret

; --- 51h: 持っているページ数を変える ---------------------------------------
;   DX = ハンドル, BX = 新しい枚数
f_51:
        push    cx
        push    si
        push    di
        push    ds
        push    cs
        pop     ds

        call    handle_check
        jc      .bad
        mov     cx, [h_count + si]      ; 今の枚数
        cmp     bx, cx
        je      .same

        ; いったん全部返して取り直す。連続した並びで持つ作りなので、
        ; その場で伸ばせるとは限らない。
        push    bx
        call    pages_free
        pop     bx
        test    bx, bx
        jz      .done_zero
        call    pages_alloc
        jc      .undo
.same:
        mov     ah, EMSE_OK
.out:
        pop     ds
        pop     di
        pop     si
        pop     cx
        iret
.done_zero:
        mov     word [h_count + si], 0
        mov     ah, EMSE_OK
        jmp     .out
.undo:
        ; 取り直せなかったので元の枚数で復帰させる
        mov     bx, cx
        call    pages_alloc
        mov     ah, EMSE_NOTENOUGH
        jmp     .out
.bad:
        mov     ah, EMSE_BADHANDLE
        jmp     .out

; --- 58h: 窓の一覧を返す ---------------------------------------------------
;   AL=0 ES:DI へ (セグメント, 窓番号) の組を並べる / AL=1 個数だけ
f_58:
        cmp     al, 0x01
        je      .count
        test    al, al
        jnz     .bad

        push    ax
        push    bx
        push    di
        mov     bx, [cs:frame_seg]
        xor     ax, ax
.loop:
        mov     [es:di], bx
        mov     [es:di + 2], ax
        add     bx, EMS_PAGE_PARAS
        inc     ax
        add     di, 4
        cmp     ax, FRAME_PAGES
        jb      .loop
        pop     di
        pop     bx
        pop     ax
        mov     cx, FRAME_PAGES
        mov     ah, EMSE_OK
        iret
.count:
        mov     cx, FRAME_PAGES
        mov     ah, EMSE_OK
        iret
.bad:
        mov     ah, EMSE_BADFUNC
        iret

; --- 59h: ハードウェアの構成 -----------------------------------------------
;   AL=1 は「まだ誰にも配っていない生のページ数」
f_59:
        cmp     al, 0x01
        jne     .bad
        mov     bx, [cs:free_pages]
        mov     dx, [cs:total_pages]
        mov     ah, EMSE_OK
        iret
.bad:
        mov     ah, EMSE_BADFUNC
        iret

; ============================================================================
; ハンドルとページの管理
;
; ハンドルは 1 から数える (0 は EMS 4.0 で「システムのハンドル」に予約)。
; ページは連続した並びでしか渡さない。飛び飛びで持てるようにすると
; 論理ページ番号から全体での番号を引くのに表がもう 1 段必要になり、
; ドライバの常駐部分が膨らむ。EMS を使うプログラムは、確保したページを
; 使い切って返す形がほとんどなので、これで実用上困らない。
; ============================================================================

; --- handle_check - DX のハンドルが有効か
;   出力: CF=0 なら SI = ハンドル表の中の位置 (h_used / h_count / h_base に
;         共通の添字)、CF=1 なら無効なハンドル
handle_check:
        push    ax
        mov     ax, dx
        test    ax, ax
        jz      .bad
        cmp     ax, MAX_HANDLES
        jae     .bad
        shl     ax, 1
        mov     si, ax
        cmp     word [h_used + si], 0
        je      .bad
        pop     ax
        clc
        ret
.bad:
        pop     ax
        stc
        ret

; --- handle_index - DX のハンドル番号を DI に (0 から数える)
handle_index:
        mov     di, dx
        ret

; --- handle_alloc - 空いているハンドルを 1 つ取る
;   出力: CF=0 なら DX = ハンドル, SI = h_count のエントリ
handle_alloc:
        push    ax
        mov     ax, 1                   ; 0 は予約
.loop:
        mov     si, ax
        shl     si, 1
        cmp     word [h_used + si], 0
        je      .found
        inc     ax
        cmp     ax, MAX_HANDLES
        jb      .loop
        pop     ax
        stc
        ret
.found:
        mov     word [h_used + si], 1
        mov     word [h_count + si], 0
        mov     word [h_base + si], 0
        mov     dx, ax
        pop     ax
        clc
        ret

; --- handle_free_slot - SI のハンドルを未使用に戻す
handle_free_slot:
        mov     word [h_used + si], 0
        mov     word [h_count + si], 0
        mov     word [h_base + si], 0
        ret

; --- pages_alloc - BX 枚ぶんの連続した空きを探して SI のハンドルに渡す
;   出力: CF=1 なら空きが足りない
pages_alloc:
        push    ax
        push    cx
        push    di
        xor     di, di                  ; 走査の開始位置
.try:
        mov     ax, di
        add     ax, bx
        cmp     ax, [total_pages]
        ja      .none

        ; di から bx 枚が全部空いているか
        mov     cx, bx
        push    di
.scan:
        mov     ax, di
        shl     ax, 1
        push    si
        mov     si, ax
        cmp     word [page_owner + si], 0
        pop     si
        jne     .busy
        inc     di
        loop    .scan
        pop     di

        ; 空いていた。持ち主を書き込む
        mov     cx, bx
        push    di
.mark:
        mov     ax, di
        shl     ax, 1
        push    si
        mov     si, ax
        mov     ax, dx
        mov     [page_owner + si], ax
        pop     si
        inc     di
        loop    .mark
        pop     di

        mov     [h_base + si], di
        mov     [h_count + si], bx
        mov     ax, [free_pages]
        sub     ax, bx
        mov     [free_pages], ax
        pop     di
        pop     cx
        pop     ax
        clc
        ret
.busy:
        pop     di
        inc     di
        jmp     .try
.none:
        pop     di
        pop     cx
        pop     ax
        stc
        ret

; --- pages_free - SI のハンドルが持っているページを空きに戻す
pages_free:
        push    ax
        push    bx
        push    cx
        push    di
        mov     cx, [h_count + si]
        jcxz   .done
        mov     di, [h_base + si]
.loop:
        ; その窓に出ていたら先に外す
        mov     ax, di
        call    unmap_if_shown
        mov     ax, di
        shl     ax, 1
        push    si
        mov     si, ax
        mov     word [page_owner + si], 0
        pop     si
        inc     di
        loop    .loop
        mov     ax, [free_pages]
        add     ax, [h_count + si]
        mov     [free_pages], ax
        mov     word [h_count + si], 0
.done:
        pop     di
        pop     cx
        pop     bx
        pop     ax
        ret

; --- unmap_if_shown - 全体での番号 AX のページが窓に出ていたら書き戻して外す
unmap_if_shown:
        push    bx
        push    cx
        xor     bx, bx
.loop:
        mov     cx, bx
        shl     cx, 1
        push    si
        mov     si, cx
        cmp     [map_page + si], ax
        pop     si
        jne     .next
        push    ax
        mov     al, bl
        call    map_page_out
        pop     ax
.next:
        inc     bx
        cmp     bx, FRAME_PAGES
        jb      .loop
        pop     cx
        pop     bx
        ret

; ============================================================================
; 窓の張り替え
;
; 本物の EMS はここでアドレスデコーダを切り替えるだけだが、こちらは
; XMS との間で 16KB を実際にコピーする。出ていたページは必ず書き戻す。
; 書き戻さないと、窓から追い出した瞬間にそのページへの書き込みが消える。
; ============================================================================

; --- map_page_in - 窓 AL に、SI のハンドルの論理ページ BX を出す
map_page_in:
        push    dx
        mov     dx, [h_base + si]
        add     dx, bx                  ; DX = 全体でのページ番号
        call    map_global_in
        pop     dx
        ret

; --- map_global_in - 窓 AL に、全体でのページ番号 DX を出す
;
; XMS の転送を呼ぶと AX が戻り値で潰れるので、窓の番号は毎回 cur_win から
; 取り直している。ここを AL のまま持ち回ると、書き戻し先が 0 番の窓に
; なって、別のページの内容が上書きされる。
map_global_in:
        push    ax
        push    bx
        push    dx
        mov     [cur_win], al
        call    map_page_out            ; いま出ているものを書き戻して外す
        mov     al, [cur_win]
        call    copy_in
        movzx   bx, byte [cur_win]
        shl     bx, 1
        pop     dx                      ; 全体でのページ番号を戻す
        mov     [map_page + bx], dx
        pop     bx
        pop     ax
        ret

; --- map_page_out - 窓 AL に出ているものを書き戻して外す
map_page_out:
        push    ax
        push    bx
        push    dx
        movzx   bx, al
        shl     bx, 1
        mov     dx, [map_page + bx]
        cmp     dx, 0xFFFF
        je      .none
        push    bx
        call    copy_out
        pop     bx
        mov     word [map_page + bx], 0xFFFF
.none:
        pop     dx
        pop     bx
        pop     ax
        ret

; --- copy_in / copy_out - XMS と窓の間で 16KB 動かす
;   入力: AL = 窓の番号, DX = 全体でのページ番号
copy_in:
        call    move_setup
        mov     ax, [xms_handle]
        mov     [mv_srch], ax
        mov     [mv_srco], ecx          ; XMS 側の位置
        mov     word [mv_dsth], 0
        mov     [mv_dsto], edi          ; 窓の far ポインタ
        jmp     move_run
copy_out:
        call    move_setup
        mov     word [mv_srch], 0
        mov     [mv_srco], edi
        mov     ax, [xms_handle]
        mov     [mv_dsth], ax
        mov     [mv_dsto], ecx
        jmp     move_run

; move_setup - ECX = XMS 側のバイト位置, EDI = 窓の far ポインタ
move_setup:
        movzx   ecx, dx
        shl     ecx, 14                 ; * 16384
        movzx   edi, al
        shl     edi, 10                 ; 窓の番号 * 0x400 段落
        movzx   eax, word [frame_seg]
        add     edi, eax
        shl     edi, 16                 ; 上位 16bit がセグメント、下位が 0
        mov     dword [mv_len], EMS_PAGE_SIZE
        ret

move_run:
        push    si
        mov     si, mv_len
        mov     ah, 0x0B
        call    far [xms_entry]
        pop     si
        ret

; ============================================================================
; 4Eh 用: 窓の状態をそのまま出し入れする
;
; 中身の形は EMS の仕様では決めていない (呼ぶ側にとっては不透明な塊)。
; ここでは「窓ごとの、全体でのページ番号」を 4 つ並べただけにしてある。
; ============================================================================
ctx_save:
        push    ax
        push    cx
        push    si
        push    di
        push    ds
        push    cs
        pop     ds
        mov     si, map_page
        mov     cx, FRAME_PAGES
        rep     movsw                   ; DS:SI (カーネル) → ES:DI (呼び出し元)
        pop     ds
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

ctx_load:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es
        push    cs
        pop     es
        mov     di, saved_tmp
        mov     cx, FRAME_PAGES
        rep     movsw                   ; DS:SI (呼び出し元) → saved_tmp
        pop     es
        push    ds
        push    cs
        pop     ds
        call    remap_from_tmp
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ============================================================================
; 表示まわり
; ============================================================================
; put_dec - AX を十進で DI に書く (末尾は詰めない。5 桁固定で右詰め)
put_dec:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     bx, 10
        add     di, 4
        mov     cx, 5
.loop:
        xor     dx, dx
        div     bx
        add     dl, '0'
        mov     [di], dl
        dec     di
        loop    .loop
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ============================================================================
; データ
; ============================================================================
req_off:        dw 0
req_seg:        dw 0
fail_msg:       dw 0
cur_win:        db 0            ; いま張り替えている窓の番号

xms_entry:      dd 0
xms_handle:     dw 0
frame_seg:      dw 0
total_pages:    dw 0
free_pages:     dw 0

; XMS のブロック転送に渡す構造体
                align 2
mv_len:         dd 0
mv_srch:        dw 0
mv_srco:        dd 0
mv_dsth:        dw 0
mv_dsto:        dd 0

msg_ok:         db 'EMM386.SYS installed, '
msg_kb:         db '00000 KB of EMS', 13, 10, '$'
msg_no_xms:     db 'EMM386.SYS: XMS not present, not installed', 13, 10, '$'
msg_no_mem:     db 'EMM386.SYS: no extended memory, not installed', 13, 10, '$'

; --- 表 -------------------------------------------------------------------
; 常駐したままになるので、ここは .SYS の大きさにそのまま乗る。
h_used:         times MAX_HANDLES dw 0
h_count:        times MAX_HANDLES dw 0
h_base:         times MAX_HANDLES dw 0
saved_valid:    times MAX_HANDLES db 0
                align 2
saved_maps:     times MAX_HANDLES * FRAME_PAGES dw 0
saved_tmp:      times FRAME_PAGES dw 0
map_page:       times FRAME_PAGES dw 0xFFFF
page_owner:     times MAX_PAGES dw 0

resident_end:
