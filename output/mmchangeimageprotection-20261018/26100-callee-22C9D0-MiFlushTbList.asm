MiFlushTbList at RVA 0x22C9D0
extent source: next public symbol (first .pdata end 0x22CA05); RVA 0x22C9D0..0x22D544 (exclusive)
shown: 200 instruction(s), max 200
All addresses are RVAs.

0022C9D0  4c 8b dc                                        mov       r11, rsp
0022C9D3  53                                              push      rbx
0022C9D4  55                                              push      rbp
0022C9D5  48 81 ec f8 01 00 00                            sub       rsp, 0x1f8
0022C9DC  48 8b 05 5d dd bd 00                            mov       rax, qword ptr [rip + 0xbddd5d]
0022C9E3  48 33 c4                                        xor       rax, rsp
0022C9E6  48 89 84 24 d0 01 00 00                         mov       qword ptr [rsp + 0x1d0], rax
0022C9EE  8b 69 1c                                        mov       ebp, dword ptr [rcx + 0x1c]
0022C9F1  48 8b d9                                        mov       rbx, rcx
0022C9F4  48 89 4c 24 70                                  mov       qword ptr [rsp + 0x70], rcx
0022C9F9  85 ed                                           test      ebp, ebp
0022C9FB  0f 84 17 02 00 00                               je        0x22cc18 ; loc_22CC18 [RVA 0x22CC18]
0022CA01  0f b6 51 18                                     movzx     edx, byte ptr [rcx + 0x18]
0022CA05  49 89 73 10                                     mov       qword ptr [r11 + 0x10], rsi
0022CA09  48 8d 71 08                                     lea       rsi, [rcx + 8]
0022CA0D  49 89 7b 18                                     mov       qword ptr [r11 + 0x18], rdi
0022CA11  4d 89 63 20                                     mov       qword ptr [r11 + 0x20], r12
0022CA15  4d 89 73 e0                                     mov       qword ptr [r11 - 0x20], r14
0022CA19  4d 89 7b d8                                     mov       qword ptr [r11 - 0x28], r15
0022CA1D  45 33 ff                                        xor       r15d, r15d
0022CA20  45 8d 67 01                                     lea       r12d, [r15 + 1]
0022CA24  45 8d 77 02                                     lea       r14d, [r15 + 2]
0022CA28  84 d2                                           test      dl, dl
0022CA2A  0f 85 1c 0a 00 00                               jne       0x22d44c ; loc_22D44C [RVA 0x22D44C]
0022CA30  44 39 26                                        cmp       dword ptr [rsi], r12d
0022CA33  75 22                                           jne       0x22ca57 ; loc_22CA57 [RVA 0x22CA57]
0022CA35  65 48 8b 04 25 88 01 00 00                      mov       rax, qword ptr gs:[0x188]
0022CA3E  48 8b 88 b8 00 00 00                            mov       rcx, qword ptr [rax + 0xb8]
0022CA45  4c 39 b9 70 06 00 00                            cmp       qword ptr [rcx + 0x670], r15
0022CA4C  75 25                                           jne       0x22ca73 ; loc_22CA73 [RVA 0x22CA73]
0022CA4E  4c 39 b9 70 01 00 00                            cmp       qword ptr [rcx + 0x170], r15
0022CA55  75 1c                                           jne       0x22ca73 ; loc_22CA73 [RVA 0x22CA73]
0022CA57  4c 8b 05 72 12 c0 00                            mov       r8, qword ptr [rip + 0xc01272]
0022CA5E  44 39 26                                        cmp       dword ptr [rsi], r12d
0022CA61  74 17                                           je        0x22ca7a ; loc_22CA7A [RVA 0x22CA7A]
0022CA63  8b 43 10                                        mov       eax, dword ptr [rbx + 0x10]
0022CA66  a8 04                                           test      al, 4
0022CA68  0f 84 59 07 00 00                               je        0x22d1c7 ; loc_22D1C7 [RVA 0x22D1C7]
0022CA6E  45 8b ce                                        mov       r9d, r14d
0022CA71  eb 0a                                           jmp       0x22ca7d ; loc_22CA7D [RVA 0x22CA7D]
0022CA73  49 c7 c0 ff ff ff ff                            mov       r8, 0xffffffffffffffff
0022CA7A  45 8b cf                                        mov       r9d, r15d
0022CA7D  b8 10 00 00 00                                  mov       eax, 0x10
0022CA82  44 89 4c 24 44                                  mov       dword ptr [rsp + 0x44], r9d
0022CA87  48 8b cb                                        mov       rcx, rbx
0022CA8A  48 8d 3c 18                                     lea       rdi, [rax + rbx]
0022CA8E  44 38 7b 19                                     cmp       byte ptr [rbx + 0x19], r15b
0022CA92  0f 85 3a 01 00 00                               jne       0x22cbd2 ; loc_22CBD2 [RVA 0x22CBD2]
0022CA98  84 d2                                           test      dl, dl
0022CA9A  75 14                                           jne       0x22cab0 ; loc_22CAB0 [RVA 0x22CAB0]
0022CA9C  4c 39 43 20                                     cmp       qword ptr [rbx + 0x20], r8
0022CAA0  0f 83 0c 06 00 00                               jae       0x22d0b2 ; loc_22D0B2 [RVA 0x22D0B2]
0022CAA6  44 38 7b 1a                                     cmp       byte ptr [rbx + 0x1a], r15b
0022CAAA  0f 85 02 06 00 00                               jne       0x22d0b2 ; loc_22D0B2 [RVA 0x22D0B2]
0022CAB0  8b 4b 14                                        mov       ecx, dword ptr [rbx + 0x14]
0022CAB3  f6 da                                           neg       dl
0022CAB5  4c 89 ac 24 f0 01 00 00                         mov       qword ptr [rsp + 0x1f0], r13
0022CABD  1b c0                                           sbb       eax, eax
0022CABF  25 00 00 00 80                                  and       eax, 0x80000000
0022CAC4  41 84 cc                                        test      r12b, cl
0022CAC7  0f 84 4e 05 00 00                               je        0x22d01b ; loc_22D01B [RVA 0x22D01B]
0022CACD  41 0b c4                                        or        eax, r12d
0022CAD0  44 8b 3e                                        mov       r15d, dword ptr [rsi]
0022CAD3  48 8d 7b 10                                     lea       rdi, [rbx + 0x10]
0022CAD7  8b d0                                           mov       edx, eax
0022CAD9  4c 8d 6b 28                                     lea       r13, [rbx + 0x28]
0022CADD  83 ca 08                                        or        edx, 8
0022CAE0  0f ba e1 08                                     bt        ecx, 8
0022CAE4  0f 43 d0                                        cmovae    edx, eax
0022CAE7  8b 07                                           mov       eax, dword ptr [rdi]
0022CAE9  89 54 24 58                                     mov       dword ptr [rsp + 0x58], edx
0022CAED  41 84 c4                                        test      r12b, al
0022CAF0  0f 84 3d 01 00 00                               je        0x22cc33 ; loc_22CC33 [RVA 0x22CC33]
0022CAF6  49 8b 4d 00                                     mov       rcx, qword ptr [r13]
0022CAFA  45 3b fc                                        cmp       r15d, r12d
0022CAFD  0f 85 89 09 00 00                               jne       0x22d48c ; loc_22D48C [RVA 0x22D48C]
0022CB03  48 b8 ff ff fe ff ff 7f 00 00                   movabs    rax, 0x7ffffffeffff
0022CB0D  48 3b c8                                        cmp       rcx, rax
0022CB10  77 1a                                           ja        0x22cb2c ; loc_22CB2C [RVA 0x22CB2C]
0022CB12  65 48 8b 04 25 88 01 00 00                      mov       rax, qword ptr gs:[0x188]
0022CB1B  48 8b 88 b8 00 00 00                            mov       rcx, qword ptr [rax + 0xb8]
0022CB22  48 83 b9 70 01 00 00 00                         cmp       qword ptr [rcx + 0x170], 0
0022CB2A  75 03                                           jne       0x22cb2f ; loc_22CB2F [RVA 0x22CB2F]
0022CB2C  45 33 e4                                        xor       r12d, r12d
0022CB2F  44 8b 35 c2 8f d9 00                            mov       r14d, dword ptr [rip + 0xd98fc2]
0022CB36  41 0f ba e6 17                                  bt        r14d, 0x17
0022CB3B  0f 82 53 06 00 00                               jb        0x22d194 ; loc_22D194 [RVA 0x22D194]
0022CB41  45 32 f6                                        xor       r14b, r14b
0022CB44  45 85 e4                                        test      r12d, r12d
0022CB47  0f 85 e8 04 00 00                               jne       0x22d035 ; loc_22D035 [RVA 0x22D035]
0022CB4D  45 84 f6                                        test      r14b, r14b
0022CB50  0f 85 df 04 00 00                               jne       0x22d035 ; loc_22D035 [RVA 0x22D035]
0022CB56  85 ed                                           test      ebp, ebp
0022CB58  0f 85 cd 08 00 00                               jne       0x22d42b ; loc_22D42B [RVA 0x22D42B]
0022CB5E  80 3d 9f 74 d9 00 00                            cmp       byte ptr [rip + 0xd9749f], 0
0022CB65  74 0d                                           je        0x22cb74 ; loc_22CB74 [RVA 0x22CB74]
0022CB67  45 8b c7                                        mov       r8d, r15d
0022CB6A  49 8b d5                                        mov       rdx, r13
0022CB6D  8b cd                                           mov       ecx, ebp
0022CB6F  e8 6c 0a 00 00                                  call      0x22d5e0 ; VmFlushTb [RVA 0x22D5E0]
0022CB74  83 3d 59 b9 c3 00 00                            cmp       dword ptr [rip + 0xc3b959], 0
0022CB7B  74 45                                           je        0x22cbc2 ; loc_22CBC2 [RVA 0x22CBC2]
0022CB7D  45 0f 20 c6                                     mov       r14, cr8
0022CB81  b8 0f 00 00 00                                  mov       eax, 0xf
0022CB86  44 0f 22 c0                                     mov       cr8, rax
0022CB8A  83 3d cf 91 d9 00 00                            cmp       dword ptr [rip + 0xd991cf], 0
0022CB91  0f 85 ec 05 00 00                               jne       0x22d183 ; loc_22D183 [RVA 0x22D183]
0022CB97  45 8b c7                                        mov       r8d, r15d
0022CB9A  49 8b d5                                        mov       rdx, r13
0022CB9D  8b cd                                           mov       ecx, ebp
0022CB9F  e8 a0 09 00 00                                  call      0x22d544 ; ExFlushTb [RVA 0x22D544]
0022CBA4  83 3d b5 91 d9 00 00                            cmp       dword ptr [rip + 0xd991b5], 0
0022CBAB  74 0d                                           je        0x22cbba ; loc_22CBBA [RVA 0x22CBBA]
0022CBAD  41 0f b6 d6                                     movzx     edx, r14b
0022CBB1  44 0f 20 c1                                     mov       rcx, cr8
0022CBB5  e8 1e 88 2c 00                                  call      0x4f53d8 ; KiLowerIrqlProcessIrqlFlags [RVA 0x4F53D8]
0022CBBA  41 0f b6 c6                                     movzx     eax, r14b
0022CBBE  44 0f 22 c0                                     mov       cr8, rax
0022CBC2  44 8b 4c 24 44                                  mov       r9d, dword ptr [rsp + 0x44]
0022CBC7  45 33 ff                                        xor       r15d, r15d
0022CBCA  4c 8b ac 24 f0 01 00 00                         mov       r13, qword ptr [rsp + 0x1f0]
0022CBD2  80 7b 19 00                                     cmp       byte ptr [rbx + 0x19], 0
0022CBD6  4c 8b b4 24 e8 01 00 00                         mov       r14, qword ptr [rsp + 0x1e8]
0022CBDE  4c 8b a4 24 28 02 00 00                         mov       r12, qword ptr [rsp + 0x228]
0022CBE6  0f 85 d3 04 00 00                               jne       0x22d0bf ; loc_22D0BF [RVA 0x22D0BF]
0022CBEC  83 27 fb                                        and       dword ptr [rdi], 0xfffffffb
0022CBEF  83 27 df                                        and       dword ptr [rdi], 0xffffffdf
0022CBF2  48 8b bc 24 20 02 00 00                         mov       rdi, qword ptr [rsp + 0x220]
0022CBFA  48 8b b4 24 18 02 00 00                         mov       rsi, qword ptr [rsp + 0x218]
0022CC02  44 89 7b 1c                                     mov       dword ptr [rbx + 0x1c], r15d
0022CC06  4c 89 7b 20                                     mov       qword ptr [rbx + 0x20], r15
0022CC0A  4c 8b bc 24 e0 01 00 00                         mov       r15, qword ptr [rsp + 0x1e0]
0022CC12  66 c7 43 19 00 00                               mov       word ptr [rbx + 0x19], 0
0022CC18  48 8b 8c 24 d0 01 00 00                         mov       rcx, qword ptr [rsp + 0x1d0]
0022CC20  48 33 cc                                        xor       rcx, rsp
0022CC23  e8 68 94 47 00                                  call      0x6a6090 ; _security_check_cookie [RVA 0x6A6090]
0022CC28  48 81 c4 f8 01 00 00                            add       rsp, 0x1f8
0022CC2F  5d                                              pop       rbp
0022CC30  5b                                              pop       rbx
0022CC31  c3                                              ret
0022CC32  cc                                              int3
0022CC33  8b 05 bf 8e d9 00                               mov       eax, dword ptr [rip + 0xd98ebf]
0022CC39  b9 0c 00 00 00                                  mov       ecx, 0xc
0022CC3E  c6 44 24 41 00                                  mov       byte ptr [rsp + 0x41], 0
0022CC43  a8 04                                           test      al, 4
0022CC45  0f 84 2f 02 00 00                               je        0x22ce7a ; loc_22CE7A [RVA 0x22CE7A]
0022CC4B  41 84 c6                                        test      r14b, al
0022CC4E  0f 85 2a 08 00 00                               jne       0x22d47e ; loc_22D47E [RVA 0x22D47E]
0022CC54  0f b6 05 6c 8e d9 00                            movzx     eax, byte ptr [rip + 0xd98e6c]
0022CC5B  24 03                                           and       al, 3
0022CC5D  41 3a c4                                        cmp       al, r12b
0022CC60  0f 84 94 08 00 00                               je        0x22d4fa ; loc_22D4FA [RVA 0x22D4FA]
0022CC66  8b 05 54 8e d9 00                               mov       eax, dword ptr [rip + 0xd98e54]
0022CC6C  41 3b c4                                        cmp       eax, r12d
0022CC6F  0f 84 05 02 00 00                               je        0x22ce7a ; loc_22CE7A [RVA 0x22CE7A]
0022CC75  45 85 c9                                        test      r9d, r9d
0022CC78  0f 85 84 06 00 00                               jne       0x22d302 ; loc_22D302 [RVA 0x22D302]
0022CC7E  f0 44 09 0c 24                                  lock or   dword ptr [rsp], r9d
0022CC83  45 0f 20 c2                                     mov       r10, cr8
0022CC87  4c 89 54 24 68                                  mov       qword ptr [rsp + 0x68], r10
0022CC8C  44 0f 22 c1                                     mov       cr8, rcx
0022CC90  44 39 0d c9 90 d9 00                            cmp       dword ptr [rip + 0xd990c9], r9d
0022CC97  0f 85 14 05 00 00                               jne       0x22d1b1 ; loc_22D1B1 [RVA 0x22D1B1]
0022CC9D  65 48 8b 14 25 20 00 00 00                      mov       rdx, qword ptr gs:[0x20]
0022CCA6  48 8b 42 08                                     mov       rax, qword ptr [rdx + 8]
0022CCAA  48 8b 88 b8 00 00 00                            mov       rcx, qword ptr [rax + 0xb8]
0022CCB1  33 c0                                           xor       eax, eax
0022CCB3  4c 8b 81 80 00 00 00                            mov       r8, qword ptr [rcx + 0x80]
0022CCBA  0f b7 c8                                        movzx     ecx, ax
0022CCBD  66 41 3b 08                                     cmp       cx, word ptr [r8]
0022CCC1  73 2f                                           jae       0x22ccf2 ; loc_22CCF2 [RVA 0x22CCF2]
0022CCC3  0f b7 c1                                        movzx     eax, cx
0022CCC6  4d 8b 4c c0 08                                  mov       r9, qword ptr [r8 + rax*8 + 8]
0022CCCB  4d 85 c9                                        test      r9, r9
0022CCCE  74 1d                                           je        0x22cced ; loc_22CCED [RVA 0x22CCED]
0022CCD0  0f b6 82 d0 00 00 00                            movzx     eax, byte ptr [rdx + 0xd0]
0022CCD7  66 3b c8                                        cmp       cx, ax
0022CCDA  0f 85 19 04 00 00                               jne       0x22d0f9 ; loc_22D0F9 [RVA 0x22D0F9]
0022CCE0  4c 3b 8a c8 00 00 00                            cmp       r9, qword ptr [rdx + 0xc8]
0022CCE7  0f 85 0c 04 00 00                               jne       0x22d0f9 ; loc_22D0F9 [RVA 0x22D0F9]
0022CCED  66 ff c1                                        inc       cx
0022CCF0  eb cb                                           jmp       0x22ccbd ; loc_22CCBD [RVA 0x22CCBD]
0022CCF2  c6 44 24 40 00                                  mov       byte ptr [rsp + 0x40], 0
0022CCF7  85 ed                                           test      ebp, ebp
0022CCF9  0f 84 4d 01 00 00                               je        0x22ce4c ; loc_22CE4C [RVA 0x22CE4C]
0022CCFF  41 8b cf                                        mov       ecx, r15d
0022CD02  48 89 6c 24 60                                  mov       qword ptr [rsp + 0x60], rbp
0022CD07  41 8b dc                                        mov       ebx, r12d
0022CD0A  49 8b c5                                        mov       rax, r13
0022CD0D  d3 c3                                           rol       ebx, cl
0022CD0F  83 e3 06                                        and       ebx, 6
0022CD12  48 89 84 24 90 00 00 00                         mov       qword ptr [rsp + 0x90], rax
0022CD1A  89 5c 24 50                                     mov       dword ptr [rsp + 0x50], ebx
0022CD1E  45 33 f6                                        xor       r14d, r14d
0022CD21  48 8b 10                                        mov       rdx, qword ptr [rax]
0022CD24  48 89 54 24 48                                  mov       qword ptr [rsp + 0x48], rdx
0022CD29  85 db                                           test      ebx, ebx
0022CD2B  74 4c                                           je        0x22cd79 ; loc_22CD79 [RVA 0x22CD79]
0022CD2D  80 3d 93 8d d9 00 00                            cmp       byte ptr [rip + 0xd98d93], 0
0022CD34  74 43                                           je        0x22cd79 ; loc_22CD79 [RVA 0x22CD79]
