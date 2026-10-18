HvlFlushRangeListTb at RVA 0x22D620
extent source: next public symbol (first .pdata end 0x22D65C); RVA 0x22D620..0x22DAF0 (exclusive)
shown: 200 instruction(s), max 200
All addresses are RVAs.

0022D620  40 55                                           push      rbp
0022D622  53                                              push      rbx
0022D623  41 56                                           push      r14
0022D625  48 8d 6c 24 a0                                  lea       rbp, [rsp - 0x60]
0022D62A  48 81 ec 60 01 00 00                            sub       rsp, 0x160
0022D631  48 8b 05 08 d1 bd 00                            mov       rax, qword ptr [rip + 0xbdd108]
0022D638  48 33 c4                                        xor       rax, rsp
0022D63B  48 89 45 30                                     mov       qword ptr [rbp + 0x30], rax
0022D63F  4c 8b b5 b0 00 00 00                            mov       r14, qword ptr [rbp + 0xb0]
0022D646  45 8b d1                                        mov       r10d, r9d
0022D649  44 89 4c 24 50                                  mov       dword ptr [rsp + 0x50], r9d
0022D64E  4c 8b d9                                        mov       r11, rcx
0022D651  48 85 d2                                        test      rdx, rdx
0022D654  0f 85 0d 03 00 00                               jne       0x22d967 ; loc_22D967 [RVA 0x22D967]
0022D65A  33 db                                           xor       ebx, ebx
0022D65C  48 89 b4 24 98 01 00 00                         mov       qword ptr [rsp + 0x198], rsi
0022D664  48 89 bc 24 58 01 00 00                         mov       qword ptr [rsp + 0x158], rdi
0022D66C  8b fb                                           mov       edi, ebx
0022D66E  4c 89 a4 24 50 01 00 00                         mov       qword ptr [rsp + 0x150], r12
0022D676  44 0f b6 a5 a0 00 00 00                         movzx     r12d, byte ptr [rbp + 0xa0]
0022D67E  48 89 5c 24 78                                  mov       qword ptr [rsp + 0x78], rbx
0022D683  48 89 5d 80                                     mov       qword ptr [rbp - 0x80], rbx
0022D687  48 89 5d 88                                     mov       qword ptr [rbp - 0x78], rbx
0022D68B  45 84 e4                                        test      r12b, r12b
0022D68E  74 2d                                           je        0x22d6bd ; loc_22D6BD [RVA 0x22D6BD]
0022D690  45 84 c0                                        test      r8b, r8b
0022D693  b8 04 00 00 00                                  mov       eax, 4
0022D698  0f 45 c3                                        cmovne    eax, ebx
0022D69B  8b c8                                           mov       ecx, eax
0022D69D  83 c9 02                                        or        ecx, 2
0022D6A0  4d 85 db                                        test      r11, r11
0022D6A3  0f 45 c8                                        cmovne    ecx, eax
0022D6A6  48 85 d2                                        test      rdx, rdx
0022D6A9  0f 85 b0 01 00 00                               jne       0x22d85f ; loc_22D85F [RVA 0x22D85F]
0022D6AF  83 c9 01                                        or        ecx, 1
0022D6B2  8b f9                                           mov       edi, ecx
0022D6B4  48 89 7d 80                                     mov       qword ptr [rbp - 0x80], rdi
0022D6B8  4c 89 5c 24 78                                  mov       qword ptr [rsp + 0x78], r11
0022D6BD  f7 05 09 84 d9 00 00 20 00 00                   test      dword ptr [rip + 0xd98409], 0x2000
0022D6C7  4c 89 ac 24 48 01 00 00                         mov       qword ptr [rsp + 0x148], r13
0022D6CF  0f 85 21 03 00 00                               jne       0x22d9f6 ; loc_22D9F6 [RVA 0x22D9F6]
0022D6D5  44 8b ad a8 00 00 00                            mov       r13d, dword ptr [rbp + 0xa8]
0022D6DC  41 8b f5                                        mov       esi, r13d
0022D6DF  83 fe 0b                                        cmp       esi, 0xb
0022D6E2  77 20                                           ja        0x22d704 ; loc_22D704 [RVA 0x22D704]
0022D6E4  8b 05 0e 84 d9 00                               mov       eax, dword ptr [rip + 0xd9840e]
0022D6EA  84 c0                                           test      al, al
0022D6EC  79 16                                           jns       0x22d704 ; loc_22D704 [RVA 0x22D704]
0022D6EE  45 85 d2                                        test      r10d, r10d
0022D6F1  0f 84 8a 01 00 00                               je        0x22d881 ; loc_22D881 [RVA 0x22D881]
0022D6F7  41 8d 45 01                                     lea       eax, [r13 + 1]
0022D6FB  83 f8 0c                                        cmp       eax, 0xc
0022D6FE  0f 86 74 01 00 00                               jbe       0x22d878 ; loc_22D878 [RVA 0x22D878]
0022D704  33 c0                                           xor       eax, eax
0022D706  4c 89 bc 24 40 01 00 00                         mov       qword ptr [rsp + 0x140], r15
0022D70E  48 89 44 24 68                                  mov       qword ptr [rsp + 0x68], rax
0022D713  0f 57 c0                                        xorps     xmm0, xmm0
0022D716  89 44 24 70                                     mov       dword ptr [rsp + 0x70], eax
0022D71A  41 b8 02 00 00 00                               mov       r8d, 2
0022D720  8b 05 aa 83 d9 00                               mov       eax, dword ptr [rip + 0xd983aa]
0022D726  0f 11 44 24 58                                  movups    xmmword ptr [rsp + 0x58], xmm0
0022D72B  a8 08                                           test      al, 8
0022D72D  0f 84 f4 02 00 00                               je        0x22da27 ; loc_22DA27 [RVA 0x22DA27]
0022D733  65 48 8b 3c 25 20 00 00 00                      mov       rdi, qword ptr gs:[0x20]
0022D73C  48 8d 8f c0 8b 00 00                            lea       rcx, [rdi + 0x8bc0]
0022D743  e8 58 68 48 00                                  call      0x6b3fa0 ; RtlInterlockedPopEntrySList/ExpInterlockedPopEntrySList/RtlpInterlockedPopEntrySList [RVA 0x6B3FA0]
0022D748  4c 8b f8                                        mov       r15, rax
0022D74B  48 85 c0                                        test      rax, rax
0022D74E  0f 84 8e 3b 4a 00                               je        0x6d12e2 ; sub_6D12E2 [RVA 0x6D12E2]
0022D754  48 89 7c 24 60                                  mov       qword ptr [rsp + 0x60], rdi
0022D759  b8 01 00 00 00                                  mov       eax, 1
0022D75E  49 8b 7f 10                                     mov       rdi, qword ptr [r15 + 0x10]
0022D762  48 89 7c 24 70                                  mov       qword ptr [rsp + 0x70], rdi
0022D767  89 44 24 58                                     mov       dword ptr [rsp + 0x58], eax
0022D76B  4c 89 7c 24 68                                  mov       qword ptr [rsp + 0x68], r15
0022D770  44 8b 54 24 50                                  mov       r10d, dword ptr [rsp + 0x50]
0022D775  41 b8 02 00 00 00                               mov       r8d, 2
0022D77B  81 fe fd 01 00 00                               cmp       esi, 0x1fd
0022D781  0f 87 20 02 00 00                               ja        0x22d9a7 ; loc_22D9A7 [RVA 0x22D9A7]
0022D787  a8 02                                           test      al, 2
0022D789  0f 85 18 02 00 00                               jne       0x22d9a7 ; loc_22D9A7 [RVA 0x22D9A7]
0022D78F  44 8b 05 3a 83 d9 00                            mov       r8d, dword ptr [rip + 0xd9833a]
0022D796  4d 8d 4f 18                                     lea       r9, [r15 + 0x18]
0022D79A  41 c1 e8 0d                                     shr       r8d, 0xd
0022D79E  49 8b d6                                        mov       rdx, r14
0022D7A1  41 80 e0 01                                     and       r8b, 1
0022D7A5  41 8b cd                                        mov       ecx, r13d
0022D7A8  e8 23 0f 00 00                                  call      0x22e6d0 ; HvlpCopyFlushVaList [RVA 0x22E6D0]
0022D7AD  45 84 e4                                        test      r12b, r12b
0022D7B0  74 3e                                           je        0x22d7f0 ; loc_22D7F0 [RVA 0x22D7F0]
0022D7B2  0f 10 44 24 78                                  movups    xmm0, xmmword ptr [rsp + 0x78]
0022D7B7  8b c6                                           mov       eax, esi
0022D7B9  48 c7 44 24 48 03 00 00 00                      mov       qword ptr [rsp + 0x48], 3
0022D7C2  f2 0f 10 4d 88                                  movsd     xmm1, qword ptr [rbp - 0x78]
0022D7C7  25 ff 0f 00 00                                  and       eax, 0xfff
0022D7CC  41 0f 11 07                                     movups    xmmword ptr [r15], xmm0
0022D7D0  89 44 24 4c                                     mov       dword ptr [rsp + 0x4c], eax
0022D7D4  48 8b 5c 24 48                                  mov       rbx, qword ptr [rsp + 0x48]
0022D7D9  f2 41 0f 11 4f 10                               movsd     qword ptr [r15 + 0x10], xmm1
0022D7DF  f7 05 e7 82 d9 00 00 20 00 00                   test      dword ptr [rip + 0xd982e7], 0x2000
0022D7E9  75 05                                           jne       0x22d7f0 ; loc_22D7F0 [RVA 0x22D7F0]
0022D7EB  49 83 4f 08 08                                  or        qword ptr [r15 + 8], 8
0022D7F0  44 8b 54 24 50                                  mov       r10d, dword ptr [rsp + 0x50]
0022D7F5  32 c0                                           xor       al, al
0022D7F7  45 85 d2                                        test      r10d, r10d
0022D7FA  74 39                                           je        0x22d835 ; loc_22D835 [RVA 0x22D835]
0022D7FC  c6 44 24 40 00                                  mov       byte ptr [rsp + 0x40], 0
0022D801  84 c0                                           test      al, al
0022D803  0f 85 b8 02 00 00                               jne       0x22dac1 ; loc_22DAC1 [RVA 0x22DAC1]
0022D809  48 8d 44 24 40                                  lea       rax, [rsp + 0x40]
0022D80E  44 8b ce                                        mov       r9d, esi
0022D811  48 89 44 24 28                                  mov       qword ptr [rsp + 0x28], rax
0022D816  45 8b c2                                        mov       r8d, r10d
0022D819  49 8b d7                                        mov       rdx, r15
0022D81C  c7 44 24 20 18 00 00 00                         mov       dword ptr [rsp + 0x20], 0x18
0022D824  48 8b cf                                        mov       rcx, rdi
0022D827  e8 28 1f 00 00                                  call      0x22f754 ; VslSlowFlushSecureRangeList [RVA 0x22F754]
0022D82C  0f b6 44 24 40                                  movzx     eax, byte ptr [rsp + 0x40]
0022D831  84 c0                                           test      al, al
0022D833  75 13                                           jne       0x22d848 ; loc_22D848 [RVA 0x22D848]
0022D835  45 84 e4                                        test      r12b, r12b
0022D838  74 0e                                           je        0x22d848 ; loc_22D848 [RVA 0x22D848]
0022D83A  45 33 c0                                        xor       r8d, r8d
0022D83D  48 8b d7                                        mov       rdx, rdi
0022D840  48 8b cb                                        mov       rcx, rbx
0022D843  e8 f8 07 00 00                                  call      0x22e040 ; HvcallInitiateHypercall [RVA 0x22E040]
0022D848  48 8d 4c 24 58                                  lea       rcx, [rsp + 0x58]
0022D84D  e8 3e 06 00 00                                  call      0x22de90 ; HvlpReleaseHypercallPage [RVA 0x22DE90]
0022D852  4c 8b bc 24 40 01 00 00                         mov       r15, qword ptr [rsp + 0x140]
0022D85A  e9 cb 00 00 00                                  jmp       0x22d92a ; loc_22D92A [RVA 0x22D92A]
0022D85F  38 1d a2 67 d9 00                               cmp       byte ptr [rip + 0xd967a2], bl
0022D865  4c 8b 42 08                                     mov       r8, qword ptr [rdx + 8]
0022D869  0f 84 78 01 00 00                               je        0x22d9e7 ; loc_22D9E7 [RVA 0x22D9E7]
0022D86F  4c 89 45 88                                     mov       qword ptr [rbp - 0x78], r8
0022D873  e9 3a fe ff ff                                  jmp       0x22d6b2 ; loc_22D6B2 [RVA 0x22D6B2]
0022D878  45 85 d2                                        test      r10d, r10d
0022D87B  0f 85 40 01 00 00                               jne       0x22d9c1 ; loc_22D9C1 [RVA 0x22D9C1]
0022D881  45 84 e4                                        test      r12b, r12b
0022D884  0f 84 a0 00 00 00                               je        0x22d92a ; loc_22D92A [RVA 0x22D92A]
0022D88A  0f 10 44 24 78                                  movups    xmm0, xmmword ptr [rsp + 0x78]
0022D88F  44 8d 14 f5 18 00 00 00                         lea       r10d, [rsi*8 + 0x18]
0022D897  81 e6 ff 0f 00 00                               and       esi, 0xfff
0022D89D  f2 0f 10 4d 88                                  movsd     xmm1, qword ptr [rbp - 0x78]
0022D8A2  f7 05 24 82 d9 00 00 20 00 00                   test      dword ptr [rip + 0xd98224], 0x2000
0022D8AC  48 c7 44 24 48 03 00 00 00                      mov       qword ptr [rsp + 0x48], 3
0022D8B5  0f 29 45 90                                     movaps    xmmword ptr [rbp - 0x70], xmm0
0022D8B9  89 74 24 4c                                     mov       dword ptr [rsp + 0x4c], esi
0022D8BD  f2 0f 11 4d a0                                  movsd     qword ptr [rbp - 0x60], xmm1
0022D8C2  0f 85 4b 01 00 00                               jne       0x22da13 ; loc_22DA13 [RVA 0x22DA13]
0022D8C8  48 83 cf 08                                     or        rdi, 8
0022D8CC  48 89 7d 98                                     mov       qword ptr [rbp - 0x68], rdi
0022D8D0  45 85 ed                                        test      r13d, r13d
0022D8D3  74 35                                           je        0x22d90a ; loc_22D90A [RVA 0x22D90A]
0022D8D5  48 8d 4d a8                                     lea       rcx, [rbp - 0x58]
0022D8D9  45 8b c5                                        mov       r8d, r13d
0022D8DC  4c 2b f1                                        sub       r14, rcx
0022D8DF  48 8d 45 a8                                     lea       rax, [rbp - 0x58]
0022D8E3  49 8b 0c 06                                     mov       rcx, qword ptr [r14 + rax]
0022D8E7  48 8d 40 08                                     lea       rax, [rax + 8]
0022D8EB  48 8b d1                                        mov       rdx, rcx
0022D8EE  81 e2 00 0c 00 00                               and       edx, 0xc00
0022D8F4  48 03 d1                                        add       rdx, rcx
0022D8F7  81 e1 00 08 00 00                               and       ecx, 0x800
0022D8FD  48 0b d1                                        or        rdx, rcx
0022D900  48 89 50 f8                                     mov       qword ptr [rax - 8], rdx
0022D904  49 83 e8 01                                     sub       r8, 1
0022D908  75 d9                                           jne       0x22d8e3 ; loc_22D8E3 [RVA 0x22D8E3]
0022D90A  81 4c 24 48 00 00 01 00                         or        dword ptr [rsp + 0x48], 0x10000
0022D912  48 8d 55 90                                     lea       rdx, [rbp - 0x70]
0022D916  48 8b 4c 24 48                                  mov       rcx, qword ptr [rsp + 0x48]
0022D91B  45 33 c9                                        xor       r9d, r9d
0022D91E  45 8b c2                                        mov       r8d, r10d
0022D921  89 5c 24 20                                     mov       dword ptr [rsp + 0x20], ebx
0022D925  e8 c6 01 00 00                                  call      0x22daf0 ; HvcallFastExtended [RVA 0x22DAF0]
0022D92A  4c 8b ac 24 48 01 00 00                         mov       r13, qword ptr [rsp + 0x148]
0022D932  41 0f b6 c4                                     movzx     eax, r12b
0022D936  4c 8b a4 24 50 01 00 00                         mov       r12, qword ptr [rsp + 0x150]
0022D93E  48 8b bc 24 58 01 00 00                         mov       rdi, qword ptr [rsp + 0x158]
0022D946  48 8b b4 24 98 01 00 00                         mov       rsi, qword ptr [rsp + 0x198]
0022D94E  48 8b 4d 30                                     mov       rcx, qword ptr [rbp + 0x30]
0022D952  48 33 cc                                        xor       rcx, rsp
0022D955  e8 36 87 47 00                                  call      0x6a6090 ; _security_check_cookie [RVA 0x6A6090]
0022D95A  48 81 c4 60 01 00 00                            add       rsp, 0x160
0022D961  41 5e                                           pop       r14
0022D963  5b                                              pop       rbx
0022D964  5d                                              pop       rbp
0022D965  c3                                              ret
0022D966  cc                                              int3
0022D967  8b 05 63 81 d9 00                               mov       eax, dword ptr [rip + 0xd98163]
0022D96D  84 c0                                           test      al, al
0022D96F  0f 89 e5 fc ff ff                               jns       0x22d65a ; loc_22D65A [RVA 0x22D65A]
0022D975  66 83 3d 9b 66 d9 00 01                         cmp       word ptr [rip + 0xd9669b], 1
0022D97D  0f 86 d7 fc ff ff                               jbe       0x22d65a ; loc_22D65A [RVA 0x22D65A]
0022D983  8b 85 a8 00 00 00                               mov       eax, dword ptr [rbp + 0xa8]
0022D989  0f b6 9d a0 00 00 00                            movzx     ebx, byte ptr [rbp + 0xa0]
0022D990  4c 89 74 24 30                                  mov       qword ptr [rsp + 0x30], r14
0022D995  89 44 24 28                                     mov       dword ptr [rsp + 0x28], eax
0022D999  88 5c 24 20                                     mov       byte ptr [rsp + 0x20], bl
0022D99D  e8 3e db 1a 00                                  call      0x3db4e0 ; HvlpFlushRangeListTbEx [RVA 0x3DB4E0]
0022D9A2  0f b6 c3                                        movzx     eax, bl
