; ============================================================================
; mem.inc  -  MCB (Memory Control Block) によるメモリ管理
;
; MCB は 16 バイトのヘッダで、確保された各ブロックの直前に置かれ、
; 連鎖して 640KB までを隙間なく覆う。
;
;   MCB(16B) | ブロック本体 | MCB(16B) | ブロック本体 | ... | MCB('Z') | 本体
;
; これを「JS の配列で管理して、メモリ上には置かない」という作り方もできるが、
; それだと当時の .EXE が動かない。max_alloc=0xFFFF で「空きを全部よこせ」と
; 言ってくるプログラムや、MCB 連鎖を自分で辿って空きを調べるプログラム
; (MEM コマンドの類、常駐ソフト、メモリマネージャ) が実在するため、
; 連鎖は本物どおりメモリ上に存在していなければならない。
; ============================================================================

; ---------------------------------------------------------------------------
; mem_init - アリーナを初期化する
;   入力: AX = アリーナの先頭セグメント (カーネルの直後)
;
; 最初は「全部が 1 つの空きブロック」から始める。
; ---------------------------------------------------------------------------
mem_init:
        push    ax
        push    bx
        push    cx
        push    di
        push    es

        mov     [first_mcb], ax
%ifdef CHAIN_CHECK
        mov     [mcb_canary], ax
%endif
        mov     es, ax

        xor     di, di
        mov     byte [es:di + MCB_SIG], MCB_SIG_LAST
        mov     word [es:di + MCB_OWNER], MCB_OWNER_FREE

        ; ブロックの大きさ = 640KB の末尾 - MCB 自身の 1 パラグラフ
        mov     bx, MEM_TOP_SEG
        sub     bx, ax
        dec     bx
        mov     [es:di + MCB_SIZE], bx

        ; 名前欄は 0 で埋めておく
        push    di
        add     di, MCB_NAME
        xor     al, al
        mov     cx, 8
        rep     stosb
        pop     di

        pop     es
        pop     di
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; mem_alloc - ブロックを確保する
;   入力: BX = 必要なパラグラフ数 (0xFFFF なら「空いているだけ全部」)
;   出力: CF=0 なら AX = 確保したブロックの先頭セグメント
;         CF=1 なら AX = ERR_NOT_ENOUGH_MEM, BX = 確保できる最大パラグラフ数
;
; 戦略は AH=58h で切り替えられる (既定は first fit)。当時のプログラムには
; 「一度全部確保してから必要な分に縮める」という作法のものが多く、
; 最大空きサイズを正しく返せることが実質的な互換性条件になる。
; ---------------------------------------------------------------------------
mem_alloc:
        push    cx
        push    dx
        push    si
        push    di
        push    es

        mov     [.want], bx
        xor     dx, dx                  ; DX = 見つけた最大空きサイズ
        xor     si, si                  ; SI = 候補ブロックのセグメント (0 = なし)
        mov     ax, [first_mcb]

.scan:
        mov     es, ax
        mov     [.cur], ax

        cmp     byte [es:MCB_SIG], MCB_SIG_MID
        je      .valid
        cmp     byte [es:MCB_SIG], MCB_SIG_LAST
        je      .valid
        jmp     .corrupt

.valid:
        cmp     word [es:MCB_OWNER], MCB_OWNER_FREE
        jne     .next

        ; 空きブロック。隣が空きなら繋げてしまう (断片化を抑える)
        call    mem_coalesce

        mov     cx, [es:MCB_SIZE]
        cmp     cx, dx
        jbe     .not_bigger
        mov     dx, cx                  ; 最大空きサイズを更新
.not_bigger:

        cmp     cx, [.want]
        jb      .next                   ; 足りない

        ; 戦略に応じて候補を選ぶ
        ; 方針は下位 2bit だけを見る。上位のビットは「UMB を先に探すか」で、
        ; MYDOS は UMB を持たないので探し方は変わらない。
        push    ax
        mov     al, [alloc_strategy]
        and     al, 0x03
        cmp     al, FIT_FIRST
        pop     ax
        je      .take_now
        push    ax
        mov     al, [alloc_strategy]
        and     al, 0x03
        cmp     al, FIT_LAST
        pop     ax
        je      .remember               ; 最後に見つかったものを使う
        ; best fit: いま覚えているものより小さければ乗り換える
        test    si, si
        jz      .remember
        push    es
        mov     es, si
        mov     ax, [es:MCB_SIZE]
        pop     es
        cmp     cx, ax
        jae     .next
.remember:
        mov     si, [.cur]
        jmp     .next
.take_now:
        mov     si, [.cur]
        jmp     .found

.next:
        cmp     byte [es:MCB_SIG], MCB_SIG_LAST
        je      .scan_done
        mov     ax, [.cur]
        add     ax, [es:MCB_SIZE]
        inc     ax                      ; MCB 自身の 1 パラグラフ
        jmp     .scan

.scan_done:
        test    si, si
        jz      .no_space

.found:
        mov     es, si

        ; 0xFFFF は「空いているだけ全部」の意味。実際の大きさに読み替える。
        mov     bx, [.want]
        cmp     bx, 0xFFFF
        jne     .have_size
        mov     bx, [es:MCB_SIZE]
.have_size:

        ; 余りが 2 パラグラフ以上あるなら分割する。
        ; 1 パラグラフしか余らない場合は MCB を置く場所がないので分割しない。
        mov     cx, [es:MCB_SIZE]
        sub     cx, bx                  ; CX = 余り
        cmp     cx, 1
        jbe     .no_split

        call    mem_split

.no_split:
        mov     word [es:MCB_OWNER], 0xFFFF     ; 呼び出し側があとで PSP を入れる
        mov     ax, si
        inc     ax                      ; MCB の次のパラグラフが本体
        clc
        jmp     .out

.no_space:
        mov     ax, ERR_NOT_ENOUGH_MEM
        mov     bx, dx                  ; 確保できる最大サイズを教える
        stc
        jmp     .out

.corrupt:
        ; どのブロックで連鎖が切れたかを出す。「メモリが足りない」と
        ; 「連鎖が壊れている」は症状が似ているので、番地と印の値が
        ; 分からないと追いようがない。
%ifdef INT21_TRACE_ALL
        push    ax
        push    bx
        push    ds
        push    cs
        pop     ds
        mov     al, '!'
        call    trace_putc
        mov     bx, [.cur]
        call    trace_hexw
        mov     al, '='
        call    trace_putc
        push    es
        mov     es, bx
        mov     al, [es:MCB_SIG]
        pop     es
        push    ax
        shr     al, 4
        call    trace_hexd
        pop     ax
        call    trace_hexd
        mov     al, '!'
        call    trace_putc
        pop     ds
        pop     bx
        pop     ax
%endif
        mov     ax, ERR_MCB_DESTROYED
        xor     bx, bx
        stc

.out:
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        ret
.want:  dw 0
.cur:   dw 0

; ---------------------------------------------------------------------------
; mem_split - ES が指す MCB のブロックを BX パラグラフに切り詰め、
;             残りを新しい空きブロックにする
;   入力: ES = MCB のセグメント, BX = 残す大きさ (パラグラフ)
;   前提: [es:MCB_SIZE] > BX + 1
; ---------------------------------------------------------------------------
mem_split:
        push    ax
        push    cx
        push    di
        push    es
        push    ds

        mov     cx, [es:MCB_SIZE]
        sub     cx, bx
        dec     cx                      ; 新しい MCB のぶんを引く
        mov     al, [es:MCB_SIG]        ; 元の連鎖上の位置を引き継ぐ

        ; 新しい MCB の位置 = 元の MCB + 1 + BX
        mov     di, es
        add     di, bx
        inc     di

        push    es
        mov     es, di
        mov     byte [es:MCB_SIG], al   ; 元が 'Z' なら新しいほうが 'Z'
        mov     word [es:MCB_OWNER], MCB_OWNER_FREE
        mov     [es:MCB_SIZE], cx
        pop     es

        mov     byte [es:MCB_SIG], MCB_SIG_MID  ; 元のブロックは中間になる
        mov     [es:MCB_SIZE], bx

        pop     ds
        pop     es
        pop     di
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; mem_coalesce - ES の空きブロックに、後ろに続く空きブロックを取り込む
;   入力: ES = 空きブロックの MCB
; ---------------------------------------------------------------------------
mem_coalesce:
        push    ax
        push    bx
        push    cx
        push    ds

.loop:
        cmp     byte [es:MCB_SIG], MCB_SIG_LAST
        je      .done                   ; 最後のブロックなら後ろはない

        ; 次の MCB の位置
        mov     ax, es
        add     ax, [es:MCB_SIZE]
        inc     ax
        mov     ds, ax

        cmp     word [ds:MCB_OWNER], MCB_OWNER_FREE
        jne     .done                   ; 次が使用中なら取り込めない
        cmp     byte [ds:MCB_SIG], MCB_SIG_MID
        je      .merge
        cmp     byte [ds:MCB_SIG], MCB_SIG_LAST
        jne     .done                   ; 連鎖が壊れている

.merge:
        mov     bl, [ds:MCB_SIG]
        mov     cx, [ds:MCB_SIZE]
        inc     cx                      ; 取り込む MCB 自身のぶん
        add     cx, [es:MCB_SIZE]
        mov     [es:MCB_SIZE], cx
        mov     [es:MCB_SIG], bl        ; 'Z' を引き継ぐ
        jmp     .loop

.done:
        pop     ds
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; mcb_in_chain - その MCB が本当に連鎖の上にあるか確かめる
;   入力: AX = MCB のセグメント
;   出力: CF=0 なら連鎖の上にある
;
; ES-1 の 1 バイトが 'M' か 'Z' に見えるかどうかだけで判断すると、
; プログラムが渡してきた出鱈目なセグメントを受け入れてしまう。そこが
; たまたま 4Dh か 5Ah なら「正しいブロック」として持ち主を 0 にするので、
; 誰かのデータの真ん中に空きブロックがあることになり、次の確保で
; アリーナごと壊れる。
;
; 実際、CuteMouse は自分の PSP の中を指すセグメントで解放を頼んでくる。
; そこには 0 で埋めた PSP の一部があるだけなのだが、直前の INT 21h が
; 呼び出し元の SS:SP を PSP に書き戻すため、その 1 バイトが 'M' に化ける
; ことがある。そうなると解放が通ってしまい、次の確保で
; 「MCB が壊れている」と返って常駐が失敗する。
;
; 連鎖を頭から辿って一致するものがあるかを見れば、この種の取り違えは
; すべて弾ける。ブロックの数はたかだか数十なので、費用は問題にならない。
; ---------------------------------------------------------------------------
mcb_in_chain:
        push    bx
        push    cx
        push    es
        mov     bx, [first_mcb]
        mov     cx, MCB_CHAIN_MAX
.loop:
        cmp     bx, ax
        je      .found
        mov     es, bx
        cmp     byte [es:MCB_SIG], MCB_SIG_MID
        jne     .no                     ; 'Z' なら終わり、それ以外は壊れている
        add     bx, [es:MCB_SIZE]
        inc     bx
        loop    .loop
.no:
        pop     es
        pop     cx
        pop     bx
        stc
        ret
.found:
        pop     es
        pop     cx
        pop     bx
        clc
        ret

; ---------------------------------------------------------------------------
; mem_free - ブロックを解放する
;   入力: ES = ブロック本体のセグメント (MCB ではない)
;   出力: CF=1 なら AX = エラーコード
; ---------------------------------------------------------------------------
mem_free:
        push    bx
        push    es

        mov     bx, es
        dec     bx                      ; MCB は本体の 1 つ手前
        mov     es, bx

        cmp     byte [es:MCB_SIG], MCB_SIG_MID
        je      .maybe
        cmp     byte [es:MCB_SIG], MCB_SIG_LAST
        je      .maybe
.bad:
        mov     ax, ERR_BAD_MCB
        stc
        jmp     .out
.maybe:
        ; 印が合っているだけでは足りない。連鎖の上にあることまで確かめる。
        push    ax
        mov     ax, bx
        call    mcb_in_chain
        pop     ax
        jc      .bad
        mov     word [es:MCB_OWNER], MCB_OWNER_FREE
        call    mem_coalesce
        clc
.out:
        pop     es
        pop     bx
        ret

; ---------------------------------------------------------------------------
; mem_resize - ブロックの大きさを変える
;   入力: ES = ブロック本体のセグメント, BX = 新しい大きさ (パラグラフ)
;   出力: CF=1 なら AX = エラー, BX = 実際に取れる最大サイズ
;
; .COM のローダが「空き全部を確保 → 実際に必要な分へ縮める」という
; 使い方をするので、縮小は必ず成功しなければならない。
;
; DS はこの関数の中で常にカーネルを指したままにし、注目している MCB は
; ES に載せ替えて扱う。MCB を辿るのに DS を使うと、その間だけ自分の変数が
; 読めなくなって話がこじれる。
; ---------------------------------------------------------------------------
mem_resize:
        push    cx
        push    dx
        push    es

        mov     dx, es
        dec     dx                      ; MCB は本体の 1 つ手前
        mov     [.mcb], dx
        mov     es, dx

        cmp     byte [es:MCB_SIG], MCB_SIG_MID
        je      .maybe
        cmp     byte [es:MCB_SIG], MCB_SIG_LAST
        je      .maybe
.bad_mcb:
        mov     ax, ERR_BAD_MCB
        stc
        jmp     .out
.maybe:
        push    ax
        mov     ax, dx                  ; DX = この MCB のセグメント
        call    mcb_in_chain
        pop     ax
        jc      .bad_mcb
.ok:
        mov     cx, [es:MCB_SIZE]       ; CX = 今の大きさ
        cmp     bx, cx
        je      .same
        ja      .grow

        ; --- 縮小 ---
        mov     dx, cx
        sub     dx, bx
        cmp     dx, 1
        jbe     .same                   ; 1 パラグラフでは MCB を置けない
        call    mem_split
        ; 切り離した側を、さらに後ろの空きと繋げておく
        mov     dx, [.mcb]
        add     dx, bx
        inc     dx
        mov     es, dx
        call    mem_coalesce
        mov     es, [.mcb]
        jmp     .same

        ; --- 拡大 ---
.grow:
        cmp     byte [es:MCB_SIG], MCB_SIG_LAST
        je      .nogrow                 ; 後ろに何もない

        mov     dx, [.mcb]
        add     dx, cx
        inc     dx
        mov     es, dx                  ; ES = 次の MCB
        cmp     word [es:MCB_OWNER], MCB_OWNER_FREE
        jne     .nogrow_restore

        call    mem_coalesce            ; 後続の空き同士を先にまとめる
        mov     dx, [es:MCB_SIZE]
        mov     [.nsize], dx
        mov     al, [es:MCB_SIG]
        mov     [.nsig], al
        mov     es, [.mcb]

        mov     dx, [.nsize]
        inc     dx                      ; 取り込む MCB 自身の 1 パラグラフ
        add     dx, cx                  ; DX = 取り込んだ後の大きさ
        cmp     dx, bx
        jb      .nogrow_dx

        mov     [es:MCB_SIZE], dx
        mov     al, [.nsig]
        mov     [es:MCB_SIG], al
        ; 余りがあれば切り離す
        sub     dx, bx
        cmp     dx, 1
        jbe     .same
        call    mem_split
        jmp     .same

.nogrow_restore:
        mov     es, [.mcb]
.nogrow:
        mov     bx, cx                  ; いまの大きさが限界
        mov     ax, ERR_NOT_ENOUGH_MEM
        stc
        jmp     .out
.nogrow_dx:
        mov     bx, dx                  ; ここまでなら伸ばせる
        mov     ax, ERR_NOT_ENOUGH_MEM
        stc
        jmp     .out

.same:
        clc
.out:
        pop     es
        pop     dx
        pop     cx
        ret
.mcb:   dw 0
.nsize: dw 0
.nsig:  db 0

; ---------------------------------------------------------------------------
; mem_free_owner - 指定した PSP が持っているブロックをすべて解放する
;   入力: AX = PSP セグメント
;
; プログラムが終了したとき、確保したまま放置したメモリを回収するために使う。
; これがないと COMMAND.COM からプログラムを起動するたびにメモリが減っていく。
; ---------------------------------------------------------------------------
mem_free_owner:
        push    ax
        push    bx
        push    cx
        push    es

        mov     cx, ax                  ; CX = 探す PSP
        mov     ax, [first_mcb]
.scan:
        mov     es, ax
        cmp     byte [es:MCB_SIG], MCB_SIG_MID
        je      .valid
        cmp     byte [es:MCB_SIG], MCB_SIG_LAST
        je      .valid
        jmp     .done                   ; 連鎖が壊れている。これ以上触らない
.valid:
        cmp     [es:MCB_OWNER], cx
        jne     .next
        mov     word [es:MCB_OWNER], MCB_OWNER_FREE
.next:
        cmp     byte [es:MCB_SIG], MCB_SIG_LAST
        je      .compact
        add     ax, [es:MCB_SIZE]
        inc     ax
        jmp     .scan

.compact:
        ; 解放した結果として隣り合った空きを繋げ直す
        mov     ax, [first_mcb]
.comp_loop:
        mov     es, ax
        cmp     word [es:MCB_OWNER], MCB_OWNER_FREE
        jne     .comp_next
        call    mem_coalesce
.comp_next:
        cmp     byte [es:MCB_SIG], MCB_SIG_LAST
        je      .done
        add     ax, [es:MCB_SIZE]
        inc     ax
        jmp     .comp_loop

.done:
        pop     es
        pop     cx
        pop     bx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; mem_largest - 確保できる最大のブロックサイズを調べる
;   出力: AX = パラグラフ数
; ---------------------------------------------------------------------------
mem_largest:
        push    bx
        push    cx
        push    es

        xor     cx, cx
        mov     bx, [first_mcb]
.scan:
        mov     es, bx
        cmp     byte [es:MCB_SIG], MCB_SIG_MID
        je      .valid
        cmp     byte [es:MCB_SIG], MCB_SIG_LAST
        je      .valid
        jmp     .done
.valid:
        cmp     word [es:MCB_OWNER], MCB_OWNER_FREE
        jne     .next
        call    mem_coalesce
        cmp     [es:MCB_SIZE], cx
        jbe     .next
        mov     cx, [es:MCB_SIZE]
.next:
        cmp     byte [es:MCB_SIG], MCB_SIG_LAST
        je      .done
        add     bx, [es:MCB_SIZE]
        inc     bx
        jmp     .scan
.done:
        mov     ax, cx
        pop     es
        pop     cx
        pop     bx
        ret

; --- 変数 ------------------------------------------------------------------
first_mcb:      dw 0
%ifdef CHAIN_CHECK
mcb_canary:     dw 0
%endif            ; 連鎖の先頭 MCB のセグメント
alloc_strategy: db FIT_FIRST
