VslpLockPagesForTransfer at RVA 0x20B30C
extent source: next public symbol (first .pdata end 0x20B603); RVA 0x20B30C..0x20B60C (exclusive)
shown: 189 instruction(s), max 200
All addresses are RVAs.

0020B30C  48 8b c4                                        mov       rax, rsp
0020B30F  44 89 48 20                                     mov       dword ptr [rax + 0x20], r9d
0020B313  44 89 40 18                                     mov       dword ptr [rax + 0x18], r8d
0020B317  48 89 50 10                                     mov       qword ptr [rax + 0x10], rdx
0020B31B  48 89 48 08                                     mov       qword ptr [rax + 8], rcx
0020B31F  53                                              push      rbx
0020B320  56                                              push      rsi
0020B321  57                                              push      rdi
0020B322  41 54                                           push      r12
0020B324  41 55                                           push      r13
0020B326  41 56                                           push      r14
0020B328  41 57                                           push      r15
0020B32A  48 83 ec 30                                     sub       rsp, 0x30
0020B32E  45 8b d9                                        mov       r11d, r9d
0020B331  45 8b c8                                        mov       r9d, r8d
0020B334  4c 8b d2                                        mov       r10, rdx
0020B337  33 f6                                           xor       esi, esi
0020B339  8b fe                                           mov       edi, esi
0020B33B  44 8b b4 24 90 00 00 00                         mov       r14d, dword ptr [rsp + 0x90]
0020B343  41 f6 c6 13                                     test      r14b, 0x13
0020B347  0f 85 53 01 00 00                               jne       0x20b4a0 ; loc_20B4A0 [RVA 0x20B4A0]
0020B34D  4c 8b fe                                        mov       r15, rsi
0020B350  48 89 74 24 28                                  mov       qword ptr [rsp + 0x28], rsi
0020B355  ba 00 10 00 00                                  mov       edx, 0x1000
0020B35A  48 83 cb ff                                     or        rbx, 0xffffffffffffffff
0020B35E  49 8b ca                                        mov       rcx, r10
0020B361  81 e1 ff 0f 00 00                               and       ecx, 0xfff
0020B367  41 8b c1                                        mov       eax, r9d
0020B36A  48 81 c1 ff 0f 00 00                            add       rcx, 0xfff
0020B371  48 03 c8                                        add       rcx, rax
0020B374  48 c1 e9 0c                                     shr       rcx, 0xc
0020B378  44 8d 2c cd 30 00 00 00                         lea       r13d, [rcx*8 + 0x30]
0020B380  41 f6 c6 01                                     test      r14b, 1
0020B384  44 0f 45 ea                                     cmovne    r13d, edx
0020B388  41 f6 c6 04                                     test      r14b, 4
0020B38C  0f 85 39 02 00 00                               jne       0x20b5cb ; loc_20B5CB [RVA 0x20B5CB]
0020B392  41 8b c6                                        mov       eax, r14d
0020B395  83 e0 20                                        and       eax, 0x20
0020B398  0f 85 77 01 00 00                               jne       0x20b515 ; loc_20B515 [RVA 0x20B515]
0020B39E  41 8b d5                                        mov       edx, r13d
0020B3A1  8d 48 40                                        lea       ecx, [rax + 0x40]
0020B3A4  41 b8 56 6d 73 54                               mov       r8d, 0x54736d56
0020B3AA  e8 41 1d 97 00                                  call      0xb7d0f0 ; ExAllocatePool2 [RVA 0xB7D0F0]
0020B3AF  48 8b f8                                        mov       rdi, rax
0020B3B2  44 8b 8c 24 80 00 00 00                         mov       r9d, dword ptr [rsp + 0x80]
0020B3BA  4c 8b 54 24 78                                  mov       r10, qword ptr [rsp + 0x78]
0020B3BF  44 8b 9c 24 88 00 00 00                         mov       r11d, dword ptr [rsp + 0x88]
0020B3C7  48 89 7c 24 20                                  mov       qword ptr [rsp + 0x20], rdi
0020B3CC  48 85 ff                                        test      rdi, rdi
0020B3CF  0f 84 09 02 00 00                               je        0x20b5de ; loc_20B5DE [RVA 0x20B5DE]
0020B3D5  41 8b d1                                        mov       edx, r9d
0020B3D8  48 89 37                                        mov       qword ptr [rdi], rsi
0020B3DB  45 8b c2                                        mov       r8d, r10d
0020B3DE  41 8b c0                                        mov       eax, r8d
0020B3E1  25 ff 0f 00 00                                  and       eax, 0xfff
0020B3E6  48 05 ff 0f 00 00                               add       rax, 0xfff
0020B3EC  48 03 d0                                        add       rdx, rax
0020B3EF  48 c1 ea 0c                                     shr       rdx, 0xc
0020B3F3  66 83 c2 06                                     add       dx, 6
0020B3F7  66 c1 e2 03                                     shl       dx, 3
0020B3FB  66 89 57 08                                     mov       word ptr [rdi + 8], dx
0020B3FF  66 89 77 0a                                     mov       word ptr [rdi + 0xa], si
0020B403  49 8b c2                                        mov       rax, r10
0020B406  48 25 00 f0 ff ff                               and       rax, 0xfffffffffffff000
0020B40C  48 89 47 20                                     mov       qword ptr [rdi + 0x20], rax
0020B410  41 81 e0 ff 0f 00 00                            and       r8d, 0xfff
0020B417  44 89 47 2c                                     mov       dword ptr [rdi + 0x2c], r8d
0020B41B  44 89 4f 28                                     mov       dword ptr [rdi + 0x28], r9d
0020B41F  41 f6 c6 06                                     test      r14b, 6
0020B423  0f 85 df 00 00 00                               jne       0x20b508 ; loc_20B508 [RVA 0x20B508]
0020B429  45 85 c9                                        test      r9d, r9d
0020B42C  74 32                                           je        0x20b460 ; loc_20B460 [RVA 0x20B460]
0020B42E  45 8b c3                                        mov       r8d, r11d
0020B431  33 d2                                           xor       edx, edx
0020B433  48 8b cf                                        mov       rcx, rdi
0020B436  e8 65 33 0f 00                                  call      0x2fe7a0 ; MmProbeAndLockPages [RVA 0x2FE7A0]
0020B43B  eb 23                                           jmp       0x20b460 ; loc_20B460 [RVA 0x20B460]
0020B43D  44 8b f8                                        mov       r15d, eax
0020B440  48 8b 44 24 28                                  mov       rax, qword ptr [rsp + 0x28]
0020B445  48 8b 7c 24 20                                  mov       rdi, qword ptr [rsp + 0x20]
0020B44A  48 89 07                                        mov       qword ptr [rdi], rax
0020B44D  33 f6                                           xor       esi, esi
0020B44F  48 83 cb ff                                     or        rbx, 0xffffffffffffffff
0020B453  44 8b b4 24 90 00 00 00                         mov       r14d, dword ptr [rsp + 0x90]
0020B45B  e9 91 01 00 00                                  jmp       0x20b5f1 ; loc_20B5F1 [RVA 0x20B5F1]
0020B460  45 8b e6                                        mov       r12d, r14d
0020B463  41 83 e4 10                                     and       r12d, 0x10
0020B467  74 66                                           je        0x20b4cf ; loc_20B4CF [RVA 0x20B4CF]
0020B469  45 8b c6                                        mov       r8d, r14d
0020B46C  48 8b d7                                        mov       rdx, rdi
0020B46F  48 8b 4c 24 70                                  mov       rcx, qword ptr [rsp + 0x70]
0020B474  e8 93 01 00 00                                  call      0x20b60c ; VslpLockMdlForTransfer [RVA 0x20B60C]
0020B479  44 8b f8                                        mov       r15d, eax
0020B47C  85 c0                                           test      eax, eax
0020B47E  0f 88 99 00 00 00                               js        0x20b51d ; loc_20B51D [RVA 0x20B51D]
0020B484  48 8b 44 24 70                                  mov       rax, qword ptr [rsp + 0x70]
0020B489  83 48 40 08                                     or        dword ptr [rax + 0x40], 8
0020B48D  33 c0                                           xor       eax, eax
0020B48F  48 83 c4 30                                     add       rsp, 0x30
0020B493  41 5f                                           pop       r15
0020B495  41 5e                                           pop       r14
0020B497  41 5d                                           pop       r13
0020B499  41 5c                                           pop       r12
0020B49B  5f                                              pop       rdi
0020B49C  5e                                              pop       rsi
0020B49D  5b                                              pop       rbx
0020B49E  c3                                              ret
0020B49F  cc                                              int3
0020B4A0  41 81 e2 ff 0f 00 00                            and       r10d, 0xfff
0020B4A7  4d 8d 81 ff 0f 00 00                            lea       r8, [r9 + 0xfff]
0020B4AE  4d 03 c2                                        add       r8, r10
0020B4B1  49 81 e0 00 f0 ff ff                            and       r8, 0xfffffffffffff000
0020B4B8  49 81 f8 00 a0 1f 00                            cmp       r8, 0x1fa000
0020B4BF  0f 87 fa 00 00 00                               ja        0x20b5bf ; loc_20B5BF [RVA 0x20B5BF]
0020B4C5  4c 8b 54 24 78                                  mov       r10, qword ptr [rsp + 0x78]
0020B4CA  e9 7e fe ff ff                                  jmp       0x20b34d ; loc_20B34D [RVA 0x20B34D]
0020B4CF  4c 89 3f                                        mov       qword ptr [rdi], r15
0020B4D2  4c 8b ff                                        mov       r15, rdi
0020B4D5  48 89 7c 24 28                                  mov       qword ptr [rsp + 0x28], rdi
0020B4DA  4c 8b d7                                        mov       r10, rdi
0020B4DD  48 89 7c 24 78                                  mov       qword ptr [rsp + 0x78], rdi
0020B4E2  45 8b cd                                        mov       r9d, r13d
0020B4E5  44 89 ac 24 80 00 00 00                         mov       dword ptr [rsp + 0x80], r13d
0020B4ED  ba 00 10 00 00                                  mov       edx, 0x1000
0020B4F2  44 3b ea                                        cmp       r13d, edx
0020B4F5  0f 86 6e ff ff ff                               jbe       0x20b469 ; loc_20B469 [RVA 0x20B469]
0020B4FB  44 8b 9c 24 88 00 00 00                         mov       r11d, dword ptr [rsp + 0x88]
0020B503  e9 56 fe ff ff                                  jmp       0x20b35e ; loc_20B35E [RVA 0x20B35E]
0020B508  48 8b cf                                        mov       rcx, rdi
0020B50B  e8 70 21 0f 00                                  call      0x2fd680 ; MmBuildMdlForNonPagedPool [RVA 0x2FD680]
0020B510  e9 4b ff ff ff                                  jmp       0x20b460 ; loc_20B460 [RVA 0x20B460]
0020B515  48 8b fe                                        mov       rdi, rsi
0020B518  e9 aa fe ff ff                                  jmp       0x20b3c7 ; loc_20B3C7 [RVA 0x20B3C7]
0020B51D  45 85 e4                                        test      r12d, r12d
0020B520  0f 84 cb 00 00 00                               je        0x20b5f1 ; loc_20B5F1 [RVA 0x20B5F1]
0020B526  48 8b cf                                        mov       rcx, rdi
0020B529  e8 22 07 00 00                                  call      0x20bc50 ; MmUnlockPages [RVA 0x20BC50]
0020B52E  33 d2                                           xor       edx, edx
0020B530  48 8b cf                                        mov       rcx, rdi
0020B533  e8 38 23 97 00                                  call      0xb7d870 ; ExFreePoolWithTag [RVA 0xB7D870]
0020B538  4c 8b 7c 24 28                                  mov       r15, qword ptr [rsp + 0x28]
0020B53D  65 48 8b 04 25 88 01 00 00                      mov       rax, qword ptr gs:[0x188]
0020B546  66 01 98 e4 01 00 00                            add       word ptr [rax + 0x1e4], bx
0020B54D  90                                              nop
0020B54E  45 33 c0                                        xor       r8d, r8d
0020B551  33 d2                                           xor       edx, edx
0020B553  48 8d 0d 5e 29 d8 00                            lea       rcx, [rip + 0xd8295e]
0020B55A  e8 11 ce 00 00                                  call      0x218370 ; KeAbPreAcquire [RVA 0x218370]
0020B55F  48 8b f8                                        mov       rdi, rax
0020B562  f0 48 0f ba 2d 4c 29 d8 00 00                   lock bts  qword ptr [rip + 0xd8294c], 0
0020B56C  73 16                                           jae       0x20b584 ; loc_20B584 [RVA 0x20B584]
0020B56E  4c 8d 05 43 29 d8 00                            lea       r8, [rip + 0xd82943]
0020B575  48 8b d0                                        mov       rdx, rax
0020B578  48 8d 0d 39 29 d8 00                            lea       rcx, [rip + 0xd82939]
0020B57F  e8 4c c8 00 00                                  call      0x217dd0 ; ExfAcquirePushLockExclusiveEx [RVA 0x217DD0]
0020B584  48 85 ff                                        test      rdi, rdi
0020B587  74 04                                           je        0x20b58d ; loc_20B58D [RVA 0x20B58D]
0020B589  c6 47 0a 01                                     mov       byte ptr [rdi + 0xa], 1
0020B58D  48 8b 3d cc 28 d8 00                            mov       rdi, qword ptr [rip + 0xd828cc]
0020B594  48 89 7c 24 20                                  mov       qword ptr [rsp + 0x20], rdi
0020B599  41 83 ce 20                                     or        r14d, 0x20
0020B59D  44 89 b4 24 90 00 00 00                         mov       dword ptr [rsp + 0x90], r14d
0020B5A5  44 8b 8c 24 80 00 00 00                         mov       r9d, dword ptr [rsp + 0x80]
0020B5AD  4c 8b 54 24 78                                  mov       r10, qword ptr [rsp + 0x78]
0020B5B2  44 8b 9c 24 88 00 00 00                         mov       r11d, dword ptr [rsp + 0x88]
0020B5BA  e9 16 fe ff ff                                  jmp       0x20b3d5 ; loc_20B3D5 [RVA 0x20B3D5]
0020B5BF  41 bf 0d 00 00 c0                               mov       r15d, 0xc000000d
0020B5C5  48 83 cb ff                                     or        rbx, 0xffffffffffffffff
0020B5C9  eb 26                                           jmp       0x20b5f1 ; loc_20B5F1 [RVA 0x20B5F1]
0020B5CB  41 81 c5 ff 0f 00 00                            add       r13d, 0xfff
0020B5D2  41 81 e5 00 f0 ff ff                            and       r13d, 0xfffff000
0020B5D9  e9 b4 fd ff ff                                  jmp       0x20b392 ; loc_20B392 [RVA 0x20B392]
0020B5DE  41 f6 c6 10                                     test      r14b, 0x10
0020B5E2  0f 85 55 ff ff ff                               jne       0x20b53d ; loc_20B53D [RVA 0x20B53D]
0020B5E8  49 8b ff                                        mov       rdi, r15
0020B5EB  41 bf 9a 00 00 c0                               mov       r15d, 0xc000009a
0020B5F1  41 83 e6 20                                     and       r14d, 0x20
0020B5F5  48 85 ff                                        test      rdi, rdi
0020B5F8  0f 85 50 53 4c 00                               jne       0x6d094e ; sub_6D094E [RVA 0x6D094E]
0020B5FE  e9 76 53 4c 00                                  jmp       0x6d0979 ; sub_6D0979 [RVA 0x6D0979]
0020B603  cc                                              int3
0020B604  cc                                              int3
0020B605  cc                                              int3
0020B606  cc                                              int3
0020B607  cc                                              int3
0020B608  cc                                              int3
0020B609  cc                                              int3
0020B60A  cc                                              int3
0020B60B  cc                                              int3
