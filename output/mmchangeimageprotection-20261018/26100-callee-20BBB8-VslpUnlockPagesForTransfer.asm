VslpUnlockPagesForTransfer at RVA 0x20BBB8
extent source: next public symbol (first .pdata end 0x20BC43); RVA 0x20BBB8..0x20BC50 (exclusive)
shown: 56 instruction(s), max 200
All addresses are RVAs.

0020BBB8  48 89 5c 24 08                                  mov       qword ptr [rsp + 8], rbx
0020BBBD  57                                              push      rdi
0020BBBE  48 83 ec 20                                     sub       rsp, 0x20
0020BBC2  f6 41 12 02                                     test      byte ptr [rcx + 0x12], 2
0020BBC6  48 8b d9                                        mov       rbx, rcx
0020BBC9  74 09                                           je        0x20bbd4 ; loc_20BBD4 [RVA 0x20BBD4]
0020BBCB  48 83 c1 08                                     add       rcx, 8
0020BBCF  e8 7c 00 00 00                                  call      0x20bc50 ; MmUnlockPages [RVA 0x20BC50]
0020BBD4  8b 43 40                                        mov       eax, dword ptr [rbx + 0x40]
0020BBD7  a8 08                                           test      al, 8
0020BBD9  74 2c                                           je        0x20bc07 ; loc_20BC07 [RVA 0x20BC07]
0020BBDB  48 8b 3b                                        mov       rdi, qword ptr [rbx]
0020BBDE  48 85 ff                                        test      rdi, rdi
0020BBE1  74 24                                           je        0x20bc07 ; loc_20BC07 [RVA 0x20BC07]
0020BBE3  f6 47 0a 02                                     test      byte ptr [rdi + 0xa], 2
0020BBE7  74 08                                           je        0x20bbf1 ; loc_20BBF1 [RVA 0x20BBF1]
0020BBE9  48 8b cf                                        mov       rcx, rdi
0020BBEC  e8 5f 00 00 00                                  call      0x20bc50 ; MmUnlockPages [RVA 0x20BC50]
0020BBF1  8b 43 40                                        mov       eax, dword ptr [rbx + 0x40]
0020BBF4  48 8b cf                                        mov       rcx, rdi
0020BBF7  48 8b 3f                                        mov       rdi, qword ptr [rdi]
0020BBFA  a8 20                                           test      al, 0x20
0020BBFC  75 e0                                           jne       0x20bbde ; loc_20BBDE [RVA 0x20BBDE]
0020BBFE  33 d2                                           xor       edx, edx
0020BC00  e8 6b 1c 97 00                                  call      0xb7d870 ; ExFreePoolWithTag [RVA 0xB7D870]
0020BC05  eb d7                                           jmp       0x20bbde ; loc_20BBDE [RVA 0x20BBDE]
0020BC07  8b 43 40                                        mov       eax, dword ptr [rbx + 0x40]
0020BC0A  a8 20                                           test      al, 0x20
0020BC0C  75 0c                                           jne       0x20bc1a ; loc_20BC1A [RVA 0x20BC1A]
0020BC0E  48 8b 5c 24 30                                  mov       rbx, qword ptr [rsp + 0x30]
0020BC13  48 83 c4 20                                     add       rsp, 0x20
0020BC17  5f                                              pop       rdi
0020BC18  c3                                              ret
0020BC19  cc                                              int3
0020BC1A  48 83 c8 ff                                     or        rax, 0xffffffffffffffff
0020BC1E  f0 48 0f c1 05 91 22 d8 00                      lock xadd qword ptr [rip + 0xd82291], rax
0020BC27  24 06                                           and       al, 6
0020BC29  3c 02                                           cmp       al, 2
0020BC2B  0f 85 8f 4d 4c 00                               jne       0x6d09c0 ; sub_6D09C0 [RVA 0x6D09C0]
0020BC31  48 8d 0d 80 22 d8 00                            lea       rcx, [rip + 0xd82280]
0020BC38  e8 33 f7 06 00                                  call      0x27b370 ; ExfTryToWakePushLock [RVA 0x27B370]
0020BC3D  90                                              nop
0020BC3E  e9 7d 4d 4c 00                                  jmp       0x6d09c0 ; sub_6D09C0 [RVA 0x6D09C0]
0020BC43  cc                                              int3
0020BC44  cc                                              int3
0020BC45  cc                                              int3
0020BC46  cc                                              int3
0020BC47  cc                                              int3
0020BC48  cc                                              int3
0020BC49  cc                                              int3
0020BC4A  cc                                              int3
0020BC4B  cc                                              int3
0020BC4C  cc                                              int3
0020BC4D  cc                                              int3
0020BC4E  cc                                              int3
0020BC4F  cc                                              int3
