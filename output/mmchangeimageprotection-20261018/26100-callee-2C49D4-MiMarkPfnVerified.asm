MiMarkPfnVerified at RVA 0x2C49D4
extent source: next public symbol (first .pdata end 0x2C4B24); RVA 0x2C49D4..0x2C4B2C (exclusive)
shown: 97 instruction(s), max 200
All addresses are RVAs.

002C49D4  48 8b c4                                        mov       rax, rsp
002C49D7  48 89 58 08                                     mov       qword ptr [rax + 8], rbx
002C49DB  48 89 68 10                                     mov       qword ptr [rax + 0x10], rbp
002C49DF  48 89 70 18                                     mov       qword ptr [rax + 0x18], rsi
002C49E3  48 89 78 20                                     mov       qword ptr [rax + 0x20], rdi
002C49E7  41 56                                           push      r14
002C49E9  48 83 ec 40                                     sub       rsp, 0x40
002C49ED  0f 57 c0                                        xorps     xmm0, xmm0
002C49F0  8b fa                                           mov       edi, edx
002C49F2  0f 11 40 e8                                     movups    xmmword ptr [rax - 0x18], xmm0
002C49F6  48 8b d9                                        mov       rbx, rcx
002C49F9  48 b8 00 00 00 00 00 de ff ff                   movabs    rax, 0xffffde0000000000
002C4A03  48 8b e9                                        mov       rbp, rcx
002C4A06  41 b6 11                                        mov       r14b, 0x11
002C4A09  48 2b e8                                        sub       rbp, rax
002C4A0C  48 b8 ab aa aa aa aa aa aa aa                   movabs    rax, 0xaaaaaaaaaaaaaaab
002C4A16  48 c1 fd 04                                     sar       rbp, 4
002C4A1A  48 0f af e8                                     imul      rbp, rax
002C4A1E  f6 c2 04                                        test      dl, 4
002C4A21  74 5c                                           je        0x2c4a7f ; loc_2C4A7F [RVA 0x2C4A7F]
002C4A23  40 f6 c7 02                                     test      dil, 2
002C4A27  0f 85 a0 00 00 00                               jne       0x2c4acd ; loc_2C4ACD [RVA 0x2C4ACD]
002C4A2D  48 8b 43 28                                     mov       rax, qword ptr [rbx + 0x28]
002C4A31  48 c1 e8 3c                                     shr       rax, 0x3c
002C4A35  24 07                                           and       al, 7
002C4A37  3c 03                                           cmp       al, 3
002C4A39  74 0d                                           je        0x2c4a48 ; loc_2C4A48 [RVA 0x2C4A48]
002C4A3B  ba 03 00 00 00                                  mov       edx, 3
002C4A40  48 8b cb                                        mov       rcx, rbx
002C4A43  e8 a8 0e f4 ff                                  call      0x2058f0 ; MiSetPfnIdentity [RVA 0x2058F0]
002C4A48  40 f6 c7 01                                     test      dil, 1
002C4A4C  0f 85 97 00 00 00                               jne       0x2c4ae9 ; loc_2C4AE9 [RVA 0x2C4AE9]
002C4A52  40 f6 c7 04                                     test      dil, 4
002C4A56  75 0b                                           jne       0x2c4a63 ; loc_2C4A63 [RVA 0x2C4A63]
002C4A58  41 8a d6                                        mov       dl, r14b
002C4A5B  48 8b cb                                        mov       rcx, rbx
002C4A5E  e8 cd 02 f7 ff                                  call      0x234d30 ; MiUnlockPageInline/MiUnlockPage [RVA 0x234D30]
002C4A63  48 8b 5c 24 50                                  mov       rbx, qword ptr [rsp + 0x50]
002C4A68  48 8b 6c 24 58                                  mov       rbp, qword ptr [rsp + 0x58]
002C4A6D  48 8b 74 24 60                                  mov       rsi, qword ptr [rsp + 0x60]
002C4A72  48 8b 7c 24 68                                  mov       rdi, qword ptr [rsp + 0x68]
002C4A77  48 83 c4 40                                     add       rsp, 0x40
002C4A7B  41 5e                                           pop       r14
002C4A7D  c3                                              ret
002C4A7E  cc                                              int3
002C4A7F  40 f6 c7 40                                     test      dil, 0x40
002C4A83  75 0a                                           jne       0x2c4a8f ; loc_2C4A8F [RVA 0x2C4A8F]
002C4A85  e8 06 02 f7 ff                                  call      0x234c90 ; MiLockPageInline [RVA 0x234C90]
002C4A8A  44 8a f0                                        mov       r14b, al
002C4A8D  eb 94                                           jmp       0x2c4a23 ; loc_2C4A23 [RVA 0x2C4A23]
002C4A8F  33 f6                                           xor       esi, esi
002C4A91  f0 48 0f ba 6b 18 3f                            lock bts  qword ptr [rbx + 0x18], 0x3f
002C4A98  73 89                                           jae       0x2c4a23 ; loc_2C4A23 [RVA 0x2C4A23]
002C4A9A  ff c6                                           inc       esi
002C4A9C  85 35 0a f8 cf 00                               test      dword ptr [rip + 0xcff80a], esi
002C4AA2  74 0d                                           je        0x2c4ab1 ; loc_2C4AB1 [RVA 0x2C4AB1]
002C4AA4  f3 90                                           pause
002C4AA6  48 8b 43 18                                     mov       rax, qword ptr [rbx + 0x18]
002C4AAA  48 85 c0                                        test      rax, rax
002C4AAD  79 e2                                           jns       0x2c4a91 ; loc_2C4A91 [RVA 0x2C4A91]
002C4AAF  eb e9                                           jmp       0x2c4a9a ; loc_2C4A9A [RVA 0x2C4A9A]
002C4AB1  8b 05 41 10 d0 00                               mov       eax, dword ptr [rip + 0xd01041]
002C4AB7  a8 40                                           test      al, 0x40
002C4AB9  74 e9                                           je        0x2c4aa4 ; loc_2C4AA4 [RVA 0x2C4AA4]
002C4ABB  e8 e0 53 f6 ff                                  call      0x229ea0 ; KiCheckVpBackingLongSpinWaitHypercall [RVA 0x229EA0]
002C4AC0  84 c0                                           test      al, al
002C4AC2  74 e0                                           je        0x2c4aa4 ; loc_2C4AA4 [RVA 0x2C4AA4]
002C4AC4  8b ce                                           mov       ecx, esi
002C4AC6  e8 a5 53 f6 ff                                  call      0x229e70 ; HvlNotifyLongSpinWait [RVA 0x229E70]
002C4ACB  eb d9                                           jmp       0x2c4aa6 ; loc_2C4AA6 [RVA 0x2C4AA6]
002C4ACD  48 8b 4b 28                                     mov       rcx, qword ptr [rbx + 0x28]
002C4AD1  48 c1 e9 3c                                     shr       rcx, 0x3c
002C4AD5  80 e1 07                                        and       cl, 7
002C4AD8  80 f9 03                                        cmp       cl, 3
002C4ADB  0f 85 97 fc 40 00                               jne       0x6d4778 ; sub_6D4778 [RVA 0x6D4778]
002C4AE1  83 e7 fd                                        and       edi, 0xfffffffd
002C4AE4  e9 8f fc 40 00                                  jmp       0x6d4778 ; sub_6D4778 [RVA 0x6D4778]
002C4AE9  48 0f ba 25 d6 0f d0 00 0f                      bt        qword ptr [rip + 0xd00fd6], 0xf
002C4AF2  0f 83 5a ff ff ff                               jae       0x2c4a52 ; loc_2C4A52 [RVA 0x2C4A52]
002C4AF8  4c 8d 44 24 30                                  lea       r8, [rsp + 0x30]
002C4AFD  ba 01 00 00 00                                  mov       edx, 1
002C4B02  48 8b cb                                        mov       rcx, rbx
002C4B05  e8 f6 46 fd ff                                  call      0x299200 ; MiGetPagePrivilege [RVA 0x299200]
002C4B0A  41 b9 04 00 00 00                               mov       r9d, 4
002C4B10  4c 8d 44 24 30                                  lea       r8, [rsp + 0x30]
002C4B15  33 d2                                           xor       edx, edx
002C4B17  48 8b cd                                        mov       rcx, rbp
002C4B1A  e8 75 7b 06 00                                  call      0x32c694 ; KeSetPagePrivilege [RVA 0x32C694]
002C4B1F  e9 2e ff ff ff                                  jmp       0x2c4a52 ; loc_2C4A52 [RVA 0x2C4A52]
002C4B24  cc                                              int3
002C4B25  cc                                              int3
002C4B26  cc                                              int3
002C4B27  cc                                              int3
002C4B28  cc                                              int3
002C4B29  cc                                              int3
002C4B2A  cc                                              int3
002C4B2B  cc                                              int3
