; ============================================================================
; lfn.inc  -  VFAT の長いファイル名
;
; FAT のディレクトリエントリは名前を 11 バイト固定で持つ。8 文字 + 拡張子
; 3 文字しか入らないのはこれが理由で、FAT という形式そのものの制限ではなく
; エントリの形の制限でしかない。
;
; VFAT は、本来のエントリの手前に「属性が 0x0F のエントリ」を並べて、
; そこに長い名前を 13 文字ずつ分けて入れる。属性 0x0F は
; 読み取り専用+隠し+システム+ボリュームラベル の組み合わせで、
; 昔の DOS はこれを「ボリュームラベルの一種」と見て読み飛ばすので、
; 知らないシステムから見ても壊れて見えない、という仕掛けになっている。
;
;   断片エントリ (32 バイト)
;     +00  byte   並び番号 (1..20)。bit6 が立っていれば連鎖の先頭
;     +01  10     文字 1..5   (UTF-16LE)
;     +0B  byte   属性 = 0x0F
;     +0C  byte   種別 = 0
;     +0D  byte   短い名前のチェックサム
;     +0E  12     文字 6..11  (UTF-16LE)
;     +1A  word   先頭クラスタ = 0 (必ず 0)
;     +1C  4      文字 12..13 (UTF-16LE)
;
; 断片はディレクトリの中で「後ろの断片が先」に並ぶ。前から読んでいくと
; 並び番号は N, N-1, ..., 1 と減っていき、そのあとに本体のエントリが来る。
;
; チェックサムは本体の 11 バイトから作る。これが合わない断片は、
; 長い名前を知らないシステムがファイルを触ったあとに取り残された
; ゴミなので、捨てなければならない。
; ============================================================================

LFN_SEQ         equ 0x00        ; byte: 並び番号
LFN_ATTR        equ 0x0B        ; byte: 属性 (必ず 0x0F)
LFN_CKSUM       equ 0x0D        ; byte: 短い名前のチェックサム
LFN_TYPE        equ 0x0C        ; byte: 種別 (常に 0)
LFN_CLUS        equ 0x1A        ; word: 先頭クラスタ (常に 0)
LFN_LAST        equ 0x40        ; 並び番号のこのビット = 連鎖の先頭
LFN_SEQMASK     equ 0x3F

LFN_CHARS       equ 13          ; 1 つの断片に入る文字数
LFN_MAXENT      equ 20          ; 断片の最大数 (13 * 20 = 260)
LFN_MAXLEN      equ 255         ; 名前の最大長

; ---------------------------------------------------------------------------
; lfn_checksum - 短い名前 (11 バイト) からチェックサムを作る
;   入力: ES:DI = 11 バイトの短い名前
;   出力: AL = チェックサム
;
; 1 ビット右回転して足す、を 11 回。VFAT が決めた通りの手順で、
; ここを間違えると自分で書いた長い名前を自分で捨てることになる。
; ---------------------------------------------------------------------------
lfn_checksum:
        push    bx
        push    cx
        push    si

        mov     si, di
        xor     al, al
        mov     cx, 11
.loop:
        mov     bl, al
        shr     bl, 1
        mov     bh, al
        shl     bh, 7
        or      bl, bh                  ; 1 ビット右回転
        mov     al, bl
        add     al, [es:si]
        inc     si
        loop    .loop

        pop     si
        pop     cx
        pop     bx
        ret

; ---------------------------------------------------------------------------
; lfn_reset - 組み立て中の長い名前を捨てる
;
; ディレクトリの走査を始めるとき、本体のエントリを 1 つ処理したあと、
; 連鎖が壊れていたときに呼ぶ。
; ---------------------------------------------------------------------------
lfn_reset:
        push    ax
        push    cx
        push    di
        push    es

        push    ds
        pop     es
        mov     di, lfn_name
        mov     cx, LFN_MAXENT * LFN_CHARS + 2
        xor     al, al
        rep     stosb

        mov     byte [lfn_valid], 0
        mov     byte [lfn_expect], 0
        mov     byte [lfn_count], 0
        mov     byte [lfn_cksum], 0

        pop     es
        pop     di
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; lfn_take - 断片を 1 つ取り込む
;   入力: ES:DI = ディレクトリエントリ (属性 0x0F であること)
;
; 並びが飛んでいたら、その場で組み立てを無効にする。壊れた連鎖から
; 中途半端な名前を作るくらいなら、短い名前だけで扱うほうが安全。
; ---------------------------------------------------------------------------
lfn_take:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di

        mov     al, [es:di + LFN_SEQ]
        mov     ah, al
        and     al, LFN_SEQMASK
        jz      .broken                 ; 並び番号 0 はありえない
        cmp     al, LFN_MAXENT
        ja      .broken

        test    ah, LFN_LAST
        jz      .continue

        ; --- 連鎖の先頭 (いちばん後ろの断片) ---
        push    ax
        push    di
        call    lfn_reset
        pop     di
        pop     ax
        mov     [lfn_count], al
        mov     [lfn_expect], al
        mov     bl, [es:di + LFN_CKSUM]
        mov     [lfn_cksum], bl
        mov     byte [lfn_valid], 1
        jmp     .place

.continue:
        cmp     byte [lfn_valid], 0
        je      .out                    ; 先頭を見ていない = 取り残されたゴミ
        cmp     al, [lfn_expect]
        jne     .broken
        mov     bl, [es:di + LFN_CKSUM]
        cmp     bl, [lfn_cksum]
        jne     .broken

.place:
        ; 書き込み先 = lfn_name + (並び番号 - 1) * 13
        dec     al
        movzx   bx, al
        mov     dl, LFN_CHARS
        mov     al, bl
        mul     dl
        mov     bx, lfn_name
        add     bx, ax

        ; 3 か所に散らばった文字を順に取り出す
        mov     si, di
        add     si, 0x01
        mov     cx, 5
        call    .chars
        mov     si, di
        add     si, 0x0E
        mov     cx, 6
        call    .chars
        mov     si, di
        add     si, 0x1C
        mov     cx, 2
        call    .chars

        dec     byte [lfn_expect]
        jmp     .out

.broken:
        mov     byte [lfn_valid], 0
.out:
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; .chars - ES:SI から CX 文字を取り出して DS:BX へ 1 バイトずつ置く
;
; UTF-16 のうち 0x00FF に収まらない文字は '_' にする。当時の DOS も
; コードページに無い文字はそう扱った。0x0000 と 0xFFFF は名前の終わり
; およびその後ろの詰め物なので 0 のままにしておく。
.chars:
        push    ax
.cloop:
        mov     ax, [es:si]
        add     si, 2
        cmp     ax, 0x0000
        je      .cskip                  ; 終端。以降は 0 のまま
        cmp     ax, 0xFFFF
        je      .cskip                  ; 詰め物
        test    ah, ah
        jz      .cstore
        mov     al, '_'                 ; 1 バイトに収まらない文字
.cstore:
        mov     [bx], al
.cskip:
        inc     bx
        loop    .cloop
        pop     ax
        ret

; ---------------------------------------------------------------------------
; lfn_ready - 直前まで集めた断片が、この本体のエントリのものか確かめる
;   入力: ES:DI = 本体のディレクトリエントリ
;   出力: CF=0 なら lfn_name に長い名前 (ASCIZ) が入っている
;         CF=1 なら長い名前は無い (短い名前だけのファイル)
;
; 連鎖が全部そろっていること (lfn_expect が 0 まで下がったこと) と、
; チェックサムが本体の 11 バイトと合うことの両方を見る。
; ---------------------------------------------------------------------------
lfn_ready:
        push    ax

        cmp     byte [lfn_valid], 0
        je      .none
        cmp     byte [lfn_expect], 0
        jne     .none

        call    lfn_checksum            ; ES:DI の 11 バイトから
        cmp     al, [lfn_cksum]
        jne     .none

        cmp     byte [lfn_name], 0
        je      .none

        pop     ax
        clc
        ret
.none:
        pop     ax
        stc
        ret

; ---------------------------------------------------------------------------
; lfn_len - lfn_name の長さ
;   出力: CX = 文字数
; ---------------------------------------------------------------------------
lfn_len:
        push    si
        mov     si, lfn_name
        xor     cx, cx
.loop:
        cmp     byte [si], 0
        je      .done
        inc     si
        inc     cx
        cmp     cx, LFN_MAXLEN
        jb      .loop
.done:
        pop     si
        ret

; ---------------------------------------------------------------------------
; lfn_match - lfn_name が DS:SI の名前と一致するか (大文字小文字を区別しない)
;   入力: DS:SI = 比べたい名前 (0 / '\' / '/' で終わる)
;   出力: ZF=1 なら一致
;
; ワイルドカードは見ない。開く・削除するといった「名指し」の経路で使う。
; ---------------------------------------------------------------------------
lfn_match:
        push    ax
        push    bx
        push    si
        push    di

        mov     di, lfn_name
.loop:
        mov     al, [si]
        cmp     al, '\'
        je      .end_of_name
        cmp     al, '/'
        je      .end_of_name
        test    al, al
        jz      .end_of_name

        call    to_upper
        mov     bl, [di]
        test    bl, bl
        jz      .no                     ; こちらが先に尽きた
        xchg    al, bl
        call    to_upper
        xchg    al, bl
        cmp     al, bl
        jne     .no
        inc     si
        inc     di
        jmp     .loop

.end_of_name:
        cmp     byte [di], 0
        jne     .no
        xor     al, al                  ; ZF=1
        jmp     .out
.no:
        or      al, 0xFF                ; ZF=0
.out:
        pop     di
        pop     si
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; name_copy_component - DS:SI のパス要素を name_long に写す
;   入力: DS:SI = パスの途中 (要素の先頭)
;   出力: name_long に ASCIZ。SI は動かさない
;
; name_to_83 は 8.3 に畳んでしまうので、長い名前で照合するには
; 畳む前のつづりが要る。
; ---------------------------------------------------------------------------
name_copy_component:
        push    ax
        push    cx
        push    si
        push    di
        push    es

        push    ds
        pop     es
        mov     di, name_long
        mov     cx, LFN_MAXLEN
.loop:
        mov     al, [si]
        test    al, al
        jz      .done
        cmp     al, '\'
        je      .done
        cmp     al, '/'
        je      .done
        stosb
        inc     si
        loop    .loop
.done:
        xor     al, al
        stosb

        pop     es
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; lfn_fits_83 - name_long がそのまま 8.3 に収まるか
;   出力: CF=0 なら収まる (断片は要らない)
;
; 収まる条件は「大文字だけ」「基本名 8 文字以内」「拡張子 3 文字以内」
; 「'.' は 1 つまで」「空白を含まない」。小文字を含むものは収まらない扱いに
; する。8.3 は大文字しか持てないので、小文字のつづりを覚えておくには
; 断片が要るため。
; ---------------------------------------------------------------------------
lfn_fits_83:
        push    ax
        push    cx
        push    dx
        push    si

        mov     si, name_long
        cmp     byte [si], 0
        je      .no                     ; 空の名前
        cmp     byte [si], '.'
        je      .no                     ; "." ".." はここへ来ない約束

        xor     cx, cx                  ; 基本名の長さ
        xor     dx, dx                  ; 0 = まだ '.' を見ていない
.base:
        mov     al, [si]
        test    al, al
        jz      .done
        cmp     al, '.'
        je      .dot
        cmp     al, ' '
        je      .no
        cmp     al, 'a'
        jb      .ok_char
        cmp     al, 'z'
        jbe     .no                     ; 小文字
.ok_char:
        inc     si
        inc     cx
        cmp     cx, 8
        ja      .no
        jmp     .base
.dot:
        test    dx, dx
        jnz     .no                     ; '.' が 2 つ以上
        inc     dx
        inc     si
        test    cx, cx
        jz      .no                     ; 先頭が '.'
        xor     cx, cx                  ; ここから拡張子の長さ
.ext:
        mov     al, [si]
        test    al, al
        jz      .done
        cmp     al, '.'
        je      .no
        cmp     al, ' '
        je      .no
        cmp     al, 'a'
        jb      .ext_ok
        cmp     al, 'z'
        jbe     .no
.ext_ok:
        inc     si
        inc     cx
        cmp     cx, 3
        ja      .no
        jmp     .ext
.done:
        pop     si
        pop     dx
        pop     cx
        pop     ax
        clc
        ret
.no:
        pop     si
        pop     dx
        pop     cx
        pop     ax
        stc
        ret

; ---------------------------------------------------------------------------
; lfn_entry_count - name_long を入れるのに要る断片の数
;   出力: CX = 断片の数 (13 文字ごとに 1 つ)
; ---------------------------------------------------------------------------
lfn_entry_count:
        push    ax
        push    dx
        call    lfn_long_len            ; CX = 長さ
        mov     ax, cx
        add     ax, LFN_CHARS - 1
        xor     dx, dx
        mov     cx, LFN_CHARS
        div     cx
        mov     cx, ax
        pop     dx
        pop     ax
        ret

; lfn_long_len - name_long の長さ
;   出力: CX = 文字数
lfn_long_len:
        push    si
        mov     si, name_long
        xor     cx, cx
.loop:
        cmp     byte [si], 0
        je      .done
        inc     si
        inc     cx
        cmp     cx, LFN_MAXLEN
        jb      .loop
.done:
        pop     si
        ret

; ---------------------------------------------------------------------------
; lfn_make_short - name_long から短い名前を作って name83 に入れる
;   入力: AX = 置き場所のディレクトリのクラスタ (重複を避けるため)
;   出力: CF=0 なら name83 に 11 バイト
;
; "My Long File Name.txt" → "MYLONG~1TXT" の形。同じものが既にあれば
; ~2, ~3 と番号を上げる。DOS が長い名前のファイルに必ず短い名前も
; 付けるのは、長い名前を知らないプログラムからも開けるようにするため。
; ---------------------------------------------------------------------------
lfn_make_short:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es

        mov     [.dirclus], ax
        push    ds
        pop     es

        ; --- 拡張子を探す (いちばん後ろの '.') ---
        mov     si, name_long
        xor     bx, bx                  ; 最後の '.' の位置 (0 = 無い)
.find_dot:
        mov     al, [si]
        test    al, al
        jz      .dot_done
        cmp     al, '.'
        jne     .not_dot
        mov     bx, si
.not_dot:
        inc     si
        jmp     .find_dot
.dot_done:
        mov     [.dotpos], bx

        ; --- 11 バイトを空白で埋める ---
        mov     di, name83
        mov     al, ' '
        mov     cx, 11
        rep     stosb

        ; --- 基本名を 6 文字ぶん取る ---
        ; 空白と '.' は落とし、8.3 に置けない文字は '_' にする。
        mov     si, name_long
        mov     di, name83
        mov     cx, 6
.base_loop:
        jcxz    .base_done
        mov     al, [si]
        test    al, al
        jz      .base_done
        mov     bx, [.dotpos]
        test    bx, bx
        jz      .no_stop
        cmp     si, bx
        jae     .base_done              ; 拡張子の '.' まで来た
.no_stop:
        inc     si
        cmp     al, ' '
        je      .base_loop              ; 空白は落とす
        cmp     al, '.'
        je      .base_loop
        call    to_upper
        call    lfn_ok83_char
        stosb
        dec     cx
        jmp     .base_loop
.base_done:
        ; 1 文字も取れなかったら '_' を置く
        cmp     di, name83
        jne     .have_base
        mov     al, '_'
        stosb
.have_base:
        mov     [.baseend], di

        ; --- 拡張子を 3 文字ぶん取る ---
        mov     bx, [.dotpos]
        test    bx, bx
        jz      .no_ext
        mov     si, bx
        inc     si
        mov     di, name83 + 8
        mov     cx, 3
.ext_loop:
        jcxz    .no_ext
        mov     al, [si]
        test    al, al
        jz      .no_ext
        inc     si
        cmp     al, ' '
        je      .ext_loop
        cmp     al, '.'
        je      .ext_loop
        call    to_upper
        call    lfn_ok83_char
        stosb
        dec     cx
        jmp     .ext_loop
.no_ext:

        ; --- "~N" を付けて、空いている番号を探す ---
        mov     byte [lfn_match_enable], 0      ; 短い名前だけで照合する
        mov     word [.serial], 1
.try:
        mov     di, [.baseend]
        ; "~N" が 8 文字に収まるように後ろへ寄せる
        mov     ax, [.serial]
        call    lfn_dec_len             ; CX = 桁数
        mov     bx, name83 + 8
        sub     bx, cx
        dec     bx                      ; '~' のぶん
        cmp     di, bx
        jbe     .place
        mov     di, bx
.place:
        mov     al, '~'
        mov     [di], al
        inc     di
        mov     ax, [.serial]
        call    lfn_put_dec             ; DI に 10 進で書く

        ; 同じ短い名前が既にないか
        push    si
        mov     si, name83
        mov     ax, [.dirclus]
        mov     cl, ATTR_DIRECTORY
        call    dir_find
        pop     si
        jc      .free                   ; 無い = この番号でよい

        inc     word [.serial]
        cmp     word [.serial], 1000
        jb      .try
        ; 1000 個も同じ名前がある。あきらめる。
        mov     byte [lfn_match_enable], 1
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret
.free:
        mov     byte [lfn_match_enable], 1
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.dirclus: dw 0
.dotpos:  dw 0
.baseend: dw 0
.serial:  dw 0

; lfn_ok83_char - AL が 8.3 に置けない文字なら '_' に直す
lfn_ok83_char:
        cmp     al, ' '
        jbe     .bad
        cmp     al, '"'
        je      .bad
        cmp     al, '*'
        je      .bad
        cmp     al, '+'
        je      .bad
        cmp     al, ','
        je      .bad
        cmp     al, '/'
        je      .bad
        cmp     al, ':'
        je      .bad
        cmp     al, ';'
        je      .bad
        cmp     al, '<'
        je      .bad
        cmp     al, '='
        je      .bad
        cmp     al, '>'
        je      .bad
        cmp     al, '?'
        je      .bad
        cmp     al, '['
        je      .bad
        cmp     al, '\'
        je      .bad
        cmp     al, ']'
        je      .bad
        cmp     al, '|'
        je      .bad
        ret
.bad:
        mov     al, '_'
        ret

; lfn_dec_len - AX の 10 進の桁数を CX に返す
lfn_dec_len:
        push    ax
        push    dx
        mov     cx, 1
        mov     dx, 10
.loop:
        cmp     ax, dx
        jb      .done
        inc     cx
        push    ax
        mov     ax, dx
        mov     dx, 10
        mul     dx
        mov     dx, ax
        pop     ax
        jmp     .loop
.done:
        pop     dx
        pop     ax
        ret

; lfn_put_dec - AX を 10 進で ES:DI に書く
lfn_put_dec:
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
        stosb
        loop    .emit
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; lfn_write_frag - 断片エントリを 1 つ書く
;   入力: AX = 並び番号 (連鎖の先頭なら 0x40 を足したもの)
;         ES:DI = 書き込み先の 32 バイト
;         name_long = 名前, name83 = 短い名前 (チェックサムのもと)
; ---------------------------------------------------------------------------
lfn_write_frag:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di

        mov     [.seq], ax              ; 並び番号 (0x40 が乗っていることもある)
        mov     [.dest], di

        ; 32 バイトを 0 で埋める
        mov     cx, DIR_ENTSIZE
        xor     al, al
        rep     stosb
        mov     di, [.dest]

        mov     ax, [.seq]
        mov     [es:di + LFN_SEQ], al
        mov     byte [es:di + LFN_ATTR], ATTR_LFN
        mov     byte [es:di + LFN_TYPE], 0
        mov     word [es:di + LFN_CLUS], 0

        ; 短い名前のチェックサム。lfn_checksum は ES:DI を見るので、
        ; いったん ES を DS に合わせて name83 を読ませる。
        push    es
        push    di
        push    ds
        pop     es
        mov     di, name83
        call    lfn_checksum
        pop     di
        pop     es
        mov     [es:di + LFN_CKSUM], al

        ; この断片が受け持つ 13 文字の先頭 = name_long + (並び番号-1)*13
        mov     ax, [.seq]
        and     al, LFN_SEQMASK
        dec     al
        movzx   bx, al
        mov     al, LFN_CHARS
        mul     bl                      ; AX = (並び番号-1) * 13
        mov     [.start], ax
        mov     si, name_long
        add     si, ax

        ; 名前の長さから、この断片に入る文字数を出す
        call    lfn_long_len            ; CX = 名前全体の長さ
        sub     cx, [.start]
        jns     .have
        xor     cx, cx
.have:
        cmp     cx, LFN_CHARS
        jbe     .len_ok
        mov     cx, LFN_CHARS
.len_ok:
        mov     [.remain], cx

        ; 3 か所に散らばった枠へ順に流し込む
        xor     dx, dx                  ; この断片の中で何文字目か
        mov     di, [.dest]
        add     di, 0x01
        mov     cx, 5
        call    .fill
        mov     di, [.dest]
        add     di, 0x0E
        mov     cx, 6
        call    .fill
        mov     di, [.dest]
        add     di, 0x1C
        mov     cx, 2
        call    .fill

        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; .fill - ES:DI へ CX 文字ぶんの枠 (word 単位) を埋める
;   DS:SI = 文字の続き, DX = この断片の中の位置, [.remain] = 入れる文字数
;
; 名前がこの断片の途中で終わるときは、終わったところに 0x0000 を 1 つ置き、
; その後ろは 0xFFFF で埋める。VFAT が決めた詰め方で、ここを守らないと
; 他のシステムから名前の終わりが分からなくなる。
.fill:
        push    ax
.floop:
        cmp     dx, [.remain]
        jb      .fchar
        je      .fterm
        mov     word [es:di], 0xFFFF    ; 終端より後ろの詰め物
        jmp     .fnext
.fterm:
        mov     word [es:di], 0x0000    ; 名前の終わり
        inc     dx                      ; 0 は一度だけ
        jmp     .fnext
.fchar:
        mov     al, [si]
        inc     si
        xor     ah, ah
        mov     [es:di], ax
        inc     dx
.fnext:
        add     di, 2
        loop    .floop
        pop     ax
        ret
.seq:    dw 0
.dest:   dw 0
.start:  dw 0
.remain: dw 0

; ---------------------------------------------------------------------------
; lfn_capture - 見つかったエントリの表向きの名前を find_longname に控える
;   入力: DS:SI = ディレクトリエントリ (32 バイト)
;
; 断片が付いていればその長い名前、無ければ 8.3 を開いたものを入れる。
; どちらの場合も「そのファイルの本当の名前」として使える形にしておく。
; ---------------------------------------------------------------------------
lfn_capture:
        push    ax
        push    cx
        push    si
        push    di
        push    es

        push    ds
        pop     es

        mov     di, si
        call    lfn_ready
        jc      .short

        ; 断片から組み立てたものを写す
        push    si
        mov     si, lfn_name
        mov     di, find_longname
        mov     cx, LFN_MAXLEN
.copy:
        lodsb
        stosb
        test    al, al
        jz      .done_copy
        loop    .copy
        xor     al, al
        stosb
.done_copy:
        pop     si
        jmp     .out

.short:
        mov     di, find_longname
        call    name_from_83            ; DS:SI = 11 バイト → ES:DI
.out:
        pop     es
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; lfn_wild_match - find_longname が DS:SI のパターンに一致するか
;   入力: DS:SI = パターン ('*' と '?' が使える)
;   出力: ZF=1 なら一致
;
; 大文字小文字は区別しない。'*' は「ここから何文字でも」なので、
; 合わなかったら '*' の直後まで戻ってやり直す (後戻り) 必要がある。
; ---------------------------------------------------------------------------
lfn_wild_match:
        push    ax
        push    bx
        push    si
        push    di

        mov     di, find_longname
        mov     word [.star_pat], 0     ; 直前の '*' の位置 (0 = まだ無い)
        mov     word [.star_name], 0

.loop:
        mov     al, [si]
        test    al, al
        jz      .pattern_end

        cmp     al, '*'
        je      .star
        cmp     al, '?'
        je      .single

        ; 普通の文字。大文字に畳んで比べる。
        mov     bl, [di]
        test    bl, bl
        jz      .backtrack
        call    to_upper
        xchg    al, bl
        call    to_upper
        xchg    al, bl
        cmp     al, bl
        jne     .backtrack
        inc     si
        inc     di
        jmp     .loop

.single:
        cmp     byte [di], 0
        je      .backtrack
        inc     si
        inc     di
        jmp     .loop

.star:
        inc     si
        mov     [.star_pat], si         ; '*' の直後を覚えておく
        mov     [.star_name], di
        jmp     .loop

.pattern_end:
        cmp     byte [di], 0
        je      .same
        ; パターンは尽きたが名前が残っている。'*' があれば伸ばして
        ; やり直す。無ければ一致しない。
.backtrack:
        cmp     word [.star_pat], 0
        je      .no
        mov     bx, [.star_name]
        cmp     byte [bx], 0
        je      .no                     ; 名前も尽きた
        inc     bx
        mov     [.star_name], bx
        mov     di, bx
        mov     si, [.star_pat]
        jmp     .loop

.same:
        xor     al, al                  ; ZF=1
        jmp     .out
.no:
        or      al, 0xFF                ; ZF=0
.out:
        pop     di
        pop     si
        pop     bx
        pop     ax
        ret
.star_pat:  dw 0
.star_name: dw 0

; ---------------------------------------------------------------------------
; find_save_long_pattern - name_long を検索パターンとして控える
;
; find_first が path_resolve を済ませた直後に呼ぶ。find_next は path を
; 解き直さないので、続きを探すときのためにここに残しておく必要がある。
; ---------------------------------------------------------------------------
find_save_long_pattern:
        push    ax
        push    cx
        push    si
        push    di
        push    es

        push    ds
        pop     es
        mov     si, name_long
        mov     di, find_pattern_long
        mov     cx, LFN_MAXLEN
.loop:
        lodsb
        stosb
        test    al, al
        jz      .done
        loop    .loop
        xor     al, al
        stosb
.done:
        pop     es
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; --- 組み立て中の長い名前 ------------------------------------------------
lfn_valid:   db 0            ; 1 = 連鎖の先頭を見て、まだ壊れていない
lfn_expect:  db 0            ; 次に来るはずの並び番号
lfn_count:   db 0            ; 連鎖の断片の数
lfn_cksum:   db 0            ; 連鎖が名乗っているチェックサム
lfn_match_enable: db 1  ; 0 のあいだ dir_find は短い名前だけで照合する
find_long_mode:  db 0   ; 1 = 検索を長い名前で行う (AX=714Eh/714Fh)
