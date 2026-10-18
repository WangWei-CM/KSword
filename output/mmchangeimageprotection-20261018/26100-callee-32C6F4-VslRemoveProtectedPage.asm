VslRemoveProtectedPage at RVA 0x32C6F4
extent source: next public symbol (first .pdata end 0x32C76C); RVA 0x32C6F4..0x32C774 (exclusive)
shown: 40 instruction(s), max 200
All addresses are RVAs.

0032C6F4  40 53                                           push      rbx
0032C6F6  56                                              push      rsi
0032C6F7  57                                              push      rdi
0032C6F8  48 81 ec a0 00 00 00                            sub       rsp, 0xa0
0032C6FF  48 8b 05 3a e0 ad 00                            mov       rax, qword ptr [rip + 0xade03a]
0032C706  48 33 c4                                        xor       rax, rsp
0032C709  48 89 84 24 90 00 00 00                         mov       qword ptr [rsp + 0x90], rax
0032C711  48 8b fa                                        mov       rdi, rdx
0032C714  41 8b f0                                        mov       esi, r8d
0032C717  33 d2                                           xor       edx, edx
0032C719  48 8b d9                                        mov       rbx, rcx
0032C71C  48 8d 4c 24 20                                  lea       rcx, [rsp + 0x20]
0032C721  44 8d 42 68                                     lea       r8d, [rdx + 0x68]
0032C725  e8 16 40 39 00                                  call      0x6c0740 ; memset [RVA 0x6C0740]
0032C72A  0f 10 07                                        movups    xmm0, xmmword ptr [rdi]
0032C72D  ba f3 00 00 00                                  mov       edx, 0xf3
0032C732  48 89 5c 24 28                                  mov       qword ptr [rsp + 0x28], rbx
0032C737  4c 8d 4c 24 20                                  lea       r9, [rsp + 0x20]
0032C73C  89 74 24 48                                     mov       dword ptr [rsp + 0x48], esi
0032C740  45 33 c0                                        xor       r8d, r8d
0032C743  b1 02                                           mov       cl, 2
0032C745  f3 0f 7f 44 24 38                               movdqu    xmmword ptr [rsp + 0x38], xmm0
0032C74B  e8 10 c5 fc ff                                  call      0x2f8c60 ; VslpEnterIumSecureMode [RVA 0x2F8C60]
0032C750  48 8b 8c 24 90 00 00 00                         mov       rcx, qword ptr [rsp + 0x90]
0032C758  48 33 cc                                        xor       rcx, rsp
0032C75B  e8 30 99 37 00                                  call      0x6a6090 ; _security_check_cookie [RVA 0x6A6090]
0032C760  48 81 c4 a0 00 00 00                            add       rsp, 0xa0
0032C767  5f                                              pop       rdi
0032C768  5e                                              pop       rsi
0032C769  5b                                              pop       rbx
0032C76A  c3                                              ret
0032C76B  cc                                              int3
0032C76C  cc                                              int3
0032C76D  cc                                              int3
0032C76E  cc                                              int3
0032C76F  cc                                              int3
0032C770  cc                                              int3
0032C771  cc                                              int3
0032C772  cc                                              int3
0032C773  cc                                              int3
