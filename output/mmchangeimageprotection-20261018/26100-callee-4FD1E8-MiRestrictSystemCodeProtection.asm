MiRestrictSystemCodeProtection at RVA 0x4FD1E8
extent source: next public symbol (first .pdata end 0x4FD25F); RVA 0x4FD1E8..0x4FD268 (exclusive)
shown: 45 instruction(s), max 200
All addresses are RVAs.

004FD1E8  41 bb 04 00 00 00                               mov       r11d, 4
004FD1EE  45 8d 53 fd                                     lea       r10d, [r11 - 3]
004FD1F2  41 84 ca                                        test      r10b, cl
004FD1F5  74 23                                           je        0x4fd21a ; loc_4FD21A [RVA 0x4FD21A]
004FD1F7  48 0f ba e1 0b                                  bt        rcx, 0xb
004FD1FC  73 0d                                           jae       0x4fd20b ; loc_4FD20B [RVA 0x4FD20B]
004FD1FE  48 85 c9                                        test      rcx, rcx
004FD201  45 8d 4b 02                                     lea       r9d, [r11 + 2]
004FD205  45 0f 48 cb                                     cmovs     r9d, r11d
004FD209  eb 1a                                           jmp       0x4fd225 ; loc_4FD225 [RVA 0x4FD225]
004FD20B  48 85 c9                                        test      rcx, rcx
004FD20E  41 b9 03 00 00 00                               mov       r9d, 3
004FD214  45 0f 48 ca                                     cmovs     r9d, r10d
004FD218  eb 0b                                           jmp       0x4fd225 ; loc_4FD225 [RVA 0x4FD225]
004FD21A  4c 8b c9                                        mov       r9, rcx
004FD21D  49 c1 e9 05                                     shr       r9, 5
004FD221  41 83 e1 1f                                     and       r9d, 0x1f
004FD225  41 f6 c1 07                                     test      r9b, 7
004FD229  75 08                                           jne       0x4fd233 ; loc_4FD233 [RVA 0x4FD233]
004FD22B  41 b8 18 00 00 00                               mov       r8d, 0x18
004FD231  eb 27                                           jmp       0x4fd25a ; loc_4FD25A [RVA 0x4FD25A]
004FD233  44 8b c2                                        mov       r8d, edx
004FD236  41 83 e0 fd                                     and       r8d, 0xfffffffd
004FD23A  41 f6 c1 02                                     test      r9b, 2
004FD23E  44 0f 45 c2                                     cmovne    r8d, edx
004FD242  41 f6 c0 07                                     test      r8b, 7
004FD246  0f 95 c2                                        setne     dl
004FD249  45 84 cb                                        test      r11b, r9b
004FD24C  0f 94 c1                                        sete      cl
004FD24F  84 d1                                           test      cl, dl
004FD251  74 07                                           je        0x4fd25a ; loc_4FD25A [RVA 0x4FD25A]
004FD253  41 83 e0 02                                     and       r8d, 2
004FD257  45 0b c2                                        or        r8d, r10d
004FD25A  41 8b c0                                        mov       eax, r8d
004FD25D  c3                                              ret
004FD25E  cc                                              int3
004FD25F  cc                                              int3
004FD260  cc                                              int3
004FD261  cc                                              int3
004FD262  cc                                              int3
004FD263  cc                                              int3
004FD264  cc                                              int3
004FD265  cc                                              int3
004FD266  cc                                              int3
004FD267  cc                                              int3
