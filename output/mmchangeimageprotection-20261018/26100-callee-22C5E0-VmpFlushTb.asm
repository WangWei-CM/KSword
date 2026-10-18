VmpFlushTb at RVA 0x22C5E0
extent source: next public symbol (first .pdata end 0x22C9CA); RVA 0x22C5E0..0x22C9D0 (exclusive)
shown: 200 instruction(s), max 200
All addresses are RVAs.

0022C5E0  48 89 5c 24 08                                  mov       qword ptr [rsp + 8], rbx
0022C5E5  55                                              push      rbp
0022C5E6  56                                              push      rsi
0022C5E7  57                                              push      rdi
0022C5E8  41 54                                           push      r12
0022C5EA  41 55                                           push      r13
0022C5EC  41 56                                           push      r14
0022C5EE  41 57                                           push      r15
0022C5F0  48 8d 6c 24 d9                                  lea       rbp, [rsp - 0x27]
0022C5F5  48 81 ec b0 00 00 00                            sub       rsp, 0xb0
0022C5FC  33 c0                                           xor       eax, eax
0022C5FE  49 83 ce ff                                     or        r14, 0xffffffffffffffff
0022C602  33 db                                           xor       ebx, ebx
0022C604  48 89 45 17                                     mov       qword ptr [rbp + 0x17], rax
0022C608  45 33 ff                                        xor       r15d, r15d
0022C60B  0f 57 c0                                        xorps     xmm0, xmm0
0022C60E  4d 8b e8                                        mov       r13, r8
0022C611  48 8b f9                                        mov       rdi, rcx
0022C614  0f 11 45 07                                     movups    xmmword ptr [rbp + 7], xmm0
0022C618  85 d2                                           test      edx, edx
0022C61A  0f 85 13 01 00 00                               jne       0x22c733 ; loc_22C733 [RVA 0x22C733]
0022C620  e8 8f 2b 00 00                                  call      0x22f1b4 ; VmpProcessContextLockExclusive [RVA 0x22F1B4]
0022C625  65 48 8b 0c 25 20 00 00 00                      mov       rcx, qword ptr gs:[0x20]
0022C62E  4c 8b f0                                        mov       r14, rax
0022C631  48 8b 99 50 8b 00 00                            mov       rbx, qword ptr [rcx + 0x8b50]
0022C638  48 8b 0d 99 5b cd 00                            mov       rcx, qword ptr [rip + 0xcd5b99]
0022C63F  48 81 c3 00 12 00 00                            add       rbx, 0x1200
0022C646  48 85 c9                                        test      rcx, rcx
0022C649  0f 85 ef 02 00 00                               jne       0x22c93e ; loc_22C93E [RVA 0x22C93E]
0022C64F  44 21 7d 6f                                     and       dword ptr [rbp + 0x6f], r15d
0022C653  0f 57 c0                                        xorps     xmm0, xmm0
0022C656  4c 21 7d b7                                     and       qword ptr [rbp - 0x49], r15
0022C65A  48 b8 01 00 00 00 00 00 10 00                   movabs    rax, 0x10000000000001
0022C664  0f 11 45 c7                                     movups    xmmword ptr [rbp - 0x39], xmm0
0022C668  48 89 45 bf                                     mov       qword ptr [rbp - 0x41], rax
0022C66C  41 bd 00 00 04 00                               mov       r13d, 0x40000
0022C672  48 8d 45 6f                                     lea       rax, [rbp + 0x6f]
0022C676  c7 44 24 30 01 00 00 00                         mov       dword ptr [rsp + 0x30], 1
0022C67E  48 89 44 24 28                                  mov       qword ptr [rsp + 0x28], rax
0022C683  4c 8d 4d 07                                     lea       r9, [rbp + 7]
0022C687  48 8d 45 c7                                     lea       rax, [rbp - 0x39]
0022C68B  4d 8b c5                                        mov       r8, r13
0022C68E  4d 2b c7                                        sub       r8, r15
0022C691  48 89 44 24 20                                  mov       qword ptr [rsp + 0x20], rax
0022C696  48 8d 55 b7                                     lea       rdx, [rbp - 0x49]
0022C69A  48 8b cf                                        mov       rcx, rdi
0022C69D  e8 5e 2b 00 00                                  call      0x22f200 ; VmpConvertPortionVpnRangeToGpnRange [RVA 0x22F200]
0022C6A2  48 8b 75 cf                                     mov       rsi, qword ptr [rbp - 0x31]
0022C6A6  4c 8b e0                                        mov       r12, rax
0022C6A9  48 85 f6                                        test      rsi, rsi
0022C6AC  75 22                                           jne       0x22c6d0 ; loc_22C6D0 [RVA 0x22C6D0]
0022C6AE  49 83 fe ff                                     cmp       r14, -1
0022C6B2  75 60                                           jne       0x22c714 ; loc_22C714 [RVA 0x22C714]
0022C6B4  48 8b 9c 24 f0 00 00 00                         mov       rbx, qword ptr [rsp + 0xf0]
0022C6BC  48 81 c4 b0 00 00 00                            add       rsp, 0xb0
0022C6C3  41 5f                                           pop       r15
0022C6C5  41 5e                                           pop       r14
0022C6C7  41 5d                                           pop       r13
0022C6C9  41 5c                                           pop       r12
0022C6CB  5f                                              pop       rdi
0022C6CC  5e                                              pop       rsi
0022C6CD  5d                                              pop       rbp
0022C6CE  c3                                              ret
0022C6CF  cc                                              int3
0022C6D0  8b 4d 6f                                        mov       ecx, dword ptr [rbp + 0x6f]
0022C6D3  4d 8b cc                                        mov       r9, r12
0022C6D6  4c 8b 45 c7                                     mov       r8, qword ptr [rbp - 0x39]
0022C6DA  48 8b d3                                        mov       rdx, rbx
0022C6DD  89 4c 24 28                                     mov       dword ptr [rsp + 0x28], ecx
0022C6E1  48 8b cf                                        mov       rcx, rdi
0022C6E4  48 89 74 24 20                                  mov       qword ptr [rsp + 0x20], rsi
0022C6E9  e8 b6 ad 23 00                                  call      0x4674a4 ; VmpInsertInvalidateListRange [RVA 0x4674A4]
0022C6EE  4c 8b c6                                        mov       r8, rsi
0022C6F1  49 8b d4                                        mov       rdx, r12
0022C6F4  48 8b cf                                        mov       rcx, rdi
0022C6F7  e8 f0 ab 23 00                                  call      0x4672ec ; VmpInvalidateOutstandingFaults [RVA 0x4672EC]
0022C6FC  4c 03 fe                                        add       r15, rsi
0022C6FF  4d 3b fd                                        cmp       r15, r13
0022C702  0f 83 5f 02 00 00                               jae       0x22c967 ; loc_22C967 [RVA 0x22C967]
0022C708  48 83 7d bf 00                                  cmp       qword ptr [rbp - 0x41], 0
0022C70D  74 9f                                           je        0x22c6ae ; loc_22C6AE [RVA 0x22C6AE]
0022C70F  e9 5e ff ff ff                                  jmp       0x22c672 ; loc_22C672 [RVA 0x22C672]
0022C714  48 83 7b 08 00                                  cmp       qword ptr [rbx + 8], 0
0022C719  74 0b                                           je        0x22c726 ; loc_22C726 [RVA 0x22C726]
0022C71B  48 8b d3                                        mov       rdx, rbx
0022C71E  48 8b cf                                        mov       rcx, rdi
0022C721  e8 be 9c 1a 00                                  call      0x3d63e4 ; VmpProcessInvalidateList [RVA 0x3D63E4]
0022C726  49 8b d6                                        mov       rdx, r14
0022C729  48 8b cf                                        mov       rcx, rdi
0022C72C  e8 23 fd 25 00                                  call      0x48c454 ; VmpProcessContextUnlockExclusive [RVA 0x48C454]
0022C731  eb 81                                           jmp       0x22c6b4 ; loc_22C6B4 [RVA 0x22C6B4]
0022C733  45 33 c0                                        xor       r8d, r8d
0022C736  8b c2                                           mov       eax, edx
0022C738  33 f6                                           xor       esi, esi
0022C73A  4c 89 45 c7                                     mov       qword ptr [rbp - 0x39], r8
0022C73E  48 8d 0c c5 00 00 00 00                         lea       rcx, [rax*8]
0022C746  49 03 cd                                        add       rcx, r13
0022C749  48 89 4d 7f                                     mov       qword ptr [rbp + 0x7f], rcx
0022C74D  4c 3b e9                                        cmp       r13, rcx
0022C750  0f 83 5e ff ff ff                               jae       0x22c6b4 ; loc_22C6B4 [RVA 0x22C6B4]
0022C756  48 8b c1                                        mov       rax, rcx
0022C759  49 8b 4d 00                                     mov       rcx, qword ptr [r13]
0022C75D  48 ba ff ff fe ff ff 7f 00 00                   movabs    rdx, 0x7ffffffeffff
0022C767  48 3b ca                                        cmp       rcx, rdx
0022C76A  77 72                                           ja        0x22c7de ; loc_22C7DE [RVA 0x22C7DE]
0022C76C  4c 8b e1                                        mov       r12, rcx
0022C76F  48 8b c1                                        mov       rax, rcx
0022C772  41 81 e4 ff 03 00 00                            and       r12d, 0x3ff
0022C779  48 c1 e8 0a                                     shr       rax, 0xa
0022C77D  49 ff c4                                        inc       r12
0022C780  83 e0 03                                        and       eax, 3
0022C783  0f 85 fa 01 00 00                               jne       0x22c983 ; loc_22C983 [RVA 0x22C983]
0022C789  48 c1 e9 0c                                     shr       rcx, 0xc
0022C78D  48 89 4d b7                                     mov       qword ptr [rbp - 0x49], rcx
0022C791  49 83 fe ff                                     cmp       r14, -1
0022C795  75 2a                                           jne       0x22c7c1 ; loc_22C7C1 [RVA 0x22C7C1]
0022C797  48 8b cf                                        mov       rcx, rdi
0022C79A  e8 15 2a 00 00                                  call      0x22f1b4 ; VmpProcessContextLockExclusive [RVA 0x22F1B4]
0022C79F  4c 8b f0                                        mov       r14, rax
0022C7A2  65 48 8b 04 25 20 00 00 00                      mov       rax, qword ptr gs:[0x20]
0022C7AB  48 8b 4d b7                                     mov       rcx, qword ptr [rbp - 0x49]
0022C7AF  4c 8b 45 c7                                     mov       r8, qword ptr [rbp - 0x39]
0022C7B3  48 8b 98 50 8b 00 00                            mov       rbx, qword ptr [rax + 0x8b50]
0022C7BA  48 81 c3 00 12 00 00                            add       rbx, 0x1200
0022C7C1  48 85 f6                                        test      rsi, rsi
0022C7C4  0f 84 1f 01 00 00                               je        0x22c8e9 ; loc_22C8E9 [RVA 0x22C8E9]
0022C7CA  4a 8d 04 06                                     lea       rax, [rsi + r8]
0022C7CE  48 3b c8                                        cmp       rcx, rax
0022C7D1  0f 85 bf 00 00 00                               jne       0x22c896 ; loc_22C896 [RVA 0x22C896]
0022C7D7  49 03 f4                                        add       rsi, r12
0022C7DA  48 8b 45 7f                                     mov       rax, qword ptr [rbp + 0x7f]
0022C7DE  49 83 c5 08                                     add       r13, 8
0022C7E2  4c 3b e8                                        cmp       r13, rax
0022C7E5  0f 82 6e ff ff ff                               jb        0x22c759 ; loc_22C759 [RVA 0x22C759]
0022C7EB  48 85 f6                                        test      rsi, rsi
0022C7EE  0f 84 ba fe ff ff                               je        0x22c6ae ; loc_22C6AE [RVA 0x22C6AE]
0022C7F4  83 65 6f 00                                     and       dword ptr [rbp + 0x6f], 0
0022C7F8  0f 57 c0                                        xorps     xmm0, xmm0
0022C7FB  0f 11 45 e7                                     movups    xmmword ptr [rbp - 0x19], xmm0
0022C7FF  4c 89 45 f7                                     mov       qword ptr [rbp - 9], r8
0022C803  48 89 75 ff                                     mov       qword ptr [rbp - 1], rsi
0022C807  48 8d 45 6f                                     lea       rax, [rbp + 0x6f]
0022C80B  c7 44 24 30 01 00 00 00                         mov       dword ptr [rsp + 0x30], 1
0022C813  48 89 44 24 28                                  mov       qword ptr [rsp + 0x28], rax
0022C818  4c 8d 4d 07                                     lea       r9, [rbp + 7]
0022C81C  48 8d 45 e7                                     lea       rax, [rbp - 0x19]
0022C820  41 b8 00 00 04 00                               mov       r8d, 0x40000
0022C826  4d 2b c7                                        sub       r8, r15
0022C829  48 89 44 24 20                                  mov       qword ptr [rsp + 0x20], rax
0022C82E  48 8d 55 f7                                     lea       rdx, [rbp - 9]
0022C832  48 8b cf                                        mov       rcx, rdi
0022C835  e8 c6 29 00 00                                  call      0x22f200 ; VmpConvertPortionVpnRangeToGpnRange [RVA 0x22F200]
0022C83A  48 8b 75 ef                                     mov       rsi, qword ptr [rbp - 0x11]
0022C83E  4c 8b e0                                        mov       r12, rax
0022C841  48 85 f6                                        test      rsi, rsi
0022C844  0f 84 64 fe ff ff                               je        0x22c6ae ; loc_22C6AE [RVA 0x22C6AE]
0022C84A  8b 4d 6f                                        mov       ecx, dword ptr [rbp + 0x6f]
0022C84D  4c 8b c8                                        mov       r9, rax
0022C850  4c 8b 45 e7                                     mov       r8, qword ptr [rbp - 0x19]
0022C854  48 8b d3                                        mov       rdx, rbx
0022C857  89 4c 24 28                                     mov       dword ptr [rsp + 0x28], ecx
0022C85B  48 8b cf                                        mov       rcx, rdi
0022C85E  48 89 74 24 20                                  mov       qword ptr [rsp + 0x20], rsi
0022C863  e8 3c ac 23 00                                  call      0x4674a4 ; VmpInsertInvalidateListRange [RVA 0x4674A4]
0022C868  4c 8b c6                                        mov       r8, rsi
0022C86B  49 8b d4                                        mov       rdx, r12
0022C86E  48 8b cf                                        mov       rcx, rdi
0022C871  e8 76 aa 23 00                                  call      0x4672ec ; VmpInvalidateOutstandingFaults [RVA 0x4672EC]
0022C876  4c 03 fe                                        add       r15, rsi
0022C879  49 81 ff 00 00 04 00                            cmp       r15, 0x40000
0022C880  0f 83 28 01 00 00                               jae       0x22c9ae ; loc_22C9AE [RVA 0x22C9AE]
0022C886  48 83 7d ff 00                                  cmp       qword ptr [rbp - 1], 0
0022C88B  0f 84 1d fe ff ff                               je        0x22c6ae ; loc_22C6AE [RVA 0x22C6AE]
0022C891  e9 71 ff ff ff                                  jmp       0x22c807 ; loc_22C807 [RVA 0x22C807]
0022C896  83 65 6f 00                                     and       dword ptr [rbp + 0x6f], 0
0022C89A  0f 57 c0                                        xorps     xmm0, xmm0
0022C89D  0f 11 45 d7                                     movups    xmmword ptr [rbp - 0x29], xmm0
0022C8A1  4c 89 45 e7                                     mov       qword ptr [rbp - 0x19], r8
0022C8A5  48 89 75 ef                                     mov       qword ptr [rbp - 0x11], rsi
0022C8A9  48 8d 45 6f                                     lea       rax, [rbp + 0x6f]
0022C8AD  c7 44 24 30 01 00 00 00                         mov       dword ptr [rsp + 0x30], 1
0022C8B5  48 89 44 24 28                                  mov       qword ptr [rsp + 0x28], rax
0022C8BA  4c 8d 4d 07                                     lea       r9, [rbp + 7]
0022C8BE  48 8d 45 d7                                     lea       rax, [rbp - 0x29]
0022C8C2  41 b8 00 00 04 00                               mov       r8d, 0x40000
0022C8C8  4d 2b c7                                        sub       r8, r15
0022C8CB  48 89 44 24 20                                  mov       qword ptr [rsp + 0x20], rax
0022C8D0  48 8d 55 e7                                     lea       rdx, [rbp - 0x19]
0022C8D4  48 8b cf                                        mov       rcx, rdi
0022C8D7  e8 24 29 00 00                                  call      0x22f200 ; VmpConvertPortionVpnRangeToGpnRange [RVA 0x22F200]
0022C8DC  48 8b 75 df                                     mov       rsi, qword ptr [rbp - 0x21]
0022C8E0  48 89 45 c7                                     mov       qword ptr [rbp - 0x39], rax
0022C8E4  48 85 f6                                        test      rsi, rsi
0022C8E7  75 10                                           jne       0x22c8f9 ; loc_22C8F9 [RVA 0x22C8F9]
0022C8E9  4c 8b 45 b7                                     mov       r8, qword ptr [rbp - 0x49]
0022C8ED  49 8b f4                                        mov       rsi, r12
0022C8F0  4c 89 45 c7                                     mov       qword ptr [rbp - 0x39], r8
0022C8F4  e9 e1 fe ff ff                                  jmp       0x22c7da ; loc_22C7DA [RVA 0x22C7DA]
0022C8F9  8b 4d 6f                                        mov       ecx, dword ptr [rbp + 0x6f]
0022C8FC  4c 8b c8                                        mov       r9, rax
