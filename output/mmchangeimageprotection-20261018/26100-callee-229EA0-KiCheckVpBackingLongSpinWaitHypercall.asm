KiCheckVpBackingLongSpinWaitHypercall at RVA 0x229EA0
extent source: next public symbol (first .pdata end 0x229F08); RVA 0x229EA0..0x229F10 (exclusive)
shown: 39 instruction(s), max 200
All addresses are RVAs.

00229EA0  ba 01 00 00 00                                  mov       edx, 1
00229EA5  44 0f 20 c0                                     mov       rax, cr8
00229EA9  3c 02                                           cmp       al, 2
00229EAB  72 4f                                           jb        0x229efc ; loc_229EFC [RVA 0x229EFC]
00229EAD  65 48 8b 04 25 20 00 00 00                      mov       rax, qword ptr gs:[0x20]
00229EB6  48 83 b8 b8 8e 00 00 00                         cmp       qword ptr [rax + 0x8eb8], 0
00229EBE  74 3c                                           je        0x229efc ; loc_229EFC [RVA 0x229EFC]
00229EC0  44 8b 0d f9 bb d9 00                            mov       r9d, dword ptr [rip + 0xd9bbf9]
00229EC7  33 c9                                           xor       ecx, ecx
00229EC9  41 3b c9                                        cmp       ecx, r9d
00229ECC  73 36                                           jae       0x229f04 ; loc_229F04 [RVA 0x229F04]
00229ECE  4c 8d 05 ab dd d9 00                            lea       r8, [rip + 0xd9ddab]
00229ED5  4d 8b 04 c8                                     mov       r8, qword ptr [r8 + rcx*8]
00229ED9  41 84 50 23                                     test      byte ptr [r8 + 0x23], dl
00229EDD  74 21                                           je        0x229f00 ; loc_229F00 [RVA 0x229F00]
00229EDF  4d 8b 80 b8 8e 00 00                            mov       r8, qword ptr [r8 + 0x8eb8]
00229EE6  4d 85 c0                                        test      r8, r8
00229EE9  74 11                                           je        0x229efc ; loc_229EFC [RVA 0x229EFC]
00229EEB  41 8a 40 41                                     mov       al, byte ptr [r8 + 0x41]
00229EEF  45 8a 50 40                                     mov       r10b, byte ptr [r8 + 0x40]
00229EF3  84 c0                                           test      al, al
00229EF5  74 05                                           je        0x229efc ; loc_229EFC [RVA 0x229EFC]
00229EF7  45 84 d2                                        test      r10b, r10b
00229EFA  75 04                                           jne       0x229f00 ; loc_229F00 [RVA 0x229F00]
00229EFC  8a c2                                           mov       al, dl
00229EFE  c3                                              ret
00229EFF  cc                                              int3
00229F00  03 ca                                           add       ecx, edx
00229F02  eb c5                                           jmp       0x229ec9 ; loc_229EC9 [RVA 0x229EC9]
00229F04  32 d2                                           xor       dl, dl
00229F06  eb f4                                           jmp       0x229efc ; loc_229EFC [RVA 0x229EFC]
00229F08  cc                                              int3
00229F09  cc                                              int3
00229F0A  cc                                              int3
00229F0B  cc                                              int3
00229F0C  cc                                              int3
00229F0D  cc                                              int3
00229F0E  cc                                              int3
00229F0F  cc                                              int3
