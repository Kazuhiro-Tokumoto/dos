; ============================================================================
; dpmirun.asm  -  最小の 32bit DPMI クライアント (手で試すための道具)
;
; 自動テストには入れていない。DPMI ホストを同梱していないため。
; ============================================================================
; ============================================================================
; dpmirun.asm  -  最小の 32bit DPMI クライアント
;
; INT 2Fh AX=1687h でホストを探し、作業領域を渡してモード切替の入口を
; far call する。CWSDPMI は 32bit クライアントしか受け付けないので
; AX=1 で入る。戻ってきた時点で CS は 32bit のセレクタになっているから、
; そこから先は bits 32 で組む。
;
; 保護モードに入ったあと、INT 31h AX=0300h (リアルモード割り込みの代行)
; で DOS に文字を出させる。これが出れば
;   ・DPMI ホストが MYDOS の上で保護モードに入れた
;   ・保護モードのプログラムが DOS を呼び返せた
; の両方が確かめられる。
; ============================================================================
        cpu     386
        bits    16
        org     0x100

start:
        mov     [real_seg], cs

        ; .COM は空きを全部持たされている。まず自分を縮めないと、
        ; ホストに渡す作業領域が確保できない。
        mov     sp, stack_top
        mov     bx, stack_top
        add     bx, 15
        mov     cl, 4
        shr     bx, cl
        mov     ah, 0x4A
        int     0x21

        mov     si, m_head
        call    puts

        ; --- ホストを探す ---
        mov     ax, 0x1687
        int     0x2F
        test    ax, ax
        jz      .found
        mov     si, m_nohost
        call    puts
        jmp     quit
.found:
        test    bl, 1                   ; 32bit クライアントを受けるか
        jnz     .can32
        mov     si, m_no32
        call    puts
        jmp     quit
.can32:
        mov     [host_paras], si
        mov     [entry], di
        mov     [entry + 2], es
        mov     si, m_found
        call    puts

        ; --- ホスト用の作業領域 ---
        mov     bx, [host_paras]
        test    bx, bx
        jz      .no_block
        mov     ah, 0x48
        int     0x21
        jc      no_mem
        mov     [host_seg], ax
        jmp     .have_block
.no_block:
        mov     word [host_seg], 0
.have_block:
        mov     si, m_switch
        call    puts

        ; --- 保護モードへ ---
        mov     es, [host_seg]
        mov     ax, 1                   ; 1 = 32bit クライアント
        call    far [entry]
        jnc     short pm_entry          ; 成功 → ここから 32bit

        ; --- 失敗: リアルモードのまま ---
        mov     si, m_failed
        call    puts
quit:
        mov     ax, 0x4C00
        int     0x21
no_mem:
        mov     si, m_nomem
        call    puts
        jmp     quit

; ============================================================================
; ここから保護モード (32bit)
; ============================================================================
        bits    32
pm_entry:
        push    ds
        pop     es                      ; ES は PSP のセレクタなので置き換える

        ; リアルモード呼び出し構造体を 0 で埋める
        mov     edi, rmcs
        mov     ecx, 50
        xor     al, al
        rep     stosb

        ; まずポインタの要らない AH=02h (1 文字出力) で試す。
        ; これが出れば代行の仕組み自体は動いている。
        mov     esi, pm_msg
.ch_loop:
        movzx   eax, byte [esi]
        test    al, al
        jz      .ch_done
        inc     esi

        push    esi
        mov     edi, rmcs
        mov     ecx, 50
        push    eax
        xor     al, al
        push    edi
        rep     stosb
        pop     edi
        pop     eax

        mov     dword [edi + RM_EAX], 0x0200
        mov     [edi + RM_EDX], eax     ; DL = 出す文字
        mov     word [edi + RM_SS], 0
        mov     word [edi + RM_SP], 0

        mov     eax, 0x0300
        mov     ebx, 0x0021
        xor     ecx, ecx
        int     0x31
        pop     esi
        jmp     .ch_loop
.ch_done:

        ; 次にポインタを渡す AH=09h を試す
        mov     edi, rmcs
        mov     ecx, 50
        xor     al, al
        push    edi
        rep     stosb
        pop     edi
        mov     dword [edi + RM_EAX], 0x0900
        movzx   eax, word [real_seg]
        mov     [edi + RM_DS], ax
        mov     dword [edi + RM_EDX], m_inpm
        mov     word [edi + RM_SS], 0
        mov     word [edi + RM_SP], 0

        mov     eax, 0x0300
        mov     ebx, 0x0021
        xor     ecx, ecx
        int     0x31

        ; そのまま終了すればホストが後始末をしてリアルモードへ戻す
        mov     eax, 0x4C00
        int     0x21

        bits    16

; --- リアルモード呼び出し構造体のオフセット ---
RM_EDI  equ 0x00
RM_ESI  equ 0x04
RM_EBP  equ 0x08
RM_EBX  equ 0x10
RM_EDX  equ 0x14
RM_ECX  equ 0x18
RM_EAX  equ 0x1C
RM_FLG  equ 0x20
RM_ES   equ 0x22
RM_DS   equ 0x24
RM_FS   equ 0x26
RM_GS   equ 0x28
RM_IP   equ 0x2A
RM_CS   equ 0x2C
RM_SP   equ 0x2E
RM_SS   equ 0x30

putc:   push ax
        push bx
        push cx
        push dx
        mov [cbuf], al
        mov dx, cbuf
        mov cx, 1
        mov bx, 1
        mov ah, 0x40
        int 0x21
        pop dx
        pop cx
        pop bx
        pop ax
        ret
puts:   push ax
        push si
.l:     lodsb
        test al, al
        jz .d
        call putc
        jmp .l
.d:     pop si
        pop ax
        ret

m_head:   db 13,10,'=== minimal 32-bit DPMI client ===',13,10,13,10,0
m_nohost: db '  no DPMI host',13,10,0
m_no32:   db '  host does not support 32-bit clients',13,10,0
m_found:  db '  host found',13,10,0
m_switch: db '  switching to protected mode...',13,10,0
m_failed: db '  MODE SWITCH FAILED (still in real mode)',13,10,0
m_nomem:  db '  cannot allocate host data area',13,10,0
m_inpm:   db '  RUNNING IN PROTECTED MODE, called DOS back through DPMI',13,10,'$'

real_seg:   dw 0
host_paras: dw 0
host_seg:   dw 0
entry:      dw 0, 0
cbuf:       db 0
rmcs:       times 50 db 0
pm_msg:     db 13,10,'  [PM] char output through INT 31h AX=0300h works',13,10,0
            times 1024 db 0
stack_top:
