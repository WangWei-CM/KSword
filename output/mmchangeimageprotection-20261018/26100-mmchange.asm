PE: C:\Windows\System32\ntoskrnl.exe
export: MmChangeImageProtection at RVA 0xA62300
.pdata extent: RVA 0xA62300..0xA62585 (exclusive), size 0x285
All addresses below are RVAs.

00A62300  48 8b c4                                        mov       rax, rsp
00A62303  48 89 58 08                                     mov       qword ptr [rax + 8], rbx
00A62307  48 89 68 10                                     mov       qword ptr [rax + 0x10], rbp
00A6230B  48 89 70 18                                     mov       qword ptr [rax + 0x18], rsi
00A6230F  44 89 48 20                                     mov       dword ptr [rax + 0x20], r9d
00A62313  57                                              push      rdi
00A62314  41 54                                           push      r12
00A62316  41 55                                           push      r13
00A62318  41 56                                           push      r14
00A6231A  41 57                                           push      r15
00A6231C  48 83 ec 30                                     sub       rsp, 0x30
00A62320  83 60 c8 00                                     and       dword ptr [rax - 0x38], 0
00A62324  49 8b d8                                        mov       rbx, r8
00A62327  41 8d 41 ff                                     lea       eax, [r9 - 1]
00A6232B  4c 8b e2                                        mov       r12, rdx
00A6232E  48 8b f9                                        mov       rdi, rcx
00A62331  83 f8 01                                        cmp       eax, 1
00A62334  0f 87 2d 02 00 00                               ja        0xa62567 ; loc_A62567 [RVA 0xA62567]
00A6233A  44 8b eb                                        mov       r13d, ebx
00A6233D  49 3b dd                                        cmp       rbx, r13
00A62340  0f 85 21 02 00 00                               jne       0xa62567 ; loc_A62567 [RVA 0xA62567]
00A62346  8a 41 0a                                        mov       al, byte ptr [rcx + 0xa]
00A62349  24 07                                           and       al, 7
00A6234B  3c 02                                           cmp       al, 2
00A6234D  0f 85 14 02 00 00                               jne       0xa62567 ; loc_A62567 [RVA 0xA62567]
00A62353  83 79 2c 00                                     cmp       dword ptr [rcx + 0x2c], 0
00A62357  0f 85 0a 02 00 00                               jne       0xa62567 ; loc_A62567 [RVA 0xA62567]
00A6235D  8b 69 28                                        mov       ebp, dword ptr [rcx + 0x28]
00A62360  f7 c5 ff 0f 00 00                               test      ebp, 0xfff
00A62366  0f 85 fb 01 00 00                               jne       0xa62567 ; loc_A62567 [RVA 0xA62567]
00A6236C  48 8b 49 20                                     mov       rcx, qword ptr [rcx + 0x20]
00A62370  48 8d 54 24 20                                  lea       rdx, [rsp + 0x20]
00A62375  c1 ed 0c                                        shr       ebp, 0xc
00A62378  e8 5b 9f 81 ff                                  call      0x27c2d8 ; MiLockLoadedDataTableEntryIfNecessary [RVA 0x27C2D8]
00A6237D  48 8b f0                                        mov       rsi, rax
00A62380  48 85 c0                                        test      rax, rax
00A62383  0f 85 96 00 00 00                               jne       0xa6241f ; loc_A6241F [RVA 0xA6241F]
00A62389  bb 25 02 00 c0                                  mov       ebx, 0xc0000225
00A6238E  eb 6f                                           jmp       0xa623ff ; loc_A623FF [RVA 0xA623FF]
00A62390  44 8b 47 28                                     mov       r8d, dword ptr [rdi + 0x28]
00A62394  41 b9 01 00 00 00                               mov       r9d, 1
00A6239A  48 8b 57 20                                     mov       rdx, qword ptr [rdi + 0x20]
00A6239E  48 8b ce                                        mov       rcx, rsi
00A623A1  e8 3a 36 86 ff                                  call      0x2c59e0 ; MiSetImageProtection [RVA 0x2C59E0]
00A623A6  83 7c 24 78 01                                  cmp       dword ptr [rsp + 0x78], 1
00A623AB  0f 85 22 01 00 00                               jne       0xa624d3 ; loc_A624D3 [RVA 0xA624D3]
00A623B1  48 f7 05 0c 37 56 00 00 40 00 00                test      qword ptr [rip + 0x56370c], 0x4000
00A623BC  0f 85 60 01 00 00                               jne       0xa62522 ; loc_A62522 [RVA 0xA62522]
00A623C2  48 f7 05 fb 36 56 00 00 80 00 00                test      qword ptr [rip + 0x5636fb], 0x8000
00A623CD  41 b9 03 00 00 00                               mov       r9d, 3
00A623D3  0f 85 70 01 00 00                               jne       0xa62549 ; loc_A62549 [RVA 0xA62549]
00A623D9  44 8b 47 28                                     mov       r8d, dword ptr [rdi + 0x28]
00A623DD  48 8b ce                                        mov       rcx, rsi
00A623E0  48 8b 57 20                                     mov       rdx, qword ptr [rdi + 0x20]
00A623E4  e8 f7 35 86 ff                                  call      0x2c59e0 ; MiSetImageProtection [RVA 0x2C59E0]
00A623E9  33 db                                           xor       ebx, ebx
00A623EB  0f ba 76 68 13                                  btr       dword ptr [rsi + 0x68], 0x13
00A623F0  83 7c 24 20 00                                  cmp       dword ptr [rsp + 0x20], 0
00A623F5  74 08                                           je        0xa623ff ; loc_A623FF [RVA 0xA623FF]
00A623F7  48 8b ce                                        mov       rcx, rsi
00A623FA  e8 51 98 81 ff                                  call      0x27bc50 ; MmUnlockLoadedDataTableEntry [RVA 0x27BC50]
00A623FF  8b c3                                           mov       eax, ebx
00A62401  48 8b 5c 24 60                                  mov       rbx, qword ptr [rsp + 0x60]
00A62406  48 8b 6c 24 68                                  mov       rbp, qword ptr [rsp + 0x68]
00A6240B  48 8b 74 24 70                                  mov       rsi, qword ptr [rsp + 0x70]
00A62410  48 83 c4 30                                     add       rsp, 0x30
00A62414  41 5f                                           pop       r15
00A62416  41 5e                                           pop       r14
00A62418  41 5d                                           pop       r13
00A6241A  41 5c                                           pop       r12
00A6241C  5f                                              pop       rdi
00A6241D  c3                                              ret
00A6241E  cc                                              int3
00A6241F  4c 8b 40 30                                     mov       r8, qword ptr [rax + 0x30]
00A62423  49 8d 14 1c                                     lea       rdx, [r12 + rbx]
00A62427  8b 40 40                                        mov       eax, dword ptr [rax + 0x40]
00A6242A  8b 4f 28                                        mov       ecx, dword ptr [rdi + 0x28]
00A6242D  49 03 c0                                        add       rax, r8
00A62430  48 03 4f 20                                     add       rcx, qword ptr [rdi + 0x20]
00A62434  48 3b c8                                        cmp       rcx, rax
00A62437  0f 87 20 01 00 00                               ja        0xa6255d ; loc_A6255D [RVA 0xA6255D]
00A6243D  4d 3b e0                                        cmp       r12, r8
00A62440  0f 82 17 01 00 00                               jb        0xa6255d ; loc_A6255D [RVA 0xA6255D]
00A62446  48 3b d0                                        cmp       rdx, rax
00A62449  0f 87 0e 01 00 00                               ja        0xa6255d ; loc_A6255D [RVA 0xA6255D]
00A6244F  48 8d 42 ff                                     lea       rax, [rdx - 1]
00A62453  49 3b c4                                        cmp       rax, r12
00A62456  0f 82 01 01 00 00                               jb        0xa6255d ; loc_A6255D [RVA 0xA6255D]
00A6245C  8b 46 68                                        mov       eax, dword ptr [rsi + 0x68]
00A6245F  b9 00 00 08 00                                  mov       ecx, 0x80000
00A62464  85 c1                                           test      ecx, eax
00A62466  0f 85 d3 00 00 00                               jne       0xa6253f ; loc_A6253F [RVA 0xA6253F]
00A6246C  0b c1                                           or        eax, ecx
00A6246E  33 db                                           xor       ebx, ebx
00A62470  49 8b c8                                        mov       rcx, r8
00A62473  89 46 68                                        mov       dword ptr [rsi + 0x68], eax
00A62476  e8 35 bd 89 ff                                  call      0x2fe1b0 ; MI_IS_PHYSICAL_ADDRESS [RVA 0x2FE1B0]
00A6247B  85 c0                                           test      eax, eax
00A6247D  0f 85 ee 00 00 00                               jne       0xa62571 ; loc_A62571 [RVA 0xA62571]
00A62483  4c 8d 7f 30                                     lea       r15, [rdi + 0x30]
00A62487  45 33 f6                                        xor       r14d, r14d
00A6248A  48 ba 00 00 00 00 00 de ff ff                   movabs    rdx, 0xffffde0000000000
00A62494  44 3b f5                                        cmp       r14d, ebp
00A62497  0f 83 f3 fe ff ff                               jae       0xa62390 ; loc_A62390 [RVA 0xA62390]
00A6249D  4b 8b 0c f7                                     mov       rcx, qword ptr [r15 + r14*8]
00A624A1  48 8d 0c 49                                     lea       rcx, [rcx + rcx*2]
00A624A5  48 c1 e1 04                                     shl       rcx, 4
00A624A9  48 8b c2                                        mov       rax, rdx
00A624AC  48 03 c8                                        add       rcx, rax
00A624AF  48 8b 41 28                                     mov       rax, qword ptr [rcx + 0x28]
00A624B3  48 85 c0                                        test      rax, rax
00A624B6  0f 88 b5 00 00 00                               js        0xa62571 ; loc_A62571 [RVA 0xA62571]
00A624BC  45 33 c0                                        xor       r8d, r8d
00A624BF  33 d2                                           xor       edx, edx
00A624C1  e8 3a 6d 83 ff                                  call      0x299200 ; MiGetPagePrivilege [RVA 0x299200]
00A624C6  a8 40                                           test      al, 0x40
00A624C8  0f 85 ad 00 00 00                               jne       0xa6257b ; loc_A6257B [RVA 0xA6257B]
00A624CE  41 ff c6                                        inc       r14d
00A624D1  eb b7                                           jmp       0xa6248a ; loc_A6248A [RVA 0xA6248A]
00A624D3  48 f7 05 ea 35 56 00 00 80 00 00                test      qword ptr [rip + 0x5635ea], 0x8000
00A624DE  0f 84 07 ff ff ff                               je        0xa623eb ; loc_A623EB [RVA 0xA623EB]
00A624E4  85 ed                                           test      ebp, ebp
00A624E6  0f 84 ff fe ff ff                               je        0xa623eb ; loc_A623EB [RVA 0xA623EB]
00A624EC  8b fd                                           mov       edi, ebp
00A624EE  48 bd 00 00 00 00 00 de ff ff                   movabs    rbp, 0xffffde0000000000
00A624F8  49 8b 07                                        mov       rax, qword ptr [r15]
00A624FB  48 8d 0c 40                                     lea       rcx, [rax + rax*2]
00A624FF  48 c1 e1 04                                     shl       rcx, 4
00A62503  48 8b c5                                        mov       rax, rbp
00A62506  48 03 c8                                        add       rcx, rax
00A62509  ba 09 00 00 00                                  mov       edx, 9
00A6250E  e8 8d 9f 8c ff                                  call      0x32c4a0 ; MiClearPfnImageVerified [RVA 0x32C4A0]
00A62513  4d 8d 7f 08                                     lea       r15, [r15 + 8]
00A62517  48 83 ef 01                                     sub       rdi, 1
00A6251B  75 db                                           jne       0xa624f8 ; loc_A624F8 [RVA 0xA624F8]
00A6251D  e9 c9 fe ff ff                                  jmp       0xa623eb ; loc_A623EB [RVA 0xA623EB]
00A62522  45 8b c5                                        mov       r8d, r13d
00A62525  49 8b d4                                        mov       rdx, r12
00A62528  48 8b cf                                        mov       rcx, rdi
00A6252B  e8 5c 00 00 00                                  call      0xa6258c ; VslValidateDynamicCodePages [RVA 0xA6258C]
00A62530  8b d8                                           mov       ebx, eax
00A62532  85 c0                                           test      eax, eax
00A62534  0f 88 b1 fe ff ff                               js        0xa623eb ; loc_A623EB [RVA 0xA623EB]
00A6253A  e9 83 fe ff ff                                  jmp       0xa623c2 ; loc_A623C2 [RVA 0xA623C2]
00A6253F  bb 43 00 00 c0                                  mov       ebx, 0xc0000043
00A62544  e9 a7 fe ff ff                                  jmp       0xa623f0 ; loc_A623F0 [RVA 0xA623F0]
00A62549  81 fb 2c 01 00 00                               cmp       ebx, 0x12c
00A6254F  b8 13 00 00 00                                  mov       eax, 0x13
00A62554  44 0f 44 c8                                     cmove     r9d, eax
00A62558  e9 7c fe ff ff                                  jmp       0xa623d9 ; loc_A623D9 [RVA 0xA623D9]
00A6255D  bb 18 00 00 c0                                  mov       ebx, 0xc0000018
00A62562  e9 89 fe ff ff                                  jmp       0xa623f0 ; loc_A623F0 [RVA 0xA623F0]
00A62567  b8 0d 00 00 c0                                  mov       eax, 0xc000000d
00A6256C  e9 90 fe ff ff                                  jmp       0xa62401 ; loc_A62401 [RVA 0xA62401]
00A62571  bb 18 00 00 c0                                  mov       ebx, 0xc0000018
00A62576  e9 70 fe ff ff                                  jmp       0xa623eb ; loc_A623EB [RVA 0xA623EB]
00A6257B  bb 45 00 00 c0                                  mov       ebx, 0xc0000045
00A62580  e9 66 fe ff ff                                  jmp       0xa623eb ; loc_A623EB [RVA 0xA623EB]
