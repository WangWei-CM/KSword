MiMakeValidPte at RVA 0x367550
extent source: next public symbol (first .pdata end 0x367556); RVA 0x367550..0x3677B0 (exclusive)
shown: 143 instruction(s), max 200
All addresses are RVAs.

00367550  40 57                                           push      rdi
00367552  48 83 ec 20                                     sub       rsp, 0x20
00367556  48 89 5c 24 30                                  mov       qword ptr [rsp + 0x30], rbx
0036755B  48 8d 05 9e 94 ca ff                            lea       rax, [rip - 0x356b62]
00367562  48 89 6c 24 38                                  mov       qword ptr [rsp + 0x38], rbp
00367567  41 8b f8                                        mov       edi, r8d
0036756A  48 89 74 24 40                                  mov       qword ptr [rsp + 0x40], rsi
0036756F  41 8b f0                                        mov       esi, r8d
00367572  83 e6 1f                                        and       esi, 0x1f
00367575  4c 89 74 24 48                                  mov       qword ptr [rsp + 0x48], r14
0036757A  48 bd 00 00 00 00 80 f6 ff ff                   movabs    rbp, 0xfffff68000000000
00367584  48 8b 1c f0                                     mov       rbx, qword ptr [rax + rsi*8]
00367588  48 b8 7f 0e 00 00 00 00 f0 ff                   movabs    rax, 0xfff0000000000e7f
00367592  48 23 d8                                        and       rbx, rax
00367595  48 b8 ff ff ff ff ff 00 00 00                   movabs    rax, 0xffffffffff
0036759F  48 23 d0                                        and       rdx, rax
003675A2  48 c1 e2 0c                                     shl       rdx, 0xc
003675A6  48 0b da                                        or        rbx, rdx
003675A9  48 83 cb 21                                     or        rbx, 0x21
003675AD  48 8b c5                                        mov       rax, rbp
003675B0  48 3b c8                                        cmp       rcx, rax
003675B3  0f 82 c0 00 00 00                               jb        0x367679 ; loc_367679 [RVA 0x367679]
003675B9  49 be ff ff ff ff ff f6 ff ff                   movabs    r14, 0xfffff6ffffffffff
003675C3  49 8b c6                                        mov       rax, r14
003675C6  48 3b c8                                        cmp       rcx, rax
003675C9  0f 87 aa 00 00 00                               ja        0x367679 ; loc_367679 [RVA 0x367679]
003675CF  48 b8 00 00 00 40 fb f6 ff ff                   movabs    rax, 0xfffff6fb40000000
003675D9  48 3b c8                                        cmp       rcx, rax
003675DC  0f 83 43 01 00 00                               jae       0x367725 ; loc_367725 [RVA 0x367725]
003675E2  48 8b d5                                        mov       rdx, rbp
003675E5  48 c1 e1 19                                     shl       rcx, 0x19
003675E9  44 8b c7                                        mov       r8d, edi
003675EC  48 c1 e2 19                                     shl       rdx, 0x19
003675F0  48 2b ca                                        sub       rcx, rdx
003675F3  48 c1 f9 10                                     sar       rcx, 0x10
003675F7  41 81 e0 00 00 00 04                            and       r8d, 0x4000000
003675FE  74 23                                           je        0x367623 ; loc_367623 [RVA 0x367623]
00367600  48 8b c5                                        mov       rax, rbp
00367603  48 3b c8                                        cmp       rcx, rax
00367606  72 1b                                           jb        0x367623 ; loc_367623 [RVA 0x367623]
00367608  49 8b c6                                        mov       rax, r14
0036760B  48 3b c8                                        cmp       rcx, rax
0036760E  77 13                                           ja        0x367623 ; loc_367623 [RVA 0x367623]
00367610  48 c1 e1 19                                     shl       rcx, 0x19
00367614  48 2b ca                                        sub       rcx, rdx
00367617  48 c1 f9 10                                     sar       rcx, 0x10
0036761B  48 8b c5                                        mov       rax, rbp
0036761E  48 3b c8                                        cmp       rcx, rax
00367621  73 e5                                           jae       0x367608 ; loc_367608 [RVA 0x367608]
00367623  48 b8 ff ff fe ff ff 7f 00 00                   movabs    rax, 0x7ffffffeffff
0036762D  48 3b c8                                        cmp       rcx, rax
00367630  76 31                                           jbe       0x367663 ; loc_367663 [RVA 0x367663]
00367632  48 b8 00 00 00 00 00 80 ff ff                   movabs    rax, 0xffff800000000000
0036763C  48 3b c8                                        cmp       rcx, rax
0036763F  72 26                                           jb        0x367667 ; loc_367667 [RVA 0x367667]
00367641  48 8b c5                                        mov       rax, rbp
00367644  48 3b c8                                        cmp       rcx, rax
00367647  0f 83 23 01 00 00                               jae       0x367770 ; loc_367770 [RVA 0x367770]
0036764D  48 3b 0d 2c 7d ac 00                            cmp       rcx, qword ptr [rip + 0xac7d2c]
00367654  0f 83 b2 00 00 00                               jae       0x36770c ; loc_36770C [RVA 0x36770C]
0036765A  0f b6 05 23 78 ac 00                            movzx     eax, byte ptr [rip + 0xac7823]
00367661  eb 0b                                           jmp       0x36766e ; loc_36766E [RVA 0x36766E]
00367663  48 83 cb 04                                     or        rbx, 4
00367667  0f b6 05 17 78 ac 00                            movzx     eax, byte ptr [rip + 0xac7817]
0036766E  85 c0                                           test      eax, eax
00367670  74 13                                           je        0x367685 ; loc_367685 [RVA 0x367685]
00367672  48 0f ba eb 08                                  bts       rbx, 8
00367677  eb 0c                                           jmp       0x367685 ; loc_367685 [RVA 0x367685]
00367679  48 0f ba eb 08                                  bts       rbx, 8
0036767E  41 81 e0 00 00 00 04                            and       r8d, 0x4000000
00367685  4c 8b 74 24 48                                  mov       r14, qword ptr [rsp + 0x48]
0036768A  48 8b 6c 24 38                                  mov       rbp, qword ptr [rsp + 0x38]
0036768F  85 ff                                           test      edi, edi
00367691  0f 88 f9 00 00 00                               js        0x367790 ; loc_367790 [RVA 0x367790]
00367697  48 8b 74 24 40                                  mov       rsi, qword ptr [rsp + 0x40]
0036769C  48 8b c3                                        mov       rax, rbx
0036769F  48 83 e0 fb                                     and       rax, 0xfffffffffffffffb
003676A3  0f ba e7 1e                                     bt        edi, 0x1e
003676A7  48 0f 43 c3                                     cmovae    rax, rbx
003676AB  48 8b 5c 24 30                                  mov       rbx, qword ptr [rsp + 0x30]
003676B0  0f ba e7 1d                                     bt        edi, 0x1d
003676B4  73 16                                           jae       0x3676cc ; loc_3676CC [RVA 0x3676CC]
003676B6  0f b6 15 c7 77 ac 00                            movzx     edx, byte ptr [rip + 0xac77c7]
003676BD  48 c1 e2 08                                     shl       rdx, 8
003676C1  48 33 c2                                        xor       rax, rdx
003676C4  48 0f ba f0 08                                  btr       rax, 8
003676C9  48 33 c2                                        xor       rax, rdx
003676CC  48 8b c8                                        mov       rcx, rax
003676CF  48 0f ba f1 08                                  btr       rcx, 8
003676D4  0f ba e7 1b                                     bt        edi, 0x1b
003676D8  48 0f 43 c8                                     cmovae    rcx, rax
003676DC  48 8b c1                                        mov       rax, rcx
003676DF  48 0f ba e8 07                                  bts       rax, 7
003676E4  45 85 c0                                        test      r8d, r8d
003676E7  48 0f 44 c1                                     cmove     rax, rcx
003676EB  48 b9 ff ff ff ff ff ff ff fa                   movabs    rcx, 0xfaffffffffffffff
003676F5  48 23 c1                                        and       rax, rcx
003676F8  48 b9 00 00 00 00 00 00 00 0a                   movabs    rcx, 0xa00000000000000
00367702  48 0b c1                                        or        rax, rcx
00367705  48 83 c4 20                                     add       rsp, 0x20
00367709  5f                                              pop       rdi
0036770A  c3                                              ret
0036770B  cc                                              int3
0036770C  48 3b 0d 7d 7c ac 00                            cmp       rcx, qword ptr [rip + 0xac7c7d]
00367713  0f 87 41 ff ff ff                               ja        0x36765a ; loc_36765A [RVA 0x36765A]
00367719  0f b6 05 65 77 ac 00                            movzx     eax, byte ptr [rip + 0xac7765]
00367720  e9 49 ff ff ff                                  jmp       0x36766e ; loc_36766E [RVA 0x36766E]
00367725  48 b8 ff ff ff 7f fb f6 ff ff                   movabs    rax, 0xfffff6fb7fffffff
0036772F  48 3b c8                                        cmp       rcx, rax
00367732  0f 87 aa fe ff ff                               ja        0x3675e2 ; loc_3675E2 [RVA 0x3675E2]
00367738  48 b8 68 df be 7d fb f6 ff ff                   movabs    rax, 0xfffff6fb7dbedf68
00367742  48 3b c8                                        cmp       rcx, rax
00367745  74 3a                                           je        0x367781 ; loc_367781 [RVA 0x367781]
00367747  0f ba e7 1a                                     bt        edi, 0x1a
0036774B  72 0d                                           jb        0x36775a ; loc_36775A [RVA 0x36775A]
0036774D  48 b8 ff ff ff ff ff ff ff 7f                   movabs    rax, 0x7fffffffffffffff
00367757  48 23 d8                                        and       rbx, rax
0036775A  e8 51 00 00 00                                  call      0x3677b0 ; MiUserPdeOrAbove [RVA 0x3677B0]
0036775F  85 c0                                           test      eax, eax
00367761  0f 84 7b fe ff ff                               je        0x3675e2 ; loc_3675E2 [RVA 0x3675E2]
00367767  48 83 cb 04                                     or        rbx, 4
0036776B  e9 72 fe ff ff                                  jmp       0x3675e2 ; loc_3675E2 [RVA 0x3675E2]
00367770  49 8b c6                                        mov       rax, r14
00367773  48 3b c8                                        cmp       rcx, rax
00367776  0f 87 d1 fe ff ff                               ja        0x36764d ; loc_36764D [RVA 0x36764D]
0036777C  e9 04 ff ff ff                                  jmp       0x367685 ; loc_367685 [RVA 0x367685]
00367781  48 b8 00 00 00 00 00 00 00 80                   movabs    rax, 0x8000000000000000
0036778B  48 0b d8                                        or        rbx, rax
0036778E  eb ca                                           jmp       0x36775a ; loc_36775A [RVA 0x36775A]
00367790  40 80 e6 05                                     and       sil, 5
00367794  40 80 fe 04                                     cmp       sil, 4
00367798  0f 85 f9 fe ff ff                               jne       0x367697 ; loc_367697 [RVA 0x367697]
0036779E  48 83 cb 42                                     or        rbx, 0x42
003677A2  e9 f0 fe ff ff                                  jmp       0x367697 ; loc_367697 [RVA 0x367697]
003677A7  cc                                              int3
003677A8  cc                                              int3
003677A9  cc                                              int3
003677AA  cc                                              int3
003677AB  cc                                              int3
003677AC  cc                                              int3
003677AD  cc                                              int3
003677AE  cc                                              int3
003677AF  cc                                              int3
