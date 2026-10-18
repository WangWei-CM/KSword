HvcallInitiateHypercall at RVA 0x22E040
extent source: next public symbol (first .pdata end 0x22E064); RVA 0x22E040..0x22E120 (exclusive)
shown: 70 instruction(s), max 200
All addresses are RVAs.

0022E040  40 53                                           push      rbx
0022E042  41 54                                           push      r12
0022E044  41 56                                           push      r14
0022E046  41 57                                           push      r15
0022E048  48 83 ec 68                                     sub       rsp, 0x68
0022E04C  48 8b 05 ed c6 bd 00                            mov       rax, qword ptr [rip + 0xbdc6ed]
0022E053  48 33 c4                                        xor       rax, rsp
0022E056  48 89 44 24 40                                  mov       qword ptr [rsp + 0x40], rax
0022E05B  8b 05 f3 7a d9 00                               mov       eax, dword ptr [rip + 0xd97af3]
0022E061  0f 57 c0                                        xorps     xmm0, xmm0
0022E064  48 89 6c 24 60                                  mov       qword ptr [rsp + 0x60], rbp
0022E069  49 8b e8                                        mov       rbp, r8
0022E06C  48 89 74 24 58                                  mov       qword ptr [rsp + 0x58], rsi
0022E071  48 8b f2                                        mov       rsi, rdx
0022E074  48 89 7c 24 50                                  mov       qword ptr [rsp + 0x50], rdi
0022E079  48 8b d9                                        mov       rbx, rcx
0022E07C  0f 11 44 24 20                                  movups    xmmword ptr [rsp + 0x20], xmm0
0022E081  0f 11 44 24 30                                  movups    xmmword ptr [rsp + 0x30], xmm0
0022E086  a8 10                                           test      al, 0x10
0022E088  75 52                                           jne       0x22e0dc ; loc_22E0DC [RVA 0x22E0DC]
0022E08A  40 32 ff                                        xor       dil, dil
0022E08D  48 8b 05 c4 37 fd 00                            mov       rax, qword ptr [rip + 0xfd37c4]
0022E094  4c 8b c5                                        mov       r8, rbp
0022E097  48 8b d6                                        mov       rdx, rsi
0022E09A  48 8b cb                                        mov       rcx, rbx
0022E09D  4c 8b f3                                        mov       r14, rbx
0022E0A0  4c 8b fb                                        mov       r15, rbx
0022E0A3  4c 8b e3                                        mov       r12, rbx
0022E0A6  ff d0                                           call      rax
0022E0A8  0f 1f 00                                        nop       dword ptr [rax]
0022E0AB  48 8b 74 24 58                                  mov       rsi, qword ptr [rsp + 0x58]
0022E0B0  40 84 ff                                        test      dil, dil
0022E0B3  48 8b 7c 24 50                                  mov       rdi, qword ptr [rsp + 0x50]
0022E0B8  48 8b d8                                        mov       rbx, rax
0022E0BB  48 8b 6c 24 60                                  mov       rbp, qword ptr [rsp + 0x60]
0022E0C0  75 2e                                           jne       0x22e0f0 ; loc_22E0F0 [RVA 0x22E0F0]
0022E0C2  48 8b 4c 24 40                                  mov       rcx, qword ptr [rsp + 0x40]
0022E0C7  48 33 cc                                        xor       rcx, rsp
0022E0CA  e8 c1 7f 47 00                                  call      0x6a6090 ; _security_check_cookie [RVA 0x6A6090]
0022E0CF  48 83 c4 68                                     add       rsp, 0x68
0022E0D3  41 5f                                           pop       r15
0022E0D5  41 5e                                           pop       r14
0022E0D7  41 5c                                           pop       r12
0022E0D9  5b                                              pop       rbx
0022E0DA  c3                                              ret
0022E0DB  cc                                              int3
0022E0DC  ba 10 00 00 a0                                  mov       edx, 0xa0000010
0022E0E1  48 8d 4c 24 20                                  lea       rcx, [rsp + 0x20]
0022E0E6  40 b7 01                                        mov       dil, 1
0022E0E9  e8 92 6c 0f 00                                  call      0x324d80 ; EtwGetKernelTraceTimestamp [RVA 0x324D80]
0022E0EE  eb 9d                                           jmp       0x22e08d ; loc_22E08D [RVA 0x22E08D]
0022E0F0  41 c1 ee 10                                     shr       r14d, 0x10
0022E0F4  48 8d 4c 24 20                                  lea       rcx, [rsp + 0x20]
0022E0F9  41 80 e6 01                                     and       r14b, 1
0022E0FD  41 c1 ef 1f                                     shr       r15d, 0x1f
0022E101  45 0f b6 c6                                     movzx     r8d, r14b
0022E105  41 0f b7 d4                                     movzx     edx, r12w
0022E109  45 0f b6 cf                                     movzx     r9d, r15b
0022E10D  e8 76 93 0f 00                                  call      0x327488 ; EtwTraceHvHypercall [RVA 0x327488]
0022E112  48 8b c3                                        mov       rax, rbx
0022E115  eb ab                                           jmp       0x22e0c2 ; loc_22E0C2 [RVA 0x22E0C2]
0022E117  cc                                              int3
0022E118  cc                                              int3
0022E119  cc                                              int3
0022E11A  cc                                              int3
0022E11B  cc                                              int3
0022E11C  cc                                              int3
0022E11D  cc                                              int3
0022E11E  cc                                              int3
0022E11F  cc                                              int3
