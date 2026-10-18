HvlpFlushRangeListTbEx at RVA 0x3DB4E0
extent source: next public symbol (first .pdata end 0x3DB5C7); RVA 0x3DB4E0..0x3DB5D0 (exclusive)
shown: 72 instruction(s), max 200
All addresses are RVAs.

003DB4E0  48 89 5c 24 08                                  mov       qword ptr [rsp + 8], rbx
003DB4E5  48 89 74 24 10                                  mov       qword ptr [rsp + 0x10], rsi
003DB4EA  57                                              push      rdi
003DB4EB  48 83 ec 60                                     sub       rsp, 0x60
003DB4EF  48 83 64 24 40 00                               and       qword ptr [rsp + 0x40], 0
003DB4F5  33 c0                                           xor       eax, eax
003DB4F7  45 8b d9                                        mov       r11d, r9d
003DB4FA  48 89 44 24 58                                  mov       qword ptr [rsp + 0x58], rax
003DB4FF  44 8a 8c 24 90 00 00 00                         mov       r9b, byte ptr [rsp + 0x90]
003DB507  0f 57 c0                                        xorps     xmm0, xmm0
003DB50A  48 8b f2                                        mov       rsi, rdx
003DB50D  0f 11 44 24 48                                  movups    xmmword ptr [rsp + 0x48], xmm0
003DB512  45 84 c9                                        test      r9b, r9b
003DB515  74 1f                                           je        0x3db536 ; loc_3DB536 [RVA 0x3DB536]
003DB517  41 f6 d8                                        neg       r8b
003DB51A  48 89 4c 24 40                                  mov       qword ptr [rsp + 0x40], rcx
003DB51F  1b d2                                           sbb       edx, edx
003DB521  f7 d2                                           not       edx
003DB523  83 e2 04                                        and       edx, 4
003DB526  8b c2                                           mov       eax, edx
003DB528  83 c8 02                                        or        eax, 2
003DB52B  48 85 c9                                        test      rcx, rcx
003DB52E  0f 45 c2                                        cmovne    eax, edx
003DB531  48 89 44 24 48                                  mov       qword ptr [rsp + 0x48], rax
003DB536  48 8b 9c 24 a0 00 00 00                         mov       rbx, qword ptr [rsp + 0xa0]
003DB53E  44 8b 94 24 98 00 00 00                         mov       r10d, dword ptr [rsp + 0x98]
003DB546  48 8b d3                                        mov       rdx, rbx
003DB549  41 8b ca                                        mov       ecx, r10d
003DB54C  e8 7f 00 00 00                                  call      0x3db5d0 ; HvlpCountFlushVaList [RVA 0x3DB5D0]
003DB551  8b 0d 79 a5 be 00                               mov       ecx, dword ptr [rip + 0xbea579]
003DB557  8b f8                                           mov       edi, eax
003DB559  c1 e9 08                                        shr       ecx, 8
003DB55C  83 e1 0f                                        and       ecx, 0xf
003DB55F  83 c1 05                                        add       ecx, 5
003DB562  03 c8                                           add       ecx, eax
003DB564  83 f9 0e                                        cmp       ecx, 0xe
003DB567  77 3e                                           ja        0x3db5a7 ; loc_3DB5A7 [RVA 0x3DB5A7]
003DB569  41 8b d2                                        mov       edx, r10d
003DB56C  41 8b cb                                        mov       ecx, r11d
003DB56F  e8 48 03 00 00                                  call      0x3db8bc ; HvlpAllowFastFlushList [RVA 0x3DB8BC]
003DB574  84 c0                                           test      al, al
003DB576  74 2f                                           je        0x3db5a7 ; loc_3DB5A7 [RVA 0x3DB5A7]
003DB578  89 7c 24 30                                     mov       dword ptr [rsp + 0x30], edi
003DB57C  48 8d 4c 24 40                                  lea       rcx, [rsp + 0x40]
003DB581  48 89 5c 24 28                                  mov       qword ptr [rsp + 0x28], rbx
003DB586  45 8b c3                                        mov       r8d, r11d
003DB589  48 8b d6                                        mov       rdx, rsi
003DB58C  44 89 54 24 20                                  mov       dword ptr [rsp + 0x20], r10d
003DB591  e8 5e 00 00 00                                  call      0x3db5f4 ; HvlpFastFlushListTbEx [RVA 0x3DB5F4]
003DB596  48 8b 5c 24 70                                  mov       rbx, qword ptr [rsp + 0x70]
003DB59B  48 8b 74 24 78                                  mov       rsi, qword ptr [rsp + 0x78]
003DB5A0  48 83 c4 60                                     add       rsp, 0x60
003DB5A4  5f                                              pop       rdi
003DB5A5  c3                                              ret
003DB5A6  cc                                              int3
003DB5A7  89 7c 24 30                                     mov       dword ptr [rsp + 0x30], edi
003DB5AB  48 8d 4c 24 40                                  lea       rcx, [rsp + 0x40]
003DB5B0  48 89 5c 24 28                                  mov       qword ptr [rsp + 0x28], rbx
003DB5B5  45 8b c3                                        mov       r8d, r11d
003DB5B8  48 8b d6                                        mov       rdx, rsi
003DB5BB  44 89 54 24 20                                  mov       dword ptr [rsp + 0x20], r10d
003DB5C0  e8 6b 17 00 00                                  call      0x3dcd30 ; HvlpSlowFlushListTbEx [RVA 0x3DCD30]
003DB5C5  eb cf                                           jmp       0x3db596 ; loc_3DB596 [RVA 0x3DB596]
003DB5C7  cc                                              int3
003DB5C8  cc                                              int3
003DB5C9  cc                                              int3
003DB5CA  cc                                              int3
003DB5CB  cc                                              int3
003DB5CC  cc                                              int3
003DB5CD  cc                                              int3
003DB5CE  cc                                              int3
003DB5CF  cc                                              int3
