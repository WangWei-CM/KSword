MiOffsetToProtos at RVA 0x310240
extent source: next public symbol (first .pdata end 0x310275); RVA 0x310240..0x3104D0 (exclusive)
shown: 166 instruction(s), max 200
All addresses are RVAs.

00310240  40 56                                           push      rsi
00310242  57                                              push      rdi
00310243  41 56                                           push      r14
00310245  48 81 ec c0 00 00 00                            sub       rsp, 0xc0
0031024C  48 8b c2                                        mov       rax, rdx
0031024F  4d 8b f0                                        mov       r14, r8
00310252  48 c1 e8 0c                                     shr       rax, 0xc
00310256  48 8b f2                                        mov       rsi, rdx
00310259  49 89 00                                        mov       qword ptr [r8], rax
0031025C  48 8b f9                                        mov       rdi, rcx
0031025F  e8 6c 02 00 00                                  call      0x3104d0 ; MiGetControlAreaPtes [RVA 0x3104D0]
00310264  49 8b 0e                                        mov       rcx, qword ptr [r14]
00310267  48 3b c8                                        cmp       rcx, rax
0031026A  0f 83 15 02 00 00                               jae       0x310485 ; loc_310485 [RVA 0x310485]
00310270  48 83 7f 40 00                                  cmp       qword ptr [rdi + 0x40], 0
00310275  48 89 9c 24 e0 00 00 00                         mov       qword ptr [rsp + 0xe0], rbx
0031027D  0f 84 0e 01 00 00                               je        0x310391 ; loc_310391 [RVA 0x310391]
00310283  8b 47 38                                        mov       eax, dword ptr [rdi + 0x38]
00310286  a8 20                                           test      al, 0x20
00310288  0f 85 cd 01 00 00                               jne       0x31045b ; loc_31045B [RVA 0x31045B]
0031028E  33 d2                                           xor       edx, edx
00310290  48 8d 4c 24 20                                  lea       rcx, [rsp + 0x20]
00310295  41 b8 98 00 00 00                               mov       r8d, 0x98
0031029B  e8 a0 04 3b 00                                  call      0x6c0740 ; memset [RVA 0x6C0740]
003102A0  48 b8 00 00 00 00 00 00 40 00                   movabs    rax, 0x40000000000000
003102AA  48 3b f0                                        cmp       rsi, rax
003102AD  0f 83 f2 00 00 00                               jae       0x3103a5 ; loc_3103A5 [RVA 0x3103A5]
003102B3  48 89 ac 24 e8 00 00 00                         mov       qword ptr [rsp + 0xe8], rbp
003102BB  48 8d 4f 48                                     lea       rcx, [rdi + 0x48]
003102BF  4c 89 bc 24 f0 00 00 00                         mov       qword ptr [rsp + 0xf0], r15
003102C7  e8 d4 03 00 00                                  call      0x3106a0 ; ExAcquireSpinLockShared [RVA 0x3106A0]
003102CC  48 8b 9f 28 01 00 00                            mov       rbx, qword ptr [rdi + 0x128]
003102D3  48 c1 ee 0c                                     shr       rsi, 0xc
003102D7  44 0f b6 f8                                     movzx     r15d, al
003102DB  8b 53 20                                        mov       edx, dword ptr [rbx + 0x20]
003102DE  8b 4b 24                                        mov       ecx, dword ptr [rbx + 0x24]
003102E1  81 e2 c0 ff 00 00                               and       edx, 0xffc0
003102E7  8b 43 20                                        mov       eax, dword ptr [rbx + 0x20]
003102EA  48 c1 e2 1a                                     shl       rdx, 0x1a
003102EE  48 0b d1                                        or        rdx, rcx
003102F1  8b 4b 28                                        mov       ecx, dword ptr [rbx + 0x28]
003102F4  48 03 ca                                        add       rcx, rdx
003102F7  4c 8d 41 ff                                     lea       r8, [rcx - 1]
003102FB  48 3b f2                                        cmp       rsi, rdx
003102FE  0f 82 b0 00 00 00                               jb        0x3103b4 ; loc_3103B4 [RVA 0x3103B4]
00310304  3d 00 00 10 00                                  cmp       eax, 0x100000
00310309  49 0f 42 c8                                     cmovb     rcx, r8
0031030D  48 3b f1                                        cmp       rsi, rcx
00310310  0f 87 9e 00 00 00                               ja        0x3103b4 ; loc_3103B4 [RVA 0x3103B4]
00310316  41 80 ff 11                                     cmp       r15b, 0x11
0031031A  0f 84 89 00 00 00                               je        0x3103a9 ; loc_3103A9 [RVA 0x3103A9]
00310320  f6 05 1f 58 cb 00 01                            test      byte ptr [rip + 0xcb581f], 1
00310327  0f 85 6e 01 00 00                               jne       0x31049b ; loc_31049B [RVA 0x31049B]
0031032D  f0 81 67 48 ff ff ff bf                         lock and  dword ptr [rdi + 0x48], 0xbfffffff
00310335  f0 ff 4f 48                                     lock dec  dword ptr [rdi + 0x48]
00310339  83 3d 20 5a cb 00 00                            cmp       dword ptr [rip + 0xcb5a20], 0
00310340  74 0d                                           je        0x31034f ; loc_31034F [RVA 0x31034F]
00310342  44 0f 20 c1                                     mov       rcx, cr8
00310346  41 0f b6 d7                                     movzx     edx, r15b
0031034A  e8 89 50 1e 00                                  call      0x4f53d8 ; KiLowerIrqlProcessIrqlFlags [RVA 0x4F53D8]
0031034F  45 0f 22 c7                                     mov       cr8, r15
00310353  48 8b ac 24 e8 00 00 00                         mov       rbp, qword ptr [rsp + 0xe8]
0031035B  4c 8b bc 24 f0 00 00 00                         mov       r15, qword ptr [rsp + 0xf0]
00310363  8b 4b 20                                        mov       ecx, dword ptr [rbx + 0x20]
00310366  8b 43 24                                        mov       eax, dword ptr [rbx + 0x24]
00310369  81 e1 c0 ff 00 00                               and       ecx, 0xffc0
0031036F  48 c1 e1 1a                                     shl       rcx, 0x1a
00310373  48 0b c8                                        or        rcx, rax
00310376  49 29 0e                                        sub       qword ptr [r14], rcx
00310379  48 8b c3                                        mov       rax, rbx
0031037C  48 8b 9c 24 e0 00 00 00                         mov       rbx, qword ptr [rsp + 0xe0]
00310384  48 81 c4 c0 00 00 00                            add       rsp, 0xc0
0031038B  41 5e                                           pop       r14
0031038D  5f                                              pop       rdi
0031038E  5e                                              pop       rsi
0031038F  c3                                              ret
00310390  cc                                              int3
00310391  48 8d 8f 80 00 00 00                            lea       rcx, [rdi + 0x80]
00310398  49 8b d6                                        mov       rdx, r14
0031039B  e8 b0 71 fd ff                                  call      0x2e7550 ; MiLocatePagefileSubsection [RVA 0x2E7550]
003103A0  48 8b d8                                        mov       rbx, rax
003103A3  eb d4                                           jmp       0x310379 ; loc_310379 [RVA 0x310379]
003103A5  33 db                                           xor       ebx, ebx
003103A7  eb ba                                           jmp       0x310363 ; loc_310363 [RVA 0x310363]
003103A9  48 8d 4f 48                                     lea       rcx, [rdi + 0x48]
003103AD  e8 9e cb ff ff                                  call      0x30cf50 ; ExReleaseSpinLockSharedFromDpcLevel [RVA 0x30CF50]
003103B2  eb 9f                                           jmp       0x310353 ; loc_310353 [RVA 0x310353]
003103B4  8b 44 24 40                                     mov       eax, dword ptr [rsp + 0x40]
003103B8  4c 8b 97 18 01 00 00                            mov       r10, qword ptr [rdi + 0x118]
003103BF  89 74 24 44                                     mov       dword ptr [rsp + 0x44], esi
003103C3  48 c1 ee 20                                     shr       rsi, 0x20
003103C7  0f b7 d6                                        movzx     edx, si
003103CA  c1 e2 06                                        shl       edx, 6
003103CD  8b ca                                           mov       ecx, edx
003103CF  33 c8                                           xor       ecx, eax
003103D1  81 e1 3f 00 ff ff                               and       ecx, 0xffff003f
003103D7  33 ca                                           xor       ecx, edx
003103D9  89 4c 24 40                                     mov       dword ptr [rsp + 0x40], ecx
003103DD  4d 85 d2                                        test      r10, r10
003103E0  0f 84 ae 00 00 00                               je        0x310494 ; loc_310494 [RVA 0x310494]
003103E6  44 8b 5c 24 44                                  mov       r11d, dword ptr [rsp + 0x44]
003103EB  0f 1f 44 00 00                                  nop       dword ptr [rax + rax]
003103F0  44 8b 4c 24 40                                  mov       r9d, dword ptr [rsp + 0x40]
003103F5  41 8b 42 e8                                     mov       eax, dword ptr [r10 - 0x18]
003103F9  41 81 e1 c0 ff 00 00                            and       r9d, 0xffc0
00310400  41 8b 4a ec                                     mov       ecx, dword ptr [r10 - 0x14]
00310404  25 c0 ff 00 00                                  and       eax, 0xffc0
00310409  45 8b 42 f0                                     mov       r8d, dword ptr [r10 - 0x10]
0031040D  48 c1 e0 1a                                     shl       rax, 0x1a
00310411  48 0b c1                                        or        rax, rcx
00310414  49 c1 e1 1a                                     shl       r9, 0x1a
00310418  41 8b 4a e8                                     mov       ecx, dword ptr [r10 - 0x18]
0031041C  4c 03 c0                                        add       r8, rax
0031041F  4d 0b cb                                        or        r9, r11
00310422  81 f9 00 00 10 00                               cmp       ecx, 0x100000
00310428  49 8d 50 ff                                     lea       rdx, [r8 - 1]
0031042C  4c 0f 42 c2                                     cmovb     r8, rdx
00310430  4d 3b c8                                        cmp       r9, r8
00310433  0f 87 86 00 00 00                               ja        0x3104bf ; loc_3104BF [RVA 0x3104BF]
00310439  4c 3b c8                                        cmp       r9, rax
0031043C  73 08                                           jae       0x310446 ; loc_310446 [RVA 0x310446]
0031043E  4d 8b 12                                        mov       r10, qword ptr [r10]
00310441  4d 85 d2                                        test      r10, r10
00310444  75 aa                                           jne       0x3103f0 ; loc_3103F0 [RVA 0x3103F0]
00310446  4d 85 d2                                        test      r10, r10
00310449  74 49                                           je        0x310494 ; loc_310494 [RVA 0x310494]
0031044B  49 8d 5a c8                                     lea       rbx, [r10 - 0x38]
0031044F  48 89 9f 28 01 00 00                            mov       qword ptr [rdi + 0x128], rbx
00310456  e9 bb fe ff ff                                  jmp       0x310316 ; loc_310316 [RVA 0x310316]
0031045B  48 8d 9f 80 00 00 00                            lea       rbx, [rdi + 0x80]
00310462  8b 43 2c                                        mov       eax, dword ptr [rbx + 0x2c]
00310465  48 3b c8                                        cmp       rcx, rax
00310468  0f 82 0b ff ff ff                               jb        0x310379 ; loc_310379 [RVA 0x310379]
0031046E  48 2b c8                                        sub       rcx, rax
00310471  49 89 0e                                        mov       qword ptr [r14], rcx
00310474  48 8b 5b 10                                     mov       rbx, qword ptr [rbx + 0x10]
00310478  8b 43 2c                                        mov       eax, dword ptr [rbx + 0x2c]
0031047B  48 3b c8                                        cmp       rcx, rax
0031047E  73 ee                                           jae       0x31046e ; loc_31046E [RVA 0x31046E]
00310480  e9 f4 fe ff ff                                  jmp       0x310379 ; loc_310379 [RVA 0x310379]
00310485  33 c0                                           xor       eax, eax
00310487  48 81 c4 c0 00 00 00                            add       rsp, 0xc0
0031048E  41 5e                                           pop       r14
00310490  5f                                              pop       rdi
00310491  5e                                              pop       rsi
00310492  c3                                              ret
00310493  cc                                              int3
00310494  33 db                                           xor       ebx, ebx
00310496  e9 7b fe ff ff                                  jmp       0x310316 ; loc_310316 [RVA 0x310316]
0031049B  8b 05 d7 cc bf 00                               mov       eax, dword ptr [rip + 0xbfccd7]
003104A1  85 c0                                           test      eax, eax
003104A3  0f 85 84 fe ff ff                               jne       0x31032d ; loc_31032D [RVA 0x31032D]
003104A9  48 8b 94 24 d8 00 00 00                         mov       rdx, qword ptr [rsp + 0xd8]
003104B1  48 8d 4f 48                                     lea       rcx, [rdi + 0x48]
003104B5  e8 de 0a 02 00                                  call      0x330f98 ; ExpReleaseSpinLockSharedFromDpcLevelInstrumented [RVA 0x330F98]
003104BA  e9 7a fe ff ff                                  jmp       0x310339 ; loc_310339 [RVA 0x310339]
003104BF  4d 8b 52 08                                     mov       r10, qword ptr [r10 + 8]
003104C3  e9 79 ff ff ff                                  jmp       0x310441 ; loc_310441 [RVA 0x310441]
003104C8  cc                                              int3
003104C9  cc                                              int3
003104CA  cc                                              int3
003104CB  cc                                              int3
003104CC  cc                                              int3
003104CD  cc                                              int3
003104CE  cc                                              int3
003104CF  cc                                              int3
