MiLockLoadedDataTableEntryIfNecessary: 14 CDB-discovered instructions verified against the local PE
RVA ranges (end exclusive): 0x27C2D8..0x27C304, 0x27C305..0x27C30F
All addresses below are RVAs; direct calls use PDB public-symbol names.

0027C2D8  48 83 ec 28                                     sub       rsp, 0x28
0027C2DC  65 4c 8b 04 25 88 01 00 00                      mov       r8, qword ptr gs:[0x188]
0027C2E5  48 8b 05 24 16 bb 00                            mov       rax, qword ptr [rip + 0xbb1624]
0027C2EC  49 3b c0                                        cmp       rax, r8
0027C2EF  74 14                                           je        0x27c305 ; loc_27C305 [RVA 0x27C305]
0027C2F1  b8 01 00 00 00                                  mov       eax, 1
0027C2F6  89 02                                           mov       dword ptr [rdx], eax
0027C2F8  8b d0                                           mov       edx, eax
0027C2FA  e8 19 00 00 00                                  call      0x27c318 ; MiLockLoadedDataTableEntry [RVA 0x27C318]
0027C2FF  48 83 c4 28                                     add       rsp, 0x28
0027C303  c3                                              ret
0027C305  83 22 00                                        and       dword ptr [rdx], 0
0027C308  e8 1f 03 00 00                                  call      0x27c62c ; MmFindDataTableEntryByAddress [RVA 0x27C62C]
0027C30D  eb f0                                           jmp       0x27c2ff ; loc_27C2FF [RVA 0x27C2FF]
