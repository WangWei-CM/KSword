MI_READ_PTE_LOCK_FREE at RVA 0x296680
extent source: next public symbol (first .pdata end 0x296714); RVA 0x296680..0x296720 (exclusive)
shown: 51 instruction(s), max 200
All addresses are RVAs.

00296680  48 8b 01                                        mov       rax, qword ptr [rcx]
00296683  48 ba 00 d0 be 7d fb f6 ff ff                   movabs    rdx, 0xfffff6fb7dbed000
0029668D  48 3b ca                                        cmp       rcx, rdx
00296690  72 23                                           jb        0x2966b5 ; loc_2966B5 [RVA 0x2966B5]
00296692  48 ba f8 d7 be 7d fb f6 ff ff                   movabs    rdx, 0xfffff6fb7dbed7f8
0029669C  48 3b ca                                        cmp       rcx, rdx
0029669F  77 14                                           ja        0x2966b5 ; loc_2966B5 [RVA 0x2966B5]
002966A1  a8 01                                           test      al, 1
002966A3  74 10                                           je        0x2966b5 ; loc_2966B5 [RVA 0x2966B5]
002966A5  a8 42                                           test      al, 0x42
002966A7  41 0f 95 c0                                     setne     r8b
002966AB  a8 20                                           test      al, 0x20
002966AD  0f 95 c2                                        setne     dl
002966B0  44 84 c2                                        test      dl, r8b
002966B3  74 02                                           je        0x2966b7 ; loc_2966B7 [RVA 0x2966B7]
002966B5  c3                                              ret
002966B6  cc                                              int3
002966B7  48 f7 05 06 f4 d2 00 00 00 60 00                test      qword ptr [rip + 0xd2f406], 0x600000
002966C2  74 f1                                           je        0x2966b5 ; loc_2966B5 [RVA 0x2966B5]
002966C4  65 48 8b 14 25 88 01 00 00                      mov       rdx, qword ptr gs:[0x188]
002966CD  4c 8b 82 b8 00 00 00                            mov       r8, qword ptr [rdx + 0xb8]
002966D4  41 80 b8 60 01 00 00 01                         cmp       byte ptr [r8 + 0x160], 1
002966DC  74 d7                                           je        0x2966b5 ; loc_2966B5 [RVA 0x2966B5]
002966DE  49 8b 90 08 05 00 00                            mov       rdx, qword ptr [r8 + 0x508]
002966E5  48 85 d2                                        test      rdx, rdx
002966E8  74 cb                                           je        0x2966b5 ; loc_2966B5 [RVA 0x2966B5]
002966EA  48 c1 e9 03                                     shr       rcx, 3
002966EE  81 e1 ff 01 00 00                               and       ecx, 0x1ff
002966F4  48 8b 0c ca                                     mov       rcx, qword ptr [rdx + rcx*8]
002966F8  f6 c1 20                                        test      cl, 0x20
002966FB  74 04                                           je        0x296701 ; loc_296701 [RVA 0x296701]
002966FD  48 83 c8 20                                     or        rax, 0x20
00296701  48 8b d0                                        mov       rdx, rax
00296704  48 83 ca 42                                     or        rdx, 0x42
00296708  f6 c1 42                                        test      cl, 0x42
0029670B  48 0f 44 d0                                     cmove     rdx, rax
0029670F  48 8b c2                                        mov       rax, rdx
00296712  c3                                              ret
00296713  cc                                              int3
00296714  cc                                              int3
00296715  cc                                              int3
00296716  cc                                              int3
00296717  cc                                              int3
00296718  cc                                              int3
00296719  cc                                              int3
0029671A  cc                                              int3
0029671B  cc                                              int3
0029671C  cc                                              int3
0029671D  cc                                              int3
0029671E  cc                                              int3
0029671F  cc                                              int3
