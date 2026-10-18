VslValidateDynamicCodePages at RVA 0xA6258C
extent source: next public symbol (first .pdata end 0xA6273A); RVA 0xA6258C..0xA62740 (exclusive)
shown: 100 instruction(s), max 200
All addresses are RVAs.

00A6258C  48 89 5c 24 20                                  mov       qword ptr [rsp + 0x20], rbx
00A62591  56                                              push      rsi
00A62592  57                                              push      rdi
00A62593  41 56                                           push      r14
00A62595  48 81 ec 30 01 00 00                            sub       rsp, 0x130
00A6259C  48 8b 05 9d 81 3a 00                            mov       rax, qword ptr [rip + 0x3a819d]
00A625A3  48 33 c4                                        xor       rax, rsp
00A625A6  48 89 84 24 28 01 00 00                         mov       qword ptr [rsp + 0x128], rax
00A625AE  41 8b f0                                        mov       esi, r8d
00A625B1  48 8b da                                        mov       rbx, rdx
00A625B4  48 8b f9                                        mov       rdi, rcx
00A625B7  33 d2                                           xor       edx, edx
00A625B9  44 8d 42 68                                     lea       r8d, [rdx + 0x68]
00A625BD  48 8d 8c 24 80 00 00 00                         lea       rcx, [rsp + 0x80]
00A625C5  e8 76 e1 c5 ff                                  call      0x6c0740 ; memset [RVA 0x6C0740]
00A625CA  0f 57 c0                                        xorps     xmm0, xmm0
00A625CD  f3 0f 7f 84 24 fc 00 00 00                      movdqu    xmmword ptr [rsp + 0xfc], xmm0
00A625D6  45 33 f6                                        xor       r14d, r14d
00A625D9  44 89 b4 24 0c 01 00 00                         mov       dword ptr [rsp + 0x10c], r14d
00A625E1  4c 89 b4 24 20 01 00 00                         mov       qword ptr [rsp + 0x120], r14
00A625E9  33 d2                                           xor       edx, edx
00A625EB  45 8d 46 48                                     lea       r8d, [r14 + 0x48]
00A625EF  48 8d 4c 24 30                                  lea       rcx, [rsp + 0x30]
00A625F4  e8 47 e1 c5 ff                                  call      0x6c0740 ; memset [RVA 0x6C0740]
00A625F9  8b 57 2c                                        mov       edx, dword ptr [rdi + 0x2c]
00A625FC  03 57 28                                        add       edx, dword ptr [rdi + 0x28]
00A625FF  48 8b 4f 20                                     mov       rcx, qword ptr [rdi + 0x20]
00A62603  e8 88 26 a0 ff                                  call      0x464c90 ; MmSizeOfMdl [RVA 0x464C90]
00A62608  4c 8b c8                                        mov       r9, rax
00A6260B  44 8b c7                                        mov       r8d, edi
00A6260E  41 8b d0                                        mov       edx, r8d
00A62611  41 ba ff 0f 00 00                               mov       r10d, 0xfff
00A62617  49 23 d2                                        and       rdx, r10
00A6261A  49 03 d2                                        add       rdx, r10
00A6261D  48 03 d0                                        add       rdx, rax
00A62620  48 c1 ea 0c                                     shr       rdx, 0xc
00A62624  48 83 fa 01                                     cmp       rdx, 1
00A62628  0f 87 05 01 00 00                               ja        0xa62733 ; loc_A62733 [RVA 0xA62733]
00A6262E  4c 89 b4 24 f0 00 00 00                         mov       qword ptr [rsp + 0xf0], r14
00A62636  66 83 c2 06                                     add       dx, 6
00A6263A  66 c1 e2 03                                     shl       dx, 3
00A6263E  66 89 94 24 f8 00 00 00                         mov       word ptr [rsp + 0xf8], dx
00A62646  66 44 89 b4 24 fa 00 00 00                      mov       word ptr [rsp + 0xfa], r14w
00A6264F  48 8b c7                                        mov       rax, rdi
00A62652  48 25 00 f0 ff ff                               and       rax, 0xfffffffffffff000
00A62658  48 89 84 24 10 01 00 00                         mov       qword ptr [rsp + 0x110], rax
00A62660  45 23 c2                                        and       r8d, r10d
00A62663  44 89 84 24 1c 01 00 00                         mov       dword ptr [rsp + 0x11c], r8d
00A6266B  44 89 8c 24 18 01 00 00                         mov       dword ptr [rsp + 0x118], r9d
00A62673  45 33 c0                                        xor       r8d, r8d
00A62676  33 d2                                           xor       edx, edx
00A62678  48 8d 8c 24 f0 00 00 00                         lea       rcx, [rsp + 0xf0]
00A62680  e8 1b c1 89 ff                                  call      0x2fe7a0 ; MmProbeAndLockPages [RVA 0x2FE7A0]
00A62685  eb 05                                           jmp       0xa6268c ; loc_A6268C [RVA 0xA6268C]
00A62687  e9 82 00 00 00                                  jmp       0xa6270e ; loc_A6270E [RVA 0xA6270E]
00A6268C  44 89 74 24 20                                  mov       dword ptr [rsp + 0x20], r14d
00A62691  45 33 c9                                        xor       r9d, r9d
00A62694  44 8b c6                                        mov       r8d, esi
00A62697  48 8b d3                                        mov       rdx, rbx
00A6269A  48 8d 4c 24 30                                  lea       rcx, [rsp + 0x30]
00A6269F  e8 68 8c 7a ff                                  call      0x20b30c ; VslpLockPagesForTransfer [RVA 0x20B30C]
00A626A4  8b d8                                           mov       ebx, eax
00A626A6  85 c0                                           test      eax, eax
00A626A8  78 55                                           js        0xa626ff ; loc_A626FF [RVA 0xA626FF]
00A626AA  48 89 bc 24 88 00 00 00                         mov       qword ptr [rsp + 0x88], rdi
00A626B2  48 8b 84 24 20 01 00 00                         mov       rax, qword ptr [rsp + 0x120]
00A626BA  48 89 84 24 90 00 00 00                         mov       qword ptr [rsp + 0x90], rax
00A626C2  48 8b 44 24 30                                  mov       rax, qword ptr [rsp + 0x30]
00A626C7  48 89 84 24 98 00 00 00                         mov       qword ptr [rsp + 0x98], rax
00A626CF  48 8b 44 24 68                                  mov       rax, qword ptr [rsp + 0x68]
00A626D4  48 89 84 24 a0 00 00 00                         mov       qword ptr [rsp + 0xa0], rax
00A626DC  ba 20 00 00 00                                  mov       edx, 0x20
00A626E1  4c 8d 8c 24 80 00 00 00                         lea       r9, [rsp + 0x80]
00A626E9  45 33 c0                                        xor       r8d, r8d
00A626EC  b1 02                                           mov       cl, 2
00A626EE  e8 6d 65 89 ff                                  call      0x2f8c60 ; VslpEnterIumSecureMode [RVA 0x2F8C60]
00A626F3  8b d8                                           mov       ebx, eax
00A626F5  48 8d 4c 24 30                                  lea       rcx, [rsp + 0x30]
00A626FA  e8 b9 94 7a ff                                  call      0x20bbb8 ; VslpUnlockPagesForTransfer [RVA 0x20BBB8]
00A626FF  48 8d 8c 24 f0 00 00 00                         lea       rcx, [rsp + 0xf0]
00A62707  e8 44 95 7a ff                                  call      0x20bc50 ; MmUnlockPages [RVA 0x20BC50]
00A6270C  8b c3                                           mov       eax, ebx
00A6270E  48 8b 8c 24 28 01 00 00                         mov       rcx, qword ptr [rsp + 0x128]
00A62716  48 33 cc                                        xor       rcx, rsp
00A62719  e8 72 39 c4 ff                                  call      0x6a6090 ; _security_check_cookie [RVA 0x6A6090]
00A6271E  48 8b 9c 24 68 01 00 00                         mov       rbx, qword ptr [rsp + 0x168]
00A62726  48 81 c4 30 01 00 00                            add       rsp, 0x130
00A6272D  41 5e                                           pop       r14
00A6272F  5f                                              pop       rdi
00A62730  5e                                              pop       rsi
00A62731  c3                                              ret
00A62732  cc                                              int3
00A62733  b8 0d 00 00 c0                                  mov       eax, 0xc000000d
00A62738  eb d4                                           jmp       0xa6270e ; loc_A6270E [RVA 0xA6270E]
00A6273A  cc                                              int3
00A6273B  cc                                              int3
00A6273C  cc                                              int3
00A6273D  cc                                              int3
00A6273E  cc                                              int3
00A6273F  cc                                              int3
