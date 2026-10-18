HvcallpExtendedFastHypercall at RVA 0x6B5870
extent source: next public symbol (first .pdata end 0x6B58C9); RVA 0x6B5870..0x6B58D0 (exclusive)
shown: 33 instruction(s), max 200
All addresses are RVAs.

006B5870  48 83 ec 08                                     sub       rsp, 8
006B5874  49 83 c0 0f                                     add       r8, 0xf
006B5878  49 c1 e8 04                                     shr       r8, 4
006B587C  49 83 f8 01                                     cmp       r8, 1
006B5880  7e 2f                                           jle       0x6b58b1 ; loc_6B58B1 [RVA 0x6B58B1]
006B5882  49 ff c8                                        dec       r8
006B5885  49 c1 e0 02                                     shl       r8, 2
006B5889  4c 8d 0d 21 00 00 00                            lea       r9, [rip + 0x21]
006B5890  4d 2b c8                                        sub       r9, r8
006B5893  41 ff e1                                        jmp       r9
006B5896  cc                                              int3
006B5897  cc                                              int3
006B5898  cc                                              int3
006B5899  0f 10 6a 60                                     movups    xmm5, xmmword ptr [rdx + 0x60]
006B589D  0f 10 62 50                                     movups    xmm4, xmmword ptr [rdx + 0x50]
006B58A1  0f 10 5a 40                                     movups    xmm3, xmmword ptr [rdx + 0x40]
006B58A5  0f 10 52 30                                     movups    xmm2, xmmword ptr [rdx + 0x30]
006B58A9  0f 10 4a 20                                     movups    xmm1, xmmword ptr [rdx + 0x20]
006B58AD  0f 10 42 10                                     movups    xmm0, xmmword ptr [rdx + 0x10]
006B58B1  4c 8b 42 08                                     mov       r8, qword ptr [rdx + 8]
006B58B5  48 8b 12                                        mov       rdx, qword ptr [rdx]
006B58B8  48 8b 05 99 bf b4 00                            mov       rax, qword ptr [rip + 0xb4bf99]
006B58BF  ff d0                                           call      rax
006B58C1  0f 1f 00                                        nop       dword ptr [rax]
006B58C4  48 83 c4 08                                     add       rsp, 8
006B58C8  c3                                              ret
006B58C9  cc                                              int3
006B58CA  cc                                              int3
006B58CB  cc                                              int3
006B58CC  cc                                              int3
006B58CD  cc                                              int3
006B58CE  cc                                              int3
006B58CF  90                                              nop
