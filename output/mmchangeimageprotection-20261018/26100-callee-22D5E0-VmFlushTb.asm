VmFlushTb at RVA 0x22D5E0
extent source: next public symbol (first .pdata end 0x22D61A); RVA 0x22D5E0..0x22D620 (exclusive)
shown: 21 instruction(s), max 200
All addresses are RVAs.

0022D5E0  48 83 ec 28                                     sub       rsp, 0x28
0022D5E4  44 8b c9                                        mov       r9d, ecx
0022D5E7  41 83 f8 01                                     cmp       r8d, 1
0022D5EB  75 27                                           jne       0x22d614 ; loc_22D614 [RVA 0x22D614]
0022D5ED  65 48 8b 04 25 88 01 00 00                      mov       rax, qword ptr gs:[0x188]
0022D5F6  4c 8b 80 b8 00 00 00                            mov       r8, qword ptr [rax + 0xb8]
0022D5FD  49 8b 88 70 06 00 00                            mov       rcx, qword ptr [r8 + 0x670]
0022D604  48 85 c9                                        test      rcx, rcx
0022D607  74 0b                                           je        0x22d614 ; loc_22D614 [RVA 0x22D614]
0022D609  4c 8b c2                                        mov       r8, rdx
0022D60C  41 8b d1                                        mov       edx, r9d
0022D60F  e8 cc ef ff ff                                  call      0x22c5e0 ; VmpFlushTb [RVA 0x22C5E0]
0022D614  48 83 c4 28                                     add       rsp, 0x28
0022D618  c3                                              ret
0022D619  cc                                              int3
0022D61A  cc                                              int3
0022D61B  cc                                              int3
0022D61C  cc                                              int3
0022D61D  cc                                              int3
0022D61E  cc                                              int3
0022D61F  cc                                              int3
