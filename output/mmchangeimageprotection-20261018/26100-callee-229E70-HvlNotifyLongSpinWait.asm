HvlNotifyLongSpinWait at RVA 0x229E70
extent source: next public symbol (first .pdata end 0x229E9A); RVA 0x229E70..0x229EA0 (exclusive)
shown: 16 instruction(s), max 200
All addresses are RVAs.

00229E70  48 83 ec 28                                     sub       rsp, 0x28
00229E74  48 c7 44 24 38 08 00 00 00                      mov       qword ptr [rsp + 0x38], 8
00229E7D  45 33 c0                                        xor       r8d, r8d
00229E80  8b d1                                           mov       edx, ecx
00229E82  c7 44 24 38 08 00 01 00                         mov       dword ptr [rsp + 0x38], 0x10008
00229E8A  48 8b 4c 24 38                                  mov       rcx, qword ptr [rsp + 0x38]
00229E8F  e8 ac 41 00 00                                  call      0x22e040 ; HvcallInitiateHypercall [RVA 0x22E040]
00229E94  48 83 c4 28                                     add       rsp, 0x28
00229E98  c3                                              ret
00229E99  cc                                              int3
00229E9A  cc                                              int3
00229E9B  cc                                              int3
00229E9C  cc                                              int3
00229E9D  cc                                              int3
00229E9E  cc                                              int3
00229E9F  cc                                              int3
