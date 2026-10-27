; Ghoul2 skinning idioms from Jedi Academy (default.xbe 0x6f0a0..0x6f300)
bits 32
org 0x10000
    ; data at 0xf0000000: matrix row (16-aligned) at +0x00, vector at +0x40 (4-aligned),
    ; packed vertex word at +0x80, weight bytes at +0x84, consts at +0xc0
    mov  edx, 0xf0000000
    movss xmm1, [edx]            ; row: x,0,y,z   (movss zeroes 1..3)
    movhps xmm1, [edx+4]
    mov  edx, 0xf0000044
    movss xmm2, [edx]            ; vector at an address only 4-aligned
    movhps xmm2, [edx+4]
    mulps xmm1, xmm2
    movaps xmm0, xmm1
    shufps xmm0, xmm0, 0x32
    addps xmm1, xmm0
    shufps xmm0, xmm0, 0x32
    addps xmm1, xmm0             ; xmm1.x = dot3
    movaps xmm7, xmm1            ; keep the dot product's every lane
    ; bone index k=1: (w >> (5*1)) & 0x1f ; weight bits: (w >> (12+2*1)) & 0x300 | byte
    mov  esi, 0xf0000080
    mov  eax, [esi]
    mov  ecx, 5
    mov  edx, eax
    shr  edx, cl
    and  edx, 0x1f
    mov  ebx, edx                ; ebx = bone index
    mov  ecx, 14
    shr  eax, cl
    movzx ecx, byte [esi+4+1]
    and  eax, 0x300
    or   eax, ecx
    mov  ebp, eax                ; ebp = 10-bit weight
    cvtsi2ss xmm0, eax
    mulss xmm0, [0xf00000c0]        ; * 1/1023
    movaps xmm6, xmm0            ; weight
    movss xmm3, [0xf00000c4]        ; 1.0
    subss xmm3, xmm0             ; last weight
    movaps xmm5, xmm3
    ; int16 vertex coordinate -> float * scale
    movsx edi, word [0xf0000088]
    cvtsi2ss xmm4, edi
    mulss xmm4, [0xf00000c8]
    movd eax, xmm1
    hlt
