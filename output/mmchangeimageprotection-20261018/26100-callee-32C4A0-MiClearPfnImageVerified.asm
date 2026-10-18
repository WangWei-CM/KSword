MiClearPfnImageVerified at RVA 0x32C4A0
extent source: next public symbol (first .pdata end 0x32C4D5); RVA 0x32C4A0..0x32C640 (exclusive)
shown: 117 instruction(s), max 200
All addresses are RVAs.

0032C4A0  48 89 5c 24 20                                  mov       qword ptr [rsp + 0x20], rbx
0032C4A5  57                                              push      rdi
0032C4A6  48 83 ec 40                                     sub       rsp, 0x40
0032C4AA  48 8b 41 28                                     mov       rax, qword ptr [rcx + 0x28]
0032C4AE  0f 57 c0                                        xorps     xmm0, xmm0
0032C4B1  48 c1 e8 3c                                     shr       rax, 0x3c
0032C4B5  8b fa                                           mov       edi, edx
0032C4B7  24 07                                           and       al, 7
0032C4B9  48 8b d9                                        mov       rbx, rcx
0032C4BC  0f 11 44 24 30                                  movups    xmmword ptr [rsp + 0x30], xmm0
0032C4C1  3c 03                                           cmp       al, 3
0032C4C3  74 0c                                           je        0x32c4d1 ; loc_32C4D1 [RVA 0x32C4D1]
0032C4C5  48 8b 5c 24 68                                  mov       rbx, qword ptr [rsp + 0x68]
0032C4CA  48 83 c4 40                                     add       rsp, 0x40
0032C4CE  5f                                              pop       rdi
0032C4CF  c3                                              ret
0032C4D0  cc                                              int3
0032C4D1  48 8b 41 08                                     mov       rax, qword ptr [rcx + 8]
0032C4D5  48 89 6c 24 50                                  mov       qword ptr [rsp + 0x50], rbp
0032C4DA  8b ef                                           mov       ebp, edi
0032C4DC  4c 89 74 24 60                                  mov       qword ptr [rsp + 0x60], r14
0032C4E1  41 b6 11                                        mov       r14b, 0x11
0032C4E4  48 89 74 24 58                                  mov       qword ptr [rsp + 0x58], rsi
0032C4E9  83 e5 04                                        and       ebp, 4
0032C4EC  0f 84 c7 00 00 00                               je        0x32c5b9 ; loc_32C5B9 [RVA 0x32C5B9]
0032C4F2  40 f6 c7 08                                     test      dil, 8
0032C4F6  0f 84 83 00 00 00                               je        0x32c57f ; loc_32C57F [RVA 0x32C57F]
0032C4FC  4c 8d 44 24 30                                  lea       r8, [rsp + 0x30]
0032C501  ba 01 00 00 00                                  mov       edx, 1
0032C506  48 8b cb                                        mov       rcx, rbx
0032C509  e8 f2 cc f6 ff                                  call      0x299200 ; MiGetPagePrivilege [RVA 0x299200]
0032C50E  85 c0                                           test      eax, eax
0032C510  74 6d                                           je        0x32c57f ; loc_32C57F [RVA 0x32C57F]
0032C512  48 b9 00 00 00 00 00 de ff ff                   movabs    rcx, 0xffffde0000000000
0032C51C  48 8b f3                                        mov       rsi, rbx
0032C51F  48 2b f1                                        sub       rsi, rcx
0032C522  48 b9 ab aa aa aa aa aa aa aa                   movabs    rcx, 0xaaaaaaaaaaaaaaab
0032C52C  48 c1 fe 04                                     sar       rsi, 4
0032C530  48 0f af f1                                     imul      rsi, rcx
0032C534  a8 20                                           test      al, 0x20
0032C536  0f 85 a9 00 00 00                               jne       0x32c5e5 ; loc_32C5E5 [RVA 0x32C5E5]
0032C53C  40 f6 c7 10                                     test      dil, 0x10
0032C540  b8 10 00 00 00                                  mov       eax, 0x10
0032C545  b9 20 00 00 00                                  mov       ecx, 0x20
0032C54A  0f 45 c1                                        cmovne    eax, ecx
0032C54D  44 8b c0                                        mov       r8d, eax
0032C550  48 8b ce                                        mov       rcx, rsi
0032C553  41 83 c8 04                                     or        r8d, 4
0032C557  40 f6 c7 01                                     test      dil, 1
0032C55B  44 0f 44 c0                                     cmove     r8d, eax
0032C55F  41 f6 c0 30                                     test      r8b, 0x30
0032C563  74 48                                           je        0x32c5ad ; loc_32C5AD [RVA 0x32C5AD]
0032C565  41 c1 e8 02                                     shr       r8d, 2
0032C569  48 8d 54 24 30                                  lea       rdx, [rsp + 0x30]
0032C56E  41 83 e0 01                                     and       r8d, 1
0032C572  e8 7d 01 00 00                                  call      0x32c6f4 ; VslRemoveProtectedPage [RVA 0x32C6F4]
0032C577  85 c0                                           test      eax, eax
0032C579  0f 88 9a 00 00 00                               js        0x32c619 ; loc_32C619 [RVA 0x32C619]
0032C57F  33 d2                                           xor       edx, edx
0032C581  48 8b cb                                        mov       rcx, rbx
0032C584  e8 67 93 ed ff                                  call      0x2058f0 ; MiSetPfnIdentity [RVA 0x2058F0]
0032C589  85 ed                                           test      ebp, ebp
0032C58B  75 0c                                           jne       0x32c599 ; loc_32C599 [RVA 0x32C599]
0032C58D  41 0f b6 d6                                     movzx     edx, r14b
0032C591  48 8b cb                                        mov       rcx, rbx
0032C594  e8 97 87 f0 ff                                  call      0x234d30 ; MiUnlockPageInline/MiUnlockPage [RVA 0x234D30]
0032C599  48 8b 74 24 58                                  mov       rsi, qword ptr [rsp + 0x58]
0032C59E  48 8b 6c 24 50                                  mov       rbp, qword ptr [rsp + 0x50]
0032C5A3  4c 8b 74 24 60                                  mov       r14, qword ptr [rsp + 0x60]
0032C5A8  e9 18 ff ff ff                                  jmp       0x32c4c5 ; loc_32C4C5 [RVA 0x32C4C5]
0032C5AD  45 33 c0                                        xor       r8d, r8d
0032C5B0  33 d2                                           xor       edx, edx
0032C5B2  e8 bd 01 00 00                                  call      0x32c774 ; VslMakeProtectedPageExecutable [RVA 0x32C774]
0032C5B7  eb be                                           jmp       0x32c577 ; loc_32C577 [RVA 0x32C577]
0032C5B9  40 f6 c7 40                                     test      dil, 0x40
0032C5BD  74 4c                                           je        0x32c60b ; loc_32C60B [RVA 0x32C60B]
0032C5BF  33 f6                                           xor       esi, esi
0032C5C1  f0 48 0f ba 6b 18 3f                            lock bts  qword ptr [rbx + 0x18], 0x3f
0032C5C8  0f 83 24 ff ff ff                               jae       0x32c4f2 ; loc_32C4F2 [RVA 0x32C4F2]
0032C5CE  ff c6                                           inc       esi
0032C5D0  85 35 d6 7c c9 00                               test      dword ptr [rip + 0xc97cd6], esi
0032C5D6  74 17                                           je        0x32c5ef ; loc_32C5EF [RVA 0x32C5EF]
0032C5D8  f3 90                                           pause
0032C5DA  48 8b 43 18                                     mov       rax, qword ptr [rbx + 0x18]
0032C5DE  48 85 c0                                        test      rax, rax
0032C5E1  79 de                                           jns       0x32c5c1 ; loc_32C5C1 [RVA 0x32C5C1]
0032C5E3  eb e9                                           jmp       0x32c5ce ; loc_32C5CE [RVA 0x32C5CE]
0032C5E5  b8 00 01 00 00                                  mov       eax, 0x100
0032C5EA  e9 5e ff ff ff                                  jmp       0x32c54d ; loc_32C54D [RVA 0x32C54D]
0032C5EF  8b 05 03 95 c9 00                               mov       eax, dword ptr [rip + 0xc99503]
0032C5F5  a8 40                                           test      al, 0x40
0032C5F7  74 df                                           je        0x32c5d8 ; loc_32C5D8 [RVA 0x32C5D8]
0032C5F9  e8 a2 d8 ef ff                                  call      0x229ea0 ; KiCheckVpBackingLongSpinWaitHypercall [RVA 0x229EA0]
0032C5FE  84 c0                                           test      al, al
0032C600  74 d6                                           je        0x32c5d8 ; loc_32C5D8 [RVA 0x32C5D8]
0032C602  8b ce                                           mov       ecx, esi
0032C604  e8 67 d8 ef ff                                  call      0x229e70 ; HvlNotifyLongSpinWait [RVA 0x229E70]
0032C609  eb cf                                           jmp       0x32c5da ; loc_32C5DA [RVA 0x32C5DA]
0032C60B  e8 80 86 f0 ff                                  call      0x234c90 ; MiLockPageInline [RVA 0x234C90]
0032C610  44 0f b6 f0                                     movzx     r14d, al
0032C614  e9 d9 fe ff ff                                  jmp       0x32c4f2 ; loc_32C4F2 [RVA 0x32C4F2]
0032C619  45 33 c9                                        xor       r9d, r9d
0032C61C  48 c7 44 24 20 00 00 00 00                      mov       qword ptr [rsp + 0x20], 0
0032C625  4c 8b c6                                        mov       r8, rsi
0032C628  ba 0b 15 05 00                                  mov       edx, 0x5150b
0032C62D  41 8d 49 1a                                     lea       ecx, [r9 + 0x1a]
0032C631  e8 ca 0f 1d 00                                  call      0x4fd600 ; KeBugCheckEx [RVA 0x4FD600]
0032C636  cc                                              int3
0032C637  cc                                              int3
0032C638  cc                                              int3
0032C639  cc                                              int3
0032C63A  cc                                              int3
0032C63B  cc                                              int3
0032C63C  cc                                              int3
0032C63D  cc                                              int3
0032C63E  cc                                              int3
0032C63F  cc                                              int3
