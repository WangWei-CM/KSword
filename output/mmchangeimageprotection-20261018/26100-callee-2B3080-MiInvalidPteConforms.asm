MiInvalidPteConforms at RVA 0x2B3080
extent source: next public symbol (first .pdata end 0x2B30AE); RVA 0x2B3080..0x2B30B4 (exclusive)
shown: 24 instruction(s), max 200
All addresses are RVAs.

002B3080  f6 c1 01                                        test      cl, 1
002B3083  75 25                                           jne       0x2b30aa ; loc_2B30AA [RVA 0x2B30AA]
002B3085  48 85 c9                                        test      rcx, rcx
002B3088  74 0c                                           je        0x2b3096 ; loc_2B3096 [RVA 0x2B3096]
002B308A  48 8b 05 ef ab b7 00                            mov       rax, qword ptr [rip + 0xb7abef]
002B3091  48 85 c0                                        test      rax, rax
002B3094  75 07                                           jne       0x2b309d ; loc_2B309D [RVA 0x2B309D]
002B3096  b8 01 00 00 00                                  mov       eax, 1
002B309B  c3                                              ret
002B309C  cc                                              int3
002B309D  48 85 c1                                        test      rcx, rax
002B30A0  b8 00 00 00 00                                  mov       eax, 0
002B30A5  0f 95 c0                                        setne     al
002B30A8  c3                                              ret
002B30A9  cc                                              int3
002B30AA  33 c0                                           xor       eax, eax
002B30AC  c3                                              ret
002B30AD  cc                                              int3
002B30AE  cc                                              int3
002B30AF  cc                                              int3
002B30B0  cc                                              int3
002B30B1  cc                                              int3
002B30B2  cc                                              int3
002B30B3  cc                                              int3
