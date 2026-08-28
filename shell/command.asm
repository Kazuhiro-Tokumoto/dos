; ============================================================================
; command.asm  -  MYDOS のコマンドインタプリタ (COMMAND.COM)
;
; カーネルの一部ではなく、ただの .COM プログラムとして作ってある。
; 使うのは INT 21h と、画面制御のための INT 10h だけ。本物の MS-DOS と
; 同じ構造で、これのおかげでシェルだけ差し替えることができる。
;
; ビルド: nasm -f bin shell/command.asm -o command.com
; ============================================================================
        cpu     386
        bits    16
        org     0x100

; --- PSP のオフセット (自分自身の PSP を読むために使う) --------------------
PSP_MEMTOP      equ 0x02
PSP_ENVSEG      equ 0x2C
PSP_CMDTAIL_LEN equ 0x80

; --- DTA (FindFirst/FindNext の結果) ---------------------------------------
FIND_ATTR       equ 0x15
FIND_TIME       equ 0x16
FIND_DATE       equ 0x18
FIND_SIZE       equ 0x1A
FIND_NAME       equ 0x1E

ATTR_DIRECTORY  equ 0x10

; --- 長い名前の検索が返す構造体 (Win32 の WIN32_FIND_DATA と同じ並び) ---
LFD_ATTR        equ 0x00        ; dword: 属性
LFD_TIME        equ 0x14        ; word:  最終更新の時刻 (DOS 形式)
LFD_DATE        equ 0x16        ; word:  最終更新の日付 (DOS 形式)
LFD_SIZE        equ 0x20        ; dword: 大きさ (下位)
LFD_NAME        equ 0x2C        ; 260 バイト: 長い名前
LFD_SHORT       equ 0x130       ; 14 バイト: 短い名前
LFD_SIZE_TOTAL  equ 318

DIR_NAME_COLS   equ 30          ; DIR の名前欄の桁数
ATTR_VOLUME     equ 0x08

MAXLINE         equ 128
BATMAX          equ 2048        ; バッチファイルの最大サイズ

; ============================================================================
start:
        ; .COM は最初に「自分に必要な分だけ残して、あとは返す」のが作法。
        ; そうしないと子プロセスを起動するメモリが残らない。
        mov     sp, stack_top
        ; 必要なパラグラフ数 = (末尾 + 15) / 16。stack_top は再配置可能な
        ; ラベルなのでアセンブル時には割れず、実行時に計算する。
        mov     bx, stack_top
        add     bx, 15
        mov     cl, 4
        shr     bx, cl
        mov     ah, 0x4A
        int     0x21

        ; DTA を自前の領域に移す。既定の PSP:0080 はコマンドテイルの
        ; 置き場でもあるので、検索結果で自分の引数を潰してしまう。
        mov     dx, dta_buf
        mov     ah, 0x1A
        int     0x21

        call    show_banner
        call    run_autoexec

main_loop:
        call    show_prompt
        call    read_line
        call    exec_line
        jmp     main_loop

; ---------------------------------------------------------------------------
show_banner:
        mov     si, msg_banner
        call    puts
        ret

; ---------------------------------------------------------------------------
; show_prompt - "A:\CURRENT>" を表示する
; ---------------------------------------------------------------------------
show_prompt:
        mov     ah, 0x19                ; カレントドライブ
        int     0x21
        add     al, 'A'
        call    putc
        mov     al, ':'
        call    putc
        mov     al, '\'
        call    putc

        mov     si, cwd_buf
        mov     ah, 0x47                ; カレントディレクトリの取得
        mov     dl, 0                   ; 0 = 現在のドライブ
        int     0x21

        mov     si, cwd_buf
        cmp     byte [si], 0
        je      .no_path
        call    puts
        mov     al, '\'
        call    putc
.no_path:
        mov     al, '>'
        call    putc
        ret

; ---------------------------------------------------------------------------
; read_line - 1 行読む (AH=0Ah)
; ---------------------------------------------------------------------------
read_line:
        mov     dx, input_buf
        mov     byte [input_buf], MAXLINE - 2
        mov     ah, 0x0A
        int     0x21

        ; 読めた長さのぶんだけ切り出して 0 終端にする
        movzx   cx, byte [input_buf + 1]
        mov     si, input_buf + 2
        mov     di, line_buf
        jcxz    .empty
.copy:
        lodsb
        stosb
        loop    .copy
.empty:
        mov     byte [di], 0
        ret

; ---------------------------------------------------------------------------
; exec_line - 1 行を解釈して実行する
; ---------------------------------------------------------------------------
exec_line:
        mov     si, line_buf
exec_line_at:
        call    skip_spaces
        cmp     byte [si], 0
        je      exec_line_ret           ; 空行

        ; --- リダイレクトとパイプを先に取り除く ---
        ;
        ; "DIR > FILE" や "TYPE X | MORE" の記号はコマンドの引数ではなく
        ; シェルの指示なので、コマンドに渡す前に行から抜いておく。
        ; DOS のプログラムはハンドル 0/1 をそのまま使うだけで、
        ; 自分がリダイレクトされていることを知らない。
        call    redir_parse

        cmp     word [pipe_ptr], 0
        jne     exec_pipeline

        call    redir_apply
        jc      exec_line_ret
        push    si
        call    exec_command
        pop     si
        call    redir_restore
        ret

; --- パイプ: 左をいったんファイルに出し、それを右に食わせる ---
;
; 当時の DOS も同じことをしていた。プロセスが同時に 2 つ動かないので、
; 本当の意味で管をつなぐことはできない。
exec_pipeline:
        push    si                      ; SI = 左側 (clean_buf)
        mov     ax, [pipe_ptr]
        push    ax                      ; 右側の位置

        ; 入れ子のパイプでも名前がぶつからないよう、深さで番号を変える
        inc     byte [pipe_depth]
        mov     al, [pipe_depth]
        add     al, '0'
        mov     [pipe_tmp_name + 4], al

        ; --- 左側: 出力を一時ファイルへ ---
        mov     di, redir_out
        mov     si, pipe_tmp_name
        call    strcpy
        mov     byte [redir_append], 0
        mov     byte [redir_in], 0
        call    redir_apply
        jc      .fail
        pop     ax
        pop     si
        push    ax
        call    exec_command
        call    redir_restore

        ; --- 右側: 入力を一時ファイルから ---
        mov     byte [redir_out], 0
        mov     di, redir_in
        mov     si, pipe_tmp_name
        call    strcpy
        call    redir_apply
        jc      .fail2
        pop     si                      ; 右側の位置
        push    si
        call    exec_line_at            ; 入れ子のパイプもここで処理される
        call    redir_restore

        mov     dx, pipe_tmp_name
        mov     ah, 0x41                ; 一時ファイルを消す
        int     0x21
        pop     ax
        dec     byte [pipe_depth]
        ret
.fail:
        pop     ax
        pop     si
        dec     byte [pipe_depth]
        ret
.fail2:
        pop     si
        dec     byte [pipe_depth]
        ret

exec_line_ret:
        ret

; --- コマンドを 1 つ実行する (リダイレクトは呼び出し側で済ませてある) ---
exec_command:
        call    skip_spaces
        cmp     byte [si], 0
        je      .done                   ; 空行

        ; コマンド名を切り出して大文字にする
        mov     di, cmd_buf
        xor     cx, cx
.name:
        mov     al, [si]
        test    al, al
        jz      .name_done
        cmp     al, ' '
        je      .name_done
        cmp     al, 9
        je      .name_done
        inc     si
        call    upcase
        stosb
        inc     cx
        cmp     cx, 12
        jb      .name
        ; 長すぎる名前は打ち切る
.name_done:
        mov     byte [di], 0
        mov     [args_ptr], si          ; 残りが引数

        ; REM とラベル行は何もしない
        mov     si, cmd_buf
        mov     di, str_rem
        call    streq
        je      .done
        mov     si, [args_ptr]

        ; "C:" のようにドライブ名だけを打たれたら切り替える。
        ; 内部コマンドの表より先に見る (DOS もそうしている)。
        mov     si, cmd_buf
        cmp     byte [si + 1], ':'
        jne     .not_drive
        cmp     byte [si + 2], 0
        jne     .not_drive
        mov     al, [si]
        cmp     al, 'A'
        jb      .not_drive
        cmp     al, 'Z'
        ja      .not_drive
        sub     al, 'A'
        mov     dl, al
        mov     ah, 0x0E                ; ドライブ選択
        int     0x21
        ; 切り替わったか確かめる。無効なドライブは黙って戻さない。
        mov     ah, 0x19
        int     0x21
        cmp     al, dl
        je      .done
        mov     si, msg_bad_drive
        call    puts
        jmp     .done
.not_drive:

        ; 内部コマンドの表を引く
        mov     bx, cmd_table
.lookup:
        mov     dx, [bx]                ; コマンド名へのポインタ
        test    dx, dx
        jz      .external
        push    bx
        mov     si, dx
        mov     di, cmd_buf
        call    streq
        pop     bx
        je      .found
        add     bx, 4
        jmp     .lookup

.found:
        mov     si, [args_ptr]
        call    skip_spaces
        call    [bx + 2]                ; ハンドラを呼ぶ
.done:
        ret

.external:
        call    run_external
        ret

; ============================================================================
; リダイレクトとパイプ
;
; DOS のプログラムはハンドル 0 (標準入力) と 1 (標準出力) をそのまま使う。
; "> FILE" はプログラムには一切見えず、シェルが起動前にハンドル 1 を
; 差し替えるだけ。AH=45h (複製) で元を控え、AH=46h (指定した番号へ複製)
; で差し替え、終わったら戻す。
; ============================================================================

; --- redir_parse - 行から記号を抜き取る ---
;   入力: DS:SI = 行
;   出力: SI = clean_buf (記号を抜いたコマンド)
;         redir_in / redir_out / redir_append / pipe_ptr
redir_parse:
        push    ax
        push    bx
        push    di

        mov     byte [redir_in], 0
        mov     byte [redir_out], 0
        mov     byte [redir_append], 0
        mov     word [pipe_ptr], 0

        mov     di, clean_buf
.loop:
        mov     al, [si]
        test    al, al
        jz      .done

        cmp     al, '"'
        je      .quote
        cmp     al, '<'
        je      .in
        cmp     al, '>'
        je      .out
        cmp     al, '|'
        je      .pipe

        stosb
        inc     si
        jmp     .loop

.quote:
        ; 引用符の中は記号として扱わない (長い名前に > が入ることは無いが、
        ; 引用符の中身をそのまま渡すという約束は守る)
        stosb
        inc     si
.qloop:
        mov     al, [si]
        test    al, al
        jz      .done
        stosb
        inc     si
        cmp     al, '"'
        jne     .qloop
        jmp     .loop

.in:
        inc     si
        push    di
        mov     di, redir_in
        call    redir_word
        pop     di
        jmp     .loop

.out:
        inc     si
        mov     byte [redir_append], 0
        cmp     byte [si], '>'
        jne     .out_go
        inc     si
        mov     byte [redir_append], 1
.out_go:
        push    di
        mov     di, redir_out
        call    redir_word
        pop     di
        jmp     .loop

.pipe:
        inc     si
        mov     [pipe_ptr], si          ; 右側の位置を覚える
        jmp     .done

.done:
        mov     byte [di], 0
        mov     si, clean_buf
        pop     di
        pop     bx
        pop     ax
        ret

; redir_word - 記号のあとのファイル名を DS:SI から ES:DI へ取る
redir_word:
        push    ax
.skip:
        mov     al, [si]
        cmp     al, ' '
        je      .adv
        cmp     al, 9
        jne     .copy
.adv:
        inc     si
        jmp     .skip
.copy:
        cmp     byte [si], '"'
        jne     .plain
        inc     si
.q:
        mov     al, [si]
        test    al, al
        jz      .end
        inc     si
        cmp     al, '"'
        je      .end
        mov     [di], al
        inc     di
        jmp     .q
.plain:
        mov     al, [si]
        test    al, al
        jz      .end
        cmp     al, ' '
        je      .end
        cmp     al, 9
        je      .end
        cmp     al, '<'
        je      .end
        cmp     al, '>'
        je      .end
        cmp     al, '|'
        je      .end
        inc     si
        mov     [di], al
        inc     di
        jmp     .plain
.end:
        mov     byte [di], 0
        pop     ax
        ret

; --- redir_apply - ハンドル 0/1 を差し替える ---
;   出力: CF=1 なら開けなかった (メッセージは出してある)
redir_apply:
        push    ax
        push    bx
        push    cx
        push    dx

        mov     word [saved_stdin], 0xFFFF
        mov     word [saved_stdout], 0xFFFF

        cmp     byte [redir_in], 0
        je      .no_in
        mov     dx, redir_in
        mov     ax, 0x3D00              ; 読み取りで開く
        int     0x21
        jc      .in_err
        mov     [new_stdin], ax

        mov     bx, 0                   ; いまのハンドル 0 を控える
        mov     ah, 0x45
        int     0x21
        jc      .in_err2
        mov     [saved_stdin], ax

        mov     bx, [new_stdin]
        mov     cx, 0
        mov     ah, 0x46                ; ハンドル 0 に重ねる
        int     0x21
        mov     bx, [new_stdin]
        mov     ah, 0x3E
        int     0x21
.no_in:

        cmp     byte [redir_out], 0
        je      .no_out
        cmp     byte [redir_append], 0
        jne     .append
        mov     dx, redir_out
        xor     cx, cx
        mov     ah, 0x3C                ; 作り直す
        int     0x21
        jc      .out_err
        jmp     .out_have
.append:
        mov     dx, redir_out
        mov     ax, 0x3D01              ; 書き込みで開く
        int     0x21
        jnc     .seek_end
        mov     dx, redir_out
        xor     cx, cx
        mov     ah, 0x3C
        int     0x21
        jc      .out_err
        jmp     .out_have
.seek_end:
        mov     bx, ax
        push    ax
        xor     cx, cx
        xor     dx, dx
        mov     ax, 0x4202              ; 末尾へ
        int     0x21
        pop     ax
.out_have:
        mov     [new_stdout], ax

        mov     bx, 1
        mov     ah, 0x45
        int     0x21
        jc      .out_err2
        mov     [saved_stdout], ax

        mov     bx, [new_stdout]
        mov     cx, 1
        mov     ah, 0x46
        int     0x21
        mov     bx, [new_stdout]
        mov     ah, 0x3E
        int     0x21
.no_out:
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        clc
        ret

.in_err:
.in_err2:
        mov     si, msg_redir_in
        call    puts
        jmp     .fail
.out_err:
.out_err2:
        mov     si, msg_redir_out
        call    puts
.fail:
        call    redir_restore
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

; --- redir_restore - 控えておいたハンドルを戻す ---
redir_restore:
        push    ax
        push    bx
        push    cx

        cmp     word [saved_stdin], 0xFFFF
        je      .no_in
        mov     bx, [saved_stdin]
        mov     cx, 0
        mov     ah, 0x46
        int     0x21
        mov     bx, [saved_stdin]
        mov     ah, 0x3E
        int     0x21
        mov     word [saved_stdin], 0xFFFF
.no_in:
        cmp     word [saved_stdout], 0xFFFF
        je      .no_out
        mov     bx, [saved_stdout]
        mov     cx, 1
        mov     ah, 0x46
        int     0x21
        mov     bx, [saved_stdout]
        mov     ah, 0x3E
        int     0x21
        mov     word [saved_stdout], 0xFFFF
.no_out:
        pop     cx
        pop     bx
        pop     ax
        ret

; ============================================================================
; 内部コマンド
; ============================================================================

; --- CLS: 画面を消す -------------------------------------------------------
cmd_cls:
        push    bp
        mov     ax, 0x0600              ; ウィンドウ全体をスクロール = クリア
        mov     bh, 0x07
        xor     cx, cx
        mov     dx, 0x184F              ; 24 行 79 桁
        int     0x10
        mov     ah, 0x02                ; カーソルを左上へ
        xor     bh, bh
        xor     dx, dx
        int     0x10
        pop     bp
        ret

; --- VER -------------------------------------------------------------------
cmd_ver:
        mov     si, msg_ver
        call    puts
        mov     ah, 0x30                ; DOS のバージョンを聞く
        int     0x21
        push    ax
        movzx   ax, al
        call    put_dec
        mov     al, '.'
        call    putc
        pop     ax
        movzx   ax, ah
        call    put_dec2
        call    newline
        ret

; --- ECHO ------------------------------------------------------------------
; --- ECHO ------------------------------------------------------------------
;
; 引数なしなら今の状態を答える。ON / OFF なら状態を変える。それ以外は
; そのまま出す。バッチの中で「実行する行を見せるかどうか」がこの状態で
; 決まるので、ただの表示コマンドではない。
cmd_echo:
        cmp     byte [si], 0
        jne     .text
        cmp     byte [echo_on], 0
        je      .say_off
        mov     si, msg_echo_on
        call    puts
        ret
.say_off:
        mov     si, msg_echo_off
        call    puts
        ret
.text:
        push    si
        mov     di, str_on
        call    stricmp_z
        pop     si
        je      .set_on
        push    si
        mov     di, str_off
        call    stricmp_z
        pop     si
        je      .set_off
        call    puts
        call    newline
        ret
.set_on:
        mov     byte [echo_on], 1
        ret
.set_off:
        mov     byte [echo_on], 0
        ret

; --- 大小を区別せずに 0 終端の 2 つを比べる (ZF=1 なら同じ) ---
stricmp_z:
        push    ax
        push    si
        push    di
.loop:
        mov     al, [si]
        call    upcase_al
        mov     ah, [di]
        cmp     al, ah
        jne     .out
        test    al, al
        jz      .out
        inc     si
        inc     di
        jmp     .loop
.out:
        pop     di
        pop     si
        pop     ax
        ret

upcase_al:
        cmp     al, 'a'
        jb      .out
        cmp     al, 'z'
        ja      .out
        sub     al, 0x20
.out:
        ret

; --- EXIT ------------------------------------------------------------------
cmd_exit:
        mov     ax, 0x4C00
        int     0x21

; --- DIR -------------------------------------------------------------------
cmd_dir:
        ; 引数がなければ "*.*"
        cmp     byte [si], 0
        jne     .have_arg
        mov     si, str_all
.have_arg:
        mov     di, path_buf
        call    strcpy_path

        ; ディレクトリ名だけを指定された場合は "\*.*" を足す。
        ; 判定は「属性を引いてディレクトリだったら」で行う。
        mov     dx, path_buf
        mov     ax, 0x4300
        int     0x21
        jc      .no_append
        test    cl, ATTR_DIRECTORY
        jz      .no_append
        mov     di, path_buf
        call    strend
        cmp     byte [di - 1], '\'
        je      .skip_sep
        mov     byte [di], '\'
        inc     di
.skip_sep:
        mov     si, str_all
        call    strcpy
.no_append:

        call    show_dir_head

        mov     dword [file_count], 0
        mov     dword [byte_count], 0

        ; まず長い名前で探す (AX=714Eh)。使えなければ 8.3 の側に落ちる。
        ; 長い名前の窓口が無い DOS では CF=1 で AX=7100h が返る約束なので、
        ; 呼ぶ側はそれを見て古いやり方に切り替えられる。
        mov     dx, path_buf
        mov     cx, ATTR_DIRECTORY      ; CL = 属性マスク
        mov     si, 1                   ; 時刻は DOS の形式で
        push    es
        push    ds
        pop     es
        mov     di, lfn_find_buf
        mov     ax, 0x714E
        int     0x21
        pop     es
        jc      .try_short
        mov     [dir_lfn_handle], ax
        mov     byte [dir_use_lfn], 1
        jmp     .entry

.try_short:
        mov     byte [dir_use_lfn], 0
        mov     dx, path_buf
        mov     cx, ATTR_DIRECTORY
        mov     ah, 0x4E                ; FindFirst
        int     0x21
        jc      .none

.entry:
        call    .show_entry
        cmp     byte [dir_use_lfn], 0
        je      .short_next

        push    es
        push    ds
        pop     es
        mov     di, lfn_find_buf
        mov     bx, [dir_lfn_handle]
        mov     si, 1
        mov     ax, 0x714F              ; 続きを探す
        int     0x21
        pop     es
        jnc     .entry

        mov     bx, [dir_lfn_handle]
        mov     ax, 0x71A1              ; 検索を閉じる
        int     0x21
        jmp     .summary

.short_next:
        mov     ah, 0x4F                ; FindNext
        int     0x21
        jnc     .entry

.summary:
        ; --- 集計 ---
        call    newline
        mov     eax, [file_count]
        call    put_dec32
        mov     si, msg_files
        call    puts
        mov     eax, [byte_count]
        call    put_dec32
        mov     si, msg_bytes
        call    puts

        ; 空き容量 (AH=36h)
        mov     ah, 0x36
        mov     dl, 0
        int     0x21
        cmp     ax, 0xFFFF
        je      .no_free
        ; 空きバイト = クラスタあたりセクタ数 * セクタサイズ * 空きクラスタ数
        movzx   eax, ax
        movzx   ecx, cx
        mul     ecx                     ; EAX = クラスタあたりバイト数
        movzx   ecx, bx
        mul     ecx
        call    put_dec32
        mov     si, msg_free
        call    puts
.no_free:
        ret

.none:
        mov     si, msg_not_found
        call    puts
        ret

; DTA の内容を 1 行で表示する
.show_entry:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si

        ; 探し方が 2 通りあるので、まず要る値を 1 か所に取り出す
        cmp     byte [dir_use_lfn], 0
        je      .from_dta

        mov     bx, lfn_find_buf
        mov     al, [bx + LFD_ATTR]
        mov     [.attr], al
        mov     ax, [bx + LFD_TIME]
        mov     [.time], ax
        mov     ax, [bx + LFD_DATE]
        mov     [.date], ax
        mov     eax, [bx + LFD_SIZE]
        mov     [.size], eax
        lea     ax, [bx + LFD_NAME]
        mov     [.name], ax
        jmp     .have_fields

.from_dta:
        mov     bx, dta_buf
        mov     al, [bx + FIND_ATTR]
        mov     [.attr], al
        mov     ax, [bx + FIND_TIME]
        mov     [.time], ax
        mov     ax, [bx + FIND_DATE]
        mov     [.date], ax
        mov     eax, [bx + FIND_SIZE]
        mov     [.size], eax
        lea     ax, [bx + FIND_NAME]
        mov     [.name], ax

.have_fields:
        ; ボリュームラベルは飛ばす
        test    byte [.attr], ATTR_VOLUME
        jnz     .skip

        ; 名前。長い名前が入るように桁を広げてある。
        mov     si, [.name]
        call    puts
        mov     si, [.name]
        call    strlen
        mov     ax, DIR_NAME_COLS
        cmp     cx, DIR_NAME_COLS - 1
        jb      .pad
        mov     ax, cx
        inc     ax                      ; 桁を超えたら空白 1 つだけ空ける
.pad:
        sub     ax, cx
        call    put_spaces

        ; ディレクトリなら <DIR>、ファイルならサイズ
        test    byte [.attr], ATTR_DIRECTORY
        jz      .is_file
        mov     si, msg_dir_tag
        call    puts
        jmp     .datetime
.is_file:
        mov     eax, [.size]
        push    eax
        call    count_digits            ; AX = 桁数
        mov     cx, 10
        sub     cx, ax
        mov     ax, cx
        call    put_spaces
        pop     eax
        call    put_dec32
        inc     dword [file_count]
        mov     eax, [.size]
        add     [byte_count], eax

.datetime:
        mov     al, ' '
        call    putc
        call    putc
        mov     ax, [.date]
        call    put_date
        mov     al, ' '
        call    putc
        mov     ax, [.time]
        call    put_time
        call    newline

.skip:
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret
.attr:  db 0
.time:  dw 0
.date:  dw 0
.size:  dd 0
.name:  dw 0

; --- TYPE ------------------------------------------------------------------
cmd_type:
        cmp     byte [si], 0
        je      .no_arg
        mov     di, path_buf
        call    strcpy_path

        mov     dx, path_buf
        mov     ax, 0x3D00              ; 読み取りで開く
        int     0x21
        jc      .cant_open
        mov     [file_handle], ax

.loop:
        mov     bx, [file_handle]
        mov     cx, 512
        mov     dx, io_buf
        mov     ah, 0x3F
        int     0x21
        jc      .close
        test    ax, ax
        jz      .close

        ; 読めたぶんをそのまま標準出力へ。Ctrl-Z が出たらそこで終わり。
        mov     cx, ax
        mov     si, io_buf
.emit:
        lodsb
        cmp     al, 0x1A
        je      .close
        call    putc
        loop    .emit
        jmp     .loop

.close:
        mov     bx, [file_handle]
        mov     ah, 0x3E
        int     0x21
        ret

.no_arg:
        mov     si, msg_need_file
        call    puts
        ret
.cant_open:
        mov     si, msg_not_found
        call    puts
        ret

; --- CD / CHDIR ------------------------------------------------------------
cmd_cd:
        cmp     byte [si], 0
        jne     .change
        ; 引数なしなら今の場所を表示する
        mov     si, str_drive
        call    puts
        mov     si, cwd_buf
        mov     ah, 0x47
        mov     dl, 0
        int     0x21
        mov     si, cwd_buf
        call    puts
        call    newline
        ret
.change:
        mov     di, path_buf
        call    strcpy_path
        mov     dx, path_buf
        mov     ah, 0x3B
        int     0x21
        jc      .fail
        ret
.fail:
        mov     si, msg_bad_path
        call    puts
        ret

; --- MD / MKDIR ------------------------------------------------------------
cmd_md:
        cmp     byte [si], 0
        je      .no_arg
        mov     di, path_buf
        call    strcpy_path
        mov     dx, path_buf
        mov     ah, 0x39
        int     0x21
        jc      .fail
        ret
.no_arg:
        mov     si, msg_need_name
        call    puts
        ret
.fail:
        mov     si, msg_md_fail
        call    puts
        ret

; --- RD / RMDIR ------------------------------------------------------------
cmd_rd:
        cmp     byte [si], 0
        je      cmd_md.no_arg
        mov     di, path_buf
        call    strcpy_path
        mov     dx, path_buf
        mov     ah, 0x3A
        int     0x21
        jc      .fail
        ret
.fail:
        mov     si, msg_rd_fail
        call    puts
        ret

; --- DEL / ERASE -----------------------------------------------------------
cmd_del:
        cmp     byte [si], 0
        je      cmd_md.no_arg
        mov     di, path_buf
        call    strcpy_path
        mov     dx, path_buf
        mov     ah, 0x41
        int     0x21
        jc      .fail
        ret
.fail:
        mov     si, msg_not_found
        call    puts
        ret

; --- REN / RENAME ----------------------------------------------------------
cmd_ren:
        cmp     byte [si], 0
        je      .usage

        ; 1 つ目の名前
        mov     di, path_buf
        call    copy_word
        call    skip_spaces
        cmp     byte [si], 0
        je      .usage

        ; 2 つ目の名前
        mov     di, path_buf2
        call    copy_word

        push    ds
        pop     es
        mov     dx, path_buf
        mov     di, path_buf2
        mov     ah, 0x56
        int     0x21
        jc      .fail
        ret
.usage:
        mov     si, msg_ren_usage
        call    puts
        ret
.fail:
        mov     si, msg_not_found
        call    puts
        ret

; --- COPY ------------------------------------------------------------------
cmd_copy:
        cmp     byte [si], 0
        je      .usage
        mov     di, path_buf
        call    copy_word
        call    skip_spaces
        cmp     byte [si], 0
        je      .usage
        mov     di, path_buf2
        call    copy_word

        mov     dx, path_buf
        mov     ax, 0x3D00
        int     0x21
        jc      .no_src
        mov     [file_handle], ax

        mov     dx, path_buf2
        xor     cx, cx
        mov     ah, 0x3C
        int     0x21
        jc      .no_dst
        mov     [file_handle2], ax

        mov     dword [byte_count], 0
.loop:
        mov     bx, [file_handle]
        mov     cx, 512
        mov     dx, io_buf
        mov     ah, 0x3F
        int     0x21
        jc      .done
        test    ax, ax
        jz      .done

        mov     cx, ax
        movzx   eax, ax
        add     [byte_count], eax
        mov     bx, [file_handle2]
        mov     dx, io_buf
        mov     ah, 0x40
        int     0x21
        jc      .done
        jmp     .loop

.done:
        mov     bx, [file_handle]
        mov     ah, 0x3E
        int     0x21
        mov     bx, [file_handle2]
        mov     ah, 0x3E
        int     0x21

        mov     si, msg_copied
        call    puts
        ret

.usage:
        mov     si, msg_copy_usage
        call    puts
        ret
.no_src:
        mov     si, msg_not_found
        call    puts
        ret
.no_dst:
        mov     bx, [file_handle]
        mov     ah, 0x3E
        int     0x21
        mov     si, msg_cant_create
        call    puts
        ret

; --- DATE / TIME -----------------------------------------------------------
cmd_date:
        mov     si, msg_date
        call    puts
        mov     ah, 0x2A
        int     0x21
        push    cx
        push    dx
        movzx   ax, dh
        call    put_dec2
        mov     al, '/'
        call    putc
        pop     dx
        push    dx
        movzx   ax, dl
        call    put_dec2
        mov     al, '/'
        call    putc
        pop     dx
        pop     cx
        mov     ax, cx
        call    put_dec
        call    newline
        ret

cmd_time:
        mov     si, msg_time
        call    puts
        mov     ah, 0x2C
        int     0x21
        push    dx
        push    cx
        movzx   ax, ch
        call    put_dec2
        mov     al, ':'
        call    putc
        pop     cx
        movzx   ax, cl
        call    put_dec2
        mov     al, ':'
        call    putc
        pop     dx
        movzx   ax, dh
        call    put_dec2
        call    newline
        ret

; --- MEM: MCB 連鎖を辿って使用状況を出す -----------------------------------
;
; AH=52h が返す「List of Lists」の 2 バイト手前に最初の MCB のセグメントが
; 入っている、という昔からの約束をそのまま使う。当時のメモリ表示ツールと
; 同じやり方で、この連鎖が本物の形で存在しているかどうかの確認にもなる。
cmd_mem:
        push    es
        mov     ah, 0x52
        int     0x21
        mov     ax, [es:bx - 2]         ; 最初の MCB
        pop     es

        mov     [mcb_cur], ax
        mov     dword [mem_used], 0
        mov     dword [mem_free], 0

        mov     si, msg_mem_head
        call    puts

.loop:
        push    es
        mov     es, [mcb_cur]
        mov     al, [es:0]              ; シグネチャ
        mov     bx, [es:1]              ; 所有者
        mov     cx, [es:3]              ; 大きさ (パラグラフ)
        pop     es

        mov     [mcb_sig], al
        mov     [mcb_owner], bx
        mov     [mcb_size], cx

        ; セグメント
        mov     ax, [mcb_cur]
        inc     ax
        call    put_hex16
        mov     si, str_sp2
        call    puts

        ; 大きさ (バイト)
        movzx   eax, word [mcb_size]
        shl     eax, 4
        push    eax
        call    count_digits
        mov     cx, 8
        sub     cx, ax
        mov     ax, cx
        call    put_spaces
        pop     eax
        call    put_dec32
        mov     si, str_sp2
        call    puts

        ; 持ち主
        mov     ax, [mcb_owner]
        test    ax, ax
        jz      .is_free
        cmp     ax, 8
        je      .is_dos
        mov     si, msg_owner_prog
        call    puts
        movzx   eax, word [mcb_size]
        shl     eax, 4
        add     [mem_used], eax
        jmp     .next
.is_free:
        mov     si, msg_owner_free
        call    puts
        movzx   eax, word [mcb_size]
        shl     eax, 4
        add     [mem_free], eax
        jmp     .next
.is_dos:
        mov     si, msg_owner_dos
        call    puts
        movzx   eax, word [mcb_size]
        shl     eax, 4
        add     [mem_used], eax

.next:
        call    newline
        cmp     byte [mcb_sig], 'Z'
        je      .summary
        mov     ax, [mcb_cur]
        add     ax, [mcb_size]
        inc     ax
        mov     [mcb_cur], ax
        jmp     .loop

.summary:
        call    newline
        mov     eax, [mem_used]
        call    put_dec32
        mov     si, msg_mem_used
        call    puts
        mov     eax, [mem_free]
        call    put_dec32
        mov     si, msg_mem_free
        call    puts

        ; いちばん大きい空きブロック (AH=48h に 0xFFFF を渡すと教えてくれる)
        mov     bx, 0xFFFF
        mov     ah, 0x48
        int     0x21
        movzx   eax, bx
        shl     eax, 4
        call    put_dec32
        mov     si, msg_mem_largest
        call    puts
        ret

; ============================================================================
; 外部プログラムの実行
;
; "NAME" と言われたら NAME.COM → NAME.EXE の順に探す。拡張子が付いて
; いればそのまま使う。見つけたら AH=4Bh に渡す。
; ============================================================================
run_external:
        ; コマンド名を作業領域へ
        mov     si, cmd_buf
        mov     di, prog_path
        call    strcpy

        ; すでに拡張子が付いているか
        mov     si, cmd_buf
        call    has_extension
        jc      .try_exact

        ; ".COM" を足して試す
        mov     di, prog_path
        call    strend
        mov     si, str_com
        call    strcpy
        call    .file_exists
        jnc     .found

        ; ".EXE" に差し替えて試す
        mov     si, cmd_buf
        mov     di, prog_path
        call    strcpy
        mov     di, prog_path
        call    strend
        mov     si, str_exe
        call    strcpy
        call    .file_exists
        jnc     .found

        ; ".BAT" に差し替えて試す
        mov     si, cmd_buf
        mov     di, prog_path
        call    strcpy
        mov     di, prog_path
        call    strend
        mov     si, str_bat
        call    strcpy
        call    .file_exists
        jnc     .as_batch
        jmp     .bad_command

.as_batch:
        call    run_batch
        ret

.try_exact:
        call    .file_exists
        jc      .bad_command
        mov     si, prog_path
        call    ends_with_bat
        jc      .as_batch

.found:
        ; コマンドテイルを DOS の形式で組む (長さ + 本文 + 0Dh)
        mov     si, [args_ptr]
        mov     di, tail_buf + 1
        xor     cx, cx
.tail:
        lodsb
        test    al, al
        jz      .tail_done
        stosb
        inc     cx
        cmp     cx, 126
        jb      .tail
.tail_done:
        mov     byte [di], 0x0D
        mov     [tail_buf], cl

        ; EXEC のパラメータブロック
        mov     ax, ds
        mov     [epb_tail + 2], ax
        mov     [epb_fcb1 + 2], ax
        mov     [epb_fcb2 + 2], ax
        mov     word [epb_env], 0       ; 0 = 親の環境を継ぐ

        ; SS:SP を控える。EXEC から戻ったとき、どのレジスタが生きているかを
        ; 当てにしないのが当時からの作法。
        mov     [save_ss], ss
        mov     [save_sp], sp

        push    ds
        pop     es
        mov     dx, prog_path
        mov     bx, epb
        mov     ax, 0x4B00
        int     0x21

        cli
        mov     ss, [cs:save_ss]
        mov     sp, [cs:save_sp]
        sti
        push    ds
        pop     es
        mov     ax, cs
        mov     ds, ax

        jc      .exec_failed
        ret

.exec_failed:
        mov     si, msg_exec_fail
        call    puts
        ret

.bad_command:
        mov     si, msg_bad_command
        call    puts
        ret

; prog_path のファイルが存在するか。CF=0 ならある
.file_exists:
        push    ax
        push    cx
        push    dx
        mov     dx, prog_path
        mov     ax, 0x4300              ; 属性の取得
        int     0x21
        pop     dx
        pop     cx
        pop     ax
        ret

; ============================================================================
; バッチファイル (.BAT)
;
; 1 行ずつ読んで exec_line に渡すだけの最小限の実装。行頭の '@' で
; その行のエコーを止める、':' で始まる行はラベルとして読み飛ばす、
; という DOS の作法には合わせてある。入れ子の呼び出しには対応していない。
; ============================================================================

; --- 起動時に AUTOEXEC.BAT があれば実行する ---
run_autoexec:
        ; ドライブ文字は決め打ちにしない。起動したドライブに置かれた
        ; AUTOEXEC.BAT を読む。ハードディスクに入れた MYDOS は C: から
        ; 立ち上がるので、A: 固定だと自分の AUTOEXEC.BAT が読まれない。
        mov     ah, 0x19
        int     0x21
        add     al, 'A'
        mov     [str_autoexec], al

        mov     si, str_autoexec
        mov     di, prog_path
        call    strcpy
        call    run_external.file_exists
        jc      .none
        call    run_batch
.none:
        ret

; --- prog_path のバッチファイルを実行する ---
run_batch:
        cmp     byte [bat_active], 0
        jne     .nested                 ; 入れ子は扱わない
        mov     byte [bat_active], 1
        mov     word [bat_len], 0

        mov     dx, prog_path
        mov     ax, 0x3D00
        int     0x21
        jc      .done
        mov     [file_handle], ax

        mov     bx, ax
        mov     cx, BATMAX
        mov     dx, bat_buf
        mov     ah, 0x3F
        int     0x21
        jc      .close
        mov     [bat_len], ax
.close:
        mov     bx, [file_handle]
        mov     ah, 0x3E
        int     0x21

        mov     si, bat_buf
        mov     cx, [bat_len]
.lines:
        jcxz    .done
        mov     di, line_buf
.copy:
        jcxz    .run
        lodsb
        dec     cx
        cmp     al, 13
        je      .run
        cmp     al, 10
        je      .run
        cmp     al, 0x1A
        je      .eof
        stosb
        jmp     .copy
.run:
        mov     byte [di], 0
        push    cx
        push    si
        call    batch_line
        pop     si
        pop     cx
        jmp     .lines
.eof:
        mov     byte [di], 0
        push    cx
        push    si
        call    batch_line
        pop     si
        pop     cx
.done:
        mov     byte [bat_active], 0
        mov     byte [echo_on], 1       ; 対話に戻るので元に戻す
.nested:
        ret

; --- 1 行ぶんを表示して実行する ---
batch_line:
        mov     si, line_buf
        call    skip_spaces
        mov     al, [si]
        test    al, al
        jz      .skip                   ; 空行
        cmp     al, ':'
        je      .skip                   ; ラベル
        cmp     al, '@'
        je      .quiet

        ; ECHO が ON のときだけ、実行する行をそのまま見せる
        cmp     byte [echo_on], 0
        je      .exec
        push    si
        call    show_prompt
        pop     si
        push    si
        call    puts
        pop     si
        call    newline
        jmp     .exec
.quiet:
        inc     si
        call    skip_spaces
.exec:
        call    exec_line_at
.skip:
        ret

; --- prog_path が ".BAT" で終わっているか (CF=1 なら終わっている) ---
ends_with_bat:
        push    ax
        push    cx
        push    si
        push    di
        mov     di, si
        call    strlen                  ; CX = 長さ
        cmp     cx, 4
        jb      .no
        add     di, cx
        sub     di, 4
        mov     si, str_bat
.cmp:
        mov     al, [si]
        test    al, al
        jz      .yes
        mov     ah, [di]
        push    ax
        mov     al, ah
        call    upcase
        mov     ah, al
        pop     ax
        cmp     al, ah
        jne     .no
        inc     si
        inc     di
        jmp     .cmp
.yes:
        pop     di
        pop     si
        pop     cx
        pop     ax
        stc
        ret
.no:
        pop     di
        pop     si
        pop     cx
        pop     ax
        clc
        ret

; ============================================================================
; 小道具
; ============================================================================

; --- 1 文字出力 ------------------------------------------------------------
putc:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     [char_buf], al
        mov     dx, char_buf
        mov     cx, 1
        mov     bx, 1                   ; 標準出力
        mov     ah, 0x40
        int     0x21
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

; --- 0 終端文字列の出力 (DS:SI) --------------------------------------------
puts:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        mov     dx, si
        call    strlen                  ; CX = 長さ
        jcxz    .done
        mov     bx, 1
        mov     ah, 0x40
        int     0x21
.done:
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; --- 文字列操作 ------------------------------------------------------------
; strlen: DS:SI → CX = 長さ (SI は保存)
strlen:
        push    ax
        push    si
        xor     cx, cx
.loop:
        lodsb
        test    al, al
        jz      .done
        inc     cx
        jmp     .loop
.done:
        pop     si
        pop     ax
        ret

; ; strcpy_path - パスとして写す。前後の二重引用符は外す。
;
; 長い名前には空白が入りうるので、シェルの引数は引用符で囲めるように
; してある。囲みはシェルの都合なので、DOS に渡す前に外しておく。
strcpy_path:
        push    ax
        push    si
        push    di
        cmp     byte [si], '"'
        jne     .plain
        inc     si
.qloop:
        mov     al, [si]
        test    al, al
        jz      .done
        cmp     al, '"'
        je      .done
        inc     si
        mov     [di], al
        inc     di
        jmp     .qloop
.plain:
        mov     al, [si]
        test    al, al
        jz      .done
        inc     si
        mov     [di], al
        inc     di
        jmp     .plain
.done:
        mov     byte [di], 0
        pop     di
        pop     si
        pop     ax
        ret

; strcpy: DS:SI → DS:DI (0 終端込み)。DI は終端の 0 を指して返る
strcpy:
        push    ax
.loop:
        lodsb
        mov     [di], al
        inc     di
        test    al, al
        jnz     .loop
        dec     di
        pop     ax
        ret

; strend: DS:DI の 0 終端を探して DI をそこに合わせる
strend:
        push    ax
.loop:
        mov     al, [di]
        test    al, al
        jz      .done
        inc     di
        jmp     .loop
.done:
        pop     ax
        ret

; streq: DS:SI と DS:DI を比べる → ZF=1 なら一致
streq:
        push    ax
        push    si
        push    di
.loop:
        mov     al, [si]
        cmp     al, [di]
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

; copy_word: DS:SI から空白までを DS:DI へ写す (0 終端付き)
copy_word:
        push    ax

        ; 二重引用符で囲まれていれば、その中は空白も名前の一部として扱う。
        ; 長い名前には空白が入りうるので、これが無いと
        ;   COPY README.TXT "my notes.txt"
        ; のような書き方ができない。囲みの中では引用符だけが終わりの印。
        cmp     byte [si], '"'
        jne     .plain
        inc     si
.quoted:
        mov     al, [si]
        test    al, al
        jz      .done
        cmp     al, '"'
        je      .close
        inc     si
        mov     [di], al
        inc     di
        jmp     .quoted
.close:
        inc     si                      ; 閉じの引用符を飛ばす
        jmp     .done

.plain:
        mov     al, [si]
        test    al, al
        jz      .done
        cmp     al, ' '
        je      .done
        cmp     al, 9
        je      .done
        inc     si
        mov     [di], al
        inc     di
        jmp     .plain
.done:
        mov     byte [di], 0
        pop     ax
        ret

; skip_spaces: DS:SI を空白でない位置まで進める
skip_spaces:
        push    ax
.loop:
        mov     al, [si]
        cmp     al, ' '
        je      .skip
        cmp     al, 9
        je      .skip
        pop     ax
        ret
.skip:
        inc     si
        jmp     .loop

; upcase: AL を大文字に
upcase:
        cmp     al, 'a'
        jb      .done
        cmp     al, 'z'
        ja      .done
        sub     al, 'a' - 'A'
.done:
        ret

; has_extension: DS:SI に '.' が含まれるか → CF=1 なら含まれる
has_extension:
        push    ax
        push    si
.loop:
        lodsb
        test    al, al
        jz      .no
        cmp     al, '.'
        je      .yes
        jmp     .loop
.yes:
        pop     si
        pop     ax
        stc
        ret
.no:
        pop     si
        pop     ax
        clc
        ret

; --- 数値の表示 ------------------------------------------------------------
; put_dec: AX を 10 進で
put_dec:
        push    eax
        movzx   eax, ax
        call    put_dec32
        pop     eax
        ret

; put_dec2: AX を 2 桁 (0 埋め) で
put_dec2:
        push    ax
        push    bx
        push    dx
        xor     dx, dx
        mov     bx, 10
        div     bx
        push    dx
        add     al, '0'
        call    putc
        pop     ax
        add     al, '0'
        call    putc
        pop     dx
        pop     bx
        pop     ax
        ret

; put_dec32: EAX を 10 進で
put_dec32:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     ebx, 10
        xor     ecx, ecx
.split:
        xor     edx, edx
        div     ebx
        push    dx
        inc     ecx
        test    eax, eax
        jnz     .split
.emit:
        pop     ax
        add     al, '0'
        call    putc
        loop    .emit
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

; count_digits: EAX の 10 進桁数 → AX
count_digits:
        push    ebx
        push    edx
        mov     ebx, 10
        xor     cx, cx
.loop:
        xor     edx, edx
        div     ebx
        inc     cx
        test    eax, eax
        jnz     .loop
        mov     ax, cx
        pop     edx
        pop     ebx
        ret

; put_spaces: AX 個の空白
put_spaces:
        push    ax
        push    cx
        mov     cx, ax
        cmp     cx, 0
        jle     .done
        test    cx, 0x8000
        jnz     .done
.loop:
        mov     al, ' '
        call    putc
        loop    .loop
.done:
        pop     cx
        pop     ax
        ret

; put_date: AX = DOS 形式の日付
put_date:
        push    ax
        push    bx
        push    cx
        mov     bx, ax

        mov     ax, bx                  ; 月
        mov     cl, 5
        shr     ax, cl
        and     ax, 0x0F
        call    put_dec2
        mov     al, '-'
        call    putc

        mov     ax, bx                  ; 日
        and     ax, 0x1F
        call    put_dec2
        mov     al, '-'
        call    putc

        mov     ax, bx                  ; 年
        mov     cl, 9
        shr     ax, cl
        add     ax, 1980
        call    put_dec

        pop     cx
        pop     bx
        pop     ax
        ret

; put_time: AX = DOS 形式の時刻
put_time:
        push    ax
        push    bx
        push    cx
        mov     bx, ax

        mov     ax, bx                  ; 時
        mov     cl, 11
        shr     ax, cl
        call    put_dec2
        mov     al, ':'
        call    putc

        mov     ax, bx                  ; 分
        mov     cl, 5
        shr     ax, cl
        and     ax, 0x3F
        call    put_dec2

        pop     cx
        pop     bx
        pop     ax
        ret

; put_hex16: AX を 16 進 4 桁で
put_hex16:
        push    ax
        push    cx
        push    dx
        mov     dx, ax
        mov     cx, 4
.loop:
        rol     dx, 4
        mov     al, dl
        and     al, 0x0F
        add     al, '0'
        cmp     al, '9'
        jbe     .emit
        add     al, 'A' - '0' - 10
.emit:
        call    putc
        loop    .loop
        pop     dx
        pop     cx
        pop     ax
        ret

; ---------------------------------------------------------------------------
; show_dir_head - DIR の見出し " Directory of X:\PATH" を出す
; ---------------------------------------------------------------------------
; 見出しに出すのは「いま並べようとしている場所」であって、カレント
; ディレクトリではない。カレントを出すと "DIR B:" のときに嘘になる
; (A: の中身が出ていないのに "Directory of A:\" と書かれる)。
; AH=60h でパスを正規化し、最後の '\' までを取り出す。
show_dir_head:
        mov     si, msg_dir_head
        call    puts

        push    es
        push    ds
        pop     es
        mov     si, path_buf
        mov     di, cwd_buf
        mov     ah, 0x60
        int     0x21
        pop     es
        jc      .fallback

        ; 最後の '\' の位置を探す
        mov     si, cwd_buf
        xor     di, di                  ; DI = '\' の次の位置 (0 = 見つからず)
        mov     bx, si
.scan:
        mov     al, [bx]
        test    al, al
        jz      .scanned
        cmp     al, '\'
        jne     .next
        mov     di, bx
        inc     di
.next:
        inc     bx
        jmp     .scan
.scanned:
        test    di, di
        jz      .fallback               ; '\' が無い = 正規化に失敗している
        mov     bx, di
        sub     bx, si
        cmp     bx, 3
        jbe     .cut                    ; "X:\" はそのまま残す
        dec     di                      ; それより深ければ末尾の '\' を落とす
.cut:
        mov     byte [di], 0
        call    puts
        jmp     .tail

.fallback:
        mov     ah, 0x19
        int     0x21
        add     al, 'A'
        call    putc
        mov     al, ':'
        call    putc
        mov     al, '\'
        call    putc
        mov     si, cwd_buf
        mov     ah, 0x47
        mov     dl, 0
        int     0x21
        mov     si, cwd_buf
        call    puts

.tail:
        mov     si, msg_crlf2
        call    puts
        ret

; ============================================================================
; 内部コマンドの表
; ============================================================================
cmd_table:
        dw str_cls,   cmd_cls
        dw str_ver,   cmd_ver
        dw str_dir,   cmd_dir
        dw str_type,  cmd_type
        dw str_cd,    cmd_cd
        dw str_chdir, cmd_cd
        dw str_md,    cmd_md
        dw str_mkdir, cmd_md
        dw str_rd,    cmd_rd
        dw str_rmdir, cmd_rd
        dw str_del,   cmd_del
        dw str_erase, cmd_del
        dw str_ren,   cmd_ren
        dw str_rename,cmd_ren
        dw str_copy,  cmd_copy
        dw str_echo,  cmd_echo
        dw str_date,  cmd_date
        dw str_time,  cmd_time
        dw str_mem,   cmd_mem
        dw str_exit,  cmd_exit
        dw 0, 0

str_cls:    db 'CLS', 0
str_ver:    db 'VER', 0
str_dir:    db 'DIR', 0
str_type:   db 'TYPE', 0
str_cd:     db 'CD', 0
str_chdir:  db 'CHDIR', 0
str_md:     db 'MD', 0
str_mkdir:  db 'MKDIR', 0
str_rd:     db 'RD', 0
str_rmdir:  db 'RMDIR', 0
str_del:    db 'DEL', 0
str_erase:  db 'ERASE', 0
str_ren:    db 'REN', 0
str_rename: db 'RENAME', 0
str_copy:   db 'COPY', 0
str_echo:   db 'ECHO', 0
str_date:   db 'DATE', 0
str_time:   db 'TIME', 0
str_mem:    db 'MEM', 0
str_exit:   db 'EXIT', 0

; ============================================================================
; メッセージ
; ============================================================================
msg_banner:     db 'MYDOS Command Interpreter', 13, 10
                db 'Type a command. Internal: DIR CD MD RD TYPE COPY DEL REN', 13, 10
                db '                          CLS VER ECHO DATE TIME MEM EXIT', 13, 10
                db 'Type a drive letter with a colon (C:) to change drives.', 13, 10, 13, 10, 0
msg_ver:        db 'MYDOS Version ', 0
msg_dir_head:   db 13, 10, ' Directory of ', 0
msg_crlf2:      db 13, 10, 13, 10, 0
msg_redir_in:   db 'Cannot open input file', 13, 10, 0
msg_redir_out:  db 'Cannot create output file', 13, 10, 0
msg_bad_drive:  db 'Invalid drive specification', 13, 10, 0
msg_dir_tag:    db '     <DIR>', 0
msg_files:      db ' file(s)', 13, 10, 0
msg_bytes:      db ' bytes', 13, 10, 0
msg_free:       db ' bytes free', 13, 10, 0
msg_not_found:  db 'File not found', 13, 10, 0
msg_bad_path:   db 'Invalid directory', 13, 10, 0
msg_need_file:  db 'Specify a file name', 13, 10, 0
msg_need_name:  db 'Specify a name', 13, 10, 0
msg_md_fail:    db 'Unable to create directory', 13, 10, 0
msg_rd_fail:    db 'Unable to remove directory', 13, 10, 0
msg_ren_usage:  db 'Usage: REN oldname newname', 13, 10, 0
msg_copy_usage: db 'Usage: COPY source dest', 13, 10, 0
msg_cant_create:db 'Unable to create destination', 13, 10, 0
msg_copied:     db '        1 file(s) copied', 13, 10, 0
msg_bad_command:db 'Bad command or file name', 13, 10, 0
msg_exec_fail:  db 'Unable to execute program', 13, 10, 0
msg_echo_on:    db 'ECHO is on', 13, 10, 0
msg_echo_off:   db 'ECHO is off', 13, 10, 0
str_on:         db 'ON', 0
str_off:        db 'OFF', 0
id_echo_on:
echo_on:        db 1
msg_date:       db 'Current date is ', 0
msg_time:       db 'Current time is ', 0
msg_mem_head:   db 13, 10, 'Segment      Size  Owner', 13, 10
                db '-------  --------  ---------------', 13, 10, 0
msg_owner_free: db 'free', 0
msg_owner_dos:  db 'DOS', 0
msg_owner_prog: db 'program', 0
msg_mem_used:   db ' bytes used', 13, 10, 0
msg_mem_free:   db ' bytes free', 13, 10, 0
msg_mem_largest:db ' bytes in largest free block', 13, 10, 0

str_drive:      db 'A:\', 0
str_all:        db '*.*', 0
str_com:        db '.COM', 0
str_exe:        db '.EXE', 0
str_bat:        db '.BAT', 0
str_rem:        db 'REM', 0
str_autoexec:   db 'A:\AUTOEXEC.BAT', 0
str_sp2:        db '  ', 0

; ============================================================================
; 変数
; ============================================================================
args_ptr:       dw 0
file_handle:    dw 0
file_handle2:   dw 0
file_count:     dd 0
byte_count:     dd 0
mem_used:       dd 0
mem_free:       dd 0
mcb_cur:        dw 0
mcb_owner:      dw 0
mcb_size:       dw 0
mcb_sig:        db 0
char_buf:       db 0
save_ss:        dw 0
save_sp:        dw 0

; EXEC のパラメータブロック
epb:
epb_env:        dw 0
epb_tail:       dw tail_buf, 0
epb_fcb1:       dw fcb_dummy, 0
epb_fcb2:       dw fcb_dummy, 0
fcb_dummy:      times 16 db 0

input_buf:      times MAXLINE db 0
line_buf:       times MAXLINE db 0
cmd_buf:        times 16 db 0
path_buf:       times MAXLINE db 0
path_buf2:      times MAXLINE db 0
prog_path:      times MAXLINE db 0
cwd_buf:        times 68 db 0
tail_buf:       times 130 db 0
io_buf:         times 512 db 0
bat_len:        dw 0
bat_active:     db 0
bat_buf:        times BATMAX db 0

; DTA は PSP:0080 のままだと入力バッファと重なるので、自前のものを使う。
; (PSP の 0080 はコマンドテイルの置き場でもあるため)
clean_buf:      times MAXLINE + 8 db 0      ; 記号を抜いたコマンド
redir_in:       times 80 db 0
redir_out:      times 80 db 0
redir_append:   db 0
pipe_ptr:       dw 0
pipe_depth:     db 0
pipe_tmp_name:  db 'PIPE0.$$$', 0
saved_stdin:    dw 0xFFFF
saved_stdout:   dw 0xFFFF
new_stdin:      dw 0
new_stdout:     dw 0

dta_buf:        times 48 db 0
lfn_find_buf:   times LFD_SIZE_TOTAL db 0   ; 長い名前の検索が返す構造体
dir_use_lfn:    db 0                        ; 1 = 長い名前の窓口で探している
dir_lfn_handle: dw 0                        ; そのときの検索の番号

                times 512 db 0
stack_top:
