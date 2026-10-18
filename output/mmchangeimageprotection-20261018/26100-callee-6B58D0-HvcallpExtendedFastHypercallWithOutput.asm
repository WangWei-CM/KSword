HvcallpExtendedFastHypercallWithOutput at RVA 0x6B58D0
extent source: next public symbol (first .pdata end 0x6B59DD); RVA 0x6B58D0..0x6B59F0 (exclusive)
shown: 93 instruction(s), max 200
All addresses are RVAs.

006B58D0  48 83 ec 28                                     sub       rsp, 0x28
006B58D4  4c 89 44 24 40                                  mov       qword ptr [rsp + 0x40], r8
006B58D9  49 c1 e0 04                                     shl       r8, 4
006B58DD  e8 8e ff ff ff                                  call      0x6b5870 ; HvcallpExtendedFastHypercall [RVA 0x6B5870]
006B58E2  66 85 c0                                        test      ax, ax
006B58E5  0f 85 ed 00 00 00                               jne       0x6b59d8 ; loc_6B59D8 [RVA 0x6B59D8]
006B58EB  4c 8b 4c 24 50                                  mov       r9, qword ptr [rsp + 0x50]
006B58F0  4c 8b 54 24 40                                  mov       r10, qword ptr [rsp + 0x40]
006B58F5  4c 8b 5c 24 58                                  mov       r11, qword ptr [rsp + 0x58]
006B58FA  49 81 e3 ff ff 00 00                            and       r11, 0xffff
006B5901  4d 85 d2                                        test      r10, r10
006B5904  75 18                                           jne       0x6b591e ; loc_6B591E [RVA 0x6B591E]
006B5906  49 89 11                                        mov       qword ptr [r9], rdx
006B5909  4d 89 41 08                                     mov       qword ptr [r9 + 8], r8
006B590D  49 83 c1 10                                     add       r9, 0x10
006B5911  49 ff cb                                        dec       r11
006B5914  4d 85 db                                        test      r11, r11
006B5917  75 05                                           jne       0x6b591e ; loc_6B591E [RVA 0x6B591E]
006B5919  e9 ba 00 00 00                                  jmp       0x6b59d8 ; loc_6B59D8 [RVA 0x6B59D8]
006B591E  49 ff ca                                        dec       r10
006B5921  48 8b c8                                        mov       rcx, rax
006B5924  49 c7 c0 06 00 00 00                            mov       r8, 6
006B592B  4d 2b c2                                        sub       r8, r10
006B592E  4d 8b d0                                        mov       r10, r8
006B5931  49 8b c2                                        mov       rax, r10
006B5934  49 83 c2 01                                     add       r10, 1
006B5938  49 f7 e2                                        mul       r10
006B593B  48 d1 e8                                        shr       rax, 1
006B593E  49 c7 c0 1b 00 00 00                            mov       r8, 0x1b
006B5945  4c 2b c0                                        sub       r8, rax
006B5948  4d 2b c3                                        sub       r8, r11
006B594B  49 c1 e0 02                                     shl       r8, 2
006B594F  48 8b c1                                        mov       rax, rcx
006B5952  49 8b d1                                        mov       rdx, r9
006B5955  48 83 ea 10                                     sub       rdx, 0x10
006B5959  4c 8d 0d 0c 00 00 00                            lea       r9, [rip + 0xc]
006B5960  4d 03 c8                                        add       r9, r8
006B5963  41 ff e1                                        jmp       r9
006B5966  cc                                              int3
006B5967  cc                                              int3
006B5968  cc                                              int3
006B5969  0f 1f 00                                        nop       dword ptr [rax]
006B596C  0f 11 6a 60                                     movups    xmmword ptr [rdx + 0x60], xmm5
006B5970  0f 11 62 50                                     movups    xmmword ptr [rdx + 0x50], xmm4
006B5974  0f 11 5a 40                                     movups    xmmword ptr [rdx + 0x40], xmm3
006B5978  0f 11 52 30                                     movups    xmmword ptr [rdx + 0x30], xmm2
006B597C  0f 11 4a 20                                     movups    xmmword ptr [rdx + 0x20], xmm1
006B5980  0f 11 42 10                                     movups    xmmword ptr [rdx + 0x10], xmm0
006B5984  eb 52                                           jmp       0x6b59d8 ; loc_6B59D8 [RVA 0x6B59D8]
006B5986  90                                              nop
006B5987  90                                              nop
006B5988  0f 11 6a 50                                     movups    xmmword ptr [rdx + 0x50], xmm5
006B598C  0f 11 62 40                                     movups    xmmword ptr [rdx + 0x40], xmm4
006B5990  0f 11 5a 30                                     movups    xmmword ptr [rdx + 0x30], xmm3
006B5994  0f 11 52 20                                     movups    xmmword ptr [rdx + 0x20], xmm2
006B5998  0f 11 4a 10                                     movups    xmmword ptr [rdx + 0x10], xmm1
006B599C  eb 3a                                           jmp       0x6b59d8 ; loc_6B59D8 [RVA 0x6B59D8]
006B599E  90                                              nop
006B599F  90                                              nop
006B59A0  0f 11 6a 40                                     movups    xmmword ptr [rdx + 0x40], xmm5
006B59A4  0f 11 62 30                                     movups    xmmword ptr [rdx + 0x30], xmm4
006B59A8  0f 11 5a 20                                     movups    xmmword ptr [rdx + 0x20], xmm3
006B59AC  0f 11 52 10                                     movups    xmmword ptr [rdx + 0x10], xmm2
006B59B0  eb 26                                           jmp       0x6b59d8 ; loc_6B59D8 [RVA 0x6B59D8]
006B59B2  90                                              nop
006B59B3  90                                              nop
006B59B4  0f 11 6a 30                                     movups    xmmword ptr [rdx + 0x30], xmm5
006B59B8  0f 11 62 20                                     movups    xmmword ptr [rdx + 0x20], xmm4
006B59BC  0f 11 5a 10                                     movups    xmmword ptr [rdx + 0x10], xmm3
006B59C0  eb 16                                           jmp       0x6b59d8 ; loc_6B59D8 [RVA 0x6B59D8]
006B59C2  90                                              nop
006B59C3  90                                              nop
006B59C4  0f 11 6a 20                                     movups    xmmword ptr [rdx + 0x20], xmm5
006B59C8  0f 11 62 10                                     movups    xmmword ptr [rdx + 0x10], xmm4
006B59CC  eb 0a                                           jmp       0x6b59d8 ; loc_6B59D8 [RVA 0x6B59D8]
006B59CE  90                                              nop
006B59CF  90                                              nop
006B59D0  0f 11 6a 10                                     movups    xmmword ptr [rdx + 0x10], xmm5
006B59D4  eb 02                                           jmp       0x6b59d8 ; loc_6B59D8 [RVA 0x6B59D8]
006B59D6  90                                              nop
006B59D7  90                                              nop
006B59D8  48 83 c4 28                                     add       rsp, 0x28
006B59DC  c3                                              ret
006B59DD  cc                                              int3
006B59DE  cc                                              int3
006B59DF  cc                                              int3
006B59E0  cc                                              int3
006B59E1  cc                                              int3
006B59E2  cc                                              int3
006B59E3  cc                                              int3
006B59E4  cc                                              int3
006B59E5  cc                                              int3
006B59E6  66 66 0f 1f 84 00 00 00 00 00                   nop       word ptr [rax + rax]
