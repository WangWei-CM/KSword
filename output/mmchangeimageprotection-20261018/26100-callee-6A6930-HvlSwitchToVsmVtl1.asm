HvlSwitchToVsmVtl1 at RVA 0x6A6930
extent source: next public symbol (first .pdata end 0x6A6A6B); RVA 0x6A6930..0x6A6A80 (exclusive)
shown: 69 instruction(s), max 200
All addresses are RVAs.

006A6930  48 81 ec 38 01 00 00                            sub       rsp, 0x138
006A6937  48 8d 84 24 00 01 00 00                         lea       rax, [rsp + 0x100]
006A693F  0f 29 74 24 30                                  movaps    xmmword ptr [rsp + 0x30], xmm6
006A6944  0f 29 7c 24 40                                  movaps    xmmword ptr [rsp + 0x40], xmm7
006A6949  44 0f 29 44 24 50                               movaps    xmmword ptr [rsp + 0x50], xmm8
006A694F  44 0f 29 4c 24 60                               movaps    xmmword ptr [rsp + 0x60], xmm9
006A6955  44 0f 29 54 24 70                               movaps    xmmword ptr [rsp + 0x70], xmm10
006A695B  44 0f 29 58 80                                  movaps    xmmword ptr [rax - 0x80], xmm11
006A6960  44 0f 29 60 90                                  movaps    xmmword ptr [rax - 0x70], xmm12
006A6965  44 0f 29 68 a0                                  movaps    xmmword ptr [rax - 0x60], xmm13
006A696A  44 0f 29 70 b0                                  movaps    xmmword ptr [rax - 0x50], xmm14
006A696F  44 0f 29 78 c0                                  movaps    xmmword ptr [rax - 0x40], xmm15
006A6974  48 89 68 f8                                     mov       qword ptr [rax - 8], rbp
006A6978  48 89 18                                        mov       qword ptr [rax], rbx
006A697B  48 89 78 08                                     mov       qword ptr [rax + 8], rdi
006A697F  48 89 70 10                                     mov       qword ptr [rax + 0x10], rsi
006A6983  4c 89 60 18                                     mov       qword ptr [rax + 0x18], r12
006A6987  4c 89 68 20                                     mov       qword ptr [rax + 0x20], r13
006A698B  4c 89 70 28                                     mov       qword ptr [rax + 0x28], r14
006A698F  4c 89 78 30                                     mov       qword ptr [rax + 0x30], r15
006A6993  48 89 54 24 08                                  mov       qword ptr [rsp + 8], rdx
006A6998  48 8b 1a                                        mov       rbx, qword ptr [rdx]
006A699B  f3 44 0f 6f 52 08                               movdqu    xmm10, xmmword ptr [rdx + 8]
006A69A1  f3 44 0f 6f 5a 18                               movdqu    xmm11, xmmword ptr [rdx + 0x18]
006A69A7  f3 44 0f 6f 62 28                               movdqu    xmm12, xmmword ptr [rdx + 0x28]
006A69AD  f3 44 0f 6f 6a 38                               movdqu    xmm13, xmmword ptr [rdx + 0x38]
006A69B3  f3 44 0f 6f 72 48                               movdqu    xmm14, xmmword ptr [rdx + 0x48]
006A69B9  f3 44 0f 6f 7a 58                               movdqu    xmm15, xmmword ptr [rdx + 0x58]
006A69BF  4d 8b c8                                        mov       r9, r8
006A69C2  45 0f 20 c0                                     mov       r8, cr8
006A69C6  65 48 8b 3c 25 88 01 00 00                      mov       rdi, qword ptr gs:[0x188]
006A69CF  48 8b 05 8a ae b5 00                            mov       rax, qword ptr [rip + 0xb5ae8a]
006A69D6  e8 a5 00 00 00                                  call      0x6a6a80 ; HvlpSwitchToVsmVtl1RetpolineHelper [RVA 0x6A6A80]
006A69DB  48 8b 54 24 08                                  mov       rdx, qword ptr [rsp + 8]
006A69E0  48 89 1a                                        mov       qword ptr [rdx], rbx
006A69E3  f3 44 0f 7f 52 08                               movdqu    xmmword ptr [rdx + 8], xmm10
006A69E9  f3 44 0f 7f 5a 18                               movdqu    xmmword ptr [rdx + 0x18], xmm11
006A69EF  f3 44 0f 7f 62 28                               movdqu    xmmword ptr [rdx + 0x28], xmm12
006A69F5  f3 44 0f 7f 6a 38                               movdqu    xmmword ptr [rdx + 0x38], xmm13
006A69FB  f3 44 0f 7f 72 48                               movdqu    xmmword ptr [rdx + 0x48], xmm14
006A6A01  f3 44 0f 7f 7a 58                               movdqu    xmmword ptr [rdx + 0x58], xmm15
006A6A07  48 8d 8c 24 00 01 00 00                         lea       rcx, [rsp + 0x100]
006A6A0F  0f 28 74 24 30                                  movaps    xmm6, xmmword ptr [rsp + 0x30]
006A6A14  0f 28 7c 24 40                                  movaps    xmm7, xmmword ptr [rsp + 0x40]
006A6A19  44 0f 28 44 24 50                               movaps    xmm8, xmmword ptr [rsp + 0x50]
006A6A1F  44 0f 28 4c 24 60                               movaps    xmm9, xmmword ptr [rsp + 0x60]
006A6A25  44 0f 28 54 24 70                               movaps    xmm10, xmmword ptr [rsp + 0x70]
006A6A2B  44 0f 28 59 80                                  movaps    xmm11, xmmword ptr [rcx - 0x80]
006A6A30  44 0f 28 61 90                                  movaps    xmm12, xmmword ptr [rcx - 0x70]
006A6A35  44 0f 28 69 a0                                  movaps    xmm13, xmmword ptr [rcx - 0x60]
006A6A3A  44 0f 28 71 b0                                  movaps    xmm14, xmmword ptr [rcx - 0x50]
006A6A3F  44 0f 28 79 c0                                  movaps    xmm15, xmmword ptr [rcx - 0x40]
006A6A44  48 8b 19                                        mov       rbx, qword ptr [rcx]
006A6A47  48 8b 79 08                                     mov       rdi, qword ptr [rcx + 8]
006A6A4B  48 8b 71 10                                     mov       rsi, qword ptr [rcx + 0x10]
006A6A4F  4c 8b 61 18                                     mov       r12, qword ptr [rcx + 0x18]
006A6A53  4c 8b 69 20                                     mov       r13, qword ptr [rcx + 0x20]
006A6A57  4c 8b 71 28                                     mov       r14, qword ptr [rcx + 0x28]
006A6A5B  4c 8b 79 30                                     mov       r15, qword ptr [rcx + 0x30]
006A6A5F  48 8b 69 f8                                     mov       rbp, qword ptr [rcx - 8]
006A6A63  48 81 c4 38 01 00 00                            add       rsp, 0x138
006A6A6A  c3                                              ret
006A6A6B  cc                                              int3
006A6A6C  cc                                              int3
006A6A6D  cc                                              int3
006A6A6E  cc                                              int3
006A6A6F  cc                                              int3
006A6A70  cc                                              int3
006A6A71  66 66 66 66 66 66 66 0f 1f 84 00 00 00 00 00    nop       word ptr [rax + rax]
