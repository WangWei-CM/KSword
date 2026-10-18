MiGetPageTablePfnBuddyRaw at RVA 0x34D7C0
extent source: next public symbol (first .pdata end 0x34D822); RVA 0x34D7C0..0x34D830 (exclusive)
shown: 43 instruction(s), max 200
All addresses are RVAs.

0034D7C0  48 8b 11                                        mov       rdx, qword ptr [rcx]
0034D7C3  8b 41 24                                        mov       eax, dword ptr [rcx + 0x24]
0034D7C6  25 00 00 ff 03                                  and       eax, 0x3ff0000
0034D7CB  48 d1 ea                                        shr       rdx, 1
0034D7CE  48 c1 e0 0f                                     shl       rax, 0xf
0034D7D2  0f ba f2 1f                                     btr       edx, 0x1f
0034D7D6  48 0b c2                                        or        rax, rdx
0034D7D9  74 23                                           je        0x34d7fe ; loc_34D7FE [RVA 0x34D7FE]
0034D7DB  48 b9 00 00 00 00 00 ff ff ff                   movabs    rcx, 0xffffff0000000000
0034D7E5  48 03 c8                                        add       rcx, rax
0034D7E8  48 83 f9 01                                     cmp       rcx, 1
0034D7EC  76 14                                           jbe       0x34d802 ; loc_34D802 [RVA 0x34D802]
0034D7EE  48 ff c8                                        dec       rax
0034D7F1  48 c1 e0 04                                     shl       rax, 4
0034D7F5  48 03 05 bc b3 ae 00                            add       rax, qword ptr [rip + 0xaeb3bc]
0034D7FC  c3                                              ret
0034D7FD  cc                                              int3
0034D7FE  33 c0                                           xor       eax, eax
0034D800  c3                                              ret
0034D801  cc                                              int3
0034D802  48 b9 01 00 00 00 00 01 00 00                   movabs    rcx, 0x10000000001
0034D80C  48 3b c1                                        cmp       rax, rcx
0034D80F  75 eb                                           jne       0x34d7fc ; loc_34D7FC [RVA 0x34D7FC]
0034D811  48 8b 0d d8 82 c7 00                            mov       rcx, qword ptr [rip + 0xc782d8]
0034D818  48 85 c9                                        test      rcx, rcx
0034D81B  74 df                                           je        0x34d7fc ; loc_34D7FC [RVA 0x34D7FC]
0034D81D  48 8b c1                                        mov       rax, rcx
0034D820  c3                                              ret
0034D821  cc                                              int3
0034D822  cc                                              int3
0034D823  cc                                              int3
0034D824  cc                                              int3
0034D825  cc                                              int3
0034D826  cc                                              int3
0034D827  cc                                              int3
0034D828  cc                                              int3
0034D829  cc                                              int3
0034D82A  cc                                              int3
0034D82B  cc                                              int3
0034D82C  cc                                              int3
0034D82D  cc                                              int3
0034D82E  cc                                              int3
0034D82F  cc                                              int3
