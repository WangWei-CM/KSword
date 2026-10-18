VslpEnterIumSecureMode: 276 CDB-discovered instructions verified against the local PE
RVA ranges (end exclusive): 0x2F8C60..0x2F8EEB, 0x2F8EEC..0x2F90DF, 0x6D72BC..0x6D72C8
All addresses below are RVAs; direct calls use PDB public-symbol names.

002F8C60  40 53                                           push      rbx
002F8C62  56                                              push      rsi
002F8C63  57                                              push      rdi
002F8C64  41 54                                           push      r12
002F8C66  41 55                                           push      r13
002F8C68  41 56                                           push      r14
002F8C6A  41 57                                           push      r15
002F8C6C  48 83 ec 70                                     sub       rsp, 0x70
002F8C70  48 8b 05 c9 1a b1 00                            mov       rax, qword ptr [rip + 0xb11ac9]
002F8C77  48 33 c4                                        xor       rax, rsp
002F8C7A  48 89 44 24 68                                  mov       qword ptr [rsp + 0x68], rax
002F8C7F  40 32 f6                                        xor       sil, sil
002F8C82  66 89 54 24 32                                  mov       word ptr [rsp + 0x32], dx
002F8C87  40 32 ff                                        xor       dil, dil
002F8C8A  44 0f b6 f1                                     movzx     r14d, cl
002F8C8E  48 83 3d ca 8b f0 00 00                         cmp       qword ptr [rip + 0xf08bca], 0
002F8C96  41 b5 0f                                        mov       r13b, 0xf
002F8C99  49 8b d9                                        mov       rbx, r9
002F8C9C  40 88 74 24 30                                  mov       byte ptr [rsp + 0x30], sil
002F8CA1  45 8b e0                                        mov       r12d, r8d
002F8CA4  4c 89 6c 24 40                                  mov       qword ptr [rsp + 0x40], r13
002F8CA9  44 0f b7 fa                                     movzx     r15d, dx
002F8CAD  0f 84 27 03 00 00                               je        0x2f8fda ; loc_2F8FDA [RVA 0x2F8FDA]
002F8CB3  45 88 31                                        mov       byte ptr [r9], r14b
002F8CB6  66 41 89 51 02                                  mov       word ptr [r9 + 2], dx
002F8CBB  65 48 8b 04 25 88 01 00 00                      mov       rax, qword ptr gs:[0x188]
002F8CC4  48 89 ac 24 c0 00 00 00                         mov       qword ptr [rsp + 0xc0], rbp
002F8CCC  8b a8 10 05 00 00                               mov       ebp, dword ptr [rax + 0x510]
002F8CD2  89 6c 24 3c                                     mov       dword ptr [rsp + 0x3c], ebp
002F8CD6  41 80 fe 02                                     cmp       r14b, 2
002F8CDA  0f 87 4d 03 00 00                               ja        0x2f902d ; loc_2F902D [RVA 0x2F902D]
002F8CE0  e8 0b f7 03 00                                  call      0x3383f0 ; KeAreInterruptsEnabled [RVA 0x3383F0]
002F8CE5  84 c0                                           test      al, al
002F8CE7  0f 84 7f 03 00 00                               je        0x2f906c ; loc_2F906C [RVA 0x2F906C]
002F8CED  65 48 8b 2c 25 88 01 00 00                      mov       rbp, qword ptr gs:[0x188]
002F8CF6  44 0f 20 c0                                     mov       rax, cr8
002F8CFA  45 33 c0                                        xor       r8d, r8d
002F8CFD  ba 01 00 00 00                                  mov       edx, 1
002F8D02  44 3a f2                                        cmp       r14b, dl
002F8D05  0f 84 14 02 00 00                               je        0x2f8f1f ; loc_2F8F1F [RVA 0x2F8F1F]
002F8D0B  48 8d 8d 1c 03 00 00                            lea       rcx, [rbp + 0x31c]
002F8D12  45 85 e4                                        test      r12d, r12d
002F8D15  0f 85 09 03 00 00                               jne       0x2f9024 ; loc_2F9024 [RVA 0x2F9024]
002F8D1B  3c 02                                           cmp       al, 2
002F8D1D  0f 82 7d 02 00 00                               jb        0x2f8fa0 ; loc_2F8FA0 [RVA 0x2F8FA0]
002F8D23  44 89 43 04                                     mov       dword ptr [rbx + 4], r8d
002F8D27  44 8b e2                                        mov       r12d, edx
002F8D2A  44 39 01                                        cmp       dword ptr [rcx], r8d
002F8D2D  40 0f b6 f6                                     movzx     esi, sil
002F8D31  0f 44 f2                                        cmove     esi, edx
002F8D34  3c 01                                           cmp       al, 1
002F8D36  0f 84 b0 01 00 00                               je        0x2f8eec ; loc_2F8EEC [RVA 0x2F8EEC]
002F8D3C  3c 02                                           cmp       al, 2
002F8D3E  0f 83 65 03 00 00                               jae       0x2f90a9 ; loc_2F90A9 [RVA 0x2F90A9]
002F8D44  40 84 f6                                        test      sil, sil
002F8D47  74 08                                           je        0x2f8d51 ; loc_2F8D51 [RVA 0x2F8D51]
002F8D49  66 ff 8d e4 01 00 00                            dec       word ptr [rbp + 0x1e4]
002F8D50  90                                              nop
002F8D51  40 84 ff                                        test      dil, dil
002F8D54  0f 85 ce 01 00 00                               jne       0x2f8f28 ; loc_2F8F28 [RVA 0x2F8F28]
002F8D5A  8b 05 f4 cd cc 00                               mov       eax, dword ptr [rip + 0xcccdf4]
002F8D60  a8 08                                           test      al, 8
002F8D62  0f 84 92 02 00 00                               je        0x2f8ffa ; loc_2F8FFA [RVA 0x2F8FFA]
002F8D68  48 8d 44 24 34                                  lea       rax, [rsp + 0x34]
002F8D6D  66 44 89 7c 24 36                               mov       word ptr [rsp + 0x36], r15w
002F8D73  41 b9 49 05 00 00                               mov       r9d, 0x549
002F8D79  48 89 44 24 48                                  mov       qword ptr [rsp + 0x48], rax
002F8D7E  41 b8 08 00 00 a0                               mov       r8d, 0xa0000008
002F8D84  66 44 89 74 24 34                               mov       word ptr [rsp + 0x34], r14w
002F8D8A  48 8d 4c 24 48                                  lea       rcx, [rsp + 0x48]
002F8D8F  48 c7 44 24 50 04 00 00 00                      mov       qword ptr [rsp + 0x50], 4
002F8D98  c7 44 24 20 02 19 50 00                         mov       dword ptr [rsp + 0x20], 0x501902
002F8DA0  e8 bb f3 ff ff                                  call      0x2f8160 ; EtwTraceKernelEvent [RVA 0x2F8160]
002F8DA5  44 8b 44 24 3c                                  mov       r8d, dword ptr [rsp + 0x3c]
002F8DAA  48 8b d3                                        mov       rdx, rbx
002F8DAD  33 c9                                           xor       ecx, ecx
002F8DAF  e8 7c db 3a 00                                  call      0x6a6930 ; HvlSwitchToVsmVtl1 [RVA 0x6A6930]
002F8DB4  0f b7 44 24 32                                  movzx     eax, word ptr [rsp + 0x32]
002F8DB9  48 8d 4c 24 58                                  lea       rcx, [rsp + 0x58]
002F8DBE  44 8b 7b 08                                     mov       r15d, dword ptr [rbx + 8]
002F8DC2  33 ff                                           xor       edi, edi
002F8DC4  66 89 44 24 3a                                  mov       word ptr [rsp + 0x3a], ax
002F8DC9  41 b9 4a 05 00 00                               mov       r9d, 0x54a
002F8DCF  48 8d 44 24 38                                  lea       rax, [rsp + 0x38]
002F8DD4  66 44 89 74 24 38                               mov       word ptr [rsp + 0x38], r14w
002F8DDA  41 b8 08 00 00 a0                               mov       r8d, 0xa0000008
002F8DE0  48 89 44 24 58                                  mov       qword ptr [rsp + 0x58], rax
002F8DE5  8d 57 01                                        lea       edx, [rdi + 1]
002F8DE8  48 c7 44 24 60 04 00 00 00                      mov       qword ptr [rsp + 0x60], 4
002F8DF1  c7 44 24 20 02 19 50 00                         mov       dword ptr [rsp + 0x20], 0x501902
002F8DF9  e8 62 f3 ff ff                                  call      0x2f8160 ; EtwTraceKernelEvent [RVA 0x2F8160]
002F8DFE  0f b6 43 01                                     movzx     eax, byte ptr [rbx + 1]
002F8E02  84 c0                                           test      al, al
002F8E04  79 09                                           jns       0x2f8e0f ; loc_2F8E0F [RVA 0x2F8E0F]
002F8E06  cc                                              int3
002F8E07  80 63 01 7f                                     and       byte ptr [rbx + 1], 0x7f
002F8E0B  0f b6 43 01                                     movzx     eax, byte ptr [rbx + 1]
002F8E0F  3c 01                                           cmp       al, 1
002F8E11  74 79                                           je        0x2f8e8c ; loc_2F8E8C [RVA 0x2F8E8C]
002F8E13  3c 06                                           cmp       al, 6
002F8E15  74 5d                                           je        0x2f8e74 ; loc_2F8E74 [RVA 0x2F8E74]
002F8E17  41 80 fe 01                                     cmp       r14b, 1
002F8E1B  74 05                                           je        0x2f8e22 ; loc_2F8E22 [RVA 0x2F8E22]
002F8E1D  45 85 e4                                        test      r12d, r12d
002F8E20  75 09                                           jne       0x2f8e2b ; loc_2F8E2B [RVA 0x2F8E2B]
002F8E22  8b 43 04                                        mov       eax, dword ptr [rbx + 4]
002F8E25  89 85 1c 03 00 00                               mov       dword ptr [rbp + 0x31c], eax
002F8E2B  0f b6 4b 01                                     movzx     ecx, byte ptr [rbx + 1]
002F8E2F  80 f9 03                                        cmp       cl, 3
002F8E32  0f 85 bc 00 00 00                               jne       0x2f8ef4 ; loc_2F8EF4 [RVA 0x2F8EF4]
002F8E38  0f b7 43 02                                     movzx     eax, word ptr [rbx + 2]
002F8E3C  3b 05 8e 8a f0 00                               cmp       eax, dword ptr [rip + 0xf08a8e]
002F8E42  0f 82 0b 01 00 00                               jb        0x2f8f53 ; loc_2F8F53 [RVA 0x2F8F53]
002F8E48  48 c7 43 08 1c 00 00 c0                         mov       qword ptr [rbx + 8], 0xffffffffc000001c
002F8E50  45 33 c0                                        xor       r8d, r8d
002F8E53  45 85 e4                                        test      r12d, r12d
002F8E56  0f 84 88 01 00 00                               je        0x2f8fe4 ; loc_2F8FE4 [RVA 0x2F8FE4]
002F8E5C  c6 03 00                                        mov       byte ptr [rbx], 0
002F8E5F  66 44 89 43 02                                  mov       word ptr [rbx + 2], r8w
002F8E64  44 0f b7 7c 24 32                               movzx     r15d, word ptr [rsp + 0x32]
002F8E6A  ba 01 00 00 00                                  mov       edx, 1
002F8E6F  e9 e6 fe ff ff                                  jmp       0x2f8d5a ; loc_2F8D5A [RVA 0x2F8D5A]
002F8E74  83 3d e5 ce cc 00 00                            cmp       dword ptr [rip + 0xcccee5], 0
002F8E7B  74 0b                                           je        0x2f8e88 ; loc_2F8E88 [RVA 0x2F8E88]
002F8E7D  44 0f 20 c1                                     mov       rcx, cr8
002F8E81  33 d2                                           xor       edx, edx
002F8E83  e8 50 c5 1f 00                                  call      0x4f53d8 ; KiLowerIrqlProcessIrqlFlags [RVA 0x4F53D8]
002F8E88  44 0f 22 c7                                     mov       cr8, rdi
002F8E8C  80 7c 24 30 00                                  cmp       byte ptr [rsp + 0x30], 0
002F8E91  74 15                                           je        0x2f8ea8 ; loc_2F8EA8 [RVA 0x2F8EA8]
002F8E93  45 33 c9                                        xor       r9d, r9d
002F8E96  48 8d 0d 63 4f c9 00                            lea       rcx, [rip + 0xc94f63]
002F8E9D  33 d2                                           xor       edx, edx
002F8E9F  45 8d 41 01                                     lea       r8d, [r9 + 1]
002F8EA3  e8 e8 9f f6 ff                                  call      0x262e90 ; KeReleaseSemaphore [RVA 0x262E90]
002F8EA8  40 84 f6                                        test      sil, sil
002F8EAB  74 08                                           je        0x2f8eb5 ; loc_2F8EB5 [RVA 0x2F8EB5]
002F8EAD  48 8b cd                                        mov       rcx, rbp
002F8EB0  e8 6b b6 f7 ff                                  call      0x274520 ; KeLeaveCriticalRegionThread [RVA 0x274520]
002F8EB5  48 8b 5c 24 40                                  mov       rbx, qword ptr [rsp + 0x40]
002F8EBA  80 fb 0f                                        cmp       bl, 0xf
002F8EBD  0f 85 fd 01 00 00                               jne       0x2f90c0 ; loc_2F90C0 [RVA 0x2F90C0]
002F8EC3  41 8b c7                                        mov       eax, r15d
002F8EC6  48 8b ac 24 c0 00 00 00                         mov       rbp, qword ptr [rsp + 0xc0]
002F8ECE  48 8b 4c 24 68                                  mov       rcx, qword ptr [rsp + 0x68]
002F8ED3  48 33 cc                                        xor       rcx, rsp
002F8ED6  e8 b5 d1 3a 00                                  call      0x6a6090 ; _security_check_cookie [RVA 0x6A6090]
002F8EDB  48 83 c4 70                                     add       rsp, 0x70
002F8EDF  41 5f                                           pop       r15
002F8EE1  41 5e                                           pop       r14
002F8EE3  41 5d                                           pop       r13
002F8EE5  41 5c                                           pop       r12
002F8EE7  5f                                              pop       rdi
002F8EE8  5e                                              pop       rsi
002F8EE9  5b                                              pop       rbx
002F8EEA  c3                                              ret
002F8EEC  40 b6 01                                        mov       sil, 1
002F8EEF  e9 55 fe ff ff                                  jmp       0x2f8d49 ; loc_2F8D49 [RVA 0x2F8D49]
002F8EF4  84 c9                                           test      cl, cl
002F8EF6  0f 84 d1 00 00 00                               je        0x2f8fcd ; loc_2F8FCD [RVA 0x2F8FCD]
002F8EFC  80 f9 02                                        cmp       cl, 2
002F8EFF  0f 85 bf 00 00 00                               jne       0x2f8fc4 ; loc_2F8FC4 [RVA 0x2F8FC4]
002F8F05  80 bd 32 02 00 00 00                            cmp       byte ptr [rbp + 0x232], 0
002F8F0C  0f 85 26 ff ff ff                               jne       0x2f8e38 ; loc_2F8E38 [RVA 0x2F8E38]
002F8F12  48 c7 43 08 30 00 00 c0                         mov       qword ptr [rbx + 8], 0xffffffffc0000030
002F8F1A  e9 31 ff ff ff                                  jmp       0x2f8e50 ; loc_2F8E50 [RVA 0x2F8E50]
002F8F1F  44 89 63 04                                     mov       dword ptr [rbx + 4], r12d
002F8F23  e9 0c fe ff ff                                  jmp       0x2f8d34 ; loc_2F8D34 [RVA 0x2F8D34]
002F8F28  45 33 c9                                        xor       r9d, r9d
002F8F2B  4c 89 44 24 20                                  mov       qword ptr [rsp + 0x20], r8
002F8F30  33 d2                                           xor       edx, edx
002F8F32  48 8d 0d c7 4e c9 00                            lea       rcx, [rip + 0xc94ec7]
002F8F39  e8 b2 d8 f1 ff                                  call      0x2167f0 ; KeWaitForSingleObject [RVA 0x2167F0]
002F8F3E  44 8b f8                                        mov       r15d, eax
002F8F41  85 c0                                           test      eax, eax
002F8F43  0f 88 5f ff ff ff                               js        0x2f8ea8 ; loc_2F8EA8 [RVA 0x2F8EA8]
002F8F49  c6 44 24 30 01                                  mov       byte ptr [rsp + 0x30], 1
002F8F4E  e9 11 ff ff ff                                  jmp       0x2f8e64 ; loc_2F8E64 [RVA 0x2F8E64]
002F8F53  0f b6 bd 32 02 00 00                            movzx     edi, byte ptr [rbp + 0x232]
002F8F5A  80 f9 03                                        cmp       cl, 3
002F8F5D  75 07                                           jne       0x2f8f66 ; loc_2F8F66 [RVA 0x2F8F66]
002F8F5F  c6 85 32 02 00 00 00                            mov       byte ptr [rbp + 0x232], 0
002F8F66  48 8b 15 53 89 f0 00                            mov       rdx, qword ptr [rip + 0xf08953]
002F8F6D  0f b7 43 02                                     movzx     eax, word ptr [rbx + 2]
002F8F71  8b 0c 82                                        mov       ecx, dword ptr [rdx + rax*4]
002F8F74  44 8b c1                                        mov       r8d, ecx
002F8F77  c1 f9 04                                        sar       ecx, 4
002F8F7A  41 83 e0 0f                                     and       r8d, 0xf
002F8F7E  48 63 c9                                        movsxd    rcx, ecx
002F8F81  48 03 ca                                        add       rcx, rdx
002F8F84  48 8d 53 08                                     lea       rdx, [rbx + 8]
002F8F88  e8 13 db 3a 00                                  call      0x6a6aa0 ; VslpDispatchIumSyscall [RVA 0x6A6AA0]
002F8F8D  48 63 c8                                        movsxd    rcx, eax
002F8F90  48 89 4b 08                                     mov       qword ptr [rbx + 8], rcx
002F8F94  40 88 bd 32 02 00 00                            mov       byte ptr [rbp + 0x232], dil
002F8F9B  e9 b0 fe ff ff                                  jmp       0x2f8e50 ; loc_2F8E50 [RVA 0x2F8E50]
002F8FA0  8b 11                                           mov       edx, dword ptr [rcx]
002F8FA2  85 d2                                           test      edx, edx
002F8FA4  75 6e                                           jne       0x2f9014 ; loc_2F9014 [RVA 0x2F9014]
002F8FA6  44 89 43 04                                     mov       dword ptr [rbx + 4], r8d
002F8FAA  ba 01 00 00 00                                  mov       edx, 1
002F8FAF  40 38 35 0e cb cc 00                            cmp       byte ptr [rip + 0xcccb0e], sil
002F8FB6  0f 84 6e fd ff ff                               je        0x2f8d2a ; loc_2F8D2A [RVA 0x2F8D2A]
002F8FBC  0f b6 fa                                        movzx     edi, dl
002F8FBF  e9 66 fd ff ff                                  jmp       0x2f8d2a ; loc_2F8D2A [RVA 0x2F8D2A]
002F8FC4  80 f9 05                                        cmp       cl, 5
002F8FC7  0f 85 83 fe ff ff                               jne       0x2f8e50 ; loc_2F8E50 [RVA 0x2F8E50]
002F8FCD  48 8b cb                                        mov       rcx, rbx
002F8FD0  e8 cb e3 1a 00                                  call      0x4a73a0 ; PsDispatchIumService [RVA 0x4A73A0]
002F8FD5  e9 76 fe ff ff                                  jmp       0x2f8e50 ; loc_2F8E50 [RVA 0x2F8E50]
002F8FDA  b8 9d 00 00 c0                                  mov       eax, 0xc000009d
002F8FDF  e9 ea fe ff ff                                  jmp       0x2f8ece ; loc_2F8ECE [RVA 0x2F8ECE]
002F8FE4  41 80 fe 01                                     cmp       r14b, 1
002F8FE8  0f 84 6e fe ff ff                               je        0x2f8e5c ; loc_2F8E5C [RVA 0x2F8E5C]
002F8FEE  44 89 85 1c 03 00 00                            mov       dword ptr [rbp + 0x31c], r8d
002F8FF5  e9 62 fe ff ff                                  jmp       0x2f8e5c ; loc_2F8E5C [RVA 0x2F8E5C]
002F8FFA  44 8b 44 24 3c                                  mov       r8d, dword ptr [rsp + 0x3c]
002F8FFF  48 8b d3                                        mov       rdx, rbx
002F9002  33 c9                                           xor       ecx, ecx
002F9004  e8 27 d9 3a 00                                  call      0x6a6930 ; HvlSwitchToVsmVtl1 [RVA 0x6A6930]
002F9009  44 8b 7b 08                                     mov       r15d, dword ptr [rbx + 8]
002F900D  33 ff                                           xor       edi, edi
002F900F  e9 ea fd ff ff                                  jmp       0x2f8dfe ; loc_2F8DFE [RVA 0x2F8DFE]
002F9014  44 8b e2                                        mov       r12d, edx
002F9017  89 53 04                                        mov       dword ptr [rbx + 4], edx
002F901A  ba 01 00 00 00                                  mov       edx, 1
002F901F  e9 06 fd ff ff                                  jmp       0x2f8d2a ; loc_2F8D2A [RVA 0x2F8D2A]
002F9024  44 89 63 04                                     mov       dword ptr [rbx + 4], r12d
002F9028  e9 fd fc ff ff                                  jmp       0x2f8d2a ; loc_2F8D2A [RVA 0x2F8D2A]
002F902D  8b 05 21 cb cc 00                               mov       eax, dword ptr [rip + 0xcccb21]
002F9033  a8 08                                           test      al, 8
002F9035  74 7a                                           je        0x2f90b1 ; loc_2F90B1 [RVA 0x2F90B1]
002F9037  41 0f b7 ce                                     movzx     ecx, r14w
002F903B  e8 1c 90 1b 00                                  call      0x4b205c ; EtwTraceEnterVtl1 [RVA 0x4B205C]
002F9040  44 8b c5                                        mov       r8d, ebp
002F9043  48 8b d3                                        mov       rdx, rbx
002F9046  33 c9                                           xor       ecx, ecx
002F9048  e8 e3 d8 3a 00                                  call      0x6a6930 ; HvlSwitchToVsmVtl1 [RVA 0x6A6930]
002F904D  41 0f b7 d7                                     movzx     edx, r15w
002F9051  41 0f b7 ce                                     movzx     ecx, r14w
002F9055  e8 d6 b7 1b 00                                  call      0x4b4830 ; EtwTraceExitVtl1 [RVA 0x4B4830]
002F905A  40 38 73 01                                     cmp       byte ptr [rbx + 1], sil
002F905E  7d 05                                           jge       0x2f9065 ; loc_2F9065 [RVA 0x2F9065]
002F9060  cc                                              int3
002F9061  80 63 01 7f                                     and       byte ptr [rbx + 1], 0x7f
002F9065  33 c0                                           xor       eax, eax
002F9067  e9 5a fe ff ff                                  jmp       0x2f8ec6 ; loc_2F8EC6 [RVA 0x2F8EC6]
002F906C  40 38 35 51 ca cc 00                            cmp       byte ptr [rip + 0xccca51], sil
002F9073  0f 84 74 fc ff ff                               je        0x2f8ced ; loc_2F8CED [RVA 0x2F8CED]
002F9079  45 0f 20 c5                                     mov       r13, cr8
002F907D  4c 89 6c 24 40                                  mov       qword ptr [rsp + 0x40], r13
002F9082  b8 0f 00 00 00                                  mov       eax, 0xf
002F9087  44 0f 22 c0                                     mov       cr8, rax
002F908B  83 3d ce cc cc 00 00                            cmp       dword ptr [rip + 0xccccce], 0
002F9092  0f 84 55 fc ff ff                               je        0x2f8ced ; loc_2F8CED [RVA 0x2F8CED]
002F9098  0f b6 d0                                        movzx     edx, al
002F909B  41 0f b6 cd                                     movzx     ecx, r13b
002F909F  e8 98 c3 1f 00                                  call      0x4f543c ; KiRaiseIrqlProcessIrqlFlags [RVA 0x4F543C]
002F90A4  e9 44 fc ff ff                                  jmp       0x2f8ced ; loc_2F8CED [RVA 0x2F8CED]
002F90A9  40 32 f6                                        xor       sil, sil
002F90AC  e9 a0 fc ff ff                                  jmp       0x2f8d51 ; loc_2F8D51 [RVA 0x2F8D51]
002F90B1  44 8b c5                                        mov       r8d, ebp
002F90B4  48 8b d3                                        mov       rdx, rbx
002F90B7  33 c9                                           xor       ecx, ecx
002F90B9  e8 72 d8 3a 00                                  call      0x6a6930 ; HvlSwitchToVsmVtl1 [RVA 0x6A6930]
002F90BE  eb 9a                                           jmp       0x2f905a ; loc_2F905A [RVA 0x2F905A]
002F90C0  83 3d 99 cc cc 00 00                            cmp       dword ptr [rip + 0xcccc99], 0
002F90C7  0f 84 ef e1 3d 00                               je        0x6d72bc ; loc_6D72BC [RVA 0x6D72BC]
002F90CD  44 0f 20 c1                                     mov       rcx, cr8
002F90D1  0f b6 d3                                        movzx     edx, bl
002F90D4  e8 ff c2 1f 00                                  call      0x4f53d8 ; KiLowerIrqlProcessIrqlFlags [RVA 0x4F53D8]
002F90D9  90                                              nop
002F90DA  e9 dd e1 3d 00                                  jmp       0x6d72bc ; loc_6D72BC [RVA 0x6D72BC]
006D72BC  0f b6 c3                                        movzx     eax, bl
006D72BF  44 0f 22 c0                                     mov       cr8, rax
006D72C3  e9 fb 1b c2 ff                                  jmp       0x2f8ec3 ; loc_2F8EC3 [RVA 0x2F8EC3]
