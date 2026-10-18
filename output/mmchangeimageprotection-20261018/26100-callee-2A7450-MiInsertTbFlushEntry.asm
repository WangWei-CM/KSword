MiInsertTbFlushEntry at RVA 0x2A7450
extent source: next public symbol (first .pdata end 0x2A7483); RVA 0x2A7450..0x2A76D4 (exclusive)
shown: 171 instruction(s), max 200
All addresses are RVAs.

002A7450  48 89 5c 24 10                                  mov       qword ptr [rsp + 0x10], rbx
002A7455  48 89 6c 24 18                                  mov       qword ptr [rsp + 0x18], rbp
002A745A  56                                              push      rsi
002A745B  57                                              push      rdi
002A745C  41 56                                           push      r14
002A745E  48 83 ec 20                                     sub       rsp, 0x20
002A7462  48 8b d9                                        mov       rbx, rcx
002A7465  41 8b f1                                        mov       esi, r9d
002A7468  bf 00 10 00 00                                  mov       edi, 0x1000
002A746D  49 8b e8                                        mov       rbp, r8
002A7470  4c 8b f2                                        mov       r14, rdx
002A7473  8d 0c f6                                        lea       ecx, [rsi + rsi*8]
002A7476  48 d3 e7                                        shl       rdi, cl
002A7479  83 7b 08 01                                     cmp       dword ptr [rbx + 8], 1
002A747D  0f 85 6f 01 00 00                               jne       0x2a75f2 ; loc_2A75F2 [RVA 0x2A75F2]
002A7483  4c 89 7c 24 40                                  mov       qword ptr [rsp + 0x40], r15
002A7488  45 33 d2                                        xor       r10d, r10d
002A748B  44 8b 4b 1c                                     mov       r9d, dword ptr [rbx + 0x1c]
002A748F  45 85 c9                                        test      r9d, r9d
002A7492  0f 84 c2 00 00 00                               je        0x2a755a ; loc_2A755A [RVA 0x2A755A]
002A7498  8b 43 10                                        mov       eax, dword ptr [rbx + 0x10]
002A749B  a8 02                                           test      al, 2
002A749D  75 6f                                           jne       0x2a750e ; loc_2A750E [RVA 0x2A750E]
002A749F  41 8d 41 ff                                     lea       eax, [r9 - 1]
002A74A3  4c 8d 1c c3                                     lea       r11, [rbx + rax*8]
002A74A7  48 8b 44 c3 28                                  mov       rax, qword ptr [rbx + rax*8 + 0x28]
002A74AC  48 8b d0                                        mov       rdx, rax
002A74AF  48 c1 ea 0a                                     shr       rdx, 0xa
002A74B3  83 e2 03                                        and       edx, 3
002A74B6  48 3b d6                                        cmp       rdx, rsi
002A74B9  0f 85 9b 00 00 00                               jne       0x2a755a ; loc_2A755A [RVA 0x2A755A]
002A74BF  4c 8b c0                                        mov       r8, rax
002A74C2  48 8b c8                                        mov       rcx, rax
002A74C5  41 81 e0 ff 03 00 00                            and       r8d, 0x3ff
002A74CC  48 81 e1 00 f0 ff ff                            and       rcx, 0xfffffffffffff000
002A74D3  49 8d 50 01                                     lea       rdx, [r8 + 1]
002A74D7  48 0f af d7                                     imul      rdx, rdi
002A74DB  48 03 d1                                        add       rdx, rcx
002A74DE  49 3b d6                                        cmp       rdx, r14
002A74E1  75 16                                           jne       0x2a74f9 ; loc_2A74F9 [RVA 0x2A74F9]
002A74E3  49 8d 0c 28                                     lea       rcx, [r8 + rbp]
002A74E7  49 3b c8                                        cmp       rcx, r8
002A74EA  76 0d                                           jbe       0x2a74f9 ; loc_2A74F9 [RVA 0x2A74F9]
002A74EC  48 81 f9 ff 03 00 00                            cmp       rcx, 0x3ff
002A74F3  0f 86 aa 01 00 00                               jbe       0x2a76a3 ; loc_2A76A3 [RVA 0x2A76A3]
002A74F9  41 8d 49 ff                                     lea       ecx, [r9 - 1]
002A74FD  48 8d 49 05                                     lea       rcx, [rcx + 5]
002A7501  48 8b 04 cb                                     mov       rax, qword ptr [rbx + rcx*8]
002A7505  4c 8d 1c cb                                     lea       r11, [rbx + rcx*8]
002A7509  48 8b c8                                        mov       rcx, rax
002A750C  eb 2e                                           jmp       0x2a753c ; loc_2A753C [RVA 0x2A753C]
002A750E  45 85 c9                                        test      r9d, r9d
002A7511  74 47                                           je        0x2a755a ; loc_2A755A [RVA 0x2A755A]
002A7513  8b 43 10                                        mov       eax, dword ptr [rbx + 0x10]
002A7516  a8 02                                           test      al, 2
002A7518  75 40                                           jne       0x2a755a ; loc_2A755A [RVA 0x2A755A]
002A751A  41 8d 49 ff                                     lea       ecx, [r9 - 1]
002A751E  48 8d 49 05                                     lea       rcx, [rcx + 5]
002A7522  48 8b 04 cb                                     mov       rax, qword ptr [rbx + rcx*8]
002A7526  4c 8d 1c cb                                     lea       r11, [rbx + rcx*8]
002A752A  48 8b d0                                        mov       rdx, rax
002A752D  48 8b c8                                        mov       rcx, rax
002A7530  48 c1 ea 0a                                     shr       rdx, 0xa
002A7534  83 e2 03                                        and       edx, 3
002A7537  48 3b d6                                        cmp       rdx, rsi
002A753A  75 1e                                           jne       0x2a755a ; loc_2A755A [RVA 0x2A755A]
002A753C  4c 8b ff                                        mov       r15, rdi
002A753F  48 8b d1                                        mov       rdx, rcx
002A7542  4c 0f af fd                                     imul      r15, rbp
002A7546  48 81 e2 00 f0 ff ff                            and       rdx, 0xfffffffffffff000
002A754D  4f 8d 04 37                                     lea       r8, [r15 + r14]
002A7551  49 3b d0                                        cmp       rdx, r8
002A7554  0f 84 06 01 00 00                               je        0x2a7660 ; loc_2A7660 [RVA 0x2A7660]
002A755A  44 3b 4b 0c                                     cmp       r9d, dword ptr [rbx + 0xc]
002A755E  74 6a                                           je        0x2a75ca ; loc_2A75CA [RVA 0x2A75CA]
002A7560  48 85 ed                                        test      rbp, rbp
002A7563  74 74                                           je        0x2a75d9 ; loc_2A75D9 [RVA 0x2A75D9]
002A7565  48 8d 45 ff                                     lea       rax, [rbp - 1]
002A7569  ba 00 04 00 00                                  mov       edx, 0x400
002A756E  48 3d ff 03 00 00                               cmp       rax, 0x3ff
002A7574  8b c6                                           mov       eax, esi
002A7576  48 0f 46 d5                                     cmovbe    rdx, rbp
002A757A  83 e0 03                                        and       eax, 3
002A757D  48 c1 e0 0a                                     shl       rax, 0xa
002A7581  48 8d 4a ff                                     lea       rcx, [rdx - 1]
002A7585  81 e1 ff 03 00 00                               and       ecx, 0x3ff
002A758B  48 0b c1                                        or        rax, rcx
002A758E  49 8b ce                                        mov       rcx, r14
002A7591  48 81 e1 00 f0 ff ff                            and       rcx, 0xfffffffffffff000
002A7598  48 0b c1                                        or        rax, rcx
002A759B  48 8b cf                                        mov       rcx, rdi
002A759E  4a 89 44 cb 28                                  mov       qword ptr [rbx + r9*8 + 0x28], rax
002A75A3  44 8b 4b 1c                                     mov       r9d, dword ptr [rbx + 0x1c]
002A75A7  48 01 53 20                                     add       qword ptr [rbx + 0x20], rdx
002A75AB  41 ff c1                                        inc       r9d
002A75AE  48 0f af ca                                     imul      rcx, rdx
002A75B2  44 89 4b 1c                                     mov       dword ptr [rbx + 0x1c], r9d
002A75B6  4c 03 f1                                        add       r14, rcx
002A75B9  48 2b ea                                        sub       rbp, rdx
002A75BC  74 a2                                           je        0x2a7560 ; loc_2A7560 [RVA 0x2A7560]
002A75BE  44 3b 4b 0c                                     cmp       r9d, dword ptr [rbx + 0xc]
002A75C2  0f 84 c0 fe ff ff                               je        0x2a7488 ; loc_2A7488 [RVA 0x2A7488]
002A75C8  eb 96                                           jmp       0x2a7560 ; loc_2A7560 [RVA 0x2A7560]
002A75CA  45 85 d2                                        test      r10d, r10d
002A75CD  74 5f                                           je        0x2a762e ; loc_2A762E [RVA 0x2A762E]
002A75CF  80 7b 19 00                                     cmp       byte ptr [rbx + 0x19], 0
002A75D3  0f 84 eb 00 00 00                               je        0x2a76c4 ; loc_2A76C4 [RVA 0x2A76C4]
002A75D9  4c 8b 7c 24 40                                  mov       r15, qword ptr [rsp + 0x40]
002A75DE  48 8b 5c 24 48                                  mov       rbx, qword ptr [rsp + 0x48]
002A75E3  48 8b 6c 24 50                                  mov       rbp, qword ptr [rsp + 0x50]
002A75E8  48 83 c4 20                                     add       rsp, 0x20
002A75EC  41 5e                                           pop       r14
002A75EE  5f                                              pop       rdi
002A75EF  5e                                              pop       rsi
002A75F0  c3                                              ret
002A75F1  cc                                              int3
002A75F2  8b 43 10                                        mov       eax, dword ptr [rbx + 0x10]
002A75F5  a8 04                                           test      al, 4
002A75F7  0f 85 86 fe ff ff                               jne       0x2a7483 ; loc_2A7483 [RVA 0x2A7483]
002A75FD  48 b9 00 00 00 00 80 f6 ff ff                   movabs    rcx, 0xfffff68000000000
002A7607  48 3b d1                                        cmp       rdx, rcx
002A760A  0f 82 73 fe ff ff                               jb        0x2a7483 ; loc_2A7483 [RVA 0x2A7483]
002A7610  48 b9 ff ff ff ff ff f6 ff ff                   movabs    rcx, 0xfffff6ffffffffff
002A761A  48 3b d1                                        cmp       rdx, rcx
002A761D  0f 87 60 fe ff ff                               ja        0x2a7483 ; loc_2A7483 [RVA 0x2A7483]
002A7623  83 c8 04                                        or        eax, 4
002A7626  89 43 10                                        mov       dword ptr [rbx + 0x10], eax
002A7629  e9 55 fe ff ff                                  jmp       0x2a7483 ; loc_2A7483 [RVA 0x2A7483]
002A762E  80 7b 19 00                                     cmp       byte ptr [rbx + 0x19], 0
002A7632  75 a5                                           jne       0x2a75d9 ; loc_2A75D9 [RVA 0x2A75D9]
002A7634  49 8b d1                                        mov       rdx, r9
002A7637  48 8d 4b 28                                     lea       rcx, [rbx + 0x28]
002A763B  4c 8d 0d de 82 3e 00                            lea       r9, [rip + 0x3e82de]
002A7642  41 b8 08 00 00 00                               mov       r8d, 8
002A7648  e8 43 93 25 00                                  call      0x500990 ; qsort [RVA 0x500990]
002A764D  48 8b cb                                        mov       rcx, rbx
002A7650  e8 1f 9b 19 00                                  call      0x441174 ; MiCompressTbFlushList [RVA 0x441174]
002A7655  41 ba 01 00 00 00                               mov       r10d, 1
002A765B  e9 2b fe ff ff                                  jmp       0x2a748b ; loc_2A748B [RVA 0x2A748B]
002A7660  81 e1 ff 03 00 00                               and       ecx, 0x3ff
002A7666  48 8d 14 29                                     lea       rdx, [rcx + rbp]
002A766A  48 81 fa ff 03 00 00                            cmp       rdx, 0x3ff
002A7671  0f 87 e3 fe ff ff                               ja        0x2a755a ; loc_2A755A [RVA 0x2A755A]
002A7677  48 3b d1                                        cmp       rdx, rcx
002A767A  0f 86 da fe ff ff                               jbe       0x2a755a ; loc_2A755A [RVA 0x2A755A]
002A7680  48 01 6b 20                                     add       qword ptr [rbx + 0x20], rbp
002A7684  49 2b c7                                        sub       rax, r15
002A7687  48 8d 0c 28                                     lea       rcx, [rax + rbp]
002A768B  48 8b d1                                        mov       rdx, rcx
002A768E  48 33 d0                                        xor       rdx, rax
002A7691  48 81 e2 00 fc ff ff                            and       rdx, 0xfffffffffffffc00
002A7698  48 33 d1                                        xor       rdx, rcx
002A769B  49 89 13                                        mov       qword ptr [r11], rdx
002A769E  e9 36 ff ff ff                                  jmp       0x2a75d9 ; loc_2A75D9 [RVA 0x2A75D9]
002A76A3  48 01 6b 20                                     add       qword ptr [rbx + 0x20], rbp
002A76A7  48 8d 0c 28                                     lea       rcx, [rax + rbp]
002A76AB  48 8b d1                                        mov       rdx, rcx
002A76AE  48 33 d0                                        xor       rdx, rax
002A76B1  48 81 e2 00 fc ff ff                            and       rdx, 0xfffffffffffffc00
002A76B8  48 33 d1                                        xor       rdx, rcx
002A76BB  49 89 53 28                                     mov       qword ptr [r11 + 0x28], rdx
002A76BF  e9 15 ff ff ff                                  jmp       0x2a75d9 ; loc_2A75D9 [RVA 0x2A75D9]
002A76C4  c6 43 19 01                                     mov       byte ptr [rbx + 0x19], 1
002A76C8  e9 0c ff ff ff                                  jmp       0x2a75d9 ; loc_2A75D9 [RVA 0x2A75D9]
002A76CD  cc                                              int3
002A76CE  cc                                              int3
002A76CF  cc                                              int3
002A76D0  cc                                              int3
002A76D1  cc                                              int3
002A76D2  cc                                              int3
002A76D3  cc                                              int3
