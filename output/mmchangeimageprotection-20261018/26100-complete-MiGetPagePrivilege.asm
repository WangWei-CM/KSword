MiGetPagePrivilege: 235 CDB-discovered instructions verified against the local PE
RVA ranges (end exclusive): 0x299200..0x29925F, 0x299260..0x299365, 0x299366..0x2993D5, 0x2993D6..0x2994D9, 0x2994DA..0x299579, 0x6D34EC..0x6D3529
All addresses below are RVAs; direct calls use PDB public-symbol names.

00299200  40 53                                           push      rbx
00299202  56                                              push      rsi
00299203  57                                              push      rdi
00299204  41 55                                           push      r13
00299206  48 83 ec 28                                     sub       rsp, 0x28
0029920A  45 33 ed                                        xor       r13d, r13d
0029920D  49 8b d8                                        mov       rbx, r8
00299210  8b f2                                           mov       esi, edx
00299212  48 8b f9                                        mov       rdi, rcx
00299215  4d 85 c0                                        test      r8, r8
00299218  74 07                                           je        0x299221 ; loc_299221 [RVA 0x299221]
0029921A  4d 89 28                                        mov       qword ptr [r8], r13
0029921D  4d 89 68 08                                     mov       qword ptr [r8 + 8], r13
00299221  48 8b 05 a0 c8 d2 00                            mov       rax, qword ptr [rip + 0xd2c8a0]
00299228  48 89 6c 24 50                                  mov       qword ptr [rsp + 0x50], rbp
0029922D  48 0f ba e0 0c                                  bt        rax, 0xc
00299232  73 1a                                           jae       0x29924e ; loc_29924E [RVA 0x29924E]
00299234  48 8b 51 28                                     mov       rdx, qword ptr [rcx + 0x28]
00299238  48 8b ca                                        mov       rcx, rdx
0029923B  48 c1 e9 3c                                     shr       rcx, 0x3c
0029923F  80 e1 07                                        and       cl, 7
00299242  80 f9 03                                        cmp       cl, 3
00299245  75 07                                           jne       0x29924e ; loc_29924E [RVA 0x29924E]
00299247  48 0f ba e2 35                                  bt        rdx, 0x35
0029924C  73 12                                           jae       0x299260 ; loc_299260 [RVA 0x299260]
0029924E  33 c0                                           xor       eax, eax
00299250  48 8b 6c 24 50                                  mov       rbp, qword ptr [rsp + 0x50]
00299255  48 83 c4 28                                     add       rsp, 0x28
00299259  41 5d                                           pop       r13
0029925B  5f                                              pop       rdi
0029925C  5e                                              pop       rsi
0029925D  5b                                              pop       rbx
0029925E  c3                                              ret
00299260  48 85 db                                        test      rbx, rbx
00299263  0f 85 fd 00 00 00                               jne       0x299366 ; loc_299366 [RVA 0x299366]
00299269  48 8b 6f 08                                     mov       rbp, qword ptr [rdi + 8]
0029926D  48 b8 00 00 00 00 00 00 00 80                   movabs    rax, 0x8000000000000000
00299277  48 0b e8                                        or        rbp, rax
0029927A  48 8b 47 28                                     mov       rax, qword ptr [rdi + 0x28]
0029927E  48 85 c0                                        test      rax, rax
00299281  0f 89 ed 00 00 00                               jns       0x299374 ; loc_299374 [RVA 0x299374]
00299287  4c 8b 47 10                                     mov       r8, qword ptr [rdi + 0x10]
0029928B  49 0f ba e0 0a                                  bt        r8, 0xa
00299290  0f 83 b3 02 00 00                               jae       0x299549 ; loc_299549 [RVA 0x299549]
00299296  48 8b 05 e3 49 b9 00                            mov       rax, qword ptr [rip + 0xb949e3]
0029929D  49 8b d0                                        mov       rdx, r8
002992A0  48 85 c0                                        test      rax, rax
002992A3  74 0b                                           je        0x2992b0 ; loc_2992B0 [RVA 0x2992B0]
002992A5  f6 c2 10                                        test      dl, 0x10
002992A8  75 06                                           jne       0x2992b0 ; loc_2992B0 [RVA 0x2992B0]
002992AA  48 f7 d0                                        not       rax
002992AD  48 23 d0                                        and       rdx, rax
002992B0  48 c1 fa 10                                     sar       rdx, 0x10
002992B4  4c 8b 0a                                        mov       r9, qword ptr [rdx]
002992B7  41 8b 41 38                                     mov       eax, dword ptr [r9 + 0x38]
002992BB  a8 20                                           test      al, 0x20
002992BD  0f 84 ac 02 00 00                               je        0x29956f ; loc_29956F [RVA 0x29956F]
002992C3  48 8b 4a 08                                     mov       rcx, qword ptr [rdx + 8]
002992C7  48 3b e9                                        cmp       rbp, rcx
002992CA  72 82                                           jb        0x29924e ; loc_29924E [RVA 0x29924E]
002992CC  8b 42 2c                                        mov       eax, dword ptr [rdx + 0x2c]
002992CF  48 8d 0c c1                                     lea       rcx, [rcx + rax*8]
002992D3  48 3b e9                                        cmp       rbp, rcx
002992D6  0f 83 72 ff ff ff                               jae       0x29924e ; loc_29924E [RVA 0x29924E]
002992DC  41 8b 41 5c                                     mov       eax, dword ptr [r9 + 0x5c]
002992E0  44 8b d0                                        mov       r10d, eax
002992E3  41 81 e2 00 00 0c 00                            and       r10d, 0xc0000
002992EA  0f 95 c1                                        setne     cl
002992ED  0f ba e0 11                                     bt        eax, 0x11
002992F1  0f 92 c0                                        setb      al
002992F4  84 c8                                           test      al, cl
002992F6  0f 85 b9 01 00 00                               jne       0x2994b5 ; loc_2994B5 [RVA 0x2994B5]
002992FC  49 c1 e8 05                                     shr       r8, 5
00299300  48 f7 05 bd c7 d2 00 00 00 02 00                test      qword ptr [rip + 0xd2c7bd], 0x20000
0029930B  0f 85 c9 01 00 00                               jne       0x2994da ; loc_2994DA [RVA 0x2994DA]
00299311  45 85 d2                                        test      r10d, r10d
00299314  0f 84 34 ff ff ff                               je        0x29924e ; loc_29924E [RVA 0x29924E]
0029931A  ba 0b 00 00 00                                  mov       edx, 0xb
0029931F  41 f6 c0 02                                     test      r8b, 2
00299323  b8 08 00 00 00                                  mov       eax, 8
00299328  0f 45 c2                                        cmovne    eax, edx
0029932B  8b d0                                           mov       edx, eax
0029932D  48 85 db                                        test      rbx, rbx
00299330  74 22                                           je        0x299354 ; loc_299354 [RVA 0x299354]
00299332  49 8b 49 60                                     mov       rcx, qword ptr [r9 + 0x60]
00299336  48 83 e1 f8                                     and       rcx, 0xfffffffffffffff8
0029933A  48 8b 49 40                                     mov       rcx, qword ptr [rcx + 0x40]
0029933E  48 89 4b 08                                     mov       qword ptr [rbx + 8], rcx
00299342  49 2b a9 88 00 00 00                            sub       rbp, qword ptr [r9 + 0x88]
00299349  48 c1 fd 03                                     sar       rbp, 3
0029934D  48 c1 e5 0c                                     shl       rbp, 0xc
00299351  48 89 2b                                        mov       qword ptr [rbx], rbp
00299354  8b c2                                           mov       eax, edx
00299356  48 8b 6c 24 50                                  mov       rbp, qword ptr [rsp + 0x50]
0029935B  48 83 c4 28                                     add       rsp, 0x28
0029935F  41 5d                                           pop       r13
00299361  5f                                              pop       rdi
00299362  5e                                              pop       rsi
00299363  5b                                              pop       rbx
00299364  c3                                              ret
00299366  48 0f ba e0 0e                                  bt        rax, 0xe
0029936B  49 0f 43 dd                                     cmovae    rbx, r13
0029936F  e9 f5 fe ff ff                                  jmp       0x299269 ; loc_299269 [RVA 0x299269]
00299374  48 f7 05 49 c7 d2 00 00 00 02 00                test      qword ptr [rip + 0xd2c749], 0x20000
0029937F  4c 89 74 24 68                                  mov       qword ptr [rsp + 0x68], r14
00299384  74 50                                           je        0x2993d6 ; loc_2993D6 [RVA 0x2993D6]
00299386  48 c1 e5 19                                     shl       rbp, 0x19
0029938A  48 b8 00 00 00 00 80 f6 ff ff                   movabs    rax, 0xfffff68000000000
00299394  48 8b c0                                        mov       rax, rax
00299397  48 c1 e0 19                                     shl       rax, 0x19
0029939B  ba 02 00 00 00                                  mov       edx, 2
002993A0  48 2b e8                                        sub       rbp, rax
002993A3  41 be 11 00 00 00                               mov       r14d, 0x11
002993A9  48 c1 fd 10                                     sar       rbp, 0x10
002993AD  48 b8 00 00 00 00 00 80 ff ff                   movabs    rax, 0xffff800000000000
002993B7  48 3b e8                                        cmp       rbp, rax
002993BA  44 0f 42 f2                                     cmovb     r14d, edx
002993BE  41 8b c6                                        mov       eax, r14d
002993C1  4c 8b 74 24 68                                  mov       r14, qword ptr [rsp + 0x68]
002993C6  48 8b 6c 24 50                                  mov       rbp, qword ptr [rsp + 0x50]
002993CB  48 83 c4 28                                     add       rsp, 0x28
002993CF  41 5d                                           pop       r13
002993D1  5f                                              pop       rdi
002993D2  5e                                              pop       rsi
002993D3  5b                                              pop       rbx
002993D4  c3                                              ret
002993D6  48 8b 47 08                                     mov       rax, qword ptr [rdi + 8]
002993DA  48 85 c0                                        test      rax, rax
002993DD  0f 84 26 01 00 00                               je        0x299509 ; loc_299509 [RVA 0x299509]
002993E3  48 c1 e5 19                                     shl       rbp, 0x19
002993E7  48 b8 00 00 00 00 80 f6 ff ff                   movabs    rax, 0xfffff68000000000
002993F1  48 8b c0                                        mov       rax, rax
002993F4  48 c1 e0 19                                     shl       rax, 0x19
002993F8  48 2b e8                                        sub       rbp, rax
002993FB  48 b8 00 00 00 00 00 80 ff ff                   movabs    rax, 0xffff800000000000
00299405  48 c1 fd 10                                     sar       rbp, 0x10
00299409  48 3b e8                                        cmp       rbp, rax
0029940C  73 7b                                           jae       0x299489 ; loc_299489 [RVA 0x299489]
0029940E  41 be 14 00 00 00                               mov       r14d, 0x14
00299414  48 85 db                                        test      rbx, rbx
00299417  74 a5                                           je        0x2993be ; loc_2993BE [RVA 0x2993BE]
00299419  4c 89 64 24 60                                  mov       qword ptr [rsp + 0x60], r12
0029941E  41 b4 11                                        mov       r12b, 0x11
00299421  4c 89 7c 24 20                                  mov       qword ptr [rsp + 0x20], r15
00299426  40 f6 c6 01                                     test      sil, 1
0029942A  0f 84 f4 00 00 00                               je        0x299524 ; loc_299524 [RVA 0x299524]
00299430  48 8b cf                                        mov       rcx, rdi
00299433  e8 f8 43 0b 00                                  call      0x34d830 ; MiGetTopLevelPfn [RVA 0x34D830]
00299438  48 8b c8                                        mov       rcx, rax
0029943B  4c 8b c0                                        mov       r8, rax
0029943E  e8 7d 43 0b 00                                  call      0x34d7c0 ; MiGetPageTablePfnBuddyRaw [RVA 0x34D7C0]
00299443  4c 8b f8                                        mov       r15, rax
00299446  48 b8 ff ff ff ff ff ff ff 7f                   movabs    rax, 0x7fffffffffffffff
00299450  f0 49 21 40 18                                  lock and  qword ptr [r8 + 0x18], rax
00299455  40 f6 c6 05                                     test      sil, 5
00299459  0f 84 d9 00 00 00                               je        0x299538 ; loc_299538 [RVA 0x299538]
0029945F  4c 8b 64 24 60                                  mov       r12, qword ptr [rsp + 0x60]
00299464  4d 39 af 70 01 00 00                            cmp       qword ptr [r15 + 0x170], r13
0029946B  0f 84 90 00 00 00                               je        0x299501 ; loc_299501 [RVA 0x299501]
00299471  48 89 2b                                        mov       qword ptr [rbx], rbp
00299474  49 8b 87 70 01 00 00                            mov       rax, qword ptr [r15 + 0x170]
0029947B  48 89 43 08                                     mov       qword ptr [rbx + 8], rax
0029947F  4c 8b 7c 24 20                                  mov       r15, qword ptr [rsp + 0x20]
00299484  e9 35 ff ff ff                                  jmp       0x2993be ; loc_2993BE [RVA 0x2993BE]
00299489  8b 47 10                                        mov       eax, dword ptr [rdi + 0x10]
0029948C  41 be 01 00 00 00                               mov       r14d, 1
00299492  c0 e8 05                                        shr       al, 5
00299495  a8 02                                           test      al, 2
00299497  b8 40 00 00 00                                  mov       eax, 0x40
0029949C  44 0f 44 f0                                     cmove     r14d, eax
002994A0  48 85 db                                        test      rbx, rbx
002994A3  0f 84 15 ff ff ff                               je        0x2993be ; loc_2993BE [RVA 0x2993BE]
002994A9  48 89 2b                                        mov       qword ptr [rbx], rbp
002994AC  4c 89 6b 08                                     mov       qword ptr [rbx + 8], r13
002994B0  e9 09 ff ff ff                                  jmp       0x2993be ; loc_2993BE [RVA 0x2993BE]
002994B5  49 8d 81 80 00 00 00                            lea       rax, [r9 + 0x80]
002994BC  48 3b d0                                        cmp       rdx, rax
002994BF  0f 84 37 fe ff ff                               je        0x2992fc ; loc_2992FC [RVA 0x2992FC]
002994C5  48 8b 6c 24 50                                  mov       rbp, qword ptr [rsp + 0x50]
002994CA  b8 28 00 00 00                                  mov       eax, 0x28
002994CF  48 83 c4 28                                     add       rsp, 0x28
002994D3  41 5d                                           pop       r13
002994D5  5f                                              pop       rdi
002994D6  5e                                              pop       rsi
002994D7  5b                                              pop       rbx
002994D8  c3                                              ret
002994DA  ba 02 00 00 00                                  mov       edx, 2
002994DF  45 85 d2                                        test      r10d, r10d
002994E2  0f 84 45 fe ff ff                               je        0x29932d ; loc_29932D [RVA 0x29932D]
002994E8  ba 0a 00 00 00                                  mov       edx, 0xa
002994ED  41 f6 c0 02                                     test      r8b, 2
002994F1  0f 84 36 fe ff ff                               je        0x29932d ; loc_29932D [RVA 0x29932D]
002994F7  ba 0b 00 00 00                                  mov       edx, 0xb
002994FC  e9 2c fe ff ff                                  jmp       0x29932d ; loc_29932D [RVA 0x29932D]
00299501  45 8b f5                                        mov       r14d, r13d
00299504  e9 76 ff ff ff                                  jmp       0x29947f ; loc_29947F [RVA 0x29947F]
00299509  41 be 04 00 00 00                               mov       r14d, 4
0029950F  48 85 db                                        test      rbx, rbx
00299512  0f 84 a6 fe ff ff                               je        0x2993be ; loc_2993BE [RVA 0x2993BE]
00299518  4c 89 2b                                        mov       qword ptr [rbx], r13
0029951B  4c 89 6b 08                                     mov       qword ptr [rbx + 8], r13
0029951F  e9 9a fe ff ff                                  jmp       0x2993be ; loc_2993BE [RVA 0x2993BE]
00299524  40 f6 c6 02                                     test      sil, 2
00299528  0f 84 e0 9f 43 00                               je        0x6d350e ; loc_6D350E [RVA 0x6D350E]
0029952E  44 89 6c 24 58                                  mov       dword ptr [rsp + 0x58], r13d
00299533  e9 b4 9f 43 00                                  jmp       0x6d34ec ; loc_6D34EC [RVA 0x6D34EC]
00299538  41 0f b6 d4                                     movzx     edx, r12b
0029953C  48 8b cf                                        mov       rcx, rdi
0029953F  e8 ec b7 f9 ff                                  call      0x234d30 ; MiUnlockPageInline/MiUnlockPage [RVA 0x234D30]
00299544  e9 16 ff ff ff                                  jmp       0x29945f ; loc_29945F [RVA 0x29945F]
00299549  48 8b 05 78 c5 d2 00                            mov       rax, qword ptr [rip + 0xd2c578]
00299550  48 c1 e8 0d                                     shr       rax, 0xd
00299554  83 e0 10                                        and       eax, 0x10
00299557  83 c8 03                                        or        eax, 3
0029955A  48 85 db                                        test      rbx, rbx
0029955D  0f 84 ed fc ff ff                               je        0x299250 ; loc_299250 [RVA 0x299250]
00299563  4c 89 2b                                        mov       qword ptr [rbx], r13
00299566  4c 89 6b 08                                     mov       qword ptr [rbx + 8], r13
0029956A  e9 e1 fc ff ff                                  jmp       0x299250 ; loc_299250 [RVA 0x299250]
0029956F  b8 02 00 00 00                                  mov       eax, 2
00299574  e9 d7 fc ff ff                                  jmp       0x299250 ; loc_299250 [RVA 0x299250]
006D34EC  f0 48 0f ba 6f 18 3f                            lock bts  qword ptr [rdi + 0x18], 0x3f
006D34F3  0f 83 37 5f bc ff                               jae       0x299430 ; loc_299430 [RVA 0x299430]
006D34F9  48 8d 4c 24 58                                  lea       rcx, [rsp + 0x58]
006D34FE  e8 7d 0d d3 ff                                  call      0x404280 ; KeYieldProcessorEx [RVA 0x404280]
006D3503  48 8b 47 18                                     mov       rax, qword ptr [rdi + 0x18]
006D3507  48 85 c0                                        test      rax, rax
006D350A  78 ed                                           js        0x6d34f9 ; loc_6D34F9 [RVA 0x6D34F9]
006D350C  eb de                                           jmp       0x6d34ec ; loc_6D34EC [RVA 0x6D34EC]
006D350E  40 f6 c6 04                                     test      sil, 4
006D3512  0f 85 18 5f bc ff                               jne       0x299430 ; loc_299430 [RVA 0x299430]
006D3518  48 8b cf                                        mov       rcx, rdi
006D351B  e8 70 17 b6 ff                                  call      0x234c90 ; MiLockPageInline [RVA 0x234C90]
006D3520  44 0f b6 e0                                     movzx     r12d, al
006D3524  e9 07 5f bc ff                                  jmp       0x299430 ; loc_299430 [RVA 0x299430]
