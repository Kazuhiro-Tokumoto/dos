; ============================================================================
; dpmichk.asm  -  DPMI ホストが入っているかを見る
;
; INT 2Fh AX=1687h は DPMI の導入確認。ホストが居れば AX=0 で返り、
; ES:DI にモード切替の入口、BX に「32bit クライアントを扱えるか」、
; SI に「ホストに渡してやる作業領域のパラグラフ数」が入る。
;
; 自動テストには入れていない。イメージに DPMI ホストを同梱していないので、
; 素の MYDOS では常に「居ない」が正しい答えになるため。フリーの
; CWSDPMI を C: に置いて手で試すときの道具。
; ============================================================================
        cpu     386
        bits    16
        org     0x100
start:
        mov     si, m_head
        call    puts

        ; --- INT 2Fh AX=1687h : DPMI の導入確認 ---
        mov     ax, 0x1687
        xor     bx, bx
        xor     cx, cx
        xor     dx, dx
        xor     si, si
        push    ds
        pop     es
        xor     di, di
        int     0x2F
        mov     [r_ax], ax
        mov     [r_bx], bx
        mov     [r_cx], cx
        mov     [r_dx], dx
        mov     [r_si], si
        mov     [r_di], di
        mov     [r_es], es

        push    cs
        pop     ds
        mov     si, m_ax
        call    puts
        mov     ax, [r_ax]
        call    hex16
        call    nl

        cmp     word [r_ax], 0
        je      .present
        mov     si, m_absent
        call    puts
        jmp     .done
.present:
        mov     si, m_present
        call    puts

        mov     si, m_ver
        call    puts
        mov     ax, [r_dx]
        call    hex16
        call    nl

        mov     si, m_flags
        call    puts
        mov     ax, [r_bx]
        call    hex16
        call    nl

        mov     si, m_cpu
        call    puts
        mov     ax, [r_cx]
        call    hex16
        call    nl

        mov     si, m_paras
        call    puts
        mov     ax, [r_si]
        call    hex16
        call    nl

        mov     si, m_entry
        call    puts
        mov     ax, [r_es]
        call    hex16
        mov     al, ':'
        call    putc
        mov     ax, [r_di]
        call    hex16
        call    nl
.done:
        mov     ax, 0x4C00
        int     0x21

putc:   push ax
        push bx
        push cx
        push dx
        push ds
        push cs
        pop ds
        mov [cbuf], al
        mov dx, cbuf
        mov cx, 1
        mov bx, 1
        mov ah, 0x40
        int 0x21
        pop ds
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
nl:     push ax
        mov al, 13
        call putc
        mov al, 10
        call putc
        pop ax
        ret
hex16:  push ax
        push cx
        mov cx, 4
.l:     rol ax, 4
        push ax
        and al, 0x0F
        cmp al, 10
        jb .d
        add al, 'A'-10-'0'
.d:     add al, '0'
        call putc
        pop ax
        loop .l
        pop cx
        pop ax
        ret

m_head:    db 13,10,'=== DPMI host check (INT 2Fh AX=1687h) ===',13,10,13,10,0
m_ax:      db '  AX (0 = present) = ',0
m_absent:  db '  no DPMI host installed',13,10,0
m_present: db '  DPMI HOST PRESENT',13,10,0
m_ver:     db '  DX version       = ',0
m_flags:   db '  BX flags         = ',0
m_cpu:     db '  CX processor     = ',0
m_paras:   db '  SI host paras    = ',0
m_entry:   db '  ES:DI entry      = ',0
r_ax: dw 0
r_bx: dw 0
r_cx: dw 0
r_dx: dw 0
r_si: dw 0
r_di: dw 0
r_es: dw 0
cbuf: db 0
