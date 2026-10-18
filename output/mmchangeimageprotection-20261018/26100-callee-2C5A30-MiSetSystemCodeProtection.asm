MiSetSystemCodeProtection at RVA 0x2C5A30
extent source: next public symbol (first .pdata end 0x2C6230); RVA 0x2C5A30..0x2C6240 (exclusive)
shown: 200 instruction(s), max 200
All addresses are RVAs.

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
002C5D3F  cc                                              int3
002C5D40  4d 85 ed                                        test      r13, r13
002C5D43  75 26                                           jne       0x2c5d6b ; loc_2C5D6B [RVA 0x2C5D6B]
002C5D45  e8 06 f9 03 00                                  call      0x305650 ; MiGetProcessorFlushList [RVA 0x305650]
002C5D4A  45 33 c9                                        xor       r9d, r9d
002C5D4D  c7 44 24 20 20 00 00 00                         mov       dword ptr [rsp + 0x20], 0x20
002C5D55  48 8d 15 e4 20 b7 00                            lea       rdx, [rip + 0xb720e4]
002C5D5C  48 8b c8                                        mov       rcx, rax
002C5D5F  4c 8b e8                                        mov       r13, rax
