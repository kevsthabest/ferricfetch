; CPU identification helpers for FFETCH.
;
; Interface contract: every routine here is called from C with a
; __cdecl declaration, so arguments arrive on the stack (never in
; registers - Watcom's default convention would put the first ones
; in AX/DX/BX/CX and name the symbols with a trailing underscore)
; and each routine returns with retf under the medium model.
;
; Why this lives in assembly rather than C: the family ladder below
; must run on an 8088, so it may only use instructions an 8088 has.
; The compiler cannot be told "emit 8086 code for this function but
; 386 code for that one" inside a single module, so the two routines
; that touch 386-only state (EFLAGS, CPUID) are isolated here and are
; only *called* after the ladder has proven the CPU can run them.
; Keeping them unreachable on old parts is what makes FFETCH safe to
; start on any machine DOS itself can boot on.

segment _TEXT class=CODE

global _cpu_family_ladder
global _cpu_cpuid
global _cpu_has_cpuid
global _xms_query_free_kb
global _ferric_api
global _pci_bios_present
global _pci_bios_read

; unsigned short cpu_family_ladder(void)
; Returns 0 = 8088/8086/V20 class, 2 = 80286, 3 = 80386, 4 = 80486 or
; better (CPUID may still be absent on early 486 parts; that is a
; separate question answered by cpu_cpuid's caller via the ID bit).
; The ladder works from how each generation treats the high FLAGS
; nibble and the EFLAGS AC bit, the standard Intel identification
; sequence, and every instruction used is legal on an 8088.
_cpu_family_ladder:
    pushf                       ; caller's FLAGS, restored at each exit
    ; Bits 12-15 of FLAGS are always 1 on an 8088/8086, always 0 on a
    ; 286 in real mode, and writable on a 386+.
    pushf
    pop ax
    and ax, 0x0FFF              ; try to clear bits 12-15
    push ax
    popf
    pushf
    pop ax
    and ax, 0xF000
    cmp ax, 0xF000
    je .is_8088
    ; 286 or better: try to set bits 12-15.
    or ax, 0xF000
    push ax
    popf
    pushf
    pop ax
    and ax, 0xF000
    jz .is_286
    ; 386 or better: the AC bit (EFLAGS bit 18) toggles only on a 486+;
    ; a 386 keeps it forced to 0. pushfd/popfd are 386 instructions,
    ; which is exactly why this point is only reached once the FLAGS
    ; tests above have already proven a 386 is present.
    pushfd
    pop eax
    mov edx, eax
    xor eax, 0x00040000         ; flip AC
    push eax
    popfd
    pushfd
    pop eax
    xor eax, edx
    push edx                    ; restore the caller's EFLAGS
    popfd
    test eax, 0x00040000
    jz .is_386
    mov ax, 4
    popf
    retf
.is_386:
    mov ax, 3
    popf
    retf
.is_286:
    mov ax, 2
    popf
    retf
.is_8088:
    xor ax, ax
    popf
    retf

; void cpu_cpuid(unsigned long leaf, unsigned long far *regs)
; Executes CPUID for `leaf` and stores EAX, EBX, ECX, EDX (in that
; order) at regs. Only called after the ladder returned 4 and the
; caller has confirmed the EFLAGS ID bit (bit 21) can be toggled,
; which is the architectural test for CPUID existing at all.
; Medium-model far call: arguments sit at [bp+6] (leaf, 4 bytes) and
; [bp+10] (far pointer: offset then segment).
_cpu_cpuid:
    push bp
    mov bp, sp
    push ebx
    push ecx
    push edx
    push esi
    push edi
    push es
    mov eax, [bp+6]
    cpuid
    les di, [bp+10]
    mov [es:di], eax
    mov [es:di+4], ebx
    mov [es:di+8], ecx
    mov [es:di+12], edx
    pop es
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop bp
    retf

; unsigned short cpu_has_cpuid(void)
; Toggles the EFLAGS ID bit (21). It only moves on CPUs that have
; CPUID, which is the architectural existence test. 386 instructions
; throughout, so the caller must have ladder-proven a 486-class part
; before calling, exactly as for cpu_cpuid.
_cpu_has_cpuid:
    pushfd
    pop eax
    mov edx, eax
    xor eax, 0x00200000
    push eax
    popfd
    pushfd
    pop eax
    push edx                    ; restore the caller's EFLAGS
    popfd
    xor eax, edx
    shr eax, 21
    and ax, 1
    retf

; void xms_query_free_kb(unsigned int entry_off, unsigned int entry_seg,
;                        unsigned int *total_kb, unsigned int *largest_kb)
; Calls the XMS driver's entry point (handed out by INT 2Fh AX=4310h)
; with AH=08h, the query-free-memory function. The XMS ABI is a raw
; far call with register arguments, which no C calling convention can
; express, so the call is assembled here around a far pointer built
; on the stack from the two halves the caller passes in.
; Arguments: [bp+6]=off, [bp+8]=seg, [bp+10]=total (near ptr),
; [bp+12]=largest (near ptr). XMS returns total in DX, largest in AX.
_xms_query_free_kb:
    push bp
    mov bp, sp
    sub sp, 4
    mov ax, [bp+6]
    mov [bp-4], ax
    mov ax, [bp+8]
    mov [bp-2], ax
    mov ah, 0x08
    call far [bp-4]
    mov bx, [bp+10]
    mov [bx], dx
    mov bx, [bp+12]
    mov [bx], ax
    mov sp, bp
    pop bp
    retf

; void ferric_api(unsigned int entry_off, unsigned int entry_seg,
;                 unsigned int ax_in, unsigned int es_in,
;                 unsigned int di_in, unsigned int cx_in,
;                 unsigned int *out_ax, unsigned int *out_bx,
;                 unsigned int *out_cf)
; Far-calls the Ferric core's V86 API entry point (the address INT 2Fh
; AX=1684h hands out for device FD01h) with the caller's AX/CX/ES/DI
; loaded, and reports AX, BX and the carry flag on return. The core
; speaks in raw registers, so - like the XMS call above - the call
; itself has to be assembled; everything else about the protocol is
; plain C in ferric.c. All registers the API is allowed to clobber
; are saved and restored, because the C compiler assumes its calling
; convention survives any call it did not emit itself.
; Arguments: [bp+6]=entry_off, [bp+8]=entry_seg, [bp+10]=ax_in,
; [bp+12]=es_in, [bp+14]=di_in, [bp+16]=cx_in, [bp+18]=out_ax,
; [bp+20]=out_bx, [bp+22]=out_cf (near pointers).
_ferric_api:
    push bp
    mov bp, sp
    sub sp, 10
    mov ax, [bp+6]
    mov [bp-4], ax              ; far call target, offset then segment
    mov ax, [bp+8]
    mov [bp-2], ax
    push bx
    push cx
    push dx
    push si
    push di
    push es
    push ds
    mov ax, [bp+10]
    mov cx, [bp+16]
    mov di, [bp+14]
    mov es, [bp+12]
    call far [bp-4]
    mov [bp-6], ax              ; stash results before restoring regs
    mov [bp-8], bx
    mov word [bp-10], 0
    jnc .saved
    mov word [bp-10], 1
.saved:
    pop ds
    pop es
    pop di
    pop si
    pop dx
    pop cx
    ; BX is written through last: it is the one register the writes
    ; below need as an address, and the caller's copy is still on the
    ; stack under them, restored by the final pop.
    mov bx, [bp+18]
    mov ax, [bp-6]
    mov [bx], ax
    mov bx, [bp+20]
    mov ax, [bp-8]
    mov [bx], ax
    mov bx, [bp+22]
    mov ax, [bp-10]
    mov [bx], ax
    pop bx
    mov sp, bp
    pop bp
    retf

; unsigned long pci_bios_present(unsigned int *ok)
; PCI BIOS installation check, INT 1Ah AX=B101h. Returns the EDX
; signature value; *ok reports carry clear and AH=0. The 16-bit C
; interrupt wrappers cannot move 32-bit registers, and the PCI BIOS
; speaks in them (EDX carries the ' PCI' signature), so the call is
; made here. 386 instructions appear below (push/pop of EDX halves),
; which is safe by construction: the caller gates on the CPU ladder,
; and a machine with a PCI BIOS is never older than a 386 anyway.
; Argument: [bp+6] = ok (near pointer). Return: EDX in DX:AX.
_pci_bios_present:
    push bp
    mov bp, sp
    push bx
    push si
    push di
    mov ax, 0xB101
    int 0x1A
    mov si, [bp+6]
    mov word [si], 0
    jc .done
    test ah, ah
    jnz .done
    mov word [si], 1
.done:
    push edx                    ; split EDX into the DX:AX return
    pop ax
    pop dx
    pop di
    pop si
    pop bx
    pop bp
    retf

; unsigned long pci_bios_read(unsigned int bus_devfn, unsigned int off,
;                             unsigned int *ok)
; PCI BIOS read-config-dword, INT 1Ah AX=B10Ah: BH=bus, BL=device/
; function, DI=offset. Returns ECX; *ok reports carry clear, AH=0.
; Arguments: [bp+6]=bus_devfn (bus in the high byte), [bp+8]=off,
; [bp+10]=ok (near pointer). Return: ECX in DX:AX.
_pci_bios_read:
    push bp
    mov bp, sp
    push bx
    push si
    push di
    mov bx, [bp+6]
    mov di, [bp+8]
    mov ax, 0xB10A
    int 0x1A
    mov si, [bp+10]
    mov word [si], 0
    jc .done
    test ah, ah
    jnz .done
    mov word [si], 1
.done:
    mov ax, cx                  ; split ECX into the DX:AX return
    shr ecx, 16
    mov dx, cx
    pop di
    pop si
    pop bx
    pop bp
    retf

global _crit24_handler

; crit24_handler: a borrowed INT 24h (critical error) handler that
; always answers "Fail" (AL=3). It exists for the disk probe in
; modules.c: asking DOS for the free space of a letter whose driver
; is loaded but whose media is not mounted - an empty CD-ROM drive,
; an unmounted redirector volume - raises a critical error, and the
; default handler stops the machine on an Abort/Retry/Fail prompt.
; That breaks FFETCH's contract that an unavailable source simply
; shortens the summary. While this handler is installed the same
; probe fails quietly and the letter is skipped. The handler is in
; place only for the duration of the probe; the caller's original
; vector is restored immediately afterwards, so nothing else in the
; session ever sees it.
_crit24_handler:
    mov al, 3                   ; 3 = fail the DOS call in progress
    iret
