MI_IS_PHYSICAL_ADDRESS at RVA 0x2FE1B0
extent source: next public symbol (first .pdata end 0x2FE2F2); RVA 0x2FE1B0..0x2FE300 (exclusive)
shown: 101 instruction(s), max 200
All addresses are RVAs.

002FE1B0  48 89 5c 24 08                                  mov       qword ptr [rsp + 8], rbx
002FE1B5  57                                              push      rdi
002FE1B6  48 83 ec 20                                     sub       rsp, 0x20
002FE1BA  48 c1 e9 09                                     shr       rcx, 9
002FE1BE  49 b8 f8 ff ff ff 7f 00 00 00                   movabs    r8, 0x7ffffffff8
002FE1C8  49 23 c8                                        and       rcx, r8
002FE1CB  48 ba 00 00 00 00 80 f6 ff ff                   movabs    rdx, 0xfffff68000000000
002FE1D5  41 bb 04 00 00 00                               mov       r11d, 4
002FE1DB  48 8b c2                                        mov       rax, rdx
002FE1DE  48 03 c8                                        add       rcx, rax
002FE1E1  48 89 0c 24                                     mov       qword ptr [rsp], rcx
002FE1E5  48 c1 e9 09                                     shr       rcx, 9
002FE1E9  49 23 c8                                        and       rcx, r8
002FE1EC  48 8b c2                                        mov       rax, rdx
002FE1EF  48 03 c8                                        add       rcx, rax
002FE1F2  48 89 4c 24 08                                  mov       qword ptr [rsp + 8], rcx
002FE1F7  48 c1 e9 09                                     shr       rcx, 9
002FE1FB  49 23 c8                                        and       rcx, r8
002FE1FE  48 8b c2                                        mov       rax, rdx
002FE201  48 03 c8                                        add       rcx, rax
002FE204  48 89 4c 24 10                                  mov       qword ptr [rsp + 0x10], rcx
002FE209  48 c1 e9 09                                     shr       rcx, 9
002FE20D  49 23 c8                                        and       rcx, r8
002FE210  48 8b c2                                        mov       rax, rdx
002FE213  48 03 c8                                        add       rcx, rax
002FE216  48 bf 00 d0 be 7d fb f6 ff ff                   movabs    rdi, 0xfffff6fb7dbed000
002FE220  48 89 4c 24 18                                  mov       qword ptr [rsp + 0x18], rcx
002FE225  45 8b d3                                        mov       r10d, r11d
002FE228  48 bb f8 d7 be 7d fb f6 ff ff                   movabs    rbx, 0xfffff6fb7dbed7f8
002FE232  4a 8b 54 d4 f8                                  mov       rdx, qword ptr [rsp + r10*8 - 8]
002FE237  49 ff ca                                        dec       r10
002FE23A  41 ff cb                                        dec       r11d
002FE23D  48 8b 02                                        mov       rax, qword ptr [rdx]
002FE240  48 8b cf                                        mov       rcx, rdi
002FE243  48 3b d1                                        cmp       rdx, rcx
002FE246  72 20                                           jb        0x2fe268 ; loc_2FE268 [RVA 0x2FE268]
002FE248  48 8b cb                                        mov       rcx, rbx
002FE24B  48 3b d1                                        cmp       rdx, rcx
002FE24E  77 18                                           ja        0x2fe268 ; loc_2FE268 [RVA 0x2FE268]
002FE250  48 8b c8                                        mov       rcx, rax
002FE253  a8 01                                           test      al, 1
002FE255  74 15                                           je        0x2fe26c ; loc_2FE26C [RVA 0x2FE26C]
002FE257  a8 42                                           test      al, 0x42
002FE259  41 0f 95 c1                                     setne     r9b
002FE25D  a8 20                                           test      al, 0x20
002FE25F  41 0f 95 c0                                     setne     r8b
002FE263  45 84 c8                                        test      r8b, r9b
002FE266  74 12                                           je        0x2fe27a ; loc_2FE27A [RVA 0x2FE27A]
002FE268  a8 01                                           test      al, 1
002FE26A  75 68                                           jne       0x2fe2d4 ; loc_2FE2D4 [RVA 0x2FE2D4]
002FE26C  33 c0                                           xor       eax, eax
002FE26E  48 8b 5c 24 30                                  mov       rbx, qword ptr [rsp + 0x30]
002FE273  48 83 c4 20                                     add       rsp, 0x20
002FE277  5f                                              pop       rdi
002FE278  c3                                              ret
002FE279  cc                                              int3
002FE27A  48 f7 05 43 78 cc 00 00 00 60 00                test      qword ptr [rip + 0xcc7843], 0x600000
002FE285  74 e1                                           je        0x2fe268 ; loc_2FE268 [RVA 0x2FE268]
002FE287  65 4c 8b 04 25 88 01 00 00                      mov       r8, qword ptr gs:[0x188]
002FE290  4d 8b 88 b8 00 00 00                            mov       r9, qword ptr [r8 + 0xb8]
002FE297  41 80 b9 60 01 00 00 01                         cmp       byte ptr [r9 + 0x160], 1
002FE29F  74 c7                                           je        0x2fe268 ; loc_2FE268 [RVA 0x2FE268]
002FE2A1  4d 8b 81 08 05 00 00                            mov       r8, qword ptr [r9 + 0x508]
002FE2A8  4d 85 c0                                        test      r8, r8
002FE2AB  74 bb                                           je        0x2fe268 ; loc_2FE268 [RVA 0x2FE268]
002FE2AD  48 c1 ea 03                                     shr       rdx, 3
002FE2B1  81 e2 ff 01 00 00                               and       edx, 0x1ff
002FE2B7  49 8b 14 d0                                     mov       rdx, qword ptr [r8 + rdx*8]
002FE2BB  f6 c2 20                                        test      dl, 0x20
002FE2BE  74 04                                           je        0x2fe2c4 ; loc_2FE2C4 [RVA 0x2FE2C4]
002FE2C0  48 83 c9 20                                     or        rcx, 0x20
002FE2C4  48 8b c1                                        mov       rax, rcx
002FE2C7  48 83 c8 42                                     or        rax, 0x42
002FE2CB  f6 c2 42                                        test      dl, 0x42
002FE2CE  48 0f 44 c1                                     cmove     rax, rcx
002FE2D2  eb 94                                           jmp       0x2fe268 ; loc_2FE268 [RVA 0x2FE268]
002FE2D4  84 c0                                           test      al, al
002FE2D6  78 0b                                           js        0x2fe2e3 ; loc_2FE2E3 [RVA 0x2FE2E3]
002FE2D8  49 83 fa 01                                     cmp       r10, 1
002FE2DC  74 8e                                           je        0x2fe26c ; loc_2FE26C [RVA 0x2FE26C]
002FE2DE  e9 4f ff ff ff                                  jmp       0x2fe232 ; loc_2FE232 [RVA 0x2FE232]
002FE2E3  48 8b 5c 24 30                                  mov       rbx, qword ptr [rsp + 0x30]
002FE2E8  41 8b c3                                        mov       eax, r11d
002FE2EB  48 83 c4 20                                     add       rsp, 0x20
002FE2EF  5f                                              pop       rdi
002FE2F0  c3                                              ret
002FE2F1  cc                                              int3
002FE2F2  cc                                              int3
002FE2F3  cc                                              int3
002FE2F4  cc                                              int3
002FE2F5  cc                                              int3
002FE2F6  cc                                              int3
002FE2F7  cc                                              int3
002FE2F8  cc                                              int3
002FE2F9  cc                                              int3
002FE2FA  cc                                              int3
002FE2FB  cc                                              int3
002FE2FC  cc                                              int3
002FE2FD  cc                                              int3
002FE2FE  cc                                              int3
002FE2FF  cc                                              int3
