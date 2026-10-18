KiFlushRangeWorker at RVA 0x22C480
extent source: next public symbol (first .pdata end 0x22C5DA); RVA 0x22C480..0x22C5E0 (exclusive)
shown: 93 instruction(s), max 200
All addresses are RVAs.

0022C480  48 89 5c 24 08                                  mov       qword ptr [rsp + 8], rbx
0022C485  48 89 6c 24 10                                  mov       qword ptr [rsp + 0x10], rbp
0022C48A  48 89 74 24 18                                  mov       qword ptr [rsp + 0x18], rsi
0022C48F  57                                              push      rdi
0022C490  41 54                                           push      r12
0022C492  41 55                                           push      r13
0022C494  41 56                                           push      r14
0022C496  41 57                                           push      r15
0022C498  48 83 ec 50                                     sub       rsp, 0x50
0022C49C  48 8b 05 9d e2 bd 00                            mov       rax, qword ptr [rip + 0xbde29d]
0022C4A3  48 33 c4                                        xor       rax, rsp
0022C4A6  48 89 44 24 40                                  mov       qword ptr [rsp + 0x40], rax
0022C4AB  48 8b 19                                        mov       rbx, qword ptr [rcx]
0022C4AE  4c 8b f1                                        mov       r14, rcx
0022C4B1  8b 41 08                                        mov       eax, dword ptr [rcx + 8]
0022C4B4  45 33 ed                                        xor       r13d, r13d
0022C4B7  4c 8d 3c c3                                     lea       r15, [rbx + rax*8]
0022C4BB  44 38 2d 05 96 d9 00                            cmp       byte ptr [rip + 0xd99605], r13b
0022C4C2  41 8b 76 0c                                     mov       esi, dword ptr [r14 + 0xc]
0022C4C6  48 8b 3b                                        mov       rdi, qword ptr [rbx]
0022C4C9  8d 46 ff                                        lea       eax, [rsi - 1]
0022C4CC  74 43                                           je        0x22c511 ; loc_22C511 [RVA 0x22C511]
0022C4CE  83 f8 01                                        cmp       eax, 1
0022C4D1  77 3e                                           ja        0x22c511 ; loc_22C511 [RVA 0x22C511]
0022C4D3  65 48 8b 04 25 88 01 00 00                      mov       rax, qword ptr gs:[0x188]
0022C4DC  48 8b 88 b8 00 00 00                            mov       rcx, qword ptr [rax + 0xb8]
0022C4E3  44 38 a9 60 01 00 00                            cmp       byte ptr [rcx + 0x160], r13b
0022C4EA  75 25                                           jne       0x22c511 ; loc_22C511 [RVA 0x22C511]
0022C4EC  f6 05 d4 95 d9 00 02                            test      byte ptr [rip + 0xd995d4], 2
0022C4F3  0f 84 d0 00 00 00                               je        0x22c5c9 ; loc_22C5C9 [RVA 0x22C5C9]
0022C4F9  48 c7 44 24 20 01 00 00 00                      mov       qword ptr [rsp + 0x20], 1
0022C502  48 89 7c 24 28                                  mov       qword ptr [rsp + 0x28], rdi
0022C507  41 8b c5                                        mov       eax, r13d
0022C50A  66 0f 38 82 44 24 20                            invpcid   rax, xmmword ptr [rsp + 0x20]
0022C511  0f 01 3f                                        invlpg    byte ptr [rdi]
0022C514  48 8b cf                                        mov       rcx, rdi
0022C517  41 bc 00 10 00 00                               mov       r12d, 0x1000
0022C51D  48 c1 e9 0a                                     shr       rcx, 0xa
0022C521  48 8b ef                                        mov       rbp, rdi
0022C524  80 e1 03                                        and       cl, 3
0022C527  0f b6 c1                                        movzx     eax, cl
0022C52A  c0 e0 03                                        shl       al, 3
0022C52D  02 c8                                           add       cl, al
0022C52F  49 d3 e4                                        shl       r12, cl
0022C532  81 e5 ff 03 00 00                               and       ebp, 0x3ff
0022C538  74 56                                           je        0x22c590 ; loc_22C590 [RVA 0x22C590]
0022C53A  ff ce                                           dec       esi
0022C53C  49 03 fc                                        add       rdi, r12
0022C53F  44 38 2d 81 95 d9 00                            cmp       byte ptr [rip + 0xd99581], r13b
0022C546  74 3f                                           je        0x22c587 ; loc_22C587 [RVA 0x22C587]
0022C548  83 fe 01                                        cmp       esi, 1
0022C54B  77 3a                                           ja        0x22c587 ; loc_22C587 [RVA 0x22C587]
0022C54D  65 48 8b 04 25 88 01 00 00                      mov       rax, qword ptr gs:[0x188]
0022C556  48 8b 88 b8 00 00 00                            mov       rcx, qword ptr [rax + 0xb8]
0022C55D  44 38 a9 60 01 00 00                            cmp       byte ptr [rcx + 0x160], r13b
0022C564  75 21                                           jne       0x22c587 ; loc_22C587 [RVA 0x22C587]
0022C566  f6 05 5a 95 d9 00 02                            test      byte ptr [rip + 0xd9955a], 2
0022C56D  74 64                                           je        0x22c5d3 ; loc_22C5D3 [RVA 0x22C5D3]
0022C56F  48 c7 44 24 30 01 00 00 00                      mov       qword ptr [rsp + 0x30], 1
0022C578  48 89 7c 24 38                                  mov       qword ptr [rsp + 0x38], rdi
0022C57D  41 8b c5                                        mov       eax, r13d
0022C580  66 0f 38 82 44 24 30                            invpcid   rax, xmmword ptr [rsp + 0x30]
0022C587  0f 01 3f                                        invlpg    byte ptr [rdi]
0022C58A  48 83 ed 01                                     sub       rbp, 1
0022C58E  75 ac                                           jne       0x22c53c ; loc_22C53C [RVA 0x22C53C]
0022C590  48 83 c3 08                                     add       rbx, 8
0022C594  49 3b df                                        cmp       rbx, r15
0022C597  0f 82 1e ff ff ff                               jb        0x22c4bb ; loc_22C4BB [RVA 0x22C4BB]
0022C59D  48 8b 4c 24 40                                  mov       rcx, qword ptr [rsp + 0x40]
0022C5A2  48 33 cc                                        xor       rcx, rsp
0022C5A5  e8 e6 9a 47 00                                  call      0x6a6090 ; _security_check_cookie [RVA 0x6A6090]
0022C5AA  4c 8d 5c 24 50                                  lea       r11, [rsp + 0x50]
0022C5AF  49 8b 5b 30                                     mov       rbx, qword ptr [r11 + 0x30]
0022C5B3  49 8b 6b 38                                     mov       rbp, qword ptr [r11 + 0x38]
0022C5B7  49 8b 73 40                                     mov       rsi, qword ptr [r11 + 0x40]
0022C5BB  49 8b e3                                        mov       rsp, r11
0022C5BE  41 5f                                           pop       r15
0022C5C0  41 5e                                           pop       r14
0022C5C2  41 5d                                           pop       r13
0022C5C4  41 5c                                           pop       r12
0022C5C6  5f                                              pop       rdi
0022C5C7  c3                                              ret
0022C5C8  cc                                              int3
0022C5C9  e8 22 f5 47 00                                  call      0x6abaf0 ; KiSetUserTbFlushPending [RVA 0x6ABAF0]
0022C5CE  e9 3e ff ff ff                                  jmp       0x22c511 ; loc_22C511 [RVA 0x22C511]
0022C5D3  e8 18 f5 47 00                                  call      0x6abaf0 ; KiSetUserTbFlushPending [RVA 0x6ABAF0]
0022C5D8  eb ad                                           jmp       0x22c587 ; loc_22C587 [RVA 0x22C587]
0022C5DA  cc                                              int3
0022C5DB  cc                                              int3
0022C5DC  cc                                              int3
0022C5DD  cc                                              int3
0022C5DE  cc                                              int3
0022C5DF  cc                                              int3
