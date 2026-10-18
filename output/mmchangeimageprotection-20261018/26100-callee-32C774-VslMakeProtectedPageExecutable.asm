VslMakeProtectedPageExecutable at RVA 0x32C774
extent source: next public symbol (first .pdata end 0x32C803); RVA 0x32C774..0x32C80C (exclusive)
shown: 45 instruction(s), max 200
All addresses are RVAs.

0032C774  48 89 5c 24 18                                  mov       qword ptr [rsp + 0x18], rbx
0032C779  48 89 74 24 20                                  mov       qword ptr [rsp + 0x20], rsi
0032C77E  57                                              push      rdi
0032C77F  48 81 ec a0 00 00 00                            sub       rsp, 0xa0
0032C786  48 8b 05 b3 df ad 00                            mov       rax, qword ptr [rip + 0xaddfb3]
0032C78D  48 33 c4                                        xor       rax, rsp
0032C790  48 89 84 24 90 00 00 00                         mov       qword ptr [rsp + 0x90], rax
0032C798  48 8b fa                                        mov       rdi, rdx
0032C79B  41 8b f0                                        mov       esi, r8d
0032C79E  33 d2                                           xor       edx, edx
0032C7A0  48 8b d9                                        mov       rbx, rcx
0032C7A3  48 8d 4c 24 20                                  lea       rcx, [rsp + 0x20]
0032C7A8  44 8d 42 68                                     lea       r8d, [rdx + 0x68]
0032C7AC  e8 8f 3f 39 00                                  call      0x6c0740 ; memset [RVA 0x6C0740]
0032C7B1  33 c0                                           xor       eax, eax
0032C7B3  48 89 5c 24 28                                  mov       qword ptr [rsp + 0x28], rbx
0032C7B8  85 f6                                           test      esi, esi
0032C7BA  48 89 7c 24 30                                  mov       qword ptr [rsp + 0x30], rdi
0032C7BF  ba fa 00 00 00                                  mov       edx, 0xfa
0032C7C4  4c 8d 4c 24 20                                  lea       r9, [rsp + 0x20]
0032C7C9  8d 48 01                                        lea       ecx, [rax + 1]
0032C7CC  0f 45 c1                                        cmovne    eax, ecx
0032C7CF  45 33 c0                                        xor       r8d, r8d
0032C7D2  b1 02                                           mov       cl, 2
0032C7D4  89 44 24 48                                     mov       dword ptr [rsp + 0x48], eax
0032C7D8  e8 83 c4 fc ff                                  call      0x2f8c60 ; VslpEnterIumSecureMode [RVA 0x2F8C60]
0032C7DD  48 8b 8c 24 90 00 00 00                         mov       rcx, qword ptr [rsp + 0x90]
0032C7E5  48 33 cc                                        xor       rcx, rsp
0032C7E8  e8 a3 98 37 00                                  call      0x6a6090 ; _security_check_cookie [RVA 0x6A6090]
0032C7ED  4c 8d 9c 24 a0 00 00 00                         lea       r11, [rsp + 0xa0]
0032C7F5  49 8b 5b 20                                     mov       rbx, qword ptr [r11 + 0x20]
0032C7F9  49 8b 73 28                                     mov       rsi, qword ptr [r11 + 0x28]
0032C7FD  49 8b e3                                        mov       rsp, r11
0032C800  5f                                              pop       rdi
0032C801  c3                                              ret
0032C802  cc                                              int3
0032C803  cc                                              int3
0032C804  cc                                              int3
0032C805  cc                                              int3
0032C806  cc                                              int3
0032C807  cc                                              int3
0032C808  cc                                              int3
0032C809  cc                                              int3
0032C80A  cc                                              int3
0032C80B  cc                                              int3
