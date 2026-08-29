; ============================================================================
; exec.inc  -  PSP の構築、.COM / .EXE のロード、EXEC と終了処理
;
; ここが「当時のバイナリが動くかどうか」の分かれ目になる。
;
; .COM は 64KB 単一セグメントに素直に置くだけだが、.EXE は
;   * MZ ヘッダの解析
;   * リロケーションテーブルの適用 (セグメント値の埋め込み直し)
;   * min_alloc / max_alloc に従った実メモリの確保
; が要る。とくに 3 番目が曲者で、当時の .EXE の多くは max_alloc = 0xFFFF、
; つまり「空きメモリを全部よこせ」と要求してくる。MCB 連鎖がメモリ上に
; 本物の形で存在していないとここで破綻する。
; ============================================================================

; --- MZ ヘッダのオフセット -------------------------------------------------
MZ_SIGNATURE    equ 0x00        ; 'MZ' (0x5A4D)
MZ_LASTPAGE     equ 0x02        ; word: 最終ページの使用バイト数 (0 なら 512)
MZ_PAGES        equ 0x04        ; word: 512 バイト単位のページ数
MZ_RELOCS       equ 0x06        ; word: リロケーションエントリ数
MZ_HDRPARAS     equ 0x08        ; word: ヘッダの大きさ (パラグラフ)
MZ_MINALLOC     equ 0x0A        ; word: 最低限必要な追加メモリ (パラグラフ)
MZ_MAXALLOC     equ 0x0C        ; word: 欲しい追加メモリ (パラグラフ)
MZ_INITSS       equ 0x0E        ; word: 初期 SS (ロードセグメントからの相対)
MZ_INITSP       equ 0x10        ; word: 初期 SP
MZ_CHECKSUM     equ 0x12
MZ_INITIP       equ 0x14        ; word: 初期 IP
MZ_INITCS       equ 0x16        ; word: 初期 CS (ロードセグメントからの相対)
MZ_RELOCTAB     equ 0x18        ; word: リロケーションテーブルのファイル内位置
MZ_OVERLAY      equ 0x1A
MZ_HDRSIZE      equ 0x1C

MZ_MAGIC        equ 0x5A4D      ; 'MZ' (リトルエンディアン)
MZ_MAGIC_ALT    equ 0x4D5A      ; 'ZM' — 一部のリンカが吐く

; --- EXEC のパラメータブロック (ES:BX が指す構造体) ------------------------
EPB_ENVSEG      equ 0x00        ; word: 環境セグメント (0 なら親のを継ぐ)
EPB_CMDTAIL     equ 0x02        ; dword: コマンドテイルへのポインタ
EPB_FCB1        equ 0x06        ; dword
EPB_FCB2        equ 0x0A        ; dword
; AL=1 (ロードのみ) のとき、DOS がここに結果を書き戻す
EPB_INITSP      equ 0x0E        ; word: 初期 SP
EPB_INITSS      equ 0x10        ; word: 初期 SS
EPB_INITIP      equ 0x12        ; word: 初期 IP
EPB_INITCS      equ 0x14        ; word: 初期 CS
; AL=3 (オーバーレイ) のときは意味が変わる
EPB_OVL_SEG     equ 0x00        ; word: 読み込み先セグメント
EPB_OVL_FACTOR  equ 0x02        ; word: リロケーション係数

; ---------------------------------------------------------------------------
; build_psp - PSP を組み立てる
;   入力: AX = PSP セグメント
;         BX = メモリ上限セグメント (この PSP が使える最後 + 1)
;         CX = 親の PSP セグメント (0 なら自分自身を親にする)
;         DX = 環境ブロックのセグメント
;
; 当時のプログラムがここを直接読みにくるので、使われないフィールドも
; 本物と同じ値で埋めておく。とくに +2 (メモリ上限) と +2C (環境) と
; +80 (コマンドライン) は、C ランタイムの起動コードが必ず参照する。
; ---------------------------------------------------------------------------
build_psp:
        pusha
        push    es

        mov     [.psp], ax
        mov     [.memtop], bx
        mov     [.parent], cx
        mov     [.env], dx

        mov     es, ax
        xor     di, di
        mov     cx, PSP_SIZE
        xor     al, al
        rep     stosb                   ; まず全部 0

        ; +00: INT 20h。ここへ far jump すればプログラムが終了する
        mov     word [es:PSP_INT20], 0x20CD

        ; +02: 使えるメモリの上限セグメント
        mov     ax, [.memtop]
        mov     [es:PSP_MEMTOP], ax

        ; +05: CP/M 形式の呼び出し口への far call。
        ;
        ; CP/M からの移植ものは、DOS を INT 21h ではなく「PSP:0005 への
        ; near call」で呼ぶ (機能番号は CL)。ここが本物の入口を指していないと、
        ; そういうプログラムは DOS の代わりに出鱈目な番地へ飛ぶ。
        ; 飛び先の 0000:00C0 には install_vectors が jmp far を置いてある。
        ;
        ; GEM のデスクトップがこれだった。1980 年代のプログラムなので
        ; call 5 を使っており、ここが BIOS を指していたために、
        ; 背景とタイトルバーを描いたところで BIOS の中へ飛んで行方不明に
        ; なっていた。
        mov     byte [es:PSP_CPM_CALL], 0x9A
        mov     word [es:PSP_CPM_CALL + 1], 0x00C0
        mov     word [es:PSP_CPM_CALL + 3], 0x0000

        ; +0A/0E/12: 終了・Ctrl-C・クリティカルエラーのベクタを保存する。
        ; プログラムがこれらを書き換えたまま終了しても、DOS が PSP から
        ; 元の値を書き戻すので前のプログラムに影響が残らない。
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     eax, [INT_TERM_ADDR * 4]
        mov     [es:PSP_INT22], eax
        mov     eax, [INT_CTRLC * 4]
        mov     [es:PSP_INT23], eax
        mov     eax, [INT_CRITERR * 4]
        mov     [es:PSP_INT24], eax
        pop     ds

        ; +16: 親の PSP
        mov     ax, [.parent]
        test    ax, ax
        jnz     .have_parent
        mov     ax, [.psp]              ; 親がいない (COMMAND.COM) なら自分
.have_parent:
        mov     [es:PSP_PARENT], ax

        ; +18: JFT。親から引き継ぐ (これがリダイレクトの継承になる)
        mov     cx, JFT_ENTRIES
        mov     di, PSP_JFT
        mov     ax, [.parent]
        test    ax, ax
        jz      .default_jft

        push    ds
        mov     ds, ax
        mov     si, PSP_JFT
        rep     movsb                   ; 親の JFT をそのまま写す
        pop     ds
        jmp     .jft_done

.default_jft:
        ; 親がいない場合は 0-2 を CON、3 を AUX、4 を PRN にする
        mov     al, JFT_FREE
        rep     stosb
        mov     byte [es:PSP_JFT + 0], 0        ; stdin  -> SFT 0 (CON)
        mov     byte [es:PSP_JFT + 1], 0        ; stdout -> SFT 0 (CON)
        mov     byte [es:PSP_JFT + 2], 0        ; stderr -> SFT 0 (CON)
        mov     byte [es:PSP_JFT + 3], 1        ; aux    -> SFT 1
        mov     byte [es:PSP_JFT + 4], 2        ; prn    -> SFT 2
.jft_done:

        ; +2C: 環境ブロック
        mov     ax, [.env]
        mov     [es:PSP_ENVSEG], ax

        ; +32/34: JFT の大きさと位置 (AH=67h でハンドル数を増やすときに使う)
        mov     word [es:PSP_JFT_SIZE], JFT_ENTRIES
        mov     word [es:PSP_JFT_PTR], PSP_JFT
        mov     ax, [.psp]
        mov     [es:PSP_JFT_PTR + 2], ax

        ; +50: INT 21h + RETF。DOS を far call で呼ぶ古い作法のため
        mov     word [es:PSP_DISPATCH], 0x21CD
        mov     byte [es:PSP_DISPATCH + 2], 0xCB

        ; +5C/6C: FCB 領域は空白で埋めておく
        mov     di, PSP_FCB1
        mov     cx, 36
        xor     al, al
        rep     stosb

        ; +80: コマンドテイルは空 (呼び出し側があとで入れる)
        mov     byte [es:PSP_CMDTAIL_LEN], 0
        mov     byte [es:PSP_CMDTAIL], 0x0D

        pop     es
        popa
        ret
.psp:     dw 0
.memtop:  dw 0
.parent:  dw 0
.env:     dw 0

; ---------------------------------------------------------------------------
; psp_set_cmdtail - PSP にコマンドラインを入れる
;   入力: AX = PSP セグメント, DS:SI = "長さバイト + 文字列" (DOS の形式)
;
; DOS のコマンドテイルは「先頭 1 バイトが長さ、続いて本文、末尾が 0x0D」。
; プログラムから見た argv はここを自分で切り分けたもの。
; ---------------------------------------------------------------------------
psp_set_cmdtail:
        push    ax
        push    cx
        push    si
        push    di
        push    es

        mov     es, ax
        movzx   cx, byte [si]
        cmp     cx, 127
        jbe     .len_ok
        mov     cx, 127
.len_ok:
        mov     [es:PSP_CMDTAIL_LEN], cl
        inc     si
        mov     di, PSP_CMDTAIL
        jcxz    .no_body
        rep     movsb
.no_body:
        mov     byte [es:di], 0x0D

        pop     es
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; build_env - 環境ブロックを作る
;   入力: AX = 環境ブロックのセグメント
;
; 形式は「NAME=VALUE を 0 終端で並べ、最後に 0 をもう 1 つ」。
; そのあとに word 1 とプログラムのフルパスが続く (DOS 3.0 以降)。
; C の getenv() と argv[0] の取得がここに依存している。
; ---------------------------------------------------------------------------
build_env:
        push    ax
        push    cx
        push    si
        push    di
        push    es

        mov     es, ax
        xor     di, di
        mov     si, env_template
        mov     cx, env_template_len
        rep     movsb

        pop     es
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; exec_save_fullpath - path_buf を正規化して exec_fullpath に控える
;
; path_buf には copy_path が入れた「呼び出し側が指定したパス」が入って
; いる。相対パスやドライブ省略のこともあるので、truename で
; "C:\SUB\PROG.EXE" の形に開いてから覚える。
; ---------------------------------------------------------------------------
exec_save_fullpath:
        push    ax
        push    cx
        push    si
        push    di
        push    es
        push    ds
        pop     es

        call    truename                ; path_buf -> path_buf2
        mov     si, path_buf2
        mov     di, exec_fullpath
        mov     cx, 127
.copy:
        lodsb
        stosb
        test    al, al
        jz      .done
        loop    .copy
        xor     al, al
        stosb
.done:
        pop     es
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; env_make_child - 子プロセス用の環境ブロックを作る
;   入力: DX = 元にする環境ブロックのセグメント (0 なら中身なし)
;         AX = 子の PSP セグメント (確保したブロックの持ち主にする)
;   出力: DX = 新しい環境ブロックのセグメント
;
; DOS は親の環境をそのまま子に渡さない。子ごとに写しを作り、終わりの
; 二重 0 のあとに word 1 と「起動したプログラムのフルパス」を置く。
; C の argv[0] はここから来るし、DOS エクステンダのように自分の EXE を
; 開き直す種類のプログラムは、この文字列だけを頼りに自分を探す。
; 親のものを使い回すと、子は親のパスを自分のパスだと思い込む。
;
; 作れなかったときは入力の DX をそのまま返す (起動自体は続ける)。
; ---------------------------------------------------------------------------
env_make_child:
        mov     byte [cs:env_child_made], 0
        push    ax
        push    bx
        push    cx
        push    si
        push    di
        push    es
        push    ds

        mov     [.psp], ax
        mov     [.src], dx

        ; --- 元の環境の長さを測る (二重 0 まで) ---
        xor     si, si
        test    dx, dx
        jz      .measured
        mov     ds, dx
.scan:
        cmp     si, 0x7FF0              ; 壊れていても暴走しないように
        jae     .measured
        cmp     byte [si], 0
        je      .scan_end
.scan_str:
        inc     si
        cmp     byte [si], 0
        jne     .scan_str
        inc     si                      ; 文字列の終端 0 を飛ばす
        jmp     .scan
.scan_end:
        inc     si                      ; 全体の終わりを示す 0 も含める
.measured:
        push    cs
        pop     ds
        mov     [.envlen], si

        ; --- フルパスの長さ ---
        mov     di, exec_fullpath
        xor     cx, cx
.plen:
        cmp     byte [di], 0
        je      .plen_done
        inc     di
        inc     cx
        jmp     .plen
.plen_done:
        inc     cx                      ; 終端の 0
        mov     [.pathlen], cx

        ; --- 必要なパラグラフ数 ---
        mov     ax, [.envlen]
        add     ax, 2                   ; word 1
        add     ax, cx
        add     ax, 15
        shr     ax, 4
        mov     bx, ax
        call    mem_alloc
        jc      .give_up
        mov     [.dst], ax
        mov     byte [env_child_made], 1

        ; 持ち主。ここで 0 を入れてはいけない。MCB の所有者 0 は「空き」を
        ; 意味するので、すぐあとに来るプログラム本体の確保がこのブロックを
        ; 空きとみなして掴み、作ったばかりの環境を上書きしてしまう。
        ; PSP はまだ無いので、mem_alloc が入れた仮の持ち主 (0xFFFF) を
        ; そのまま残し、.make_psp で本物に入れ替える。
        cmp     word [.psp], 0
        je      .owner_later
        mov     bx, ax
        dec     bx
        mov     es, bx
        mov     bx, [.psp]
        mov     [es:MCB_OWNER], bx
.owner_later:

        ; --- 元の環境を写す ---
        mov     es, ax
        xor     di, di
        mov     cx, [.envlen]
        jcxz    .no_src
        mov     ds, [.src]
        xor     si, si
        rep     movsb
.no_src:
        push    cs
        pop     ds

        ; --- word 1 とフルパス ---
        mov     ax, 1
        stosw
        mov     si, exec_fullpath
        mov     cx, [.pathlen]
        rep     movsb

        mov     dx, [.dst]
        jmp     .out
.give_up:
        mov     dx, [.src]
.out:
        pop     ds
        pop     es
        pop     di
        pop     si
        pop     cx
        pop     bx
        pop     ax
        ret
.psp:      dw 0
.src:      dw 0
.dst:      dw 0
.envlen:   dw 0
.pathlen:  dw 0

; 起動しようとしているプログラムの正規化したフルパス。
exec_fullpath:  times 128 db 0

; 子に渡す環境ブロック。プログラム本体より先に確保する。
exec_child_env: dw 0
env_child_made: db 0            ; 1 なら env_make_child が確保した

env_template:
        db 'COMSPEC=A:\COMMAND.COM', 0
        db 'PATH=A:\', 0
        db 'PROMPT=$p$g', 0
        db 0                            ; 環境の終わり
        dw 1                            ; 続きがある印
        db 'A:\COMMAND.COM', 0          ; 起動したプログラムのフルパス
env_template_len equ $ - env_template
ENV_PARAS       equ (env_template_len + 15) / 16

; ---------------------------------------------------------------------------
; file_read_big - ファイルからセグメント境界をまたいで読み込む
;   入力: BX = ハンドル, ECX = 読むバイト数, DX = 転送先セグメント
;   出力: CF=1 なら失敗
;
; file_read は 16bit のオフセットしか扱えないので、512 バイトずつ読んでは
; セグメントを 32 パラグラフ進める。これなら 64KB を超えるプログラムも
; そのまま読める。
; ---------------------------------------------------------------------------
file_read_big:
        push    ax
        push    cx
        push    dx
        push    di
        push    es

        mov     [.left], ecx
        mov     [.seg], dx

.loop:
        cmp     dword [.left], 0
        je      .done

        mov     ecx, [.left]
        cmp     ecx, SECTOR_SIZE
        jbe     .last
        mov     ecx, SECTOR_SIZE
.last:
        mov     es, [.seg]
        xor     di, di
        call    file_read
        jc      .fail
        test    ax, ax
        jz      .done                   ; これ以上読めない (EOF)

        movzx   ecx, ax
        sub     [.left], ecx

        ; 読んだバイト数のぶんだけセグメントを進める
        add     ax, 15
        mov     cl, 4
        shr     ax, cl
        add     [.seg], ax
        jmp     .loop

.done:
        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     ax
        clc
        ret
.fail:
        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     ax
        stc
        ret
.left:  dd 0
.seg:   dw 0

; ---------------------------------------------------------------------------
; prog_load - プログラムを読み込んで実行できる状態にする
;   入力: DS:SI = パス, [exec_cmdtail] にコマンドテイルへの far ポインタ
;         [exec_envseg] に環境セグメント (0 なら親のを継ぐ)
;   出力: CF=0 なら
;           [exec_psp]   = 作った PSP のセグメント
;           [exec_entry] = 入口の far アドレス
;           [exec_stack] = 初期 SS:SP
;         CF=1 なら AX = エラーコード
; ---------------------------------------------------------------------------
prog_load:
        push    bx
        push    cx
        push    dx
        push    di
        push    es
%ifdef CHAIN_CHECK
        mov     byte [chain_where], 'A'
        call    chain_check
%endif

        ; 子に渡すフルパスを、path_buf がまだ残っているうちに控える。
        call    exec_save_fullpath

        ; --- 環境ブロックを先に作る ---
        ;
        ; .COM は空きの一番大きいブロックを丸ごと取るし、.EXE も
        ; maxalloc=FFFF なら同じことをする。本体を置いたあとでは
        ; 環境ブロックのぶんが残らないので、順番はここでなければならない。
        push    dx                      ; DS:DX = パス。file_open がまだ使う
        mov     dx, [exec_envseg]
        test    dx, dx
        jnz     .env_src_ok
        push    es
        mov     es, [cur_psp]
        mov     dx, [es:PSP_ENVSEG]
        pop     es
.env_src_ok:
        xor     ax, ax                  ; 持ち主は PSP ができてから入れる
        call    env_make_child
        mov     [exec_child_env], dx
        pop     dx
%ifdef CHAIN_CHECK
        mov     byte [chain_where], 'B'
        call    chain_check
%endif

        ; --- ファイルを開く ---
        xor     al, al                  ; 読み取り専用
        call    file_open
        jc      .err
        mov     [.handle], ax
        mov     bx, ax

        ; --- 先頭 28 バイトを読んで .EXE かどうか見る ---
        push    ds
        pop     es
        mov     di, exe_header
        mov     cx, MZ_HDRSIZE
        call    file_read
        jc      .read_err
        mov     [.hdrlen], ax
%ifdef CHAIN_CHECK
        mov     byte [chain_where], 'C'
        call    chain_check
%endif

        cmp     ax, MZ_HDRSIZE
        jb      .as_com                 ; 28 バイトもないなら .COM
        mov     ax, [exe_header + MZ_SIGNATURE]
        cmp     ax, MZ_MAGIC
        je      .as_exe
        cmp     ax, MZ_MAGIC_ALT
        je      .as_exe

; ===========================================================================
; .COM: PSP の直後 (PSP:0100) にそのまま置くだけ
; ===========================================================================
.as_com:
        ; 空きの一番大きいブロックを丸ごと取る (DOS と同じ振る舞い)
        call    mem_largest
        mov     bx, ax
        cmp     bx, (PSP_SIZE / 16) + 1
        jb      .no_mem
        call    mem_alloc
        jc      .no_mem
        mov     [.psp], ax
        mov     [.paras], bx
        call    .claim_block

        ; PSP を作る
        mov     cx, bx
        add     cx, ax                  ; CX = 使える上限セグメント
        mov     [.memtop], cx
        call    .make_psp

        ; ファイルを頭から読み直して PSP:0100 へ
        mov     bx, [.handle]
        xor     al, al
        xor     cx, cx
        xor     dx, dx
        call    file_seek               ; 先頭に戻す

        ; PSP:0100 は「PSP セグメント + 16 パラグラフ」の先頭に等しい
        mov     dx, [.psp]
        add     dx, PSP_SIZE / 16
        mov     ecx, 0x0000FF00         ; .COM は最大 65280 バイト
        mov     bx, [.handle]
        call    file_read_big
        jc      .read_err

        ; 入口は PSP:0100
        mov     ax, [.psp]
        mov     word [exec_entry], 0x0100
        mov     [exec_entry + 2], ax

        ; スタックはブロックの末尾。64KB を超える場合は 0xFFFE で止める
        mov     bx, [.paras]
        cmp     bx, 0x1000
        jb      .com_small
        mov     word [.sp], 0xFFFE
        jmp     .com_stack
.com_small:
        mov     cl, 4
        shl     bx, cl
        sub     bx, 2
        mov     [.sp], bx
.com_stack:
        mov     [exec_stack + 2], ax    ; SS = PSP
        mov     bx, [.sp]
        mov     [exec_stack], bx

        ; スタックの底に 0 を積んでおく。.COM が ret で終われるようにする
        ; (ret すると PSP:0000 の INT 20h に飛ぶ)
        push    es
        mov     es, ax
        mov     di, [.sp]
        mov     word [es:di], 0
        pop     es

        jmp     .finish

; ===========================================================================
; .EXE: ヘッダを解析し、リロケーションを当ててから配置する
; ===========================================================================
.as_exe:
        ; --- イメージの大きさを求める ---
        ;   ページ数 * 512 - ヘッダ長。ただし最終ページが半端なら差し引く。
        movzx   eax, word [exe_header + MZ_PAGES]
        shl     eax, 9                  ; * 512
        movzx   ecx, word [exe_header + MZ_LASTPAGE]
        test    ecx, ecx
        jz      .full_page
        sub     eax, SECTOR_SIZE
        add     eax, ecx
.full_page:
        movzx   ecx, word [exe_header + MZ_HDRPARAS]
        shl     ecx, 4                  ; ヘッダのバイト数
        mov     [.hdrbytes], ecx
        sub     eax, ecx
        jbe     .bad_exe
        mov     [.imgsize], eax

        ; --- 場所をいくつ取るかを求める ---
        ;
        ; 読み込むバイト数からではなく、ページ数から数える。DOS は半端な
        ; 最終ページを切り上げ、512 バイト単位で場所を取るからである。
        ;
        ;   パラグラフ数 = ページ数 * 32 - ヘッダのパラグラフ数
        ;
        ; ここを「実際に読むバイト数を 16 で割った値」にすると、最終ページの
        ; 半端なぶんだけ確保が小さくなる。プログラムから見ると PSP:0002 が
        ; 本来より下を指すことになり、そこを上限として自分のスタックを
        ; 置くプログラム (当時の常駐ソフトの定石) が、すぐ上にある次の MCB
        ; を踏む。CuteMouse がまさにこれで、常駐したあとの確保がすべて
        ; 「MCB が壊れている」で返るようになっていた。
        ;
        ; 16bit で計算するのは意図的で、ページ数が 2048 以上のときに
        ; 上位が落ちる。1MB を超えるイメージを持つ NE の DOS スタブは
        ; 「1MB で割った余りだけ読む」作りになっているので、この桁あふれが
        ; そのまま正しい動きになる。
        mov     ax, [exe_header + MZ_PAGES]
        shl     ax, 5                   ; ページ数 * 32 = パラグラフ数
        sub     ax, [exe_header + MZ_HDRPARAS]
        jbe     .bad_exe
        mov     [.imgparas], ax

        ; 読む長さが確保した長さを超えないようにしておく
        movzx   ecx, ax
        shl     ecx, 4
        cmp     [.imgsize], ecx
        jbe     .size_ok
        mov     [.imgsize], ecx
.size_ok:

        ; --- 必要なメモリを決める ---
        ;   PSP(16) + イメージ + min_alloc が最低限、
        ;   PSP(16) + イメージ + max_alloc が理想。
        movzx   ecx, word [.imgparas]
        add     ecx, PSP_SIZE / 16
        movzx   eax, word [exe_header + MZ_MINALLOC]
        add     eax, ecx
        cmp     eax, 0xFFFF
        jbe     .min_ok
        mov     eax, 0xFFFF
.min_ok:
        mov     [.minparas], ax

        movzx   eax, word [exe_header + MZ_MAXALLOC]
        add     eax, ecx
        cmp     eax, 0xFFFF
        jbe     .max_ok
        mov     eax, 0xFFFF             ; 0xFFFF 指定は「空き全部」の意味
.max_ok:
        mov     [.maxparas], ax

        ; min も max も 0 のときは、DOS は空きをすべて渡し、その中の
        ; いちばん上にイメージを置く。RBIL に載っている作法で、これを
        ; 前提にしているプログラムが実在する。
        mov     ax, [exe_header + MZ_MINALLOC]
        or      ax, [exe_header + MZ_MAXALLOC]
        jnz     .alloc_ready
        mov     word [.maxparas], 0xFFFF
.alloc_ready:

%ifdef CHAIN_CHECK
        mov     byte [chain_where], 'D'
        call    chain_check
%endif
        call    mem_largest             ; AX = 確保できる最大
%ifdef CHAIN_CHECK
        push    ax
        mov     byte [chain_where], 'E'
        call    chain_check
        pop     ax
%endif
        cmp     ax, [.minparas]
        jb      .no_mem

        mov     bx, [.maxparas]
        cmp     bx, ax
        jbe     .want_ok
        mov     bx, ax                  ; 欲しい量より空きが少ないので空き全部
.want_ok:
        call    mem_alloc
        jc      .no_mem
        mov     [.psp], ax
        mov     [.paras], bx
        call    .claim_block

        mov     cx, bx
        add     cx, ax
        mov     [.memtop], cx
        call    .make_psp

        ; --- イメージ本体を読み込む ---
        mov     bx, [.handle]
        xor     al, al                  ; 起点 = ファイル先頭
        mov     edx, [.hdrbytes]        ; DX = 下位 16bit
        mov     ecx, edx
        shr     ecx, 16                 ; CX = 上位 16bit
        call    file_seek               ; CX:DX = ヘッダの直後へ
        jc      .read_err

        mov     ax, [.psp]
        add     ax, PSP_SIZE / 16
        ; min も max も 0 なら、確保したブロックのいちばん上に置く
        mov     cx, [exe_header + MZ_MINALLOC]
        or      cx, [exe_header + MZ_MAXALLOC]
        jnz     .load_low
        mov     cx, [.paras]
        sub     cx, PSP_SIZE / 16
        sub     cx, [.imgparas]
        jbe     .load_low
        add     ax, cx
.load_low:
        mov     [.loadseg], ax          ; イメージの置き場所
        mov     dx, ax
        mov     bx, [.handle]
        mov     ecx, [.imgsize]
        call    file_read_big
        jc      .read_err

        ; --- リロケーションを当てる ---
        call    .apply_relocs
        jc      .read_err

        ; --- 入口とスタックを決める ---
        mov     ax, [.loadseg]
        add     ax, [exe_header + MZ_INITCS]
        mov     [exec_entry + 2], ax
        mov     ax, [exe_header + MZ_INITIP]
        mov     [exec_entry], ax

        mov     ax, [.loadseg]
        add     ax, [exe_header + MZ_INITSS]
        mov     [exec_stack + 2], ax
        mov     ax, [exe_header + MZ_INITSP]
        mov     [exec_stack], ax

.finish:
        ; ファイルを閉じる
        mov     bx, [.handle]
        call    file_close

        mov     ax, [.psp]
        mov     [exec_psp], ax
        clc
        jmp     .out

.bad_exe:
        mov     ax, ERR_BAD_FORMAT
        jmp     .close_err
.no_mem:
        mov     ax, ERR_NOT_ENOUGH_MEM
        jmp     .close_err
.read_err:
        mov     ax, ERR_BAD_DATA
.close_err:
        push    ax
        mov     bx, [.handle]
        call    file_close
        pop     ax
        stc
        jmp     .out
.err:
        stc
.out:
        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        ret

; --- 確保したブロックの持ち主を、これから作る PSP にする ---
;
; mem_alloc は所有者を 0xFFFF (仮) にして返す。ここで本物の PSP を入れて
; おかないと、プログラムが終了しても mem_free_owner が持ち主を照合できず、
; 確保した領域がそのまま残り続ける。1 本走らせるたびにメモリが減っていく
; という形で表に出る。
.claim_block:
        push    ax
        push    cx
        push    di
        push    es

        mov     ax, [.psp]
        mov     cx, ax
        dec     cx
        mov     es, cx                  ; ES = このブロックの MCB
        mov     [es:MCB_OWNER], ax

        ; MCB の名前欄にプログラム名を入れておく (DOS 4.0 以降の作法。
        ; MEM の類が「どのプログラムが持っているか」を表示するのに使う)
        mov     di, MCB_NAME
        push    si
        mov     cx, 8
.name_loop:
        lodsb
        test    al, al
        jz      .name_pad
        cmp     al, '\'
        jne     .name_store
        ; パス区切りが出てきたら、そこまでを捨てて名前を取り直す
        mov     di, MCB_NAME
        mov     cx, 8
        jmp     .name_loop
.name_store:
        cmp     al, '.'
        je      .name_pad
        mov     [es:di], al
        inc     di
        dec     cx
        jcxz    .name_skip
        jmp     .name_loop
.name_skip:
        ; 8 文字を超えた分は読み飛ばす
        lodsb
        test    al, al
        jnz     .name_skip
        jmp     .name_done
.name_pad:
        jcxz    .name_done
        mov     byte [es:di], 0
        inc     di
        dec     cx
        jmp     .name_pad
.name_done:
        pop     si

        pop     es
        pop     di
        pop     cx
        pop     ax
        ret

; --- PSP を作り、コマンドテイルと環境を入れる ---
.make_psp:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    es

        ; 環境ブロックは prog_load の入口で作ってある。ここでは持ち主を
        ; 子の PSP にするだけ。入れておかないと、子が終わっても
        ; mem_free_owner が拾えず、起動のたびにメモリが減っていく。
        mov     dx, [exec_child_env]
        cmp     byte [env_child_made], 0
        je      .env_ok
        mov     bx, dx
        dec     bx
        mov     es, bx
        mov     bx, [.psp]
        mov     [es:MCB_OWNER], bx
.env_ok:

        mov     ax, [.psp]
        mov     bx, [.memtop]
        mov     cx, [cur_psp]
        call    build_psp

        ; コマンドテイル
        mov     ax, [exec_psp_cmdsrc]
        mov     dx, [exec_psp_cmdsrc + 2]
        test    dx, dx
        jz      .no_tail
        push    ds
        mov     ds, dx
        mov     si, ax
        mov     ax, [cs:prog_load.psp]
        call    psp_set_cmdtail
        pop     ds
.no_tail:

        ; コマンドテイルの最初の 2 語を解析して PSP:5C と PSP:6C に置く。
        ; DOS がここを埋めてくれる前提で書かれたプログラムがあるので、
        ; 空のままにはできない。
        mov     ax, [.psp]
        call    psp_set_fcbs

        pop     es
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; --- リロケーションテーブルを読んで、埋め込まれたセグメント値を直す ---
;
; .EXE の中のセグメント参照は「ロード先が 0 だったら」という前提で
; 書かれている。実際に置かれた場所を足して回るのがこの処理で、
; ここを飛ばすと far call や far ポインタが全部あらぬ方向を向く。
.apply_relocs:
        push    ax
        push    bx
        push    cx
        push    dx
        push    di
        push    es

        mov     cx, [exe_header + MZ_RELOCS]
        test    cx, cx
        jz      .reloc_done
        mov     [.nrelocs], cx

        ; テーブルの位置へシークする
        mov     bx, [prog_load.handle]
        xor     al, al
        xor     cx, cx
        mov     dx, [exe_header + MZ_RELOCTAB]
        call    file_seek
        jc      .reloc_fail

.reloc_loop:
        cmp     word [.nrelocs], 0
        je      .reloc_done

        ; 4 バイト (オフセット word + セグメント word) 読む
        push    ds
        pop     es
        mov     di, reloc_entry
        mov     cx, 4
        mov     bx, [prog_load.handle]
        call    file_read
        jc      .reloc_fail
        cmp     ax, 4
        jb      .reloc_done

        ; 直す場所 = ロードセグメント + テーブルのセグメント : オフセット
        mov     ax, [prog_load.loadseg]
        add     ax, [reloc_entry + 2]
        mov     es, ax
        mov     di, [reloc_entry]
        ; 通常はロード先セグメントを足すが、オーバーレイのときは
        ; 呼び出し元が指定したリロケーション係数を使う。
        mov     ax, [reloc_factor]
        cmp     ax, 0xFFFF
        jne     .use_factor
        mov     ax, [prog_load.loadseg]
.use_factor:
        add     [es:di], ax             ; 埋め込まれた値にセグメントを足す

        dec     word [.nrelocs]
        jmp     .reloc_loop

.reloc_done:
        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
.reloc_fail:
        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret
.nrelocs:  dw 0

.handle:    dw 0
.hdrlen:    dw 0
.psp:       dw 0
.paras:     dw 0
.memtop:    dw 0
.sp:        dw 0
.loadseg:   dw 0
.imgsize:   dd 0
.hdrbytes:  dd 0
.imgparas:  dw 0
.minparas:  dw 0
.maxparas:  dw 0

; ---------------------------------------------------------------------------
; psp_set_fcbs - コマンドテイルの最初の 2 語を PSP の FCB 欄に解析して入れる
;   入力: AX = PSP セグメント
;
; DOS は EXEC のときにこれをやる。AH=29h と同じ解析器を使う。
; ---------------------------------------------------------------------------
psp_set_fcbs:
        push    ax
        push    cx
        push    si
        push    di
        push    ds
        push    es

        mov     es, ax
        mov     ds, ax                  ; コマンドテイルも PSP の中にある

        mov     si, PSP_CMDTAIL
        mov     di, PSP_FCB1
        mov     byte [cs:f_29.opts], 0x01       ; 先頭の区切りを飛ばす
        call    fcb_parse

        mov     di, PSP_FCB2
        mov     byte [cs:f_29.opts], 0x01
        call    fcb_parse

        pop     es
        pop     ds
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; prog_load_overlay - オーバーレイを読み込む (AH=4Bh AL=3)
;   入力: DS:SI = パス, AX = ロード先セグメント, DX = リロケーション係数
;   出力: CF=1 なら AX = エラーコード
;
; PSP を作らず、メモリも確保しない。指定されたセグメントに中身を置いて
; リロケーションを当てるだけ。当時の大きめのアプリ (Turbo Pascal や
; Microsoft C のオーバーレイ) が、常駐部分から呼び出す形で使う。
; ---------------------------------------------------------------------------
prog_load_overlay:
        push    bx
        push    cx
        push    dx
        push    di
        push    es

        mov     [.loadseg], ax
        mov     [.relocfac], dx

        xor     al, al
        call    file_open
        jc      .err
        mov     [.handle], ax
        mov     bx, ax

        push    ds
        pop     es
        mov     di, exe_header
        mov     cx, MZ_HDRSIZE
        call    file_read
        jc      .read_err

        cmp     ax, MZ_HDRSIZE
        jb      .flat
        mov     ax, [exe_header + MZ_SIGNATURE]
        cmp     ax, MZ_MAGIC
        je      .mz
        cmp     ax, MZ_MAGIC_ALT
        jne     .flat

.mz:
        ; イメージの大きさを求める (prog_load と同じ計算)
        movzx   eax, word [exe_header + MZ_PAGES]
        shl     eax, 9
        movzx   ecx, word [exe_header + MZ_LASTPAGE]
        test    ecx, ecx
        jz      .full_page
        sub     eax, SECTOR_SIZE
        add     eax, ecx
.full_page:
        movzx   ecx, word [exe_header + MZ_HDRPARAS]
        shl     ecx, 4
        mov     [.hdrbytes], ecx
        sub     eax, ecx
        jbe     .bad
        mov     [.imgsize], eax

        ; ヘッダの直後へシークして本体を読む
        mov     bx, [.handle]
        xor     al, al
        mov     edx, [.hdrbytes]
        mov     ecx, edx
        shr     ecx, 16
        call    file_seek
        jc      .read_err

        mov     dx, [.loadseg]
        mov     bx, [.handle]
        mov     ecx, [.imgsize]
        call    file_read_big
        jc      .read_err

        ; リロケーションを当てる。足す値はロード先ではなく、
        ; 呼び出し元が指定したリロケーション係数。
        mov     ax, [.loadseg]
        mov     [prog_load.loadseg], ax
        mov     ax, [.relocfac]
        mov     [reloc_factor], ax
        mov     ax, [.handle]
        mov     [prog_load.handle], ax
        call    prog_load.apply_relocs
        mov     word [reloc_factor], 0xFFFF     ; 既定 (= ロード先を使う) に戻す
        jc      .read_err
%ifdef INT21_TRACE_ALL
        call    .trace_ovl
%endif
        jmp     .done

%ifdef INT21_TRACE_ALL
; 読み込んだオーバーレイの素性を出す (調査用)
.trace_ovl:
        pusha
        push    ds
        push    es
        push    cs
        pop     ds
        mov     al, '['
        call    trace_putc
        mov     bx, [.loadseg]
        call    trace_hexw
        mov     al, '+'
        call    trace_putc
        mov     bx, [.relocfac]
        call    trace_hexw
        mov     al, '+'
        call    trace_putc
        mov     bx, [.imgsize + 2]
        call    trace_hexw
        mov     bx, [.imgsize]
        call    trace_hexw
        mov     al, '='
        call    trace_putc
        mov     es, [.loadseg]
        xor     di, di
        xor     dx, dx
        mov     ecx, [.imgsize]
.sumloop:
        mov     al, [es:di]
        movzx   bx, al
        add     dx, bx
        rol     dx, 1
        inc     di
        jnz     .nowrap
        mov     bx, es
        add     bx, 0x1000
        mov     es, bx
.nowrap:
        dec     ecx
        jnz     .sumloop
        mov     bx, dx
        call    trace_hexw
        mov     al, ']'
        call    trace_putc
        pop     es
        pop     ds
        popa
        ret
%endif

.flat:
        ; .COM 形式のオーバーレイ。先頭から丸ごと読むだけ
        mov     bx, [.handle]
        xor     al, al
        xor     cx, cx
        xor     dx, dx
        call    file_seek
        mov     dx, [.loadseg]
        mov     bx, [.handle]
        mov     ecx, 0x0000FF00
        call    file_read_big
        jc      .read_err

.done:
        mov     bx, [.handle]
        call    file_close
        xor     ax, ax
        clc
        jmp     .out

.bad:
        mov     ax, ERR_BAD_FORMAT
        jmp     .close_err
.read_err:
        mov     ax, ERR_BAD_DATA
.close_err:
        push    ax
        mov     bx, [.handle]
        call    file_close
        pop     ax
        stc
        jmp     .out
.err:
        stc
.out:
        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        ret
.handle:   dw 0
.loadseg:  dw 0
.relocfac: dw 0
.imgsize:  dd 0
.hdrbytes: dd 0

; ---------------------------------------------------------------------------
; do_exec - AH=4Bh の本体。子プロセスを読み込んで制御を渡す
;   入力: DS:SI = パス, ES:BX = パラメータブロック
;   出力: 成功した場合ここへは戻ってこない (子が終了すると exec_return へ)
;         失敗した場合 CF=1, AX = エラーコード
;
; 親の register frame は「親のスタック」に積む。カーネルのどこかに
; 1 組だけ置くと、COMMAND.COM → プログラム → プログラム と入れ子に
; なったときに上書きされてしまうため。
; ---------------------------------------------------------------------------
do_exec:
        ; 親の DTA を控えておく (子が終わったら戻す)
        mov     ax, [dta_seg]
        mov     [exec_parent_dta_seg], ax
        mov     ax, [dta_off]
        mov     [exec_parent_dta_off], ax

        ; パラメータブロックから必要な値を取り出す
        mov     ax, [es:bx + EPB_ENVSEG]
        mov     [exec_envseg], ax
        mov     eax, [es:bx + EPB_CMDTAIL]
        mov     [exec_psp_cmdsrc], eax

        call    prog_load
        jc      .fail

        ; --- 親のレジスタを親のスタックへ積む ---
        cli
        mov     ax, [u_sp]
        mov     bx, [u_ss]
        mov     ss, bx
        mov     sp, ax

        ; INT 21h の入れ子の深さも積んでおく。子は AH=4Ch で終わるが、
        ; その AH=4Ch は「戻ってこない呼び出し」なので、そのままだと
        ; 深さが 1 段ずつ増えたまま戻らない。プログラムを起動するたびに
        ; 深くなっていき、何本か走らせたところで DOS が呼び出しを
        ; 受け付けなくなる。ここで控えて exec_return で戻す。
        movzx   ax, byte [indos_flag]
        push    ax

        push    word [u_ds]
        push    word [u_es]
        push    word [u_bp]
        push    word [u_di]
        push    word [u_si]
        push    word [u_dx]
        push    word [u_cx]
        push    word [u_bx]
        push    word [u_ax]

        ; 親の PSP に SS:SP を保存する (終了処理がここから復帰する)
        mov     es, [cur_psp]
        mov     [es:PSP_STACK], sp
        mov     [es:PSP_STACK + 2], ss

        ; --- INT 22h を「EXEC から戻る」ところへ向ける ---
        ; 子の PSP には、この時点の INT 22h/23h/24h が保存されている
        ; (build_psp がそうしている)。子が AH=4Ch を呼ぶと DOS はそれを
        ; IVT へ書き戻し、その先へジャンプする。
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     word [INT_TERM_ADDR * 4], exec_return
        mov     [INT_TERM_ADDR * 4 + 2], cs
        pop     ds

        ; 子の PSP に、いま設定した INT 22h を入れ直す
        mov     es, [exec_psp]
        mov     word [es:PSP_INT22], exec_return
        mov     [es:PSP_INT22 + 2], cs

        ; --- 子へ制御を渡す ---
        mov     ax, [exec_psp]
        mov     [cur_psp], ax

        ; DTA を子の PSP:0080 に向け直す。
        ;
        ; DOS の既定の DTA は「いま走っているプログラムの PSP + 80h」で、
        ; プログラムが AH=1Ah を呼ばずに AH=4Eh や FCB 検索を使うと、
        ; ここへ結果が書かれる。ここを親のままにしておくと、子の検索結果が
        ; 親 (COMMAND.COM) の PSP に書き込まれ、子は自分の PSP を読んで
        ; 「何も見つからなかった」と判断する。当時のユーティリティは
        ; だいたい既定の DTA をそのまま使うので、まず動かない。
        mov     ax, [exec_parent_dta_seg]       ; 親のぶんを控えておく
        mov     [exec_saved_dta_seg], ax
        mov     ax, [exec_parent_dta_off]
        mov     [exec_saved_dta_off], ax
        mov     ax, [exec_psp]
        mov     [dta_seg], ax
        mov     word [dta_off], PSP_CMDTAIL_LEN

        ; 子は「DOS の中」ではないので、入れ子の深さを 0 に戻す。
        ;
        ; AH=4Bh は子が終わるまで戻ってこない呼び出しなので、そのままだと
        ; 親の 1 段が積まれたまま子が走ることになる。子がまた子を起動すれば
        ; 2 段、3 段と積み上がり、INT21_STACKS に達したところで DOS が
        ; 呼び出しを受け付けなくなる (機能番号が違うと言って返す)。
        ;
        ; GEM がまさにこれだった。COMMAND.COM → GEMVDI → GEM.EXE →
        ; DESKTOP.APP と 3 段重なり、デスクトップが最初に DOS を呼んだ
        ; ところで「機能番号が違う」で弾かれ、画面は背景とタイトルバーを
        ; 出したまま何も起きなくなっていた。
        ;
        ; ここで積んだ深さは exec_return で書き戻すので、親から見た
        ; 深さは変わらない。
        mov     byte [indos_flag], 0

        mov     bx, [exec_stack + 2]
        mov     cx, [exec_stack]
        mov     ss, bx
        mov     sp, cx
        mov     ds, ax
        mov     es, ax
        xor     ax, ax                  ; AL/AH = FCB のドライブ有効性 (両方 0)
        xor     bx, bx
        xor     cx, cx
        xor     dx, dx
        xor     si, si
        xor     di, di
        xor     bp, bp
        sti
        jmp     far [cs:exec_entry]

.fail:
        stc
        ret

; ---------------------------------------------------------------------------
; exec_return - 子が終了したときに飛んでくる場所
;
; ここに来た時点で:
;   * SS:SP は親のスタック (do_exec が積んだフレームの先頭)
;   * cur_psp は親に戻っている
;   * INT 22h/23h/24h は子の PSP から書き戻し済み
; あとは積んでおいたフレームを取り出して、AH=4Bh の呼び出し元へ返すだけ。
; ---------------------------------------------------------------------------
exec_return:
        cli
        mov     ax, cs
        mov     ds, ax

        pop     word [u_ax]
        pop     word [u_bx]
        pop     word [u_cx]
        pop     word [u_dx]
        pop     word [u_si]
        pop     word [u_di]
        pop     word [u_bp]
        pop     word [u_es]
        pop     word [u_ds]

        ; AH=4Bh を呼ばれたときの入れ子の深さに戻す
        pop     ax
        mov     [indos_flag], al

        ; DTA も親のものに戻す
        mov     ax, [exec_saved_dta_seg]
        mov     [dta_seg], ax
        mov     ax, [exec_saved_dta_off]
        mov     [dta_off], ax

        ; いまの SS:SP が、AH=4Bh を呼んだときの IRET フレームを指している
        mov     [u_ss], ss
        mov     [u_sp], sp

        ; EXEC は成功して戻る
        mov     word [u_ax], 0
        mov     byte [ret_cf], 0
        jmp     int21_exit

; ---------------------------------------------------------------------------
; do_tsr - AH=31h / INT 27h の本体 (常駐終了)
;   入力: AX = 終了コード, DX = 残すパラグラフ数
;
; 普通の終了との違いは 2 つだけ:
;   * 確保したメモリを解放せず、指定されたパラグラフ数まで縮めて残す
;   * 開いているファイルを閉じない
; 割り込みベクタは PSP から書き戻すが、常駐ソフトが自分で書き換えた
; INT 9h や INT 1Ch はそのまま生き続ける (PSP に保存されているのは
; 22h/23h/24h の 3 本だけなので)。これが常駐の仕組みそのもの。
; ---------------------------------------------------------------------------
do_tsr:
        mov     [term_keep_paras], dx
        mov     byte [term_keep], 1
        jmp     do_terminate.entry

; ---------------------------------------------------------------------------
; do_terminate - AH=4Ch / INT 20h の本体
;   入力: AL = 終了コード, AH = 終了の種類 (0=通常, 3=TSR)
;
; 手順は本物の DOS と同じ:
;   1. PSP に保存してある INT 22h/23h/24h を IVT へ書き戻す
;   2. このプロセスが開いたファイルを閉じる
;   3. このプロセスが確保したメモリを解放する
;   4. カレント PSP を親に戻す
;   5. 親の PSP に保存されている SS:SP を復元する
;   6. (書き戻した) INT 22h の先へジャンプする
; ---------------------------------------------------------------------------
do_terminate:
        mov     byte [term_keep], 0
.entry:
        cli
        mov     [last_exit_code], ax

        mov     ax, [cur_psp]
        mov     [.psp], ax

        ; --- 開いているファイルを閉じる (常駐する場合は開いたまま残す) ---
        cmp     byte [term_keep], 0
        jne     .keep_files
        call    .close_files
.keep_files:

        ; --- FAT を書き戻しておく ---
        call    disk_flush_fat

        ; --- PSP からベクタを書き戻す ---
        mov     es, [.psp]
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     eax, [es:PSP_INT22]
        mov     [INT_TERM_ADDR * 4], eax
        mov     eax, [es:PSP_INT23]
        mov     [INT_CTRLC * 4], eax
        mov     eax, [es:PSP_INT24]
        mov     [INT_CRITERR * 4], eax
        pop     ds

        ; 戻り先を控えておく (メモリを解放すると PSP が読めなくなるため)
        mov     eax, [es:PSP_INT22]
        mov     [.retaddr], eax
        mov     ax, [es:PSP_PARENT]
        mov     [.parent], ax

        ; --- メモリを解放する (常駐する場合は縮めて残す) ---
        cmp     byte [term_keep], 0
        jne     .keep_memory
        mov     ax, [.psp]
        call    mem_free_owner
        jmp     .to_parent

.keep_memory:
        push    es
        mov     es, [.psp]
        mov     bx, [term_keep_paras]
        cmp     bx, PSP_SIZE / 16
        jae     .paras_ok
        mov     bx, PSP_SIZE / 16       ; PSP のぶんは最低でも残す
.paras_ok:
        call    mem_resize
        pop     es

.to_parent:
        ; --- 親に戻る ---
        mov     ax, [.parent]
        mov     [cur_psp], ax
        mov     es, ax
        mov     bx, [es:PSP_STACK]
        mov     cx, [es:PSP_STACK + 2]
        mov     ss, cx
        mov     sp, bx

        jmp     far [cs:.retaddr]

; このプロセスが開いたファイルを閉じる
.close_files:
        push    ax
        push    bx
        push    cx
        push    si
        push    es

        mov     es, [.psp]
        xor     bx, bx
.cf_loop:
        cmp     bx, JFT_ENTRIES
        jae     .cf_done
        mov     al, [es:bx + PSP_JFT]
        cmp     al, JFT_FREE
        je      .cf_next
        push    bx
        push    es
        call    file_close
        pop     es
        pop     bx
.cf_next:
        inc     bx
        jmp     .cf_loop
.cf_done:
        pop     es
        pop     si
        pop     cx
        pop     bx
        pop     ax
        ret

.psp:     dw 0
.parent:  dw 0
.retaddr: dd 0

; --- 変数 ------------------------------------------------------------------
term_keep:          db 0        ; 1 なら常駐終了 (メモリとファイルを残す)
term_keep_paras:    dw 0        ; 常駐で残すパラグラフ数
last_exit_code:     dw 0        ; AH=4Dh が返す値 (AL=コード, AH=種類)
exec_psp:           dw 0
exec_entry:         dd 0        ; 子の入口 (offset, segment)
exec_stack:         dd 0        ; 子の初期スタック (SP, SS)
exec_envseg:        dw 0
exec_parent_dta_seg: dw 0
exec_parent_dta_off: dw 0
exec_saved_dta_seg:  dw 0
exec_saved_dta_off:  dw 0
exec_psp_cmdsrc:    dd 0        ; コマンドテイルへの far ポインタ
exe_header:         times MZ_HDRSIZE db 0
reloc_entry:        dd 0
reloc_factor:       dw 0xFFFF   ; FFFFh = ロード先セグメントをそのまま使う
