MmUnlockLoadedDataTableEntry: 12 CDB-discovered instructions verified against the local PE
RVA ranges (end exclusive): 0x27BC50..0x27BC75, 0x27BC76..0x27BC81
All addresses below are RVAs; direct calls use PDB public-symbol names.

0027BC50  48 83 ec 28                                     sub       rsp, 0x28
0027BC54  33 d2                                           xor       edx, edx
0027BC56  e8 e9 fd ff ff                                  call      0x27ba44 ; MiUnlockLoaderEntry [RVA 0x27BA44]
0027BC5B  65 48 8b 0c 25 88 01 00 00                      mov       rcx, qword ptr gs:[0x188]
0027BC64  48 85 c9                                        test      rcx, rcx
0027BC67  74 0d                                           je        0x27bc76 ; loc_27BC76 [RVA 0x27BC76]
0027BC69  33 d2                                           xor       edx, edx
0027BC6B  e8 18 00 00 00                                  call      0x27bc88 ; MiReleaseLoadLock [RVA 0x27BC88]
0027BC70  48 83 c4 28                                     add       rsp, 0x28
0027BC74  c3                                              ret
0027BC76  65 48 8b 0c 25 88 01 00 00                      mov       rcx, qword ptr gs:[0x188]
0027BC7F  eb e8                                           jmp       0x27bc69 ; loc_27BC69 [RVA 0x27BC69]
