; ============================================================================
; dosint.asm  -  DOS の内部データ構造を、当時のツールと同じやり方で検証する
;
; メモリマネージャ、常駐ソフト、ファイラ、デバッガの類は INT 21h だけでは
; 足りず、DOS の内部構造を直接辿る。入口は AH=52h が返す List of Lists で、
; そこから DPB 連鎖・SFT 連鎖・デバイス連鎖・CDS 配列・バッファ連鎖に届く。
;
; ここを独自の形にしていると「INT 21h は全部通るのに当時のツールだけ動かない」
; という状態になる。外から見える形が本物と一致しているかを確かめる。
; ============================================================================
        cpu     386
        bits    16
        org     0x100

; --- List of Lists のオフセット ---
LOL_FIRST_DPB   equ 0x00
LOL_FIRST_SFT   equ 0x04
LOL_CLOCK       equ 0x08
LOL_CON         equ 0x0C
LOL_MAXSECTOR   equ 0x10
LOL_BUFFERS     equ 0x12
LOL_CDS         equ 0x16
LOL_NUMBLOCK    equ 0x20
LOL_LASTDRIVE   equ 0x21
LOL_NULDEV      equ 0x22

; --- デバイスヘッダ ---
DEV_NEXT        equ 0x00
DEV_ATTR        equ 0x04
DEV_NAME        equ 0x0A
DEVA_CHAR       equ 0x8000

; --- SFT ---
SFTH_NEXT       equ 0x00
SFTH_COUNT      equ 0x04
SFTH_ENTRIES    equ 0x06
SFT_REFCNT      equ 0x00
SFT_SIZE        equ 0x11
SFT_NAME        equ 0x20
SFT_ENTSIZE     equ 59

; --- CDS ---
CDS_PATH        equ 0x00
CDS_FLAGS       equ 0x43
CDS_CLUSTER     equ 0x49
CDS_ENTSIZE     equ 88
CDSF_PHYSICAL   equ 0x4000

; --- DPB ---
DPB_DRIVE       equ 0x00
DPB_SECSIZE     equ 0x02
DPB_MEDIA       equ 0x17
DPB_NEXT        equ 0x19

; --- MCB ---
MCB_SIG         equ 0x00
MCB_SIZE        equ 0x03

; --- バッファ ---
BUF_NEXT        equ 0x00
BUF_HDRSIZE     equ 0x14

start:
        mov     sp, stack_top
        mov     bx, stack_top
        add     bx, 15
        mov     cl, 4
        shr     bx, cl
        mov     ah, 0x4A
        int     0x21

        mov     si, msg_head
        call    puts

        ; List of Lists を取っておく
        mov     ah, 0x52
        int     0x21
        mov     [lol_seg], es
        mov     [lol_off], bx

        call    t_lol
        call    t_mcb
        call    t_dpb
        call    t_devchain
        call    t_sft
        call    t_cds
        call    t_buffers
        call    t_country
        call    t_sda
        call    t_dbcs
        call    t_memstrat
        call    t_cpm
        call    t_memtop

        call    newline
        mov     si, msg_result
        call    puts
        mov     ax, [pass_count]
        call    put_dec
        mov     si, msg_result2
        call    puts
        mov     ax, [fail_count]
        call    put_dec
        call    newline
        mov     si, msg_end
        call    puts

        mov     ax, [fail_count]
        test    ax, ax
        jz      .clean
        mov     ax, 0x4C01
        int     0x21
.clean:
        mov     ax, 0x4C00
        int     0x21

; ============================================================================
; 1. List of Lists そのもの
; ============================================================================
t_lol:
        mov     si, n_lol
        call    begin

        mov     es, [lol_seg]
        mov     bx, [lol_off]

        ; 最大セクタサイズ
        cmp     word [es:bx + LOL_MAXSECTOR], 512
        jne     fail
        ; ブロックデバイスは 1 台以上
        cmp     byte [es:bx + LOL_NUMBLOCK], 1
        jb      fail
        ; LASTDRIVE
        cmp     byte [es:bx + LOL_LASTDRIVE], 1
        jb      fail
        cmp     byte [es:bx + LOL_LASTDRIVE], 26
        ja      fail
        ; 各ポインタが null でないこと
        cmp     word [es:bx + LOL_FIRST_DPB + 2], 0
        je      fail
        cmp     word [es:bx + LOL_FIRST_SFT + 2], 0
        je      fail
        cmp     word [es:bx + LOL_CLOCK + 2], 0
        je      fail
        cmp     word [es:bx + LOL_CON + 2], 0
        je      fail
        cmp     word [es:bx + LOL_CDS + 2], 0
        je      fail
        cmp     word [es:bx + LOL_BUFFERS + 2], 0
        je      fail
        jmp     pass

; ============================================================================
; 2. List of Lists の 2 バイト手前から MCB 連鎖を辿る
;
; MEM や常駐ソフトが空きを調べるときの定番の入口。
; ============================================================================
t_mcb:
        mov     si, n_mcb
        call    begin

        mov     es, [lol_seg]
        mov     bx, [lol_off]
        mov     ax, [es:bx - 2]         ; 最初の MCB
        test    ax, ax
        jz      fail

        xor     cx, cx                  ; CX = 数えたブロック数
.loop:
        mov     es, ax
        mov     dl, [es:MCB_SIG]
        cmp     dl, 'M'
        je      .valid
        cmp     dl, 'Z'
        je      .valid
        jmp     fail                    ; シグネチャが壊れている
.valid:
        inc     cx
        cmp     cx, 200
        jae     fail                    ; 終わりが来ない
        cmp     dl, 'Z'
        je      .done
        add     ax, [es:MCB_SIZE]
        inc     ax
        jmp     .loop
.done:
        cmp     cx, 2                   ; 少なくとも DOS 用と自分のぶん
        jb      fail
        jmp     pass

; ============================================================================
; 3. DPB 連鎖
; ============================================================================
t_dpb:
        mov     si, n_dpb
        call    begin

        mov     es, [lol_seg]
        mov     bx, [lol_off]
        les     bx, [es:bx + LOL_FIRST_DPB]

        cmp     byte [es:bx + DPB_DRIVE], 0     ; A:
        jne     fail
        cmp     word [es:bx + DPB_SECSIZE], 512
        jne     fail
        cmp     byte [es:bx + DPB_MEDIA], 0xF0  ; 1.44MB フロッピー
        jne     fail

        ; AH=32h が返すものと同じ場所を指しているはず
        push    es
        push    bx
        push    ds
        mov     dl, 1
        mov     ah, 0x32
        int     0x21
        mov     cx, bx
        mov     dx, ds
        pop     ds
        pop     bx
        pop     es
        cmp     cx, bx
        jne     fail
        mov     ax, es
        cmp     dx, ax
        jne     fail

        ; 連鎖を辿る。ドライブが増えても終端 (FFFF:FFFF) に届き、
        ; 途中のドライブ番号は増える一方でなければならない。
        ; ハードディスクを繋いだ環境では A: の次に C: が並ぶ。
        mov     cl, [es:bx + DPB_DRIVE]
        xor     ch, ch                  ; CH = 数えた DPB の数
.walk:
        inc     ch
        cmp     ch, 30
        jae     fail                    ; 終わりが来ない
        cmp     word [es:bx + DPB_NEXT + 2], 0xFFFF
        je      .end
        les     bx, [es:bx + DPB_NEXT]
        cmp     word [es:bx + DPB_SECSIZE], 512
        jne     fail
        cmp     byte [es:bx + DPB_DRIVE], cl
        jbe     fail                    ; 番号は増える一方
        mov     cl, [es:bx + DPB_DRIVE]
        jmp     .walk
.end:
        jmp     pass

; ============================================================================
; 4. デバイスドライバの連鎖
;
; NUL は List of Lists の +22h に埋め込まれているのが約束。そこから
; 順に辿ると CON / AUX / PRN / CLOCK$ / ブロックデバイスが出てくる。
; ============================================================================
t_devchain:
        mov     si, n_dev
        call    begin

        mov     es, [lol_seg]
        mov     bx, [lol_off]
        add     bx, LOL_NULDEV          ; ES:BX = NUL のヘッダ

        ; NUL の名前
        mov     si, e_nul
        call    dev_name_is
        jne     fail
        test    word [es:bx + DEV_ATTR], DEVA_CHAR
        jz      fail

        ; 連鎖を辿って CON / CLOCK$ / ブロックデバイスに出会えること
        xor     cx, cx                  ; CX = 数えたデバイス数
        mov     word [found_con], 0
        mov     word [found_clock], 0
        mov     word [found_block], 0
.loop:
        inc     cx
        cmp     cx, 32
        jae     fail                    ; 終わりが来ない

        test    word [es:bx + DEV_ATTR], DEVA_CHAR
        jz      .is_block

        mov     si, e_con
        call    dev_name_is
        jne     .not_con
        mov     word [found_con], 1
.not_con:
        mov     si, e_clock
        call    dev_name_is
        jne     .not_clock
        mov     word [found_clock], 1
.not_clock:
        jmp     .next
.is_block:
        mov     word [found_block], 1
.next:
        mov     ax, [es:bx + DEV_NEXT]
        mov     dx, [es:bx + DEV_NEXT + 2]
        cmp     ax, 0xFFFF
        je      .done
        mov     es, dx
        mov     bx, ax
        jmp     .loop
.done:
        cmp     word [found_con], 1
        jne     fail
        cmp     word [found_clock], 1
        jne     fail
        cmp     word [found_block], 1
        jne     fail
        cmp     cx, 5                   ; NUL CON AUX PRN CLOCK$ ブロック
        jb      fail

        ; List of Lists の +0Ch が指す CON と、連鎖で見つけた CON が一致すること
        mov     es, [lol_seg]
        mov     bx, [lol_off]
        les     bx, [es:bx + LOL_CON]
        mov     si, e_con
        call    dev_name_is
        jne     fail

        mov     es, [lol_seg]
        mov     bx, [lol_off]
        les     bx, [es:bx + LOL_CLOCK]
        mov     si, e_clock
        call    dev_name_is
        jne     fail
        jmp     pass

; ES:BX = デバイスヘッダ, DS:SI = 8 バイトの名前 → ZF=1 なら一致
dev_name_is:
        push    ax
        push    cx
        push    si
        push    di
        mov     di, bx
        add     di, DEV_NAME
        mov     cx, 8
.loop:
        mov     al, [si]
        cmp     al, [es:di]
        jne     .no
        inc     si
        inc     di
        loop    .loop
        xor     al, al
        jmp     .out
.no:
        or      al, 0xFF
.out:
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

; ============================================================================
; 5. SFT 連鎖
;
; 開いているファイルの一覧。ファイラやデバッガが「どのプロセスが何を
; 開いているか」を見るのに辿る。開いたファイルの名前が 8.3 形式で
; +20h に入っているかを確かめる。
; ============================================================================
t_sft:
        mov     si, n_sft
        call    begin

        ; 検証用のファイルを作って開いたままにする
        mov     dx, f_probe
        xor     cx, cx
        mov     ah, 0x3C
        int     0x21
        jc      fail
        mov     [probe_handle], ax

        mov     bx, ax
        mov     cx, probe_len
        mov     dx, probe_data
        mov     ah, 0x40
        int     0x21
        jc      .close_fail

        mov     es, [lol_seg]
        mov     bx, [lol_off]
        les     bx, [es:bx + LOL_FIRST_SFT]

        ; テーブルの数は 1 以上
        cmp     word [es:bx + SFTH_COUNT], 1
        jb      .close_fail

        ; エントリを順に見て、いま開いたファイルを探す
        mov     cx, [es:bx + SFTH_COUNT]
        add     bx, SFTH_ENTRIES
        mov     word [found_sft], 0
.loop:
        cmp     word [es:bx + SFT_REFCNT], 0
        je      .next                   ; 空き

        push    cx
        push    si
        push    di
        mov     di, bx
        add     di, SFT_NAME
        mov     si, e_probe
        mov     cx, 11
.cmp:
        mov     al, [si]
        cmp     al, [es:di]
        jne     .no_match
        inc     si
        inc     di
        loop    .cmp
        pop     di
        pop     si
        pop     cx
        ; 見つかった。サイズも合っているはず
        cmp     word [es:bx + SFT_SIZE], probe_len
        jne     .close_fail
        mov     word [found_sft], 1
        jmp     .next
.no_match:
        pop     di
        pop     si
        pop     cx
.next:
        add     bx, SFT_ENTSIZE
        loop    .loop

        cmp     word [found_sft], 1
        jne     .close_fail

        mov     bx, [probe_handle]
        mov     ah, 0x3E
        int     0x21
        mov     dx, f_probe
        mov     ah, 0x41
        int     0x21
        jmp     pass

.close_fail:
        mov     bx, [probe_handle]
        mov     ah, 0x3E
        int     0x21
        mov     dx, f_probe
        mov     ah, 0x41
        int     0x21
        jmp     fail

; ============================================================================
; 6. CDS 配列
;
; ドライブごとのカレントディレクトリ。CD したら内容が追随することを見る。
; ============================================================================
t_cds:
        mov     si, n_cds
        call    begin

        ; ルートにいる状態で "A:\" になっているか
        call    get_cds0
        cmp     byte [es:bx + CDS_PATH], 'A'
        jne     fail
        cmp     byte [es:bx + CDS_PATH + 1], ':'
        jne     fail
        cmp     byte [es:bx + CDS_PATH + 2], '\'
        jne     fail
        cmp     byte [es:bx + CDS_PATH + 3], 0
        jne     fail
        test    word [es:bx + CDS_FLAGS], CDSF_PHYSICAL
        jz      fail
        cmp     word [es:bx + CDS_CLUSTER], 0   ; ルート
        jne     fail

        ; サブディレクトリを作って移動する
        mov     dx, d_probe
        mov     ah, 0x39
        int     0x21
        jc      fail
        mov     dx, d_probe
        mov     ah, 0x3B
        int     0x21
        jc      .rm

        ; CDS が追随しているか
        call    get_cds0
        push    ds
        pop     ds
        mov     si, e_cdspath
        mov     di, bx
        call    cmp_es_str
        jne     .back

        ; クラスタもルート以外になっているはず
        cmp     word [es:bx + CDS_CLUSTER], 0
        je      .back

        mov     dx, d_up
        mov     ah, 0x3B
        int     0x21
        mov     dx, d_probe
        mov     ah, 0x3A
        int     0x21
        jmp     pass

.back:
        mov     dx, d_up
        mov     ah, 0x3B
        int     0x21
.rm:
        mov     dx, d_probe
        mov     ah, 0x3A
        int     0x21
        jmp     fail

; ES:BX = ドライブ A: の CDS
get_cds0:
        push    ax
        mov     es, [lol_seg]
        mov     bx, [lol_off]
        les     bx, [es:bx + LOL_CDS]
        pop     ax
        ret

; DS:SI と ES:DI を比べる → ZF=1 なら一致
cmp_es_str:
        push    ax
        push    si
        push    di
.loop:
        mov     al, [si]
        cmp     al, [es:di]
        jne     .no
        test    al, al
        jz      .yes
        inc     si
        inc     di
        jmp     .loop
.yes:
        xor     al, al
        jmp     .out
.no:
        or      al, 0xFF
.out:
        pop     di
        pop     si
        pop     ax
        ret

; ============================================================================
; 7. ディスクバッファ連鎖 (BUFFERS=)
; ============================================================================
t_buffers:
        mov     si, n_buf
        call    begin

        mov     es, [lol_seg]
        mov     bx, [lol_off]
        les     bx, [es:bx + LOL_BUFFERS]

        xor     cx, cx
.loop:
        inc     cx
        cmp     cx, 64
        jae     fail                    ; 終わりが来ない
        mov     ax, [es:bx + BUF_NEXT]
        mov     dx, [es:bx + BUF_NEXT + 2]
        cmp     ax, 0xFFFF
        je      .done
        mov     es, dx
        mov     bx, ax
        jmp     .loop
.done:
        cmp     cx, 1
        jb      fail
        jmp     pass

; ============================================================================
; 小道具
; ============================================================================
; ============================================================================
; 8. AH=65h の表 (照合順序ほか)
;
; AL=02h/04h/05h/06h/07h は「表そのもの」ではなく「表への far ポインタ」を
; 返す約束になっている。ここを取り違えて国別情報を書いてしまうと、
; 呼び出し側はその中身をアドレスとして読む。SORT は AL=06h の照合順序表
; から文字の重みを引くので、並びがめちゃくちゃになる。
; ============================================================================
t_country:
        mov     si, n_country
        call    begin

        ; --- AL=06h: 照合順序の表 ---
        push    ds
        pop     es
        mov     di, ctry_buf
        mov     cx, 5
        mov     bx, 0xFFFF              ; いまのドライブ / 国
        mov     dx, 0xFFFF
        mov     ax, 0x6506
        int     0x21
        jc      fail
        cmp     byte [ctry_buf], 0x06   ; 情報の種類がそのまま返ること
        jne     fail

        ; far ポインタの先は「長さ 256 + 256 バイト」
        les     di, [ctry_buf + 1]
        cmp     word [es:di], 256
        jne     fail

        ; 重みは 'A' が 'A'、小文字の 'a' も 'A' と同じ (大小を区別しない)
        add     di, 2
        mov     al, [es:di + 'A']
        cmp     al, 'A'
        jne     fail
        mov     al, [es:di + 'a']
        cmp     al, 'A'
        jne     fail
        mov     al, [es:di + '0']
        cmp     al, '0'
        jne     fail

        ; --- AL=02h: 大文字への変換表 ---
        push    ds
        pop     es
        mov     di, ctry_buf
        mov     cx, 5
        mov     bx, 0xFFFF
        mov     dx, 0xFFFF
        mov     ax, 0x6502
        int     0x21
        jc      fail
        cmp     byte [ctry_buf], 0x02
        jne     fail
        les     di, [ctry_buf + 1]
        cmp     word [es:di], 128
        jne     fail

        jmp     pass

; ---------------------------------------------------------------------------
; AH=5D06h - SDA (入れ替え可能データ領域)
;
; Windows のリアルモードや DOS マルチタスカ、それに常駐ソフトが
; 「いま DOS を呼んでいいか」を判断するために使う。ここで確かめるのは
; 返ってきたポインタが飾りではないこと — つまり、AH=34h が返す InDOS
; フラグと同じ番地を指していて、しかも中の値が生きていること。
; ---------------------------------------------------------------------------
t_sda:
        mov     si, n_sda
        call    begin

        ; --- AH=34h で InDOS の番地を取っておく ---
        push    es
        mov     ah, 0x34
        int     0x21                    ; ES:BX = InDOS フラグ
        mov     [indos_seg], es
        mov     [indos_off], bx
        pop     es

        ; --- AH=5D06h ---
        ; 戻り値の DS:SI が指すのは DOS の中なので、値を控えるより先に
        ; DS を自分のものへ戻さないと、書き込み先が DOS の中になる。
        push    ds
        mov     ax, 0x5D06
        int     0x21
        mov     bx, ds
        pop     ds
        jc      fail
        mov     [sda_seg], bx
        mov     [sda_off], si
        mov     [sda_size], cx
        mov     [sda_always], dx

        ; SDA+01h が InDOS フラグそのものであること (写しではなく本体)
        mov     ax, [sda_seg]
        cmp     ax, [indos_seg]
        jne     fail
        mov     ax, [sda_off]
        inc     ax
        cmp     ax, [indos_off]
        jne     fail

        ; 大きさは 0 でなく、常に入れ替える範囲のほうが小さいこと
        cmp     word [sda_size], 0x18
        jb      fail
        mov     ax, [sda_always]
        cmp     ax, [sda_size]
        ja      fail
        test    ax, ax
        jz      fail

        ; SDA+16h はカレントドライブ。AH=19h と一致するはず
        push    es
        mov     es, [sda_seg]
        mov     di, [sda_off]
        mov     ah, 0x19
        int     0x21                    ; AL = カレントドライブ
        cmp     al, [es:di + 0x16]
        pop     es
        jne     fail

        ; SDA+10h はいまの PSP。AH=51h と一致するはず
        push    es
        mov     es, [sda_seg]
        mov     di, [sda_off]
        mov     ah, 0x51
        int     0x21                    ; BX = PSP
        cmp     bx, [es:di + 0x10]
        pop     es
        jne     fail

        ; --- AH=5D0Bh: SDA の一覧 ---
        push    ds
        mov     ax, 0x5D0B
        int     0x21
        mov     bx, ds
        pop     ds
        jc      fail
        mov     [list_seg], bx
        mov     [list_off], si

        push    es
        mov     es, [list_seg]
        mov     di, [list_off]
        cmp     word [es:di], 1         ; 個数
        jne     .listbad
        mov     ax, [es:di + 2]         ; 先頭の SDA のオフセット
        cmp     ax, [sda_off]
        jne     .listbad
        mov     ax, [es:di + 4]         ; セグメント
        cmp     ax, [sda_seg]
        jne     .listbad
        pop     es
        jmp     pass
.listbad:
        pop     es
        jmp     fail

; ---------------------------------------------------------------------------
; MCB 連鎖の終わり - BIOS が申告する RAM の末尾と一致すること
;
; 640KB 決め打ちにはできない。BIOS は EBDA (拡張 BIOS データ領域) を
; 640KB のすぐ下に置くことがあり、そこには PS/2 マウスの状態や
; ハードディスクの諸元表が入っている。BIOS は 0040:0013 に「使える
; 大きさ (KB)」を書いてあり、EBDA がある機械では 640 ではなく 639 になる。
;
; ここを 0xA000 に決め打ちしていると、上のほうまで使うプログラムが
; EBDA を踏み、そのあと BIOS が出鱈目な値を返すようになる。
; ---------------------------------------------------------------------------
t_memtop:
        mov     si, n_memtop
        call    begin

        ; BIOS の申告 (KB) をパラグラフに直す
        push    es
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:0x0413]
        pop     es
        mov     cl, 6
        shl     ax, cl                  ; KB * 64
        mov     [mt_bios], ax

        ; MCB 連鎖の先頭を List of Lists から取る
        push    es
        mov     ah, 0x52
        int     0x21                    ; ES:BX = LoL
        mov     ax, [es:bx - 2]         ; LoL-2 = 最初の MCB
        pop     es
        mov     [mt_seg], ax

        ; 'Z' まで辿って、末尾のセグメントを求める
        mov     cx, 200
.walk:
        push    es
        mov     es, [mt_seg]
        mov     al, [es:0]
        mov     dx, [es:3]
        pop     es
        cmp     al, 0x5A                ; 'Z'
        je      .last
        cmp     al, 0x4D                ; 'M'
        jne     fail
        mov     ax, [mt_seg]
        add     ax, dx
        inc     ax
        mov     [mt_seg], ax
        loop    .walk
        jmp     fail
.last:
        ; 末尾 = 最後の MCB + 1 + 大きさ
        mov     ax, [mt_seg]
        add     ax, dx
        inc     ax
        cmp     ax, [mt_bios]
        jne     fail
        jmp     pass

; ---------------------------------------------------------------------------
; PSP+05h - CP/M 形式の呼び出し口
;
; CP/M からの移植ものは、DOS を INT 21h ではなく「PSP:0005 への near call」で
; 呼ぶ。機能番号は AH ではなく CL に入れ、使えるのは 0〜24h まで。
; PSP:0005 には far call が埋め込んであり、その飛び先が DOS の入口になる。
;
; ここが本物の入口を指していないと、そういうプログラムは DOS の代わりに
; 出鱈目な番地へ飛ぶ。GEM のデスクトップがまさにこれで、飛び先が BIOS を
; 指していたために、画面を描き始めたところで行方不明になっていた。
;
; .COM は CS = PSP なので、call 5 がそのまま PSP:0005 に届く。
; ---------------------------------------------------------------------------
t_cpm:
        mov     si, n_cpm
        call    begin

        ; PSP:0005 は far call の命令でなければならない
        cmp     byte [0x0005], 0x9A
        jne     fail

        ; まず普通に、いまのドライブを聞いておく
        mov     ah, 0x19
        int     0x21
        mov     [cpm_drive], al

        ; 同じことを CP/M 形式で聞く (機能番号は CL)
        mov     cx, 0x0019
        xor     ax, ax
        call    0x0005
        cmp     al, [cpm_drive]
        jne     fail

        ; 24h より大きい番号は AL=0 で返る約束
        mov     cx, 0x0030
        mov     al, 0xFF
        call    0x0005
        test    al, al
        jnz     fail

        jmp     pass

; ---------------------------------------------------------------------------
; AH=63h - 2 バイト文字 (DBCS) の先頭バイト表
;
; 表は「先頭バイトになりうる範囲」の組を並べ、0 の組で終わる。2 バイト文字を
; 扱わない実装でも、空の表 (0 の組だけ) を返さなければならない。
;
; ここで見るのは「DS:SI が本当に置き換わっているか」。AL=0 で成功だけ返して
; DS:SI をそのままにすると、呼んだ側は自分の DS:SI が指す先を表だと思って
; 読み始める。そこにたまたま 0 が無ければ、どこまでも範囲の組を拾い続けて
; 戻ってこない。Mpxplay (DOS/32A) が起動直後に無言で止まっていたのがこれ。
; ---------------------------------------------------------------------------
t_dbcs:
        mov     si, n_dbcs
        call    begin

        ; わざと「表ではない場所」を DS:SI に入れてから呼ぶ。
        ; 置き換えられなければ、この番地がそのまま返ってくる。
        push    ds
        pop     es
        mov     word [dbcs_decoy], 0x0102       ; 0 で終わらない中身
        mov     word [dbcs_decoy + 2], 0x0304

        push    ds
        mov     si, dbcs_decoy
        mov     ax, 0x6300
        int     0x21
        mov     bx, ds
        pop     ds
        jc      fail
        test    al, al
        jnz     fail
        mov     [dbcs_seg], bx
        mov     [dbcs_off], si

        ; 返ってきたのが、こちらが渡した番地そのままではないこと
        mov     ax, ds
        cmp     ax, [dbcs_seg]
        jne     .moved
        cmp     si, dbcs_decoy
        je      fail                    ; DS:SI が置き換えられていない
.moved:
        ; 表の終わりが読めること (空の表なら先頭がいきなり 0 の組)
        push    es
        mov     es, [dbcs_seg]
        mov     di, [dbcs_off]
        mov     cx, 32                  ; 32 組以内に終端があるはず
.scan:
        cmp     word [es:di], 0
        je      .found_end
        add     di, 2
        loop    .scan
        pop     es
        jmp     fail
.found_end:
        pop     es

        ; AL=01h / AL=02h の旗が往復すること
        mov     ax, 0x6301
        mov     dl, 1
        int     0x21
        jc      fail
        mov     ax, 0x6302
        mov     dl, 0
        int     0x21
        jc      fail
        cmp     dl, 1
        jne     fail

        mov     ax, 0x6301              ; 元に戻す
        mov     dl, 0
        int     0x21
        jmp     pass

; ---------------------------------------------------------------------------
; AH=58h - メモリの割り当て方
;
; 下位機能は 4 つある。00h/01h が方針の取得と設定、02h/03h が
; 「UMB を MCB の連鎖に入れるかどうか」の取得と設定。
;
; 方針の値は DOS 5.0 で増えていて、上位のビットが「上 (UMB) を先に探すか」
; を表す。40h や 80h を付けた値を「範囲外」で弾くと、UMB へ載ろうとする
; プログラムがそこで転ぶ。CuteMouse は 00h→02h→03h→01h(41h)→01h(81h) と
; 順に呼んでくるので、1 つでも食い違うと常駐に失敗する。
; ---------------------------------------------------------------------------
t_memstrat:
        mov     si, n_memstrat
        call    begin

        ; いまの方針を控える
        mov     ax, 0x5800
        int     0x21
        jc      fail
        mov     [strat_save], ax

        ; 上位ビット付きの方針が通ること
        mov     ax, 0x5801
        mov     bx, 0x0041              ; 上を先に / いちばん近いもの
        int     0x21
        jc      fail
        mov     ax, 0x5800
        int     0x21
        jc      fail
        cmp     al, 0x41
        jne     fail

        mov     ax, 0x5801
        mov     bx, 0x0081
        int     0x21
        jc      fail

        ; 出鱈目な値は断ること
        mov     ax, 0x5801
        mov     bx, 0x00C3
        int     0x21
        jnc     fail

        ; UMB を連鎖に入れるかどうかの取得と設定
        mov     ax, 0x5802
        int     0x21
        jc      fail
        mov     ax, 0x5803
        mov     bx, 1
        int     0x21
        jc      fail
        mov     ax, 0x5802
        int     0x21
        jc      fail
        cmp     al, 1
        jne     fail
        mov     ax, 0x5803
        mov     bx, 0
        int     0x21
        jc      fail

        ; 元に戻す
        mov     ax, 0x5801
        mov     bx, [strat_save]
        int     0x21
        jmp     pass

begin:
        push    si
        mov     si, str_indent
        call    puts
        pop     si
        mov     [test_name], si
        ret

pass:
        inc     word [pass_count]
        mov     si, str_pass
        call    puts
        mov     si, [test_name]
        call    puts
        call    newline
        ret

fail:
        inc     word [fail_count]
        mov     si, str_fail
        call    puts
        mov     si, [test_name]
        call    puts
        call    newline
        ret

putc:
        push    ax
        push    bx
        push    cx
        push    dx
        push    ds
        push    cs
        pop     ds
        mov     [char_buf], al
        mov     dx, char_buf
        mov     cx, 1
        mov     bx, 1
        mov     ah, 0x40
        int     0x21
        pop     ds
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

newline:
        push    ax
        mov     al, 13
        call    putc
        mov     al, 10
        call    putc
        pop     ax
        ret

puts:
        push    ax
        push    si
.loop:
        lodsb
        test    al, al
        jz      .done
        call    putc
        jmp     .loop
.done:
        pop     si
        pop     ax
        ret

put_dec:
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
        call    putc
        loop    .emit
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ============================================================================
; データ
; ============================================================================
ctry_buf:    times 48 db 0
msg_head:    db 13, 10, '=== MYDOS internal structure test (via AH=52h) ===', 13, 10, 13, 10, 0
msg_result:  db '### RESULT pass=', 0
msg_result2: db ' fail=', 0
msg_end:     db '###TEST-END###', 13, 10, 0

str_indent:  db '  ', 0
str_pass:    db '[PASS] ', 0
str_fail:    db '[FAIL] ', 0

n_lol:       db 'AH=52h  List of Lists fields are populated', 0
n_mcb:       db 'LoL-2   MCB chain walks to a Z block', 0
n_dpb:       db 'LoL+00  DPB chain matches AH=32h and the BPB', 0
n_dev:       db 'LoL+22  device chain NUL-CON-AUX-PRN-CLOCK$-block', 0
n_sft:       db 'LoL+04  SFT chain shows an open file by name', 0
n_cds:       db 'LoL+16  CDS array follows CHDIR', 0
n_buf:       db 'LoL+12  disk buffer chain is linked', 0
n_memstrat:  db 'AH=58h  allocation strategy and the UMB link', 0
strat_save:  dw 0
n_dbcs:      db 'AH=63h  the DBCS table pointer really is replaced', 0
dbcs_seg:    dw 0
dbcs_off:    dw 0
dbcs_decoy:  times 8 db 0xFF
n_memtop:    db 'MCB     the arena stops where the BIOS says RAM ends', 0
n_cpm:       db 'PSP+05  the CP/M style call reaches DOS', 0
cpm_drive:   db 0
mt_bios:     dw 0
mt_seg:      dw 0
n_sda:       db 'AH=5D06h/5D0Bh  the swappable data area is the real one', 0
indos_seg:   dw 0
indos_off:   dw 0
sda_seg:     dw 0
sda_off:     dw 0
sda_size:    dw 0
sda_always:  dw 0
list_seg:    dw 0
list_off:    dw 0
n_country:   db 'AH=65h  the country tables are far pointers, not data', 0

e_nul:       db 'NUL     '
e_con:       db 'CON     '
e_clock:     db 'CLOCK$  '
e_probe:     db 'PROBE   DAT'
e_cdspath:   db 'A:\PROBEDIR', 0

f_probe:     db 'PROBE.DAT', 0
d_probe:     db 'PROBEDIR', 0
d_up:        db '..', 0
probe_data:  db 'SFT probe payload', 13, 10
probe_len    equ $ - probe_data

; --- 変数 ------------------------------------------------------------------
test_name:    dw 0
pass_count:   dw 0
fail_count:   dw 0
lol_seg:      dw 0
lol_off:      dw 0
probe_handle: dw 0
found_con:    dw 0
found_clock:  dw 0
found_block:  dw 0
found_sft:    dw 0
char_buf:     db 0

              times 512 db 0
stack_top:
