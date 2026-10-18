MiGetTopLevelPfn at RVA 0x34D830
extent source: next public symbol (first .pdata end 0x34D993); RVA 0x34D830..0x34D9A0 (exclusive)
shown: 111 instruction(s), max 200
All addresses are RVAs.

0034D830  48 89 5c 24 10                                  mov       qword ptr [rsp + 0x10], rbx
0034D835  48 89 6c 24 18                                  mov       qword ptr [rsp + 0x18], rbp
0034D83A  48 89 74 24 20                                  mov       qword ptr [rsp + 0x20], rsi
0034D83F  57                                              push      rdi
0034D840  41 54                                           push      r12
0034D842  41 55                                           push      r13
0034D844  41 56                                           push      r14
0034D846  41 57                                           push      r15
0034D848  48 83 ec 60                                     sub       rsp, 0x60
0034D84C  48 8b 05 ed ce ab 00                            mov       rax, qword ptr [rip + 0xabceed]
0034D853  48 33 c4                                        xor       rax, rsp
0034D856  48 89 44 24 50                                  mov       qword ptr [rsp + 0x50], rax
0034D85B  0f 57 c0                                        xorps     xmm0, xmm0
0034D85E  49 bd 00 00 00 00 00 de ff ff                   movabs    r13, 0xffffde0000000000
0034D868  0f 11 44 24 30                                  movups    xmmword ptr [rsp + 0x30], xmm0
0034D86D  48 8b d9                                        mov       rbx, rcx
0034D870  4c 8b f9                                        mov       r15, rcx
0034D873  0f 11 44 24 40                                  movups    xmmword ptr [rsp + 0x40], xmm0
0034D878  49 8b c5                                        mov       rax, r13
0034D87B  48 8b f9                                        mov       rdi, rcx
0034D87E  49 bc ff ff ff ff ff 00 00 00                   movabs    r12, 0xffffffffff
0034D888  48 2b f8                                        sub       rdi, rax
0034D88B  48 b8 ab aa aa aa aa aa aa aa                   movabs    rax, 0xaaaaaaaaaaaaaaab
0034D895  48 c1 ff 04                                     sar       rdi, 4
0034D899  48 0f af f8                                     imul      rdi, rax
0034D89D  33 f6                                           xor       esi, esi
0034D89F  8b ee                                           mov       ebp, esi
0034D8A1  48 8b 43 28                                     mov       rax, qword ptr [rbx + 0x28]
0034D8A5  49 23 c4                                        and       rax, r12
0034D8A8  8b cd                                           mov       ecx, ebp
0034D8AA  48 3b c7                                        cmp       rax, rdi
0034D8AD  74 36                                           je        0x34d8e5 ; loc_34D8E5 [RVA 0x34D8E5]
0034D8AF  48 8b 7b 28                                     mov       rdi, qword ptr [rbx + 0x28]
0034D8B3  49 23 fc                                        and       rdi, r12
0034D8B6  48 8d 1c 7f                                     lea       rbx, [rdi + rdi*2]
0034D8BA  48 c1 e3 04                                     shl       rbx, 4
0034D8BE  49 8b c5                                        mov       rax, r13
0034D8C1  48 03 d8                                        add       rbx, rax
0034D8C4  ff c5                                           inc       ebp
0034D8C6  83 fd 04                                        cmp       ebp, 4
0034D8C9  76 75                                           jbe       0x34d940 ; loc_34D940 [RVA 0x34D940]
0034D8CB  45 33 c9                                        xor       r9d, r9d
0034D8CE  48 89 74 24 20                                  mov       qword ptr [rsp + 0x20], rsi
0034D8D3  4d 8b c7                                        mov       r8, r15
0034D8D6  ba 96 96 00 00                                  mov       edx, 0x9696
0034D8DB  41 8d 49 1a                                     lea       ecx, [r9 + 0x1a]
0034D8DF  e8 1c fd 1a 00                                  call      0x4fd600 ; KeBugCheckEx [RVA 0x4FD600]
0034D8E4  cc                                              int3
0034D8E5  48 8d 4c 24 30                                  lea       rcx, [rsp + 0x30]
0034D8EA  48 ba ff ff ff ff ff ff ff 7f                   movabs    rdx, 0x7fffffffffffffff
0034D8F4  48 8b 01                                        mov       rax, qword ptr [rcx]
0034D8F7  48 85 c0                                        test      rax, rax
0034D8FA  74 15                                           je        0x34d911 ; loc_34D911 [RVA 0x34D911]
0034D8FC  48 3b c3                                        cmp       rax, rbx
0034D8FF  74 05                                           je        0x34d906 ; loc_34D906 [RVA 0x34D906]
0034D901  f0 48 21 50 18                                  lock and  qword ptr [rax + 0x18], rdx
0034D906  ff c6                                           inc       esi
0034D908  48 83 c1 08                                     add       rcx, 8
0034D90C  83 fe 04                                        cmp       esi, 4
0034D90F  72 e3                                           jb        0x34d8f4 ; loc_34D8F4 [RVA 0x34D8F4]
0034D911  48 8b c3                                        mov       rax, rbx
0034D914  48 8b 4c 24 50                                  mov       rcx, qword ptr [rsp + 0x50]
0034D919  48 33 cc                                        xor       rcx, rsp
0034D91C  e8 6f 87 35 00                                  call      0x6a6090 ; _security_check_cookie [RVA 0x6A6090]
0034D921  4c 8d 5c 24 60                                  lea       r11, [rsp + 0x60]
0034D926  49 8b 5b 38                                     mov       rbx, qword ptr [r11 + 0x38]
0034D92A  49 8b 6b 40                                     mov       rbp, qword ptr [r11 + 0x40]
0034D92E  49 8b 73 48                                     mov       rsi, qword ptr [r11 + 0x48]
0034D932  49 8b e3                                        mov       rsp, r11
0034D935  41 5f                                           pop       r15
0034D937  41 5e                                           pop       r14
0034D939  41 5d                                           pop       r13
0034D93B  41 5c                                           pop       r12
0034D93D  5f                                              pop       rdi
0034D93E  c3                                              ret
0034D93F  cc                                              int3
0034D940  48 89 5c cc 30                                  mov       qword ptr [rsp + rcx*8 + 0x30], rbx
0034D945  44 8b f6                                        mov       r14d, esi
0034D948  0f 1f 84 00 00 00 00 00                         nop       dword ptr [rax + rax]
0034D950  f0 48 0f ba 6b 18 3f                            lock bts  qword ptr [rbx + 0x18], 0x3f
0034D957  0f 83 44 ff ff ff                               jae       0x34d8a1 ; loc_34D8A1 [RVA 0x34D8A1]
0034D95D  41 ff c6                                        inc       r14d
0034D960  44 85 35 45 69 c7 00                            test      dword ptr [rip + 0xc76945], r14d
0034D967  74 0d                                           je        0x34d976 ; loc_34D976 [RVA 0x34D976]
0034D969  f3 90                                           pause
0034D96B  48 8b 43 18                                     mov       rax, qword ptr [rbx + 0x18]
0034D96F  48 85 c0                                        test      rax, rax
0034D972  79 dc                                           jns       0x34d950 ; loc_34D950 [RVA 0x34D950]
0034D974  eb e7                                           jmp       0x34d95d ; loc_34D95D [RVA 0x34D95D]
0034D976  8b 05 7c 81 c7 00                               mov       eax, dword ptr [rip + 0xc7817c]
0034D97C  a8 40                                           test      al, 0x40
0034D97E  74 e9                                           je        0x34d969 ; loc_34D969 [RVA 0x34D969]
0034D980  e8 1b c5 ed ff                                  call      0x229ea0 ; KiCheckVpBackingLongSpinWaitHypercall [RVA 0x229EA0]
0034D985  84 c0                                           test      al, al
0034D987  74 e0                                           je        0x34d969 ; loc_34D969 [RVA 0x34D969]
0034D989  41 8b ce                                        mov       ecx, r14d
0034D98C  e8 df c4 ed ff                                  call      0x229e70 ; HvlNotifyLongSpinWait [RVA 0x229E70]
0034D991  eb d8                                           jmp       0x34d96b ; loc_34D96B [RVA 0x34D96B]
0034D993  cc                                              int3
0034D994  cc                                              int3
0034D995  cc                                              int3
0034D996  cc                                              int3
0034D997  cc                                              int3
0034D998  cc                                              int3
0034D999  cc                                              int3
0034D99A  cc                                              int3
0034D99B  cc                                              int3
0034D99C  cc                                              int3
0034D99D  cc                                              int3
0034D99E  cc                                              int3
0034D99F  cc                                              int3
