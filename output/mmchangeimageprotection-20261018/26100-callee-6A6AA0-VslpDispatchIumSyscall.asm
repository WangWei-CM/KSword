VslpDispatchIumSyscall at RVA 0x6A6AA0
extent source: next public symbol (first .pdata end 0x6A6AE6); RVA 0x6A6AA0..0x6A6AF0 (exclusive)
shown: 33 instruction(s), max 200
All addresses are RVAs.

006A6AA0  56                                              push      rsi
006A6AA1  57                                              push      rdi
006A6AA2  48 83 ec 68                                     sub       rsp, 0x68
006A6AA6  48 8b c1                                        mov       rax, rcx
006A6AA9  49 83 e0 07                                     and       r8, 7
006A6AAD  74 14                                           je        0x6a6ac3 ; loc_6A6AC3 [RVA 0x6A6AC3]
006A6AAF  48 8b f2                                        mov       rsi, rdx
006A6AB2  48 83 c6 20                                     add       rsi, 0x20
006A6AB6  48 8b fc                                        mov       rdi, rsp
006A6AB9  48 83 c7 20                                     add       rdi, 0x20
006A6ABD  49 8b c8                                        mov       rcx, r8
006A6AC0  f3 48 a5                                        rep movsq qword ptr [rdi], qword ptr [rsi]
006A6AC3  4c 8b 4a 18                                     mov       r9, qword ptr [rdx + 0x18]
006A6AC7  4c 8b 42 10                                     mov       r8, qword ptr [rdx + 0x10]
006A6ACB  48 8b 0a                                        mov       rcx, qword ptr [rdx]
006A6ACE  48 8b 52 08                                     mov       rdx, qword ptr [rdx + 8]
006A6AD2  ff d0                                           call      rax
006A6AD4  0f 1f 00                                        nop       dword ptr [rax]
006A6AD7  65 ff 04 25 b8 2e 00 00                         inc       dword ptr gs:[0x2eb8]
006A6ADF  48 83 c4 68                                     add       rsp, 0x68
006A6AE3  5f                                              pop       rdi
006A6AE4  5e                                              pop       rsi
006A6AE5  c3                                              ret
006A6AE6  cc                                              int3
006A6AE7  cc                                              int3
006A6AE8  cc                                              int3
006A6AE9  cc                                              int3
006A6AEA  cc                                              int3
006A6AEB  cc                                              int3
006A6AEC  cc                                              int3
006A6AED  cc                                              int3
006A6AEE  cc                                              int3
006A6AEF  cc                                              int3
