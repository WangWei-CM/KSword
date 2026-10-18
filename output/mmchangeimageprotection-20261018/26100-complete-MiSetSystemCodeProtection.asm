MiSetSystemCodeProtection: 482 CDB-discovered instructions verified against the local PE
RVA ranges (end exclusive): 0x2C5A30..0x2C5D3F, 0x2C5D40..0x2C6230, 0x6D489E..0x6D48B3
All addresses below are RVAs; direct calls use PDB public-symbol names.

002C5A30  48 8b c4                                        mov       rax, rsp
002C5A33  44 89 48 20                                     mov       dword ptr [rax + 0x20], r9d
002C5A37  4c 89 40 18                                     mov       qword ptr [rax + 0x18], r8
002C5A3B  48 89 48 08                                     mov       qword ptr [rax + 8], rcx
002C5A3F  53                                              push      rbx
002C5A40  55                                              push      rbp
002C5A41  56                                              push      rsi
002C5A42  57                                              push      rdi
002C5A43  41 54                                           push      r12
002C5A45  41 55                                           push      r13
002C5A47  41 56                                           push      r14
002C5A49  41 57                                           push      r15
002C5A4B  48 81 ec 88 00 00 00                            sub       rsp, 0x88
002C5A52  45 33 ed                                        xor       r13d, r13d
002C5A55  41 8b e9                                        mov       ebp, r9d
002C5A58  4c 89 68 88                                     mov       qword ptr [rax - 0x78], r13
002C5A5C  49 8b f8                                        mov       rdi, r8
002C5A5F  48 8b f2                                        mov       rsi, rdx
002C5A62  48 8b d9                                        mov       rbx, rcx
002C5A65  45 8d 7d 01                                     lea       r15d, [r13 + 1]
002C5A69  41 83 f9 18                                     cmp       r9d, 0x18
002C5A6D  74 0a                                           je        0x2c5a79 ; loc_2C5A79 [RVA 0x2C5A79]
002C5A6F  40 f6 c5 10                                     test      bpl, 0x10
002C5A73  0f 85 54 04 00 00                               jne       0x2c5ecd ; loc_2C5ECD [RVA 0x2C5ECD]
002C5A79  44 89 6c 24 40                                  mov       dword ptr [rsp + 0x40], r13d
002C5A7E  4c 8b e6                                        mov       r12, rsi
002C5A81  48 b8 00 00 00 00 80 f6 ff ff                   movabs    rax, 0xfffff68000000000
002C5A8B  49 c1 e4 19                                     shl       r12, 0x19
002C5A8F  48 8b c8                                        mov       rcx, rax
002C5A92  48 c1 e1 19                                     shl       rcx, 0x19
002C5A96  4c 2b e1                                        sub       r12, rcx
002C5A99  49 c1 fc 10                                     sar       r12, 0x10
002C5A9D  49 8b cc                                        mov       rcx, r12
002C5AA0  e8 0b 87 03 00                                  call      0x2fe1b0 ; MI_IS_PHYSICAL_ADDRESS [RVA 0x2FE1B0]
002C5AA5  85 c0                                           test      eax, eax
002C5AA7  0f 85 7b 02 00 00                               jne       0x2c5d28 ; loc_2C5D28 [RVA 0x2C5D28]
002C5AAD  44 8b f5                                        mov       r14d, ebp
002C5AB0  81 fd 00 01 00 00                               cmp       ebp, 0x100
002C5AB6  0f 84 09 03 00 00                               je        0x2c5dc5 ; loc_2C5DC5 [RVA 0x2C5DC5]
002C5ABC  8b c5                                           mov       eax, ebp
002C5ABE  48 89 7c 24 58                                  mov       qword ptr [rsp + 0x58], rdi
002C5AC3  83 e0 05                                        and       eax, 5
002C5AC6  4c 89 6c 24 68                                  mov       qword ptr [rsp + 0x68], r13
002C5ACB  41 83 e6 fe                                     and       r14d, 0xfffffffe
002C5ACF  4c 89 6c 24 48                                  mov       qword ptr [rsp + 0x48], r13
002C5AD4  3c 05                                           cmp       al, 5
002C5AD6  4c 8b c7                                        mov       r8, rdi
002C5AD9  48 8b d6                                        mov       rdx, rsi
002C5ADC  48 8b cb                                        mov       rcx, rbx
002C5ADF  44 0f 45 f5                                     cmovne    r14d, ebp
002C5AE3  45 8b ce                                        mov       r9d, r14d
002C5AE6  41 d1 e9                                        shr       r9d, 1
002C5AE9  41 83 e1 02                                     and       r9d, 2
002C5AED  e8 7e 07 00 00                                  call      0x2c6270 ; MiMakeDriverPagesPrivate [RVA 0x2C6270]
002C5AF2  85 c0                                           test      eax, eax
002C5AF4  0f 88 c4 02 00 00                               js        0x2c5dbe ; loc_2C5DBE [RVA 0x2C5DBE]
002C5AFA  33 ff                                           xor       edi, edi
002C5AFC  48 8d 0d 3d 23 b7 00                            lea       rcx, [rip + 0xb7233d]
002C5B03  48 89 7c 24 38                                  mov       qword ptr [rsp + 0x38], rdi
002C5B08  e8 c3 ae fc ff                                  call      0x2909d0 ; MiLockWorkingSetShared [RVA 0x2909D0]
002C5B0D  88 84 24 d8 00 00 00                            mov       byte ptr [rsp + 0xd8], al
002C5B14  e9 d6 00 00 00                                  jmp       0x2c5bef ; loc_2C5BEF [RVA 0x2C5BEF]
002C5B19  f0 48 0f ba 6b 18 3f                            lock bts  qword ptr [rbx + 0x18], 0x3f
002C5B20  0f 82 82 01 00 00                               jb        0x2c5ca8 ; loc_2C5CA8 [RVA 0x2C5CA8]
002C5B26  48 8b 43 10                                     mov       rax, qword ptr [rbx + 0x10]
002C5B2A  8b 54 24 30                                     mov       edx, dword ptr [rsp + 0x30]
002C5B2E  48 c1 e2 05                                     shl       rdx, 5
002C5B32  48 8b ca                                        mov       rcx, rdx
002C5B35  48 33 c8                                        xor       rcx, rax
002C5B38  48 81 e1 1f fc ff ff                            and       rcx, 0xfffffffffffffc1f
002C5B3F  48 33 ca                                        xor       rcx, rdx
002C5B42  83 7c 24 40 00                                  cmp       dword ptr [rsp + 0x40], 0
002C5B47  48 89 4b 10                                     mov       qword ptr [rbx + 0x10], rcx
002C5B4B  74 0d                                           je        0x2c5b5a ; loc_2C5B5A [RVA 0x2C5B5A]
002C5B4D  ba 04 00 00 00                                  mov       edx, 4
002C5B52  48 8b cb                                        mov       rcx, rbx
002C5B55  e8 7a ee ff ff                                  call      0x2c49d4 ; MiMarkPfnVerified [RVA 0x2C49D4]
002C5B5A  33 c0                                           xor       eax, eax
002C5B5C  48 8b ef                                        mov       rbp, rdi
002C5B5F  83 e5 42                                        and       ebp, 0x42
002C5B62  0f 85 60 01 00 00                               jne       0x2c5cc8 ; loc_2C5CC8 [RVA 0x2C5CC8]
002C5B68  48 b9 ff ff ff ff ff ff ff 7f                   movabs    rcx, 0x7fffffffffffffff
002C5B72  f0 48 21 4b 18                                  lock and  qword ptr [rbx + 0x18], rcx
002C5B77  48 85 c0                                        test      rax, rax
002C5B7A  0f 85 25 06 00 00                               jne       0x2c61a5 ; loc_2C61A5 [RVA 0x2C61A5]
002C5B80  44 8b 44 24 30                                  mov       r8d, dword ptr [rsp + 0x30]
002C5B85  48 8b ce                                        mov       rcx, rsi
002C5B88  48 8b 54 24 60                                  mov       rdx, qword ptr [rsp + 0x60]
002C5B8D  e8 be 19 0a 00                                  call      0x367550 ; MiMakeValidPte [RVA 0x367550]
002C5B92  48 8b d8                                        mov       rbx, rax
002C5B95  8b 44 24 30                                     mov       eax, dword ptr [rsp + 0x30]
002C5B99  a8 04                                           test      al, 4
002C5B9B  0f 85 2d 06 00 00                               jne       0x2c61ce ; loc_2C61CE [RVA 0x2C61CE]
002C5BA1  48 8b d3                                        mov       rdx, rbx
002C5BA4  48 8b ce                                        mov       rcx, rsi
002C5BA7  e8 44 fa fc ff                                  call      0x2955f0 ; MiWriteValidPteNewProtection [RVA 0x2955F0]
002C5BAC  48 8b 05 15 ff cf 00                            mov       rax, qword ptr [rip + 0xcfff15]
002C5BB3  48 0f ba e0 08                                  bt        rax, 8
002C5BB8  72 0b                                           jb        0x2c5bc5 ; loc_2C5BC5 [RVA 0x2C5BC5]
002C5BBA  48 0f ba e0 09                                  bt        rax, 9
002C5BBF  0f 83 7b 01 00 00                               jae       0x2c5d40 ; loc_2C5D40 [RVA 0x2C5D40]
002C5BC5  48 8b d3                                        mov       rdx, rbx
002C5BC8  48 8b cf                                        mov       rcx, rdi
002C5BCB  e8 70 06 00 00                                  call      0x2c6240 ; MI_TIGHTER_PERMISSIONS [RVA 0x2C6240]
002C5BD0  85 c0                                           test      eax, eax
002C5BD2  0f 85 68 01 00 00                               jne       0x2c5d40 ; loc_2C5D40 [RVA 0x2C5D40]
002C5BD8  8b ac 24 e8 00 00 00                            mov       ebp, dword ptr [rsp + 0xe8]
002C5BDF  48 83 c6 08                                     add       rsi, 8
002C5BE3  49 81 c4 00 10 00 00                            add       r12, 0x1000
002C5BEA  48 8b 7c 24 38                                  mov       rdi, qword ptr [rsp + 0x38]
002C5BEF  48 3b b4 24 e0 00 00 00                         cmp       rsi, qword ptr [rsp + 0xe0]
002C5BF7  0f 87 f0 00 00 00                               ja        0x2c5ced ; loc_2C5CED [RVA 0x2C5CED]
002C5BFD  48 85 ff                                        test      rdi, rdi
002C5C00  0f 84 7b 01 00 00                               je        0x2c5d81 ; loc_2C5D81 [RVA 0x2C5D81]
002C5C06  48 f7 c6 ff 0f 00 00                            test      rsi, 0xfff
002C5C0D  0f 84 3f 05 00 00                               je        0x2c6152 ; loc_2C6152 [RVA 0x2C6152]
002C5C13  48 3b 74 24 58                                  cmp       rsi, qword ptr [rsp + 0x58]
002C5C18  0f 87 c4 01 00 00                               ja        0x2c5de2 ; loc_2C5DE2 [RVA 0x2C5DE2]
002C5C1E  83 bc 24 f0 00 00 00 00                         cmp       dword ptr [rsp + 0xf0], 0
002C5C26  48 8b 3e                                        mov       rdi, qword ptr [rsi]
002C5C29  0f 85 56 05 00 00                               jne       0x2c6185 ; loc_2C6185 [RVA 0x2C6185]
002C5C2F  41 8b c6                                        mov       eax, r14d
002C5C32  89 44 24 30                                     mov       dword ptr [rsp + 0x30], eax
002C5C36  41 84 ff                                        test      r15b, dil
002C5C39  0f 84 b7 02 00 00                               je        0x2c5ef6 ; loc_2C5EF6 [RVA 0x2C5EF6]
002C5C3F  48 8b c7                                        mov       rax, rdi
002C5C42  48 b9 ff ff ff ff ff 00 00 00                   movabs    rcx, 0xffffffffff
002C5C4C  48 c1 e8 0c                                     shr       rax, 0xc
002C5C50  48 23 c1                                        and       rax, rcx
002C5C53  48 89 44 24 60                                  mov       qword ptr [rsp + 0x60], rax
002C5C58  48 8d 1c 40                                     lea       rbx, [rax + rax*2]
002C5C5C  48 b8 00 00 00 00 00 de ff ff                   movabs    rax, 0xffffde0000000000
002C5C66  48 c1 e3 04                                     shl       rbx, 4
002C5C6A  48 8b c0                                        mov       rax, rax
002C5C6D  48 03 d8                                        add       rbx, rax
002C5C70  48 8b 43 28                                     mov       rax, qword ptr [rbx + 0x28]
002C5C74  48 85 c0                                        test      rax, rax
002C5C77  0f 88 62 ff ff ff                               js        0x2c5bdf ; loc_2C5BDF [RVA 0x2C5BDF]
002C5C7D  45 33 c0                                        xor       r8d, r8d
002C5C80  48 8b cb                                        mov       rcx, rbx
002C5C83  41 8d 50 02                                     lea       edx, [r8 + 2]
002C5C87  e8 74 35 fd ff                                  call      0x299200 ; MiGetPagePrivilege [RVA 0x299200]
002C5C8C  a8 40                                           test      al, 0x40
002C5C8E  0f 85 01 05 00 00                               jne       0x2c6195 ; loc_2C6195 [RVA 0x2C6195]
002C5C94  48 f7 05 29 fe cf 00 00 80 00 00                test      qword ptr [rip + 0xcffe29], 0x8000
002C5C9F  75 34                                           jne       0x2c5cd5 ; loc_2C5CD5 [RVA 0x2C5CD5]
002C5CA1  33 ed                                           xor       ebp, ebp
002C5CA3  e9 71 fe ff ff                                  jmp       0x2c5b19 ; loc_2C5B19 [RVA 0x2C5B19]
002C5CA8  41 03 ef                                        add       ebp, r15d
002C5CAB  85 2d fb e5 cf 00                               test      dword ptr [rip + 0xcfe5fb], ebp
002C5CB1  0f 84 ef 01 00 00                               je        0x2c5ea6 ; loc_2C5EA6 [RVA 0x2C5EA6]
002C5CB7  f3 90                                           pause
002C5CB9  48 8b 43 18                                     mov       rax, qword ptr [rbx + 0x18]
002C5CBD  48 85 c0                                        test      rax, rax
002C5CC0  0f 89 53 fe ff ff                               jns       0x2c5b19 ; loc_2C5B19 [RVA 0x2C5B19]
002C5CC6  eb e0                                           jmp       0x2c5ca8 ; loc_2C5CA8 [RVA 0x2C5CA8]
002C5CC8  48 8b cb                                        mov       rcx, rbx
002C5CCB  e8 70 b7 fe ff                                  call      0x2b1440 ; MiCaptureDirtyBitToPfn [RVA 0x2B1440]
002C5CD0  e9 93 fe ff ff                                  jmp       0x2c5b68 ; loc_2C5B68 [RVA 0x2C5B68]
002C5CD5  f6 44 24 30 04                                  test      byte ptr [rsp + 0x30], 4
002C5CDA  74 c5                                           je        0x2c5ca1 ; loc_2C5CA1 [RVA 0x2C5CA1]
002C5CDC  48 8b 43 28                                     mov       rax, qword ptr [rbx + 0x28]
002C5CE0  48 c1 e8 3c                                     shr       rax, 0x3c
002C5CE4  24 07                                           and       al, 7
002C5CE6  3c 03                                           cmp       al, 3
002C5CE8  75 b7                                           jne       0x2c5ca1 ; loc_2C5CA1 [RVA 0x2C5CA1]
002C5CEA  45 33 ff                                        xor       r15d, r15d
002C5CED  4d 85 ed                                        test      r13, r13
002C5CF0  74 0d                                           je        0x2c5cff ; loc_2C5CFF [RVA 0x2C5CFF]
002C5CF2  49 8b cd                                        mov       rcx, r13
002C5CF5  e8 d6 6c f6 ff                                  call      0x22c9d0 ; MiFlushTbList [RVA 0x22C9D0]
002C5CFA  e8 81 89 f6 ff                                  call      0x22e680 ; MiReleaseProcessorFlushList [RVA 0x22E680]
002C5CFF  48 8b 54 24 38                                  mov       rdx, qword ptr [rsp + 0x38]
002C5D04  48 85 d2                                        test      rdx, rdx
002C5D07  74 0c                                           je        0x2c5d15 ; loc_2C5D15 [RVA 0x2C5D15]
002C5D09  48 8d 0d 30 21 b7 00                            lea       rcx, [rip + 0xb72130]
002C5D10  e8 eb 92 f5 ff                                  call      0x21f000 ; MiUnlockPageTableInternal/MiUnlockPageTable [RVA 0x21F000]
002C5D15  8a 94 24 d8 00 00 00                            mov       dl, byte ptr [rsp + 0xd8]
002C5D1C  48 8d 0d 1d 21 b7 00                            lea       rcx, [rip + 0xb7211d]
002C5D23  e8 48 b7 fc ff                                  call      0x291470 ; MiUnlockWorkingSetShared [RVA 0x291470]
002C5D28  41 8b c7                                        mov       eax, r15d
002C5D2B  48 81 c4 88 00 00 00                            add       rsp, 0x88
002C5D32  41 5f                                           pop       r15
002C5D34  41 5e                                           pop       r14
002C5D36  41 5d                                           pop       r13
002C5D38  41 5c                                           pop       r12
002C5D3A  5f                                              pop       rdi
002C5D3B  5e                                              pop       rsi
002C5D3C  5d                                              pop       rbp
002C5D3D  5b                                              pop       rbx
002C5D3E  c3                                              ret
002C5D40  4d 85 ed                                        test      r13, r13
002C5D43  75 26                                           jne       0x2c5d6b ; loc_2C5D6B [RVA 0x2C5D6B]
002C5D45  e8 06 f9 03 00                                  call      0x305650 ; MiGetProcessorFlushList [RVA 0x305650]
002C5D4A  45 33 c9                                        xor       r9d, r9d
002C5D4D  c7 44 24 20 20 00 00 00                         mov       dword ptr [rsp + 0x20], 0x20
002C5D55  48 8d 15 e4 20 b7 00                            lea       rdx, [rip + 0xb720e4]
002C5D5C  48 8b c8                                        mov       rcx, rax
002C5D5F  4c 8b e8                                        mov       r13, rax
002C5D62  44 8b 40 0c                                     mov       r8d, dword ptr [rax + 0xc]
002C5D66  e8 75 2f fe ff                                  call      0x2a8ce0 ; MiInitializeTbFlushList [RVA 0x2A8CE0]
002C5D6B  45 33 c9                                        xor       r9d, r9d
002C5D6E  4d 8b c7                                        mov       r8, r15
002C5D71  49 8b d4                                        mov       rdx, r12
002C5D74  49 8b cd                                        mov       rcx, r13
002C5D77  e8 d4 16 fe ff                                  call      0x2a7450 ; MiInsertTbFlushEntry [RVA 0x2A7450]
002C5D7C  e9 57 fe ff ff                                  jmp       0x2c5bd8 ; loc_2C5BD8 [RVA 0x2C5BD8]
002C5D81  48 b8 f8 ff ff ff 7f 00 00 00                   movabs    rax, 0x7ffffffff8
002C5D8B  48 8b fe                                        mov       rdi, rsi
002C5D8E  48 c1 ef 09                                     shr       rdi, 9
002C5D92  48 23 f8                                        and       rdi, rax
002C5D95  48 b8 00 00 00 00 80 f6 ff ff                   movabs    rax, 0xfffff68000000000
002C5D9F  48 8b c0                                        mov       rax, rax
002C5DA2  48 03 f8                                        add       rdi, rax
002C5DA5  48 8d 0d 94 20 b7 00                            lea       rcx, [rip + 0xb72094]
002C5DAC  48 8b d7                                        mov       rdx, rdi
002C5DAF  48 89 7c 24 38                                  mov       qword ptr [rsp + 0x38], rdi
002C5DB4  e8 a7 04 f4 ff                                  call      0x206260 ; MiLockPageTable/MiLockNestedPageTable [RVA 0x206260]
002C5DB9  e9 55 fe ff ff                                  jmp       0x2c5c13 ; loc_2C5C13 [RVA 0x2C5C13]
002C5DBE  33 c0                                           xor       eax, eax
002C5DC0  e9 66 ff ff ff                                  jmp       0x2c5d2b ; loc_2C5D2B [RVA 0x2C5D2B]
002C5DC5  48 8b 4b 70                                     mov       rcx, qword ptr [rbx + 0x70]
002C5DC9  e8 c2 2c fe ff                                  call      0x2a8a90 ; MiSectionControlArea [RVA 0x2A8A90]
002C5DCE  48 89 44 24 68                                  mov       qword ptr [rsp + 0x68], rax
002C5DD3  4c 89 6c 24 48                                  mov       qword ptr [rsp + 0x48], r13
002C5DD8  4c 89 6c 24 58                                  mov       qword ptr [rsp + 0x58], r13
002C5DDD  e9 18 fd ff ff                                  jmp       0x2c5afa ; loc_2C5AFA [RVA 0x2C5AFA]
002C5DE2  48 8b 84 24 d0 00 00 00                         mov       rax, qword ptr [rsp + 0xd0]
002C5DEA  49 8b dc                                        mov       rbx, r12
002C5DED  4c 8b 74 24 48                                  mov       r14, qword ptr [rsp + 0x48]
002C5DF2  48 2b 58 30                                     sub       rbx, qword ptr [rax + 0x30]
002C5DF6  4d 85 f6                                        test      r14, r14
002C5DF9  0f 85 e2 00 00 00                               jne       0x2c5ee1 ; loc_2C5EE1 [RVA 0x2C5EE1]
002C5DFF  4d 85 ed                                        test      r13, r13
002C5E02  0f 85 68 03 00 00                               jne       0x2c6170 ; loc_2C6170 [RVA 0x2C6170]
002C5E08  48 8b d7                                        mov       rdx, rdi
002C5E0B  48 8d 0d 2e 20 b7 00                            lea       rcx, [rip + 0xb7202e]
002C5E12  e8 e9 91 f5 ff                                  call      0x21f000 ; MiUnlockPageTableInternal/MiUnlockPageTable [RVA 0x21F000]
002C5E17  8a 94 24 d8 00 00 00                            mov       dl, byte ptr [rsp + 0xd8]
002C5E1E  48 8d 0d 1b 20 b7 00                            lea       rcx, [rip + 0xb7201b]
002C5E25  e8 46 b6 fc ff                                  call      0x291470 ; MiUnlockWorkingSetShared [RVA 0x291470]
002C5E2A  48 8b 4c 24 68                                  mov       rcx, qword ptr [rsp + 0x68]
002C5E2F  4c 8d 44 24 50                                  lea       r8, [rsp + 0x50]
002C5E34  48 8b d3                                        mov       rdx, rbx
002C5E37  e8 04 a4 04 00                                  call      0x310240 ; MiOffsetToProtos [RVA 0x310240]
002C5E3C  48 8d 0d fd 1f b7 00                            lea       rcx, [rip + 0xb71ffd]
002C5E43  48 89 44 24 48                                  mov       qword ptr [rsp + 0x48], rax
002C5E48  4c 8b f0                                        mov       r14, rax
002C5E4B  e8 80 ab fc ff                                  call      0x2909d0 ; MiLockWorkingSetShared [RVA 0x2909D0]
002C5E50  48 8b d7                                        mov       rdx, rdi
002C5E53  48 8d 0d e6 1f b7 00                            lea       rcx, [rip + 0xb71fe6]
002C5E5A  e8 01 04 f4 ff                                  call      0x206260 ; MiLockPageTable/MiLockNestedPageTable [RVA 0x206260]
002C5E5F  48 8b 54 24 50                                  mov       rdx, qword ptr [rsp + 0x50]
002C5E64  41 8b 46 34                                     mov       eax, dword ptr [r14 + 0x34]
002C5E68  41 8b 4e 2c                                     mov       ecx, dword ptr [r14 + 0x2c]
002C5E6C  25 ff ff ff 3f                                  and       eax, 0x3fffffff
002C5E71  45 8b 76 20                                     mov       r14d, dword ptr [r14 + 0x20]
002C5E75  48 2b c8                                        sub       rcx, rax
002C5E78  48 2b ca                                        sub       rcx, rdx
002C5E7B  41 d1 ee                                        shr       r14d, 1
002C5E7E  41 83 e6 1f                                     and       r14d, 0x1f
002C5E82  48 8d 41 ff                                     lea       rax, [rcx - 1]
002C5E86  48 8d 04 c6                                     lea       rax, [rsi + rax*8]
002C5E8A  48 89 44 24 58                                  mov       qword ptr [rsp + 0x58], rax
002C5E8F  41 8b c6                                        mov       eax, r14d
002C5E92  83 e0 05                                        and       eax, 5
002C5E95  3c 05                                           cmp       al, 5
002C5E97  0f 85 81 fd ff ff                               jne       0x2c5c1e ; loc_2C5C1E [RVA 0x2C5C1E]
002C5E9D  41 83 e6 fe                                     and       r14d, 0xfffffffe
002C5EA1  e9 78 fd ff ff                                  jmp       0x2c5c1e ; loc_2C5C1E [RVA 0x2C5C1E]
002C5EA6  8b 05 4c fc cf 00                               mov       eax, dword ptr [rip + 0xcffc4c]
002C5EAC  a8 40                                           test      al, 0x40
002C5EAE  0f 84 03 fe ff ff                               je        0x2c5cb7 ; loc_2C5CB7 [RVA 0x2C5CB7]
002C5EB4  e8 e7 3f f6 ff                                  call      0x229ea0 ; KiCheckVpBackingLongSpinWaitHypercall [RVA 0x229EA0]
002C5EB9  84 c0                                           test      al, al
002C5EBB  0f 84 f6 fd ff ff                               je        0x2c5cb7 ; loc_2C5CB7 [RVA 0x2C5CB7]
002C5EC1  8b cd                                           mov       ecx, ebp
002C5EC3  e8 a8 3f f6 ff                                  call      0x229e70 ; HvlNotifyLongSpinWait [RVA 0x229E70]
002C5EC8  e9 ec fd ff ff                                  jmp       0x2c5cb9 ; loc_2C5CB9 [RVA 0x2C5CB9]
002C5ECD  83 e5 ef                                        and       ebp, 0xffffffef
002C5ED0  44 89 7c 24 40                                  mov       dword ptr [rsp + 0x40], r15d
002C5ED5  89 ac 24 e8 00 00 00                            mov       dword ptr [rsp + 0xe8], ebp
002C5EDC  e9 9d fb ff ff                                  jmp       0x2c5a7e ; loc_2C5A7E [RVA 0x2C5A7E]
002C5EE1  4d 8b 76 10                                     mov       r14, qword ptr [r14 + 0x10]
002C5EE5  33 d2                                           xor       edx, edx
002C5EE7  4c 89 74 24 48                                  mov       qword ptr [rsp + 0x48], r14
002C5EEC  48 89 54 24 50                                  mov       qword ptr [rsp + 0x50], rdx
002C5EF1  e9 6e ff ff ff                                  jmp       0x2c5e64 ; loc_2C5E64 [RVA 0x2C5E64]
002C5EF6  48 0f ba e7 0a                                  bt        rdi, 0xa
002C5EFB  0f 82 de fc ff ff                               jb        0x2c5bdf ; loc_2C5BDF [RVA 0x2C5BDF]
002C5F01  48 0f ba e7 0b                                  bt        rdi, 0xb
002C5F06  0f 83 f2 02 00 00                               jae       0x2c61fe ; loc_2C61FE [RVA 0x2C61FE]
002C5F0C  48 8b ce                                        mov       rcx, rsi
002C5F0F  e8 6c 07 fd ff                                  call      0x296680 ; MI_READ_PTE_LOCK_FREE [RVA 0x296680]
002C5F14  8b ac 24 e8 00 00 00                            mov       ebp, dword ptr [rsp + 0xe8]
002C5F1B  48 8b d8                                        mov       rbx, rax
002C5F1E  48 8b 7c 24 38                                  mov       rdi, qword ptr [rsp + 0x38]
002C5F23  48 0f ba e0 0b                                  bt        rax, 0xb
002C5F28  0f 83 c1 fc ff ff                               jae       0x2c5bef ; loc_2C5BEF [RVA 0x2C5BEF]
002C5F2E  48 8b c8                                        mov       rcx, rax
002C5F31  e8 4a d1 fe ff                                  call      0x2b3080 ; MiInvalidPteConforms [RVA 0x2B3080]
002C5F36  85 c0                                           test      eax, eax
002C5F38  74 d2                                           je        0x2c5f0c ; loc_2C5F0C [RVA 0x2C5F0C]
002C5F3A  48 8b 05 3f 7d b6 00                            mov       rax, qword ptr [rip + 0xb67d3f]
002C5F41  48 85 c0                                        test      rax, rax
002C5F44  74 12                                           je        0x2c5f58 ; loc_2C5F58 [RVA 0x2C5F58]
002C5F46  f6 c3 10                                        test      bl, 0x10
002C5F49  0f 85 f1 01 00 00                               jne       0x2c6140 ; loc_2C6140 [RVA 0x2C6140]
002C5F4F  48 8b c8                                        mov       rcx, rax
002C5F52  48 f7 d1                                        not       rcx
002C5F55  48 23 cb                                        and       rcx, rbx
002C5F58  48 b8 ff ff ff ff ff 00 00 00                   movabs    rax, 0xffffffffff
002C5F62  48 c1 e9 0c                                     shr       rcx, 0xc
002C5F66  48 23 c8                                        and       rcx, rax
002C5F69  48 b8 00 00 00 00 00 de ff ff                   movabs    rax, 0xffffde0000000000
002C5F73  48 8d 14 49                                     lea       rdx, [rcx + rcx*2]
002C5F77  48 03 d2                                        add       rdx, rdx
002C5F7A  48 8b c0                                        mov       rax, rax
002C5F7D  48 3b 0d 5c 7d b6 00                            cmp       rcx, qword ptr [rip + 0xb67d5c]
002C5F84  48 8d 3c d0                                     lea       rdi, [rax + rdx*8]
002C5F88  77 82                                           ja        0x2c5f0c ; loc_2C5F0C [RVA 0x2C5F0C]
002C5F8A  48 b8 28 00 00 00 00 de ff ff                   movabs    rax, 0xffffde0000000028
002C5F94  48 8b 04 d0                                     mov       rax, qword ptr [rax + rdx*8]
002C5F98  48 c1 e8 36                                     shr       rax, 0x36
002C5F9C  41 85 c7                                        test      r15d, eax
002C5F9F  0f 84 67 ff ff ff                               je        0x2c5f0c ; loc_2C5F0C [RVA 0x2C5F0C]
002C5FA5  33 ed                                           xor       ebp, ebp
002C5FA7  f0 48 0f ba 6f 18 3f                            lock bts  qword ptr [rdi + 0x18], 0x3f
002C5FAE  0f 82 5b 01 00 00                               jb        0x2c610f ; loc_2C610F [RVA 0x2C610F]
002C5FB4  48 8b ce                                        mov       rcx, rsi
002C5FB7  e8 c4 06 fd ff                                  call      0x296680 ; MI_READ_PTE_LOCK_FREE [RVA 0x296680]
002C5FBC  48 3b c3                                        cmp       rax, rbx
002C5FBF  0f 85 08 01 00 00                               jne       0x2c60cd ; loc_2C60CD [RVA 0x2C60CD]
002C5FC5  48 8b 47 28                                     mov       rax, qword ptr [rdi + 0x28]
002C5FC9  48 8b 15 b8 a0 b6 00                            mov       rdx, qword ptr [rip + 0xb6a0b8]
002C5FD0  48 c1 e8 2b                                     shr       rax, 0x2b
002C5FD4  25 ff 03 00 00                                  and       eax, 0x3ff
002C5FD9  48 8b 04 c2                                     mov       rax, qword ptr [rdx + rax*8]
002C5FDD  8b 40 04                                        mov       eax, dword ptr [rax + 4]
002C5FE0  84 c0                                           test      al, al
002C5FE2  79 1d                                           jns       0x2c6001 ; loc_2C6001 [RVA 0x2C6001]
002C5FE4  33 d2                                           xor       edx, edx
002C5FE6  48 8b cf                                        mov       rcx, rdi
002C5FE9  e8 7e e2 0c 00                                  call      0x39426c ; MiLockSpecialPurposeMemoryCachedPage [RVA 0x39426C]
002C5FEE  8b ac 24 e8 00 00 00                            mov       ebp, dword ptr [rsp + 0xe8]
002C5FF5  48 8b f8                                        mov       rdi, rax
002C5FF8  48 85 c0                                        test      rax, rax
002C5FFB  0f 84 e9 fb ff ff                               je        0x2c5bea ; loc_2C5BEA [RVA 0x2C5BEA]
002C6001  48 8b 47 08                                     mov       rax, qword ptr [rdi + 8]
002C6005  48 b9 00 00 00 00 00 00 00 80                   movabs    rcx, 0x8000000000000000
002C600F  48 0b c1                                        or        rax, rcx
002C6012  48 3b c6                                        cmp       rax, rsi
002C6015  0f 85 c6 00 00 00                               jne       0x2c60e1 ; loc_2C60E1 [RVA 0x2C60E1]
002C601B  8b ac 24 e8 00 00 00                            mov       ebp, dword ptr [rsp + 0xe8]
002C6022  48 85 ff                                        test      rdi, rdi
002C6025  48 8b 7c 24 38                                  mov       rdi, qword ptr [rsp + 0x38]
002C602A  0f 84 bf fb ff ff                               je        0x2c5bef ; loc_2C5BEF [RVA 0x2C5BEF]
002C6030  48 8b 0e                                        mov       rcx, qword ptr [rsi]
002C6033  48 8b 15 46 7c b6 00                            mov       rdx, qword ptr [rip + 0xb67c46]
002C603A  48 8b c1                                        mov       rax, rcx
002C603D  48 85 d2                                        test      rdx, rdx
002C6040  74 12                                           je        0x2c6054 ; loc_2C6054 [RVA 0x2C6054]
002C6042  f6 c1 10                                        test      cl, 0x10
002C6045  0f 85 fe 00 00 00                               jne       0x2c6149 ; loc_2C6149 [RVA 0x2C6149]
002C604B  48 8b c2                                        mov       rax, rdx
002C604E  48 f7 d0                                        not       rax
002C6051  48 23 c1                                        and       rax, rcx
002C6054  48 c1 e8 0c                                     shr       rax, 0xc
002C6058  48 ba ff ff ff ff ff 00 00 00                   movabs    rdx, 0xffffffffff
002C6062  48 23 c2                                        and       rax, rdx
002C6065  49 bb 10 00 00 00 00 de ff ff                   movabs    r11, 0xffffde0000000010
002C606F  4c 8d 0c 40                                     lea       r9, [rax + rax*2]
002C6073  4d 03 c9                                        add       r9, r9
002C6076  49 8b c3                                        mov       rax, r11
002C6079  44 8b 44 24 30                                  mov       r8d, dword ptr [rsp + 0x30]
002C607E  49 c7 c2 1f fc ff ff                            mov       r10, 0xfffffffffffffc1f
002C6085  4a 8b 04 c8                                     mov       rax, qword ptr [rax + r9*8]
002C6089  49 c1 e0 05                                     shl       r8, 5
002C608D  49 8b d0                                        mov       rdx, r8
002C6090  48 33 d0                                        xor       rdx, rax
002C6093  49 23 d2                                        and       rdx, r10
002C6096  49 33 d0                                        xor       rdx, r8
002C6099  49 8b c3                                        mov       rax, r11
002C609C  4a 89 14 c8                                     mov       qword ptr [rax + r9*8], rdx
002C60A0  49 8b c0                                        mov       rax, r8
002C60A3  48 33 c1                                        xor       rax, rcx
002C60A6  49 23 c2                                        and       rax, r10
002C60A9  49 33 c0                                        xor       rax, r8
002C60AC  48 89 06                                        mov       qword ptr [rsi], rax
002C60AF  48 b8 18 00 00 00 00 de ff ff                   movabs    rax, 0xffffde0000000018
002C60B9  48 b9 ff ff ff ff ff ff ff 7f                   movabs    rcx, 0x7fffffffffffffff
002C60C3  f0 4a 21 0c c8                                  lock and  qword ptr [rax + r9*8], rcx
002C60C8  e9 0b fb ff ff                                  jmp       0x2c5bd8 ; loc_2C5BD8 [RVA 0x2C5BD8]
002C60CD  48 b8 ff ff ff ff ff ff ff 7f                   movabs    rax, 0x7fffffffffffffff
002C60D7  f0 48 21 47 18                                  lock and  qword ptr [rdi + 0x18], rax
002C60DC  e9 2b fe ff ff                                  jmp       0x2c5f0c ; loc_2C5F0C [RVA 0x2C5F0C]
002C60E1  48 8b 47 28                                     mov       rax, qword ptr [rdi + 0x28]
002C60E5  48 0f ba e0 28                                  bt        rax, 0x28
002C60EA  0f 82 2b ff ff ff                               jb        0x2c601b ; loc_2C601B [RVA 0x2C601B]
002C60F0  48 8b 47 08                                     mov       rax, qword ptr [rdi + 8]
002C60F4  4c 8b cb                                        mov       r9, rbx
002C60F7  4c 8b c6                                        mov       r8, rsi
002C60FA  48 89 44 24 20                                  mov       qword ptr [rsp + 0x20], rax
002C60FF  ba 11 04 00 00                                  mov       edx, 0x411
002C6104  b9 1a 00 00 00                                  mov       ecx, 0x1a
002C6109  e8 f2 74 23 00                                  call      0x4fd600 ; KeBugCheckEx [RVA 0x4FD600]
002C610E  cc                                              int3
002C610F  41 03 ef                                        add       ebp, r15d
002C6112  85 2d 94 e1 cf 00                               test      dword ptr [rip + 0xcfe194], ebp
002C6118  0f 84 cd 00 00 00                               je        0x2c61eb ; loc_2C61EB [RVA 0x2C61EB]
002C611E  f3 90                                           pause
002C6120  eb 10                                           jmp       0x2c6132 ; loc_2C6132 [RVA 0x2C6132]
002C6122  e8 79 3d f6 ff                                  call      0x229ea0 ; KiCheckVpBackingLongSpinWaitHypercall [RVA 0x229EA0]
002C6127  84 c0                                           test      al, al
002C6129  74 f3                                           je        0x2c611e ; loc_2C611E [RVA 0x2C611E]
002C612B  8b cd                                           mov       ecx, ebp
002C612D  e8 3e 3d f6 ff                                  call      0x229e70 ; HvlNotifyLongSpinWait [RVA 0x229E70]
002C6132  48 8b 47 18                                     mov       rax, qword ptr [rdi + 0x18]
002C6136  48 85 c0                                        test      rax, rax
002C6139  78 d4                                           js        0x2c610f ; loc_2C610F [RVA 0x2C610F]
002C613B  e9 67 fe ff ff                                  jmp       0x2c5fa7 ; loc_2C5FA7 [RVA 0x2C5FA7]
002C6140  48 83 e1 ef                                     and       rcx, 0xffffffffffffffef
002C6144  e9 0f fe ff ff                                  jmp       0x2c5f58 ; loc_2C5F58 [RVA 0x2C5F58]
002C6149  48 83 e0 ef                                     and       rax, 0xffffffffffffffef
002C614D  e9 02 ff ff ff                                  jmp       0x2c6054 ; loc_2C6054 [RVA 0x2C6054]
002C6152  4d 85 ed                                        test      r13, r13
002C6155  0f 84 43 e7 40 00                               je        0x6d489e ; loc_6D489E [RVA 0x6D489E]
002C615B  49 8b cd                                        mov       rcx, r13
002C615E  e8 6d 68 f6 ff                                  call      0x22c9d0 ; MiFlushTbList [RVA 0x22C9D0]
002C6163  e8 18 85 f6 ff                                  call      0x22e680 ; MiReleaseProcessorFlushList [RVA 0x22E680]
002C6168  45 33 ed                                        xor       r13d, r13d
002C616B  e9 2e e7 40 00                                  jmp       0x6d489e ; loc_6D489E [RVA 0x6D489E]
002C6170  49 8b cd                                        mov       rcx, r13
002C6173  e8 58 68 f6 ff                                  call      0x22c9d0 ; MiFlushTbList [RVA 0x22C9D0]
002C6178  e8 03 85 f6 ff                                  call      0x22e680 ; MiReleaseProcessorFlushList [RVA 0x22E680]
002C617D  45 33 ed                                        xor       r13d, r13d
002C6180  e9 83 fc ff ff                                  jmp       0x2c5e08 ; loc_2C5E08 [RVA 0x2C5E08]
002C6185  41 8b d6                                        mov       edx, r14d
002C6188  48 8b cf                                        mov       rcx, rdi
002C618B  e8 58 70 23 00                                  call      0x4fd1e8 ; MiRestrictSystemCodeProtection [RVA 0x4FD1E8]
002C6190  e9 9d fa ff ff                                  jmp       0x2c5c32 ; loc_2C5C32 [RVA 0x2C5C32]
002C6195  f6 44 24 30 06                                  test      byte ptr [rsp + 0x30], 6
002C619A  0f 85 4a fb ff ff                               jne       0x2c5cea ; loc_2C5CEA [RVA 0x2C5CEA]
002C61A0  e9 fc fa ff ff                                  jmp       0x2c5ca1 ; loc_2C5CA1 [RVA 0x2C5CA1]
002C61A5  48 8b 4b 28                                     mov       rcx, qword ptr [rbx + 0x28]
002C61A9  48 8b d0                                        mov       rdx, rax
002C61AC  48 8b 05 d5 9e b6 00                            mov       rax, qword ptr [rip + 0xb69ed5]
002C61B3  45 8b c7                                        mov       r8d, r15d
002C61B6  48 c1 e9 2b                                     shr       rcx, 0x2b
002C61BA  81 e1 ff 03 00 00                               and       ecx, 0x3ff
002C61C0  48 8b 0c c8                                     mov       rcx, qword ptr [rax + rcx*8]
002C61C4  e8 e7 1b fd ff                                  call      0x297db0 ; MiReleasePageFileInfo [RVA 0x297DB0]
002C61C9  e9 b2 f9 ff ff                                  jmp       0x2c5b80 ; loc_2C5B80 [RVA 0x2C5B80]
002C61CE  48 85 ed                                        test      rbp, rbp
002C61D1  0f 95 c1                                        setne     cl
002C61D4  41 84 c7                                        test      r15b, al
002C61D7  0f 94 c0                                        sete      al
002C61DA  84 c8                                           test      al, cl
002C61DC  0f 84 bf f9 ff ff                               je        0x2c5ba1 ; loc_2C5BA1 [RVA 0x2C5BA1]
002C61E2  48 83 cb 42                                     or        rbx, 0x42
002C61E6  e9 b6 f9 ff ff                                  jmp       0x2c5ba1 ; loc_2C5BA1 [RVA 0x2C5BA1]
002C61EB  8b 05 07 f9 cf 00                               mov       eax, dword ptr [rip + 0xcff907]
002C61F1  a8 40                                           test      al, 0x40
002C61F3  0f 84 25 ff ff ff                               je        0x2c611e ; loc_2C611E [RVA 0x2C611E]
002C61F9  e9 24 ff ff ff                                  jmp       0x2c6122 ; loc_2C6122 [RVA 0x2C6122]
002C61FE  48 85 ff                                        test      rdi, rdi
002C6201  0f 84 d8 f9 ff ff                               je        0x2c5bdf ; loc_2C5BDF [RVA 0x2C5BDF]
002C6207  81 fd 00 01 00 00                               cmp       ebp, 0x100
002C620D  0f 84 cc f9 ff ff                               je        0x2c5bdf ; loc_2C5BDF [RVA 0x2C5BDF]
002C6213  8b c8                                           mov       ecx, eax
002C6215  48 c1 e1 05                                     shl       rcx, 5
002C6219  48 8b c1                                        mov       rax, rcx
002C621C  48 33 c7                                        xor       rax, rdi
002C621F  48 25 1f fc ff ff                               and       rax, 0xfffffffffffffc1f
002C6225  48 33 c1                                        xor       rax, rcx
002C6228  48 89 06                                        mov       qword ptr [rsi], rax
002C622B  e9 af f9 ff ff                                  jmp       0x2c5bdf ; loc_2C5BDF [RVA 0x2C5BDF]
006D489E  48 8b d7                                        mov       rdx, rdi
006D48A1  48 8d 0d 98 35 76 00                            lea       rcx, [rip + 0x763598]
006D48A8  e8 53 a7 b4 ff                                  call      0x21f000 ; MiUnlockPageTableInternal/MiUnlockPageTable [RVA 0x21F000]
006D48AD  90                                              nop
006D48AE  e9 ce 14 bf ff                                  jmp       0x2c5d81 ; loc_2C5D81 [RVA 0x2C5D81]
