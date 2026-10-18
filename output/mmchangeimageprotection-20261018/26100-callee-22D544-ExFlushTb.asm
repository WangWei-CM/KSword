ExFlushTb at RVA 0x22D544
extent source: next public symbol (first .pdata end 0x22D5DA); RVA 0x22D544..0x22D5E0 (exclusive)
shown: 44 instruction(s), max 200
All addresses are RVAs.

0022D544  48 89 5c 24 08                                  mov       qword ptr [rsp + 8], rbx
0022D549  48 89 74 24 10                                  mov       qword ptr [rsp + 0x10], rsi
0022D54E  57                                              push      rdi
0022D54F  48 83 ec 20                                     sub       rsp, 0x20
0022D553  65 48 8b 04 25 88 01 00 00                      mov       rax, qword ptr gs:[0x188]
0022D55C  48 8b fa                                        mov       rdi, rdx
0022D55F  8b f1                                           mov       esi, ecx
0022D561  48 8b 98 b8 00 00 00                            mov       rbx, qword ptr [rax + 0xb8]
0022D568  41 83 f8 01                                     cmp       r8d, 1
0022D56C  75 41                                           jne       0x22d5af ; loc_22D5AF [RVA 0x22D5AF]
0022D56E  48 8b 8b 08 06 00 00                            mov       rcx, qword ptr [rbx + 0x608]
0022D575  48 8b 9b e8 07 00 00                            mov       rbx, qword ptr [rbx + 0x7e8]
0022D57C  48 85 c9                                        test      rcx, rcx
0022D57F  75 42                                           jne       0x22d5c3 ; loc_22D5C3 [RVA 0x22D5C3]
0022D581  48 85 db                                        test      rbx, rbx
0022D584  74 18                                           je        0x22d59e ; loc_22D59E [RVA 0x22D59E]
0022D586  48 8b 05 7b 97 bd 00                            mov       rax, qword ptr [rip + 0xbd977b]
0022D58D  48 8b cb                                        mov       rcx, rbx
0022D590  48 8b 40 48                                     mov       rax, qword ptr [rax + 0x48]
0022D594  4c 8b c7                                        mov       r8, rdi
0022D597  8b d6                                           mov       edx, esi
0022D599  e8 62 6f 48 00                                  call      0x6b4500 ; guard_dispatch_icall/guard_dispatch_icall_no_overrides [RVA 0x6B4500]
0022D59E  48 8b 5c 24 30                                  mov       rbx, qword ptr [rsp + 0x30]
0022D5A3  48 8b 74 24 38                                  mov       rsi, qword ptr [rsp + 0x38]
0022D5A8  48 83 c4 20                                     add       rsp, 0x20
0022D5AC  5f                                              pop       rdi
0022D5AD  c3                                              ret
0022D5AE  cc                                              int3
0022D5AF  48 8b 05 52 97 bd 00                            mov       rax, qword ptr [rip + 0xbd9752]
0022D5B6  48 8b 0d 23 d1 cc 00                            mov       rcx, qword ptr [rip + 0xccd123]
0022D5BD  48 8b 40 50                                     mov       rax, qword ptr [rax + 0x50]
0022D5C1  eb d1                                           jmp       0x22d594 ; loc_22D594 [RVA 0x22D594]
0022D5C3  48 8b 05 3e 97 bd 00                            mov       rax, qword ptr [rip + 0xbd973e]
0022D5CA  4c 8b c7                                        mov       r8, rdi
0022D5CD  8b d6                                           mov       edx, esi
0022D5CF  48 8b 40 48                                     mov       rax, qword ptr [rax + 0x48]
0022D5D3  e8 28 6f 48 00                                  call      0x6b4500 ; guard_dispatch_icall/guard_dispatch_icall_no_overrides [RVA 0x6B4500]
0022D5D8  eb a7                                           jmp       0x22d581 ; loc_22D581 [RVA 0x22D581]
0022D5DA  cc                                              int3
0022D5DB  cc                                              int3
0022D5DC  cc                                              int3
0022D5DD  cc                                              int3
0022D5DE  cc                                              int3
0022D5DF  cc                                              int3
