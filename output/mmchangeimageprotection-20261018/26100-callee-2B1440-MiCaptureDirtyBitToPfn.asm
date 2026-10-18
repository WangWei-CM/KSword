MiCaptureDirtyBitToPfn at RVA 0x2B1440
extent source: next public symbol (first .pdata end 0x2B1462); RVA 0x2B1440..0x2B1500 (exclusive)
shown: 66 instruction(s), max 200
All addresses are RVAs.

002B1440  40 53                                           push      rbx
002B1442  48 83 ec 20                                     sub       rsp, 0x20
002B1446  48 8b d9                                        mov       rbx, rcx
002B1449  45 33 db                                        xor       r11d, r11d
002B144C  0f b6 49 22                                     movzx     ecx, byte ptr [rcx + 0x22]
002B1450  f6 c1 10                                        test      cl, 0x10
002B1453  74 0a                                           je        0x2b145f ; loc_2B145F [RVA 0x2B145F]
002B1455  49 8b c3                                        mov       rax, r11
002B1458  48 83 c4 20                                     add       rsp, 0x20
002B145C  5b                                              pop       rbx
002B145D  c3                                              ret
002B145E  cc                                              int3
002B145F  8b 43 10                                        mov       eax, dword ptr [rbx + 0x10]
002B1462  48 89 7c 24 40                                  mov       qword ptr [rsp + 0x40], rdi
002B1467  48 0f ba e0 0a                                  bt        rax, 0xa
002B146C  72 1a                                           jb        0x2b1488 ; loc_2B1488 [RVA 0x2B1488]
002B146E  f6 c1 08                                        test      cl, 8
002B1471  75 15                                           jne       0x2b1488 ; loc_2B1488 [RVA 0x2B1488]
002B1473  48 8b 43 10                                     mov       rax, qword ptr [rbx + 0x10]
002B1477  48 89 44 24 38                                  mov       qword ptr [rsp + 0x38], rax
002B147C  c0 e8 02                                        shr       al, 2
002B147F  a8 01                                           test      al, 1
002B1481  75 2c                                           jne       0x2b14af ; loc_2B14AF [RVA 0x2B14AF]
002B1483  4c 89 5c 24 38                                  mov       qword ptr [rsp + 0x38], r11
002B1488  8b 7b 20                                        mov       edi, dword ptr [rbx + 0x20]
002B148B  89 7c 24 30                                     mov       dword ptr [rsp + 0x30], edi
002B148F  c1 ef 10                                        shr       edi, 0x10
002B1492  40 f6 c7 08                                     test      dil, 8
002B1496  75 34                                           jne       0x2b14cc ; loc_2B14CC [RVA 0x2B14CC]
002B1498  40 80 cf 10                                     or        dil, 0x10
002B149C  40 88 7c 24 32                                  mov       byte ptr [rsp + 0x32], dil
002B14A1  8b 44 24 30                                     mov       eax, dword ptr [rsp + 0x30]
002B14A5  48 8b 7c 24 40                                  mov       rdi, qword ptr [rsp + 0x40]
002B14AA  89 43 20                                        mov       dword ptr [rbx + 0x20], eax
002B14AD  eb a6                                           jmp       0x2b1455 ; loc_2B1455 [RVA 0x2B1455]
002B14AF  48 8b 43 10                                     mov       rax, qword ptr [rbx + 0x10]
002B14B3  48 8d 4c 24 38                                  lea       rcx, [rsp + 0x38]
002B14B8  48 83 e0 fb                                     and       rax, 0xfffffffffffffffb
002B14BC  48 89 43 10                                     mov       qword ptr [rbx + 0x10], rax
002B14C0  e8 17 4b f9 ff                                  call      0x245fdc ; MiClearPageFileReservation [RVA 0x245FDC]
002B14C5  4c 8b 5c 24 38                                  mov       r11, qword ptr [rsp + 0x38]
002B14CA  eb bc                                           jmp       0x2b1488 ; loc_2B1488 [RVA 0x2B1488]
002B14CC  48 8b cb                                        mov       rcx, rbx
002B14CF  e8 d8 2e 00 00                                  call      0x2b43ac ; MiCanPfnOriginalPteBeLost [RVA 0x2B43AC]
002B14D4  85 c0                                           test      eax, eax
002B14D6  74 c0                                           je        0x2b1498 ; loc_2B1498 [RVA 0x2B1498]
002B14D8  f6 43 22 10                                     test      byte ptr [rbx + 0x22], 0x10
002B14DC  75 ba                                           jne       0x2b1498 ; loc_2B1498 [RVA 0x2B1498]
002B14DE  8b 43 10                                        mov       eax, dword ptr [rbx + 0x10]
002B14E1  c0 e8 02                                        shr       al, 2
002B14E4  a8 01                                           test      al, 1
002B14E6  74 b0                                           je        0x2b1498 ; loc_2B1498 [RVA 0x2B1498]
002B14E8  48 8b 4b 10                                     mov       rcx, qword ptr [rbx + 0x10]
002B14EC  48 83 e1 fb                                     and       rcx, 0xfffffffffffffffb
002B14F0  48 89 4b 10                                     mov       qword ptr [rbx + 0x10], rcx
002B14F4  eb a2                                           jmp       0x2b1498 ; loc_2B1498 [RVA 0x2B1498]
002B14F6  cc                                              int3
002B14F7  cc                                              int3
002B14F8  cc                                              int3
002B14F9  cc                                              int3
002B14FA  cc                                              int3
002B14FB  cc                                              int3
002B14FC  cc                                              int3
002B14FD  cc                                              int3
002B14FE  cc                                              int3
002B14FF  cc                                              int3
