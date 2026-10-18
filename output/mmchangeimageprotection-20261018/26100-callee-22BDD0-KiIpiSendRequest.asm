KiIpiSendRequest at RVA 0x22BDD0
extent source: next public symbol (first .pdata end 0x22C46D); RVA 0x22BDD0..0x22C480 (exclusive)
shown: 200 instruction(s), max 200
All addresses are RVAs.

0022BDD0  48 89 5c 24 18                                  mov       qword ptr [rsp + 0x18], rbx
0022BDD5  55                                              push      rbp
0022BDD6  56                                              push      rsi
0022BDD7  57                                              push      rdi
0022BDD8  41 54                                           push      r12
0022BDDA  41 55                                           push      r13
0022BDDC  41 56                                           push      r14
0022BDDE  41 57                                           push      r15
0022BDE0  48 8d 6c 24 e1                                  lea       rbp, [rsp - 0x1f]
0022BDE5  48 81 ec e0 00 00 00                            sub       rsp, 0xe0
0022BDEC  48 8b 05 4d e9 bd 00                            mov       rax, qword ptr [rip + 0xbde94d]
0022BDF3  48 33 c4                                        xor       rax, rsp
0022BDF6  48 89 45 0f                                     mov       qword ptr [rbp + 0xf], rax
0022BDFA  33 c0                                           xor       eax, eax
0022BDFC  4c 89 4d bf                                     mov       qword ptr [rbp - 0x41], r9
0022BE00  33 ff                                           xor       edi, edi
0022BE02  89 54 24 34                                     mov       dword ptr [rsp + 0x34], edx
0022BE06  f7 05 38 9d d9 00 00 00 40 00                   test      dword ptr [rip + 0xd99d38], 0x400000
0022BE10  0f 57 c9                                        xorps     xmm1, xmm1
0022BE13  0f 57 c0                                        xorps     xmm0, xmm0
0022BE16  48 89 45 b7                                     mov       qword ptr [rbp - 0x49], rax
0022BE1A  49 8b f0                                        mov       rsi, r8
0022BE1D  48 89 7d c7                                     mov       qword ptr [rbp - 0x39], rdi
0022BE21  44 8b e2                                        mov       r12d, edx
0022BE24  4c 8b f9                                        mov       r15, rcx
0022BE27  0f 11 45 a7                                     movups    xmmword ptr [rbp - 0x59], xmm0
0022BE2B  0f 11 4d df                                     movups    xmmword ptr [rbp - 0x21], xmm1
0022BE2F  0f 11 4d ef                                     movups    xmmword ptr [rbp - 0x11], xmm1
0022BE33  0f 85 9f 05 00 00                               jne       0x22c3d8 ; loc_22C3D8 [RVA 0x22C3D8]
0022BE39  88 44 24 30                                     mov       byte ptr [rsp + 0x30], al
0022BE3D  49 8d 9f c8 2f 00 00                            lea       rbx, [r15 + 0x2fc8]
0022BE44  33 d2                                           xor       edx, edx
0022BE46  4c 8d 73 08                                     lea       r14, [rbx + 8]
0022BE4A  48 c7 03 01 00 20 00                            mov       qword ptr [rbx], 0x200001
0022BE51  49 8b ce                                        mov       rcx, r14
0022BE54  4d 8d af d0 30 00 00                            lea       r13, [r15 + 0x30d0]
0022BE5B  41 b8 00 01 00 00                               mov       r8d, 0x100
0022BE61  e8 da 48 49 00                                  call      0x6c0740 ; memset [RVA 0x6C0740]
0022BE66  49 8d 4d 08                                     lea       rcx, [r13 + 8]
0022BE6A  49 c7 45 00 01 00 20 00                         mov       qword ptr [r13], 0x200001
0022BE72  33 d2                                           xor       edx, edx
0022BE74  41 b8 00 01 00 00                               mov       r8d, 0x100
0022BE7A  e8 c1 48 49 00                                  call      0x6c0740 ; memset [RVA 0x6C0740]
0022BE7F  0f b7 4b 02                                     movzx     ecx, word ptr [rbx + 2]
0022BE83  0f b7 d1                                        movzx     edx, cx
0022BE86  45 85 e4                                        test      r12d, r12d
0022BE89  0f 85 ed 02 00 00                               jne       0x22c17c ; loc_22C17C [RVA 0x22C17C]
0022BE8F  0f b7 06                                        movzx     eax, word ptr [rsi]
0022BE92  66 3b c1                                        cmp       ax, cx
0022BE95  66 89 4b 02                                     mov       word ptr [rbx + 2], cx
0022BE99  89 7b 04                                        mov       dword ptr [rbx + 4], edi
0022BE9C  0f b7 cf                                        movzx     ecx, di
0022BE9F  66 0f 46 d0                                     cmovbe    dx, ax
0022BEA3  66 89 13                                        mov       word ptr [rbx], dx
0022BEA6  66 3b fa                                        cmp       di, dx
0022BEA9  0f 82 4a 04 00 00                               jb        0x22c2f9 ; loc_22C2F9 [RVA 0x22C2F9]
0022BEAF  66 3b 4b 02                                     cmp       cx, word ptr [rbx + 2]
0022BEB3  73 11                                           jae       0x22bec6 ; loc_22BEC6 [RVA 0x22BEC6]
0022BEB5  0f b7 c1                                        movzx     eax, cx
0022BEB8  66 ff c1                                        inc       cx
0022BEBB  48 89 7c c3 08                                  mov       qword ptr [rbx + rax*8 + 8], rdi
0022BEC0  66 3b 4b 02                                     cmp       cx, word ptr [rbx + 2]
0022BEC4  72 ef                                           jb        0x22beb5 ; loc_22BEB5 [RVA 0x22BEB5]
0022BEC6  4c 8b 65 7f                                     mov       r12, qword ptr [rbp + 0x7f]
0022BECA  48 8b 4d bf                                     mov       rcx, qword ptr [rbp - 0x41]
0022BECE  49 8b c4                                        mov       rax, r12
0022BED1  83 e0 0f                                        and       eax, 0xf
0022BED4  48 c1 e1 07                                     shl       rcx, 7
0022BED8  48 0b c8                                        or        rcx, rax
0022BEDB  41 c7 87 80 2d 00 00 01 00 00 00                mov       dword ptr [r15 + 0x2d80], 1
0022BEE6  41 8b 47 24                                     mov       eax, dword ptr [r15 + 0x24]
0022BEEA  8b d7                                           mov       edx, edi
0022BEEC  89 45 83                                        mov       dword ptr [rbp - 0x7d], eax
0022BEEF  0f 57 c0                                        xorps     xmm0, xmm0
0022BEF2  0f b7 05 97 4e cf 00                            movzx     eax, word ptr [rip + 0xcf4e97]
0022BEF9  44 8b c7                                        mov       r8d, edi
0022BEFC  41 89 87 84 2d 00 00                            mov       dword ptr [r15 + 0x2d84], eax
0022BF03  44 8b cf                                        mov       r9d, edi
0022BF06  89 54 24 3c                                     mov       dword ptr [rsp + 0x3c], edx
0022BF0A  44 8b df                                        mov       r11d, edi
0022BF0D  89 7c 24 40                                     mov       dword ptr [rsp + 0x40], edi
0022BF11  89 7c 24 38                                     mov       dword ptr [rsp + 0x38], edi
0022BF15  0f 11 45 cf                                     movups    xmmword ptr [rbp - 0x31], xmm0
0022BF19  85 c0                                           test      eax, eax
0022BF1B  0f 84 f8 01 00 00                               je        0x22c119 ; loc_22C119 [RVA 0x22C119]
0022BF21  4c 8d 15 58 bd d9 00                            lea       r10, [rip + 0xd9bd58]
0022BF28  48 8b 05 59 4e cf 00                            mov       rax, qword ptr [rip + 0xcf4e59]
0022BF2F  41 8b d3                                        mov       edx, r11d
0022BF32  4c 8b 04 d0                                     mov       r8, qword ptr [rax + rdx*8]
0022BF36  4d 85 c0                                        test      r8, r8
0022BF39  0f 84 56 04 00 00                               je        0x22c395 ; loc_22C395 [RVA 0x22C395]
0022BF3F  49 8b 90 80 00 00 00                            mov       rdx, qword ptr [r8 + 0x80]
0022BF46  33 c0                                           xor       eax, eax
0022BF48  45 0f b7 80 88 00 00 00                         movzx     r8d, word ptr [r8 + 0x88]
0022BF50  0f 57 c0                                        xorps     xmm0, xmm0
0022BF53  0f 11 45 cf                                     movups    xmmword ptr [rbp - 0x31], xmm0
0022BF57  66 44 89 45 d7                                  mov       word ptr [rbp - 0x29], r8w
0022BF5C  0f 11 45 ff                                     movups    xmmword ptr [rbp - 1], xmm0
0022BF60  89 45 d9                                        mov       dword ptr [rbp - 0x27], eax
0022BF63  66 89 45 dd                                     mov       word ptr [rbp - 0x23], ax
0022BF67  66 44 39 03                                     cmp       word ptr [rbx], r8w
0022BF6B  0f 86 77 01 00 00                               jbe       0x22c0e8 ; loc_22C0E8 [RVA 0x22C0E8]
0022BF71  4a 23 54 c3 08                                  and       rdx, qword ptr [rbx + r8*8 + 8]
0022BF76  48 89 55 cf                                     mov       qword ptr [rbp - 0x31], rdx
0022BF7A  0f 84 6c 01 00 00                               je        0x22c0ec ; loc_22C0EC [RVA 0x22C0EC]
0022BF80  48 8b f7                                        mov       rsi, rdi
0022BF83  66 44 89 45 b7                                  mov       word ptr [rbp - 0x49], r8w
0022BF88  48 89 55 af                                     mov       qword ptr [rbp - 0x51], rdx
0022BF8C  45 8b f0                                        mov       r14d, r8d
0022BF8F  48 89 7d a7                                     mov       qword ptr [rbp - 0x59], rdi
0022BF93  0f 1f 40 00                                     nop       dword ptr [rax]
0022BF97  66 0f 1f 84 00 00 00 00 00                      nop       word ptr [rax + rax]
0022BFA0  89 7d 9f                                        mov       dword ptr [rbp - 0x61], edi
0022BFA3  45 8d 46 01                                     lea       r8d, [r14 + 1]
0022BFA7  48 85 d2                                        test      rdx, rdx
0022BFAA  0f 84 0e 01 00 00                               je        0x22c0be ; loc_22C0BE [RVA 0x22C0BE]
0022BFB0  4c 0f bc c2                                     bsf       r8, rdx
0022BFB4  41 0f b6 c0                                     movzx     eax, r8b
0022BFB8  48 0f b3 c2                                     btr       rdx, rax
0022BFBC  44 89 45 9f                                     mov       dword ptr [rbp - 0x61], r8d
0022BFC0  48 89 55 af                                     mov       qword ptr [rbp - 0x51], rdx
0022BFC4  41 8b d6                                        mov       edx, r14d
0022BFC7  c1 e2 06                                        shl       edx, 6
0022BFCA  41 0f b6 c0                                     movzx     eax, r8b
0022BFCE  03 d0                                           add       edx, eax
0022BFD0  48 8b 05 21 67 cf 00                            mov       rax, qword ptr [rip + 0xcf6721]
0022BFD7  8b 14 90                                        mov       edx, dword ptr [rax + rdx*4]
0022BFDA  44 8b ca                                        mov       r9d, edx
0022BFDD  4d 8b 14 d2                                     mov       r10, qword ptr [r10 + rdx*8]
0022BFE1  41 0f 0d 8a c0 9a 00 00                         prefetchw byte ptr [r10 + 0x9ac0]
0022BFE9  8b 55 83                                        mov       edx, dword ptr [rbp - 0x7d]
0022BFEC  48 81 c2 3b 03 00 00                            add       rdx, 0x33b
0022BFF3  48 c1 e2 06                                     shl       rdx, 6
0022BFF7  49 03 d2                                        add       rdx, r10
0022BFFA  0f 0d 0a                                        prefetchw byte ptr [rdx]
0022BFFD  48 85 f6                                        test      rsi, rsi
0022C000  75 0c                                           jne       0x22c00e ; loc_22C00E [RVA 0x22C00E]
0022C002  f3 48 0f b8 45 cf                               popcnt    rax, qword ptr [rbp - 0x31]
0022C008  48 8d 72 38                                     lea       rsi, [rdx + 0x38]
0022C00C  89 06                                           mov       dword ptr [rsi], eax
0022C00E  48 89 72 30                                     mov       qword ptr [rdx + 0x30], rsi
0022C012  49 83 fc 05                                     cmp       r12, 5
0022C016  75 1b                                           jne       0x22c033 ; loc_22C033 [RVA 0x22C033]
0022C018  48 8b 45 bf                                     mov       rax, qword ptr [rbp - 0x41]
0022C01C  0f 10 00                                        movups    xmm0, xmmword ptr [rax]
0022C01F  0f 11 42 10                                     movups    xmmword ptr [rdx + 0x10], xmm0
0022C023  0f 10 48 10                                     movups    xmm1, xmmword ptr [rax + 0x10]
0022C027  0f 11 4a 20                                     movups    xmmword ptr [rdx + 0x20], xmm1
0022C02B  48 8b 42 28                                     mov       rax, qword ptr [rdx + 0x28]
0022C02F  48 89 45 c7                                     mov       qword ptr [rbp - 0x39], rax
0022C033  48 89 4a 08                                     mov       qword ptr [rdx + 8], rcx
0022C037  49 8b 82 c0 9a 00 00                            mov       rax, qword ptr [r10 + 0x9ac0]
0022C03E  4c 8b c0                                        mov       r8, rax
0022C041  48 89 02                                        mov       qword ptr [rdx], rax
0022C044  f0 49 0f b1 92 c0 9a 00 00                      lock cmpxchg qword ptr [r10 + 0x9ac0], rdx
0022C04D  49 3b c0                                        cmp       rax, r8
0022C050  75 ec                                           jne       0x22c03e ; loc_22C03E [RVA 0x22C03E]
0022C052  48 85 c0                                        test      rax, rax
0022C055  0f 85 2d 03 00 00                               jne       0x22c388 ; loc_22C388 [RVA 0x22C388]
0022C05B  48 8b 05 8e 66 cf 00                            mov       rax, qword ptr [rip + 0xcf668e]
0022C062  c7 44 24 3c 01 00 00 00                         mov       dword ptr [rsp + 0x3c], 1
0022C06A  46 8b 0c 88                                     mov       r9d, dword ptr [rax + r9*4]
0022C06E  41 8b d1                                        mov       edx, r9d
0022C071  41 0f b7 45 00                                  movzx     eax, word ptr [r13]
0022C076  41 83 e1 3f                                     and       r9d, 0x3f
0022C07A  c1 ea 06                                        shr       edx, 6
0022C07D  3b c2                                           cmp       eax, edx
0022C07F  0f 86 6b 03 00 00                               jbe       0x22c3f0 ; loc_22C3F0 [RVA 0x22C3F0]
0022C085  4c 8d 04 d5 00 00 00 00                         lea       r8, [rdx*8]
0022C08D  41 8b c1                                        mov       eax, r9d
0022C090  4b 8b 54 28 08                                  mov       rdx, qword ptr [r8 + r13 + 8]
0022C095  48 0f ab c2                                     bts       rdx, rax
0022C099  4b 89 54 28 08                                  mov       qword ptr [r8 + r13 + 8], rdx
0022C09E  49 8b 82 b8 8e 00 00                            mov       rax, qword ptr [r10 + 0x8eb8]
0022C0A5  4c 8d 15 d4 bb d9 00                            lea       r10, [rip + 0xd9bbd4]
0022C0AC  48 85 c0                                        test      rax, rax
0022C0AF  0f 85 72 03 00 00                               jne       0x22c427 ; loc_22C427 [RVA 0x22C427]
0022C0B5  48 8b 55 af                                     mov       rdx, qword ptr [rbp - 0x51]
0022C0B9  e9 e2 fe ff ff                                  jmp       0x22bfa0 ; loc_22BFA0 [RVA 0x22BFA0]
0022C0BE  0f b7 45 b7                                     movzx     eax, word ptr [rbp - 0x49]
0022C0C2  66 ff c0                                        inc       ax
0022C0C5  44 0f b7 f0                                     movzx     r14d, ax
0022C0C9  66 44 89 75 b7                                  mov       word ptr [rbp - 0x49], r14w
0022C0CE  45 3b f0                                        cmp       r14d, r8d
0022C0D1  0f 83 f7 02 00 00                               jae       0x22c3ce ; loc_22C3CE [RVA 0x22C3CE]
0022C0D7  4a 8b 14 f5 08 00 00 00                         mov       rdx, qword ptr [r14*8 + 8]
0022C0DF  48 89 55 af                                     mov       qword ptr [rbp - 0x51], rdx
0022C0E3  e9 bf fe ff ff                                  jmp       0x22bfa7 ; loc_22BFA7 [RVA 0x22BFA7]
0022C0E8  48 89 7d cf                                     mov       qword ptr [rbp - 0x31], rdi
0022C0EC  41 ff c1                                        inc       r9d
0022C0EF  44 89 4c 24 38                                  mov       dword ptr [rsp + 0x38], r9d
0022C0F4  0f b7 05 95 4c cf 00                            movzx     eax, word ptr [rip + 0xcf4c95]
0022C0FB  41 ff c3                                        inc       r11d
0022C0FE  44 3b d8                                        cmp       r11d, eax
0022C101  0f 82 21 fe ff ff                               jb        0x22bf28 ; loc_22BF28 [RVA 0x22BF28]
0022C107  45 85 c9                                        test      r9d, r9d
0022C10A  0f 85 a1 01 00 00                               jne       0x22c2b1 ; loc_22C2B1 [RVA 0x22C2B1]
0022C110  44 8b 44 24 40                                  mov       r8d, dword ptr [rsp + 0x40]
0022C115  8b 54 24 3c                                     mov       edx, dword ptr [rsp + 0x3c]
0022C119  49 83 fc 05                                     cmp       r12, 5
