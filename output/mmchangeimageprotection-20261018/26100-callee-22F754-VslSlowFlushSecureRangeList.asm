VslSlowFlushSecureRangeList at RVA 0x22F754
extent source: next public symbol (first .pdata end 0x22F859); RVA 0x22F754..0x22F860 (exclusive)
shown: 73 instruction(s), max 200
All addresses are RVAs.

0022F754  48 89 5c 24 10                                  mov       qword ptr [rsp + 0x10], rbx
0022F759  48 89 6c 24 18                                  mov       qword ptr [rsp + 0x18], rbp
0022F75E  56                                              push      rsi
0022F75F  57                                              push      rdi
0022F760  41 56                                           push      r14
0022F762  48 81 ec a0 00 00 00                            sub       rsp, 0xa0
0022F769  48 8b 05 d0 af bd 00                            mov       rax, qword ptr [rip + 0xbdafd0]
0022F770  48 33 c4                                        xor       rax, rsp
0022F773  48 89 84 24 90 00 00 00                         mov       qword ptr [rsp + 0x90], rax
0022F77B  48 8b bc 24 e8 00 00 00                         mov       rdi, qword ptr [rsp + 0xe8]
0022F783  4c 8b f2                                        mov       r14, rdx
0022F786  33 d2                                           xor       edx, edx
0022F788  41 8b f1                                        mov       esi, r9d
0022F78B  41 8b e8                                        mov       ebp, r8d
0022F78E  48 8b d9                                        mov       rbx, rcx
0022F791  48 8d 4c 24 20                                  lea       rcx, [rsp + 0x20]
0022F796  44 8d 42 68                                     lea       r8d, [rdx + 0x68]
0022F79A  e8 a1 0f 49 00                                  call      0x6c0740 ; memset [RVA 0x6C0740]
0022F79F  44 0f 20 c0                                     mov       rax, cr8
0022F7A3  3c 02                                           cmp       al, 2
0022F7A5  0f 87 8f 00 00 00                               ja        0x22f83a ; loc_22F83A [RVA 0x22F83A]
0022F7AB  8b 94 24 e0 00 00 00                            mov       edx, dword ptr [rsp + 0xe0]
0022F7B2  83 fd 01                                        cmp       ebp, 1
0022F7B5  74 56                                           je        0x22f80d ; loc_22F80D [RVA 0x22F80D]
0022F7B7  48 89 54 24 40                                  mov       qword ptr [rsp + 0x40], rdx
0022F7BC  4c 8d 4c 24 20                                  lea       r9, [rsp + 0x20]
0022F7C1  ba f2 00 00 00                                  mov       edx, 0xf2
0022F7C6  48 89 5c 24 30                                  mov       qword ptr [rsp + 0x30], rbx
0022F7CB  45 33 c0                                        xor       r8d, r8d
0022F7CE  48 89 74 24 38                                  mov       qword ptr [rsp + 0x38], rsi
0022F7D3  b1 02                                           mov       cl, 2
0022F7D5  e8 86 94 0c 00                                  call      0x2f8c60 ; VslpEnterIumSecureMode [RVA 0x2F8C60]
0022F7DA  85 c0                                           test      eax, eax
0022F7DC  78 06                                           js        0x22f7e4 ; loc_22F7E4 [RVA 0x22F7E4]
0022F7DE  8a 4c 24 30                                     mov       cl, byte ptr [rsp + 0x30]
0022F7E2  88 0f                                           mov       byte ptr [rdi], cl
0022F7E4  48 8b 8c 24 90 00 00 00                         mov       rcx, qword ptr [rsp + 0x90]
0022F7EC  48 33 cc                                        xor       rcx, rsp
0022F7EF  e8 9c 68 47 00                                  call      0x6a6090 ; _security_check_cookie [RVA 0x6A6090]
0022F7F4  4c 8d 9c 24 a0 00 00 00                         lea       r11, [rsp + 0xa0]
0022F7FC  49 8b 5b 28                                     mov       rbx, qword ptr [r11 + 0x28]
0022F800  49 8b 6b 30                                     mov       rbp, qword ptr [r11 + 0x30]
0022F804  49 8b e3                                        mov       rsp, r11
0022F807  41 5e                                           pop       r14
0022F809  5f                                              pop       rdi
0022F80A  5e                                              pop       rsi
0022F80B  c3                                              ret
0022F80C  cc                                              int3
0022F80D  65 48 8b 04 25 88 01 00 00                      mov       rax, qword ptr gs:[0x188]
0022F816  4e 8d 04 32                                     lea       r8, [rdx + r14]
0022F81A  48 8b 88 b8 00 00 00                            mov       rcx, qword ptr [rax + 0xb8]
0022F821  48 8b 81 70 01 00 00                            mov       rax, qword ptr [rcx + 0x170]
0022F828  33 c9                                           xor       ecx, ecx
0022F82A  48 89 44 24 28                                  mov       qword ptr [rsp + 0x28], rax
0022F82F  3b ce                                           cmp       ecx, esi
0022F831  72 0e                                           jb        0x22f841 ; loc_22F841 [RVA 0x22F841]
0022F833  33 c0                                           xor       eax, eax
0022F835  c6 07 00                                        mov       byte ptr [rdi], 0
0022F838  eb aa                                           jmp       0x22f7e4 ; loc_22F7E4 [RVA 0x22F7E4]
0022F83A  b8 ff 00 00 00                                  mov       eax, 0xff
0022F83F  eb f4                                           jmp       0x22f835 ; loc_22F835 [RVA 0x22F835]
0022F841  49 b9 ff ff fe ff ff 7f 00 00                   movabs    r9, 0x7ffffffeffff
0022F84B  4d 39 0c c8                                     cmp       qword ptr [r8 + rcx*8], r9
0022F84F  0f 86 62 ff ff ff                               jbe       0x22f7b7 ; loc_22F7B7 [RVA 0x22F7B7]
0022F855  ff c1                                           inc       ecx
0022F857  eb d6                                           jmp       0x22f82f ; loc_22F82F [RVA 0x22F82F]
0022F859  cc                                              int3
0022F85A  cc                                              int3
0022F85B  cc                                              int3
0022F85C  cc                                              int3
0022F85D  cc                                              int3
0022F85E  cc                                              int3
0022F85F  cc                                              int3
