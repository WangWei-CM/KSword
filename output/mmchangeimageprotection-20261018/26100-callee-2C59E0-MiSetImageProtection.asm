MiSetImageProtection at RVA 0x2C59E0
extent source: next public symbol (first .pdata end 0x2C5A28); RVA 0x2C59E0..0x2C5A30 (exclusive)
shown: 26 instruction(s), max 200
All addresses are RVAs.

002C59E0  48 83 ec 38                                     sub       rsp, 0x38
002C59E4  41 ff c8                                        dec       r8d
002C59E7  49 ba 00 00 00 00 80 f6 ff ff                   movabs    r10, 0xfffff68000000000
002C59F1  4c 03 c2                                        add       r8, rdx
002C59F4  49 bb f8 ff ff ff 7f 00 00 00                   movabs    r11, 0x7ffffffff8
002C59FE  49 c1 e8 09                                     shr       r8, 9
002C5A02  4d 23 c3                                        and       r8, r11
002C5A05  49 8b c2                                        mov       rax, r10
002C5A08  48 c1 ea 09                                     shr       rdx, 9
002C5A0C  4c 03 c0                                        add       r8, rax
002C5A0F  49 23 d3                                        and       rdx, r11
002C5A12  49 8b c2                                        mov       rax, r10
002C5A15  83 64 24 20 00                                  and       dword ptr [rsp + 0x20], 0
002C5A1A  48 03 d0                                        add       rdx, rax
002C5A1D  e8 0e 00 00 00                                  call      0x2c5a30 ; MiSetSystemCodeProtection [RVA 0x2C5A30]
002C5A22  48 83 c4 38                                     add       rsp, 0x38
002C5A26  c3                                              ret
002C5A27  cc                                              int3
002C5A28  cc                                              int3
002C5A29  cc                                              int3
002C5A2A  cc                                              int3
002C5A2B  cc                                              int3
002C5A2C  cc                                              int3
002C5A2D  cc                                              int3
002C5A2E  cc                                              int3
002C5A2F  cc                                              int3
