MiWriteValidPteNewProtection: 88 CDB-discovered instructions verified against the local PE
RVA ranges (end exclusive): 0x2955F0..0x2956A5, 0x2956A6..0x295773
All addresses below are RVAs; direct calls use PDB public-symbol names.

002955F0  48 89 5c 24 10                                  mov       qword ptr [rsp + 0x10], rbx
002955F5  48 89 74 24 18                                  mov       qword ptr [rsp + 0x18], rsi
002955FA  57                                              push      rdi
002955FB  48 83 ec 20                                     sub       rsp, 0x20
002955FF  48 8b 19                                        mov       rbx, qword ptr [rcx]
00295602  48 8b f9                                        mov       rdi, rcx
00295605  49 bb 00 d0 be 7d fb f6 ff ff                   movabs    r11, 0xfffff6fb7dbed000
0029560F  49 8b c3                                        mov       rax, r11
00295612  49 b9 f8 d7 be 7d fb f6 ff ff                   movabs    r9, 0xfffff6fb7dbed7f8
0029561C  48 3b c8                                        cmp       rcx, rax
0029561F  72 22                                           jb        0x295643 ; loc_295643 [RVA 0x295643]
00295621  49 8b c1                                        mov       rax, r9
00295624  48 3b c8                                        cmp       rcx, rax
00295627  77 1a                                           ja        0x295643 ; loc_295643 [RVA 0x295643]
00295629  48 8b c3                                        mov       rax, rbx
0029562C  f6 c3 01                                        test      bl, 1
0029562F  74 12                                           je        0x295643 ; loc_295643 [RVA 0x295643]
00295631  f6 c3 42                                        test      bl, 0x42
00295634  41 0f 95 c0                                     setne     r8b
00295638  f6 c3 20                                        test      bl, 0x20
0029563B  0f 95 c1                                        setne     cl
0029563E  44 84 c1                                        test      cl, r8b
00295641  74 63                                           je        0x2956a6 ; loc_2956A6 [RVA 0x2956A6]
00295643  48 b8 00 00 00 00 00 00 00 7f                   movabs    rax, 0x7f00000000000000
0029564D  33 f6                                           xor       esi, esi
0029564F  48 23 d8                                        and       rbx, rax
00295652  48 b8 ff ff ff ff ff ff ff 80                   movabs    rax, 0x80ffffffffffffff
0029565C  48 23 d0                                        and       rdx, rax
0029565F  48 0b da                                        or        rbx, rdx
00295662  48 89 5c 24 30                                  mov       qword ptr [rsp + 0x30], rbx
00295667  49 8b c3                                        mov       rax, r11
0029566A  48 3b f8                                        cmp       rdi, rax
0029566D  72 0c                                           jb        0x29567b ; loc_29567B [RVA 0x29567B]
0029566F  49 8b c1                                        mov       rax, r9
00295672  48 3b f8                                        cmp       rdi, rax
00295675  0f 86 d2 00 00 00                               jbe       0x29574d ; loc_29574D [RVA 0x29574D]
0029567B  48 0f ba 25 44 04 d3 00 24                      bt        qword ptr [rip + 0xd30444], 0x24
00295684  0f 82 84 00 00 00                               jb        0x29570e ; loc_29570E [RVA 0x29570E]
0029568A  48 89 1f                                        mov       qword ptr [rdi], rbx
0029568D  85 f6                                           test      esi, esi
0029568F  0f 85 ce 00 00 00                               jne       0x295763 ; loc_295763 [RVA 0x295763]
00295695  48 8b 5c 24 38                                  mov       rbx, qword ptr [rsp + 0x38]
0029569A  48 8b 74 24 40                                  mov       rsi, qword ptr [rsp + 0x40]
0029569F  48 83 c4 20                                     add       rsp, 0x20
002956A3  5f                                              pop       rdi
002956A4  c3                                              ret
002956A6  48 f7 05 17 04 d3 00 00 00 60 00                test      qword ptr [rip + 0xd30417], 0x600000
002956B1  74 90                                           je        0x295643 ; loc_295643 [RVA 0x295643]
002956B3  65 48 8b 0c 25 88 01 00 00                      mov       rcx, qword ptr gs:[0x188]
002956BC  4c 8b 81 b8 00 00 00                            mov       r8, qword ptr [rcx + 0xb8]
002956C3  41 80 b8 60 01 00 00 01                         cmp       byte ptr [r8 + 0x160], 1
002956CB  0f 84 72 ff ff ff                               je        0x295643 ; loc_295643 [RVA 0x295643]
002956D1  4d 8b 90 08 05 00 00                            mov       r10, qword ptr [r8 + 0x508]
002956D8  4d 85 d2                                        test      r10, r10
002956DB  0f 84 62 ff ff ff                               je        0x295643 ; loc_295643 [RVA 0x295643]
002956E1  48 8b cf                                        mov       rcx, rdi
002956E4  48 c1 e9 03                                     shr       rcx, 3
002956E8  81 e1 ff 01 00 00                               and       ecx, 0x1ff
002956EE  49 8b 0c ca                                     mov       rcx, qword ptr [r10 + rcx*8]
002956F2  f6 c1 20                                        test      cl, 0x20
002956F5  74 04                                           je        0x2956fb ; loc_2956FB [RVA 0x2956FB]
002956F7  48 83 c8 20                                     or        rax, 0x20
002956FB  48 8b d8                                        mov       rbx, rax
002956FE  48 83 cb 42                                     or        rbx, 0x42
00295702  f6 c1 42                                        test      cl, 0x42
00295705  48 0f 44 d8                                     cmove     rbx, rax
00295709  e9 35 ff ff ff                                  jmp       0x295643 ; loc_295643 [RVA 0x295643]
0029570E  f6 c3 20                                        test      bl, 0x20
00295711  0f 85 73 ff ff ff                               jne       0x29568a ; loc_29568A [RVA 0x29568A]
00295717  48 b9 00 00 00 00 80 f6 ff ff                   movabs    rcx, 0xfffff68000000000
00295721  48 b8 00 00 00 00 40 00 00 00                   movabs    rax, 0x4000000000
0029572B  48 03 c8                                        add       rcx, rax
0029572E  48 3b f9                                        cmp       rdi, rcx
00295731  0f 82 53 ff ff ff                               jb        0x29568a ; loc_29568A [RVA 0x29568A]
00295737  41 b8 80 00 00 00                               mov       r8d, 0x80
0029573D  48 8b d3                                        mov       rdx, rbx
00295740  48 8b cf                                        mov       rcx, rdi
00295743  e8 18 14 01 00                                  call      0x2a6b60 ; MiCheckLinearProtectedPteAccessedBit [RVA 0x2A6B60]
00295748  e9 3d ff ff ff                                  jmp       0x29568a ; loc_29568A [RVA 0x29568A]
0029574D  48 8d 54 24 30                                  lea       rdx, [rsp + 0x30]
00295752  e8 f1 31 01 00                                  call      0x2a8948 ; MiSanitizeShadowPxe [RVA 0x2A8948]
00295757  48 8b 5c 24 30                                  mov       rbx, qword ptr [rsp + 0x30]
0029575C  8b f0                                           mov       esi, eax
0029575E  e9 18 ff ff ff                                  jmp       0x29567b ; loc_29567B [RVA 0x29567B]
00295763  48 8b d3                                        mov       rdx, rbx
00295766  48 8b cf                                        mov       rcx, rdi
00295769  e8 1e 35 01 00                                  call      0x2a8c8c ; MiWritePteShadow [RVA 0x2A8C8C]
0029576E  e9 22 ff ff ff                                  jmp       0x295695 ; loc_295695 [RVA 0x295695]
