VslGetNestedPageProtectionFlags at RVA 0x3324F4
extent source: next public symbol (first .pdata end 0x33256C); RVA 0x3324F4..0x332574 (exclusive)
shown: 37 instruction(s), max 200
All addresses are RVAs.

003324F4  40 53                                           push      rbx
003324F6  48 81 ec a0 00 00 00                            sub       rsp, 0xa0
003324FD  48 8b 05 3c 82 ad 00                            mov       rax, qword ptr [rip + 0xad823c]
00332504  48 33 c4                                        xor       rax, rsp
00332507  48 89 84 24 90 00 00 00                         mov       qword ptr [rsp + 0x90], rax
0033250F  33 d2                                           xor       edx, edx
00332511  48 8b d9                                        mov       rbx, rcx
00332514  48 8d 4c 24 20                                  lea       rcx, [rsp + 0x20]
00332519  44 8d 42 68                                     lea       r8d, [rdx + 0x68]
0033251D  e8 1e e2 38 00                                  call      0x6c0740 ; memset [RVA 0x6C0740]
00332522  48 83 3d 36 f3 ec 00 00                         cmp       qword ptr [rip + 0xecf336], 0
0033252A  75 25                                           jne       0x332551 ; loc_332551 [RVA 0x332551]
0033252C  48 85 db                                        test      rbx, rbx
0033252F  75 2f                                           jne       0x332560 ; loc_332560 [RVA 0x332560]
00332531  8b 05 15 f3 ec 00                               mov       eax, dword ptr [rip + 0xecf315]
00332537  48 8b 8c 24 90 00 00 00                         mov       rcx, qword ptr [rsp + 0x90]
0033253F  48 33 cc                                        xor       rcx, rsp
00332542  e8 49 3b 37 00                                  call      0x6a6090 ; _security_check_cookie [RVA 0x6A6090]
00332547  48 81 c4 a0 00 00 00                            add       rsp, 0xa0
0033254E  5b                                              pop       rbx
0033254F  c3                                              ret
00332550  cc                                              int3
00332551  8b 05 f5 f2 ec 00                               mov       eax, dword ptr [rip + 0xecf2f5]
00332557  85 c0                                           test      eax, eax
00332559  75 d1                                           jne       0x33252c ; loc_33252C [RVA 0x33252C]
0033255B  e9 0c 79 3a 00                                  jmp       0x6d9e6c ; sub_6D9E6C [RVA 0x6D9E6C]
00332560  48 8b 05 01 f3 ec 00                            mov       rax, qword ptr [rip + 0xecf301]
00332567  48 89 03                                        mov       qword ptr [rbx], rax
0033256A  eb c5                                           jmp       0x332531 ; loc_332531 [RVA 0x332531]
0033256C  cc                                              int3
0033256D  cc                                              int3
0033256E  cc                                              int3
0033256F  cc                                              int3
00332570  cc                                              int3
00332571  cc                                              int3
00332572  cc                                              int3
00332573  cc                                              int3
