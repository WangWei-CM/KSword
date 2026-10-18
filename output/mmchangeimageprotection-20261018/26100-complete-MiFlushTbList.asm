MiFlushTbList: 642 CDB-discovered instructions verified against the local PE
RVA ranges (end exclusive): 0x22C9D0..0x22CC32, 0x22CC33..0x22D53B
All addresses below are RVAs; direct calls use PDB public-symbol names.

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
0022CD36  65 48 8b 04 25 88 01 00 00                      mov       rax, qword ptr gs:[0x188]
0022CD3F  48 8b 88 b8 00 00 00                            mov       rcx, qword ptr [rax + 0xb8]
0022CD46  80 b9 60 01 00 00 00                            cmp       byte ptr [rcx + 0x160], 0
0022CD4D  75 2a                                           jne       0x22cd79 ; loc_22CD79 [RVA 0x22CD79]
0022CD4F  f6 05 71 8d d9 00 02                            test      byte ptr [rip + 0xd98d71], 2
0022CD56  0f 84 fc 06 00 00                               je        0x22d458 ; loc_22D458 [RVA 0x22D458]
0022CD5C  4c 89 a4 24 98 00 00 00                         mov       qword ptr [rsp + 0x98], r12
0022CD64  48 89 94 24 a0 00 00 00                         mov       qword ptr [rsp + 0xa0], rdx
0022CD6C  41 8b c6                                        mov       eax, r14d
0022CD6F  66 0f 38 82 84 24 98 00 00 00                   invpcid   rax, xmmword ptr [rsp + 0x98]
0022CD79  0f 01 3a                                        invlpg    byte ptr [rdx]
0022CD7C  4c 8b c2                                        mov       r8, rdx
0022CD7F  48 8b ca                                        mov       rcx, rdx
0022CD82  48 c1 e9 0a                                     shr       rcx, 0xa
0022CD86  41 bd 00 10 00 00                               mov       r13d, 0x1000
0022CD8C  80 e1 03                                        and       cl, 3
0022CD8F  0f b6 c1                                        movzx     eax, cl
0022CD92  c0 e0 03                                        shl       al, 3
0022CD95  02 c8                                           add       cl, al
0022CD97  49 d3 e5                                        shl       r13, cl
0022CD9A  41 81 e0 ff 03 00 00                            and       r8d, 0x3ff
0022CDA1  4c 89 84 24 88 00 00 00                         mov       qword ptr [rsp + 0x88], r8
0022CDA9  74 73                                           je        0x22ce1e ; loc_22CE1E [RVA 0x22CE1E]
0022CDAB  33 db                                           xor       ebx, ebx
0022CDAD  49 03 d5                                        add       rdx, r13
0022CDB0  41 8d 47 ff                                     lea       eax, [r15 - 1]
0022CDB4  38 1d 0d 8d d9 00                               cmp       byte ptr [rip + 0xd98d0d], bl
0022CDBA  48 89 54 24 48                                  mov       qword ptr [rsp + 0x48], rdx
0022CDBF  74 46                                           je        0x22ce07 ; loc_22CE07 [RVA 0x22CE07]
0022CDC1  41 3b c4                                        cmp       eax, r12d
0022CDC4  77 41                                           ja        0x22ce07 ; loc_22CE07 [RVA 0x22CE07]
0022CDC6  65 48 8b 04 25 88 01 00 00                      mov       rax, qword ptr gs:[0x188]
0022CDCF  48 8b 88 b8 00 00 00                            mov       rcx, qword ptr [rax + 0xb8]
0022CDD6  38 99 60 01 00 00                               cmp       byte ptr [rcx + 0x160], bl
0022CDDC  75 29                                           jne       0x22ce07 ; loc_22CE07 [RVA 0x22CE07]
0022CDDE  f6 05 e2 8c d9 00 02                            test      byte ptr [rip + 0xd98ce2], 2
0022CDE5  0f 84 7c 06 00 00                               je        0x22d467 ; loc_22D467 [RVA 0x22D467]
0022CDEB  4c 89 a4 24 a8 00 00 00                         mov       qword ptr [rsp + 0xa8], r12
0022CDF3  48 89 94 24 b0 00 00 00                         mov       qword ptr [rsp + 0xb0], rdx
0022CDFB  8b c3                                           mov       eax, ebx
0022CDFD  66 0f 38 82 84 24 a8 00 00 00                   invpcid   rax, xmmword ptr [rsp + 0xa8]
0022CE07  0f 01 3a                                        invlpg    byte ptr [rdx]
0022CE0A  4d 2b c4                                        sub       r8, r12
0022CE0D  4c 89 84 24 88 00 00 00                         mov       qword ptr [rsp + 0x88], r8
0022CE15  75 96                                           jne       0x22cdad ; loc_22CDAD [RVA 0x22CDAD]
0022CE17  8b 5c 24 50                                     mov       ebx, dword ptr [rsp + 0x50]
0022CE1B  45 33 f6                                        xor       r14d, r14d
0022CE1E  48 8b 84 24 90 00 00 00                         mov       rax, qword ptr [rsp + 0x90]
0022CE26  48 83 c0 08                                     add       rax, 8
0022CE2A  4c 29 64 24 60                                  sub       qword ptr [rsp + 0x60], r12
0022CE2F  48 89 84 24 90 00 00 00                         mov       qword ptr [rsp + 0x90], rax
0022CE37  0f 85 e4 fe ff ff                               jne       0x22cd21 ; loc_22CD21 [RVA 0x22CD21]
0022CE3D  48 8b 5c 24 70                                  mov       rbx, qword ptr [rsp + 0x70]
0022CE42  41 be 02 00 00 00                               mov       r14d, 2
0022CE48  4c 8d 6b 28                                     lea       r13, [rbx + 0x28]
0022CE4C  83 3d 0d 8f d9 00 00                            cmp       dword ptr [rip + 0xd98f0d], 0
0022CE53  74 0e                                           je        0x22ce63 ; loc_22CE63 [RVA 0x22CE63]
0022CE55  44 0f 20 c1                                     mov       rcx, cr8
0022CE59  0f b6 54 24 68                                  movzx     edx, byte ptr [rsp + 0x68]
0022CE5E  e8 75 85 2c 00                                  call      0x4f53d8 ; KiLowerIrqlProcessIrqlFlags [RVA 0x4F53D8]
0022CE63  0f b6 44 24 68                                  movzx     eax, byte ptr [rsp + 0x68]
0022CE68  44 0f 22 c0                                     mov       cr8, rax
0022CE6C  8b 54 24 58                                     mov       edx, dword ptr [rsp + 0x58]
0022CE70  45 32 c0                                        xor       r8b, r8b
0022CE73  44 88 64 24 41                                  mov       byte ptr [rsp + 0x41], r12b
0022CE78  eb 08                                           jmp       0x22ce82 ; loc_22CE82 [RVA 0x22CE82]
0022CE7A  45 32 c0                                        xor       r8b, r8b
0022CE7D  44 88 44 24 40                                  mov       byte ptr [rsp + 0x40], r8b
0022CE82  49 8b 4d 00                                     mov       rcx, qword ptr [r13]
0022CE86  45 3b fc                                        cmp       r15d, r12d
0022CE89  0f 85 31 06 00 00                               jne       0x22d4c0 ; loc_22D4C0 [RVA 0x22D4C0]
0022CE8F  48 b8 ff ff fe ff ff 7f 00 00                   movabs    rax, 0x7ffffffeffff
0022CE99  48 3b c8                                        cmp       rcx, rax
0022CE9C  77 1e                                           ja        0x22cebc ; loc_22CEBC [RVA 0x22CEBC]
0022CE9E  65 48 8b 04 25 88 01 00 00                      mov       rax, qword ptr gs:[0x188]
0022CEA7  48 8b 88 b8 00 00 00                            mov       rcx, qword ptr [rax + 0xb8]
0022CEAE  48 83 b9 70 01 00 00 00                         cmp       qword ptr [rcx + 0x170], 0
0022CEB6  0f 85 70 04 00 00                               jne       0x22d32c ; loc_22D32C [RVA 0x22D32C]
0022CEBC  45 33 c9                                        xor       r9d, r9d
0022CEBF  45 8b f1                                        mov       r14d, r9d
0022CEC2  45 84 c0                                        test      r8b, r8b
0022CEC5  0f 85 67 04 00 00                               jne       0x22d332 ; loc_22D332 [RVA 0x22D332]
0022CECB  0f b6 4c 24 41                                  movzx     ecx, byte ptr [rsp + 0x41]
0022CED0  84 c9                                           test      cl, cl
0022CED2  0f 85 6d 03 00 00                               jne       0x22d245 ; loc_22D245 [RVA 0x22D245]
0022CED8  4c 89 6c 24 78                                  mov       qword ptr [rsp + 0x78], r13
0022CEDD  89 ac 24 80 00 00 00                            mov       dword ptr [rsp + 0x80], ebp
0022CEE4  44 89 bc 24 84 00 00 00                         mov       dword ptr [rsp + 0x84], r15d
0022CEEC  45 0f 20 c6                                     mov       r14, cr8
0022CEF0  4c 89 74 24 48                                  mov       qword ptr [rsp + 0x48], r14
0022CEF5  b8 0c 00 00 00                                  mov       eax, 0xc
0022CEFA  44 0f 22 c0                                     mov       cr8, rax
0022CEFE  83 3d 5b 8e d9 00 00                            cmp       dword ptr [rip + 0xd98e5b], 0
0022CF05  0f 85 95 02 00 00                               jne       0x22d1a0 ; loc_22D1A0 [RVA 0x22D1A0]
0022CF0B  65 48 8b 0c 25 20 00 00 00                      mov       rcx, qword ptr gs:[0x20]
0022CF14  44 8b 44 24 44                                  mov       r8d, dword ptr [rsp + 0x44]
0022CF19  48 89 4c 24 60                                  mov       qword ptr [rsp + 0x60], rcx
0022CF1E  45 85 c0                                        test      r8d, r8d
0022CF21  0f 85 b9 02 00 00                               jne       0x22d1e0 ; loc_22D1E0 [RVA 0x22D1E0]
0022CF27  f0 44 09 04 24                                  lock or   dword ptr [rsp], r8d
0022CF2C  48 8b 41 08                                     mov       rax, qword ptr [rcx + 8]
0022CF30  4c 8d b1 b8 2d 00 00                            lea       r14, [rcx + 0x2db8]
0022CF37  4d 8d 66 08                                     lea       r12, [r14 + 8]
0022CF3B  33 d2                                           xor       edx, edx
0022CF3D  41 b8 00 01 00 00                               mov       r8d, 0x100
0022CF43  48 8b 88 b8 00 00 00                            mov       rcx, qword ptr [rax + 0xb8]
0022CF4A  48 8b 81 80 00 00 00                            mov       rax, qword ptr [rcx + 0x80]
0022CF51  49 8b cc                                        mov       rcx, r12
0022CF54  48 89 44 24 70                                  mov       qword ptr [rsp + 0x70], rax
0022CF59  49 c7 06 01 00 20 00                            mov       qword ptr [r14], 0x200001
0022CF60  e8 db 37 49 00                                  call      0x6c0740 ; memset [RVA 0x6C0740]
0022CF65  41 0f b7 4e 02                                  movzx     ecx, word ptr [r14 + 2]
0022CF6A  4c 8b 44 24 70                                  mov       r8, qword ptr [rsp + 0x70]
0022CF6F  0f b7 d1                                        movzx     edx, cx
0022CF72  41 0f b7 00                                     movzx     eax, word ptr [r8]
0022CF76  66 3b c1                                        cmp       ax, cx
0022CF79  66 41 89 4e 02                                  mov       word ptr [r14 + 2], cx
0022CF7E  66 0f 46 d0                                     cmovbe    dx, ax
0022CF82  45 33 c9                                        xor       r9d, r9d
0022CF85  66 41 89 16                                     mov       word ptr [r14], dx
0022CF89  41 0f b7 c9                                     movzx     ecx, r9w
0022CF8D  45 89 4e 04                                     mov       dword ptr [r14 + 4], r9d
0022CF91  66 44 3b ca                                     cmp       r9w, dx
0022CF95  0f 82 93 01 00 00                               jb        0x22d12e ; loc_22D12E [RVA 0x22D12E]
0022CF9B  66 41 3b 4e 02                                  cmp       cx, word ptr [r14 + 2]
0022CFA0  73 12                                           jae       0x22cfb4 ; loc_22CFB4 [RVA 0x22CFB4]
0022CFA2  0f b7 c1                                        movzx     eax, cx
0022CFA5  66 ff c1                                        inc       cx
0022CFA8  4d 89 4c c6 08                                  mov       qword ptr [r14 + rax*8 + 8], r9
0022CFAD  66 41 3b 4e 02                                  cmp       cx, word ptr [r14 + 2]
0022CFB2  72 ee                                           jb        0x22cfa2 ; loc_22CFA2 [RVA 0x22CFA2]
0022CFB4  48 8b 05 35 57 cf 00                            mov       rax, qword ptr [rip + 0xcf5735]
0022CFBB  4c 8b 64 24 60                                  mov       r12, qword ptr [rsp + 0x60]
0022CFC0  41 8b 4c 24 24                                  mov       ecx, dword ptr [r12 + 0x24]
0022CFC5  44 8b 04 88                                     mov       r8d, dword ptr [rax + rcx*4]
0022CFC9  41 8b c8                                        mov       ecx, r8d
0022CFCC  41 0f b7 06                                     movzx     eax, word ptr [r14]
0022CFD0  c1 e9 06                                        shr       ecx, 6
0022CFD3  3b c1                                           cmp       eax, ecx
0022CFD5  0f 87 fa 00 00 00                               ja        0x22d0d5 ; loc_22D0D5 [RVA 0x22D0D5]
0022CFDB  41 0f b7 c9                                     movzx     ecx, r9w
0022CFDF  66 41 3b 0e                                     cmp       cx, word ptr [r14]
0022CFE3  0f 83 e6 01 00 00                               jae       0x22d1cf ; loc_22D1CF [RVA 0x22D1CF]
0022CFE9  0f b7 c1                                        movzx     eax, cx
0022CFEC  49 8b 54 c6 08                                  mov       rdx, qword ptr [r14 + rax*8 + 8]
0022CFF1  48 85 d2                                        test      rdx, rdx
0022CFF4  74 20                                           je        0x22d016 ; loc_22D016 [RVA 0x22D016]
0022CFF6  41 0f b6 84 24 d0 00 00 00                      movzx     eax, byte ptr [r12 + 0xd0]
0022CFFF  66 3b c8                                        cmp       cx, ax
0022D002  0f 85 bc 03 00 00                               jne       0x22d3c4 ; loc_22D3C4 [RVA 0x22D3C4]
0022D008  49 3b 94 24 c8 00 00 00                         cmp       rdx, qword ptr [r12 + 0xc8]
0022D010  0f 85 ae 03 00 00                               jne       0x22d3c4 ; loc_22D3C4 [RVA 0x22D3C4]
0022D016  66 ff c1                                        inc       cx
0022D019  eb c4                                           jmp       0x22cfdf ; loc_22CFDF [RVA 0x22CFDF]
0022D01B  41 84 ce                                        test      r14b, cl
0022D01E  0f 85 cd 00 00 00                               jne       0x22d0f1 ; loc_22D0F1 [RVA 0x22D0F1]
0022D024  f6 c1 04                                        test      cl, 4
0022D027  0f 84 a3 fa ff ff                               je        0x22cad0 ; loc_22CAD0 [RVA 0x22CAD0]
0022D02D  83 c8 04                                        or        eax, 4
0022D030  e9 9b fa ff ff                                  jmp       0x22cad0 ; loc_22CAD0 [RVA 0x22CAD0]
0022D035  33 d2                                           xor       edx, edx
0022D037  48 8d 8c 24 c0 00 00 00                         lea       rcx, [rsp + 0xc0]
0022D03F  41 b8 08 01 00 00                               mov       r8d, 0x108
0022D045  e8 f6 36 49 00                                  call      0x6c0740 ; memset [RVA 0x6C0740]
0022D04A  80 3d ef 47 fd 00 00                            cmp       byte ptr [rip + 0xfd47ef], 0
0022D051  0f 84 f9 00 00 00                               je        0x22d150 ; loc_22D150 [RVA 0x22D150]
0022D057  41 83 ff 03                                     cmp       r15d, 3
0022D05B  0f 85 af 02 00 00                               jne       0x22d310 ; loc_22D310 [RVA 0x22D310]
0022D061  48 c7 44 24 50 00 00 00 00                      mov       qword ptr [rsp + 0x50], 0
0022D06A  c6 44 24 40 01                                  mov       byte ptr [rsp + 0x40], 1
0022D06F  48 8d 8c 24 c0 00 00 00                         lea       rcx, [rsp + 0xc0]
0022D077  e8 b8 0d 00 00                                  call      0x22de34 ; KiPrepareFlushCurrentAffinity [RVA 0x22DE34]
0022D07C  44 0f b6 44 24 40                               movzx     r8d, byte ptr [rsp + 0x40]
0022D082  48 8d 94 24 c0 00 00 00                         lea       rdx, [rsp + 0xc0]
0022D08A  48 8b 4c 24 50                                  mov       rcx, qword ptr [rsp + 0x50]
0022D08F  45 8b cc                                        mov       r9d, r12d
0022D092  4c 89 6c 24 30                                  mov       qword ptr [rsp + 0x30], r13
0022D097  89 6c 24 28                                     mov       dword ptr [rsp + 0x28], ebp
0022D09B  44 88 74 24 20                                  mov       byte ptr [rsp + 0x20], r14b
0022D0A0  e8 7b 05 00 00                                  call      0x22d620 ; HvlFlushRangeListTb [RVA 0x22D620]
0022D0A5  84 c0                                           test      al, al
0022D0A7  0f 85 b1 fa ff ff                               jne       0x22cb5e ; loc_22CB5E [RVA 0x22CB5E]
0022D0AD  e9 a4 fa ff ff                                  jmp       0x22cb56 ; loc_22CB56 [RVA 0x22CB56]
0022D0B2  44 88 63 19                                     mov       byte ptr [rbx + 0x19], r12b
0022D0B6  48 8d 7b 10                                     lea       rdi, [rbx + 0x10]
0022D0BA  e9 13 fb ff ff                                  jmp       0x22cbd2 ; loc_22CBD2 [RVA 0x22CBD2]
0022D0BF  8b 07                                           mov       eax, dword ptr [rdi]
0022D0C1  8b 0e                                           mov       ecx, dword ptr [rsi]
0022D0C3  a8 01                                           test      al, 1
0022D0C5  0f 84 63 04 00 00                               je        0x22d52e ; loc_22D52E [RVA 0x22D52E]
0022D0CB  e8 2c 0e 00 00                                  call      0x22defc ; KeFlushCurrentTbOnly [RVA 0x22DEFC]
0022D0D0  e9 17 fb ff ff                                  jmp       0x22cbec ; loc_22CBEC [RVA 0x22CBEC]
0022D0D5  49 8b 44 ce 08                                  mov       rax, qword ptr [r14 + rcx*8 + 8]
0022D0DA  49 8d 14 ce                                     lea       rdx, [r14 + rcx*8]
0022D0DE  41 8b c8                                        mov       ecx, r8d
0022D0E1  83 e1 3f                                        and       ecx, 0x3f
0022D0E4  48 0f b3 c8                                     btr       rax, rcx
0022D0E8  48 89 42 08                                     mov       qword ptr [rdx + 8], rax
0022D0EC  e9 ea fe ff ff                                  jmp       0x22cfdb ; loc_22CFDB [RVA 0x22CFDB]
0022D0F1  41 0b c6                                        or        eax, r14d
0022D0F4  e9 d7 f9 ff ff                                  jmp       0x22cad0 ; loc_22CAD0 [RVA 0x22CAD0]
0022D0F9  83 3d 60 8c d9 00 00                            cmp       dword ptr [rip + 0xd98c60], 0
0022D100  74 12                                           je        0x22d114 ; loc_22D114 [RVA 0x22D114]
0022D102  44 0f 20 c1                                     mov       rcx, cr8
0022D106  41 0f b6 d2                                     movzx     edx, r10b
0022D10A  e8 c9 82 2c 00                                  call      0x4f53d8 ; KiLowerIrqlProcessIrqlFlags [RVA 0x4F53D8]
0022D10F  4c 8b 54 24 68                                  mov       r10, qword ptr [rsp + 0x68]
0022D114  41 0f b6 c2                                     movzx     eax, r10b
0022D118  44 0f 22 c0                                     mov       cr8, rax
0022D11C  8b 54 24 58                                     mov       edx, dword ptr [rsp + 0x58]
0022D120  45 0f b6 c4                                     movzx     r8d, r12b
0022D124  44 88 64 24 40                                  mov       byte ptr [rsp + 0x40], r12b
0022D129  e9 54 fd ff ff                                  jmp       0x22ce82 ; loc_22CE82 [RVA 0x22CE82]
0022D12E  4d 2b c6                                        sub       r8, r14
0022D131  44 0f b7 ca                                     movzx     r9d, dx
0022D135  0f b7 ca                                        movzx     ecx, dx
0022D138  4b 8b 04 20                                     mov       rax, qword ptr [r8 + r12]
0022D13C  49 89 04 24                                     mov       qword ptr [r12], rax
0022D140  4d 8d 64 24 08                                  lea       r12, [r12 + 8]
0022D145  49 83 e9 01                                     sub       r9, 1
0022D149  75 ed                                           jne       0x22d138 ; loc_22D138 [RVA 0x22D138]
0022D14B  e9 4b fe ff ff                                  jmp       0x22cf9b ; loc_22CF9B [RVA 0x22CF9B]
0022D150  41 83 ff 01                                     cmp       r15d, 1
0022D154  0f 85 07 ff ff ff                               jne       0x22d061 ; loc_22D061 [RVA 0x22D061]
0022D15A  65 48 8b 04 25 88 01 00 00                      mov       rax, qword ptr gs:[0x188]
0022D163  c6 44 24 40 00                                  mov       byte ptr [rsp + 0x40], 0
0022D168  48 8b 88 b8 00 00 00                            mov       rcx, qword ptr [rax + 0xb8]
0022D16F  48 8b 41 28                                     mov       rax, qword ptr [rcx + 0x28]
0022D173  48 25 00 f0 ff ff                               and       rax, 0xfffffffffffff000
0022D179  48 89 44 24 50                                  mov       qword ptr [rsp + 0x50], rax
0022D17E  e9 ec fe ff ff                                  jmp       0x22d06f ; loc_22D06F [RVA 0x22D06F]
0022D183  0f b6 d0                                        movzx     edx, al
0022D186  41 0f b6 ce                                     movzx     ecx, r14b
0022D18A  e8 ad 82 2c 00                                  call      0x4f543c ; KiRaiseIrqlProcessIrqlFlags [RVA 0x4F543C]
0022D18F  e9 03 fa ff ff                                  jmp       0x22cb97 ; loc_22CB97 [RVA 0x22CB97]
0022D194  41 d0 ee                                        shr       r14b, 1
0022D197  41 80 e6 01                                     and       r14b, 1
0022D19B  e9 a4 f9 ff ff                                  jmp       0x22cb44 ; loc_22CB44 [RVA 0x22CB44]
0022D1A0  0f b6 d0                                        movzx     edx, al
0022D1A3  41 0f b6 ce                                     movzx     ecx, r14b
0022D1A7  e8 90 82 2c 00                                  call      0x4f543c ; KiRaiseIrqlProcessIrqlFlags [RVA 0x4F543C]
0022D1AC  e9 5a fd ff ff                                  jmp       0x22cf0b ; loc_22CF0B [RVA 0x22CF0B]
0022D1B1  0f b6 d1                                        movzx     edx, cl
0022D1B4  41 0f b6 ca                                     movzx     ecx, r10b
0022D1B8  e8 7f 82 2c 00                                  call      0x4f543c ; KiRaiseIrqlProcessIrqlFlags [RVA 0x4F543C]
0022D1BD  4c 8b 54 24 68                                  mov       r10, qword ptr [rsp + 0x68]
0022D1C2  e9 d6 fa ff ff                                  jmp       0x22cc9d ; loc_22CC9D [RVA 0x22CC9D]
0022D1C7  45 8b cc                                        mov       r9d, r12d
0022D1CA  e9 ae f8 ff ff                                  jmp       0x22ca7d ; loc_22CA7D [RVA 0x22CA7D]
0022D1CF  4c 8b 74 24 48                                  mov       r14, qword ptr [rsp + 0x48]
0022D1D4  48 8d 4c 24 78                                  lea       rcx, [rsp + 0x78]
0022D1D9  e8 a2 f2 ff ff                                  call      0x22c480 ; KiFlushRangeWorker [RVA 0x22C480]
0022D1DE  eb 47                                           jmp       0x22d227 ; loc_22D227 [RVA 0x22D227]
0022D1E0  8b 05 da 88 d9 00                               mov       eax, dword ptr [rip + 0xd988da]
0022D1E6  41 3b c4                                        cmp       eax, r12d
0022D1E9  76 e9                                           jbe       0x22d1d4 ; loc_22D1D4 [RVA 0x22D1D4]
0022D1EB  45 3b c4                                        cmp       r8d, r12d
0022D1EE  4c 8d 4c 24 78                                  lea       r9, [rsp + 0x78]
0022D1F3  ba 04 00 00 00                                  mov       edx, 4
0022D1F8  b8 04 00 00 80                                  mov       eax, 0x80000004
0022D1FD  0f 45 c2                                        cmovne    eax, edx
0022D200  45 33 c0                                        xor       r8d, r8d
0022D203  41 8b d4                                        mov       edx, r12d
0022D206  48 89 44 24 20                                  mov       qword ptr [rsp + 0x20], rax
0022D20B  e8 c0 eb ff ff                                  call      0x22bdd0 ; KiIpiSendRequest [RVA 0x22BDD0]
0022D210  4c 8b 64 24 60                                  mov       r12, qword ptr [rsp + 0x60]
0022D215  48 8d 4c 24 78                                  lea       rcx, [rsp + 0x78]
0022D21A  e8 61 f2 ff ff                                  call      0x22c480 ; KiFlushRangeWorker [RVA 0x22C480]
0022D21F  49 8b cc                                        mov       rcx, r12
0022D222  e8 a9 0b 00 00                                  call      0x22ddd0 ; KiIpiWaitForRequestBarrier [RVA 0x22DDD0]
0022D227  83 3d 32 8b d9 00 00                            cmp       dword ptr [rip + 0xd98b32], 0
0022D22E  74 0d                                           je        0x22d23d ; loc_22D23D [RVA 0x22D23D]
0022D230  44 0f 20 c1                                     mov       rcx, cr8
0022D234  41 0f b6 d6                                     movzx     edx, r14b
0022D238  e8 9b 81 2c 00                                  call      0x4f53d8 ; KiLowerIrqlProcessIrqlFlags [RVA 0x4F53D8]
0022D23D  41 0f b6 c6                                     movzx     eax, r14b
0022D241  44 0f 22 c0                                     mov       cr8, rax
0022D245  41 83 ff 01                                     cmp       r15d, 1
0022D249  75 2f                                           jne       0x22d27a ; loc_22D27A [RVA 0x22D27A]
0022D24B  80 3d b2 6d d9 00 00                            cmp       byte ptr [rip + 0xd96db2], 0
0022D252  74 26                                           je        0x22d27a ; loc_22D27A [RVA 0x22D27A]
0022D254  65 48 8b 04 25 88 01 00 00                      mov       rax, qword ptr gs:[0x188]
0022D25D  48 8b 88 b8 00 00 00                            mov       rcx, qword ptr [rax + 0xb8]
0022D264  48 8b 89 70 06 00 00                            mov       rcx, qword ptr [rcx + 0x670]
0022D26B  48 85 c9                                        test      rcx, rcx
0022D26E  74 0a                                           je        0x22d27a ; loc_22D27A [RVA 0x22D27A]
0022D270  4d 8b c5                                        mov       r8, r13
0022D273  8b d5                                           mov       edx, ebp
0022D275  e8 66 f3 ff ff                                  call      0x22c5e0 ; VmpFlushTb [RVA 0x22C5E0]
0022D27A  83 3d 53 b2 c3 00 00                            cmp       dword ptr [rip + 0xc3b253], 0
0022D281  0f 84 3b f9 ff ff                               je        0x22cbc2 ; loc_22CBC2 [RVA 0x22CBC2]
0022D287  45 0f 20 c6                                     mov       r14, cr8
0022D28B  b8 0f 00 00 00                                  mov       eax, 0xf
0022D290  44 0f 22 c0                                     mov       cr8, rax
0022D294  83 3d c5 8a d9 00 00                            cmp       dword ptr [rip + 0xd98ac5], 0
0022D29B  75 57                                           jne       0x22d2f4 ; loc_22D2F4 [RVA 0x22D2F4]
0022D29D  65 48 8b 04 25 88 01 00 00                      mov       rax, qword ptr gs:[0x188]
0022D2A6  48 8b 88 b8 00 00 00                            mov       rcx, qword ptr [rax + 0xb8]
0022D2AD  41 83 ff 01                                     cmp       r15d, 1
0022D2B1  0f 85 40 01 00 00                               jne       0x22d3f7 ; loc_22D3F7 [RVA 0x22D3F7]
0022D2B7  4c 8b 89 08 06 00 00                            mov       r9, qword ptr [rcx + 0x608]
0022D2BE  4c 8b b9 e8 07 00 00                            mov       r15, qword ptr [rcx + 0x7e8]
0022D2C5  4d 85 c9                                        test      r9, r9
0022D2C8  0f 85 3a 02 00 00                               jne       0x22d508 ; loc_22D508 [RVA 0x22D508]
0022D2CE  4d 85 ff                                        test      r15, r15
0022D2D1  0f 84 cd f8 ff ff                               je        0x22cba4 ; loc_22CBA4 [RVA 0x22CBA4]
0022D2D7  48 8b 05 2a 9a bd 00                            mov       rax, qword ptr [rip + 0xbd9a2a]
0022D2DE  49 8b cf                                        mov       rcx, r15
0022D2E1  4d 8b c5                                        mov       r8, r13
0022D2E4  8b d5                                           mov       edx, ebp
0022D2E6  48 8b 40 48                                     mov       rax, qword ptr [rax + 0x48]
0022D2EA  e8 11 72 48 00                                  call      0x6b4500 ; guard_dispatch_icall/guard_dispatch_icall_no_overrides [RVA 0x6B4500]
0022D2EF  e9 b0 f8 ff ff                                  jmp       0x22cba4 ; loc_22CBA4 [RVA 0x22CBA4]
0022D2F4  0f b6 d0                                        movzx     edx, al
0022D2F7  41 0f b6 ce                                     movzx     ecx, r14b
0022D2FB  e8 3c 81 2c 00                                  call      0x4f543c ; KiRaiseIrqlProcessIrqlFlags [RVA 0x4F543C]
0022D300  eb 9b                                           jmp       0x22d29d ; loc_22D29D [RVA 0x22D29D]
0022D302  45 0f b6 c4                                     movzx     r8d, r12b
0022D306  44 88 64 24 40                                  mov       byte ptr [rsp + 0x40], r12b
0022D30B  e9 72 fb ff ff                                  jmp       0x22ce82 ; loc_22CE82 [RVA 0x22CE82]
0022D310  45 85 ff                                        test      r15d, r15d
0022D313  0f 85 48 fd ff ff                               jne       0x22d061 ; loc_22D061 [RVA 0x22D061]
0022D319  48 c7 44 24 50 00 00 00 00                      mov       qword ptr [rsp + 0x50], 0
0022D322  44 88 7c 24 40                                  mov       byte ptr [rsp + 0x40], r15b
0022D327  e9 43 fd ff ff                                  jmp       0x22d06f ; loc_22D06F [RVA 0x22D06F]
0022D32C  45 8b f4                                        mov       r14d, r12d
0022D32F  45 33 c9                                        xor       r9d, r9d
0022D332  80 3d 07 45 fd 00 00                            cmp       byte ptr [rip + 0xfd4507], 0
0022D339  74 60                                           je        0x22d39b ; loc_22D39B [RVA 0x22D39B]
0022D33B  41 83 ff 03                                     cmp       r15d, 3
0022D33F  0f 85 d3 00 00 00                               jne       0x22d418 ; loc_22D418 [RVA 0x22D418]
0022D345  4d 8b d1                                        mov       r10, r9
0022D348  41 0f b6 d4                                     movzx     edx, r12b
0022D34C  83 7c 24 44 00                                  cmp       dword ptr [rsp + 0x44], 0
0022D351  75 6c                                           jne       0x22d3bf ; loc_22D3BF [RVA 0x22D3BF]
0022D353  f0 83 0c 24 00                                  lock or   dword ptr [rsp], 0
0022D358  65 48 8b 04 25 88 01 00 00                      mov       rax, qword ptr gs:[0x188]
0022D361  48 8b 88 b8 00 00 00                            mov       rcx, qword ptr [rax + 0xb8]
0022D368  48 8b 81 80 00 00 00                            mov       rax, qword ptr [rcx + 0x80]
0022D36F  4c 89 6c 24 30                                  mov       qword ptr [rsp + 0x30], r13
0022D374  45 8b ce                                        mov       r9d, r14d
0022D377  89 6c 24 28                                     mov       dword ptr [rsp + 0x28], ebp
0022D37B  49 8b ca                                        mov       rcx, r10
0022D37E  44 88 44 24 20                                  mov       byte ptr [rsp + 0x20], r8b
0022D383  44 0f b6 c2                                     movzx     r8d, dl
0022D387  48 8b d0                                        mov       rdx, rax
0022D38A  e8 91 02 00 00                                  call      0x22d620 ; HvlFlushRangeListTb [RVA 0x22D620]
0022D38F  0f b6 4c 24 41                                  movzx     ecx, byte ptr [rsp + 0x41]
0022D394  0a c8                                           or        cl, al
0022D396  e9 35 fb ff ff                                  jmp       0x22ced0 ; loc_22CED0 [RVA 0x22CED0]
0022D39B  45 3b fc                                        cmp       r15d, r12d
0022D39E  75 a5                                           jne       0x22d345 ; loc_22D345 [RVA 0x22D345]
0022D3A0  65 48 8b 04 25 88 01 00 00                      mov       rax, qword ptr gs:[0x188]
0022D3A9  48 8b 88 b8 00 00 00                            mov       rcx, qword ptr [rax + 0xb8]
0022D3B0  4c 8b 51 28                                     mov       r10, qword ptr [rcx + 0x28]
0022D3B4  49 81 e2 00 f0 ff ff                            and       r10, 0xfffffffffffff000
0022D3BB  32 d2                                           xor       dl, dl
0022D3BD  eb 8d                                           jmp       0x22d34c ; loc_22D34C [RVA 0x22D34C]
0022D3BF  49 8b c1                                        mov       rax, r9
0022D3C2  eb ab                                           jmp       0x22d36f ; loc_22D36F [RVA 0x22D36F]
0022D3C4  83 7c 24 44 01                                  cmp       dword ptr [rsp + 0x44], 1
0022D3C9  4c 8d 4c 24 78                                  lea       r9, [rsp + 0x78]
0022D3CE  ba 04 00 00 00                                  mov       edx, 4
0022D3D3  b8 04 00 00 80                                  mov       eax, 0x80000004
0022D3D8  0f 45 c2                                        cmovne    eax, edx
0022D3DB  4d 8b c6                                        mov       r8, r14
0022D3DE  33 d2                                           xor       edx, edx
0022D3E0  48 89 44 24 20                                  mov       qword ptr [rsp + 0x20], rax
0022D3E5  49 8b cc                                        mov       rcx, r12
0022D3E8  e8 e3 e9 ff ff                                  call      0x22bdd0 ; KiIpiSendRequest [RVA 0x22BDD0]
0022D3ED  4c 8b 74 24 48                                  mov       r14, qword ptr [rsp + 0x48]
0022D3F2  e9 1e fe ff ff                                  jmp       0x22d215 ; loc_22D215 [RVA 0x22D215]
0022D3F7  48 8b 05 0a 99 bd 00                            mov       rax, qword ptr [rip + 0xbd990a]
0022D3FE  4d 8b c5                                        mov       r8, r13
0022D401  48 8b 0d d8 d2 cc 00                            mov       rcx, qword ptr [rip + 0xccd2d8]
0022D408  8b d5                                           mov       edx, ebp
0022D40A  48 8b 40 50                                     mov       rax, qword ptr [rax + 0x50]
0022D40E  e8 ed 70 48 00                                  call      0x6b4500 ; guard_dispatch_icall/guard_dispatch_icall_no_overrides [RVA 0x6B4500]
0022D413  e9 8c f7 ff ff                                  jmp       0x22cba4 ; loc_22CBA4 [RVA 0x22CBA4]
0022D418  45 85 ff                                        test      r15d, r15d
0022D41B  0f 85 24 ff ff ff                               jne       0x22d345 ; loc_22D345 [RVA 0x22D345]
0022D421  4d 8b d1                                        mov       r10, r9
0022D424  32 d2                                           xor       dl, dl
0022D426  e9 21 ff ff ff                                  jmp       0x22d34c ; loc_22D34C [RVA 0x22D34C]
0022D42B  4d 8b f5                                        mov       r14, r13
0022D42E  4c 8b e5                                        mov       r12, rbp
0022D431  49 8b 0e                                        mov       rcx, qword ptr [r14]
0022D434  41 8b d7                                        mov       edx, r15d
0022D437  e8 a4 b6 1d 00                                  call      0x408ae0 ; KiFlushRangeTb [RVA 0x408AE0]
0022D43C  4d 8d 76 08                                     lea       r14, [r14 + 8]
0022D440  49 83 ec 01                                     sub       r12, 1
0022D444  0f 84 14 f7 ff ff                               je        0x22cb5e ; loc_22CB5E [RVA 0x22CB5E]
0022D44A  eb e5                                           jmp       0x22d431 ; loc_22D431 [RVA 0x22D431]
0022D44C  49 c7 c0 ff ff ff ff                            mov       r8, 0xffffffffffffffff
0022D453  e9 06 f6 ff ff                                  jmp       0x22ca5e ; loc_22CA5E [RVA 0x22CA5E]
0022D458  e8 93 e6 47 00                                  call      0x6abaf0 ; KiSetUserTbFlushPending [RVA 0x6ABAF0]
0022D45D  48 8b 54 24 48                                  mov       rdx, qword ptr [rsp + 0x48]
0022D462  e9 12 f9 ff ff                                  jmp       0x22cd79 ; loc_22CD79 [RVA 0x22CD79]
0022D467  e8 84 e6 47 00                                  call      0x6abaf0 ; KiSetUserTbFlushPending [RVA 0x6ABAF0]
0022D46C  48 8b 54 24 48                                  mov       rdx, qword ptr [rsp + 0x48]
0022D471  4c 8b 84 24 88 00 00 00                         mov       r8, qword ptr [rsp + 0x88]
0022D479  e9 89 f9 ff ff                                  jmp       0x22ce07 ; loc_22CE07 [RVA 0x22CE07]
0022D47E  45 0f b6 c4                                     movzx     r8d, r12b
0022D482  44 88 64 24 40                                  mov       byte ptr [rsp + 0x40], r12b
0022D487  e9 f6 f9 ff ff                                  jmp       0x22ce82 ; loc_22CE82 [RVA 0x22CE82]
0022D48C  45 85 ff                                        test      r15d, r15d
0022D48F  0f 85 97 f6 ff ff                               jne       0x22cb2c ; loc_22CB2C [RVA 0x22CB2C]
0022D495  81 e2 09 00 00 80                               and       edx, 0x80000009
0022D49B  81 fa 01 00 00 80                               cmp       edx, 0x80000001
0022D4A1  0f 85 85 f6 ff ff                               jne       0x22cb2c ; loc_22CB2C [RVA 0x22CB2C]
0022D4A7  33 c9                                           xor       ecx, ecx
0022D4A9  e8 46 50 10 00                                  call      0x3324f4 ; VslGetNestedPageProtectionFlags [RVA 0x3324F4]
0022D4AE  0f ba e0 0e                                     bt        eax, 0xe
0022D4B2  0f 83 74 f6 ff ff                               jae       0x22cb2c ; loc_22CB2C [RVA 0x22CB2C]
0022D4B8  45 8b e6                                        mov       r12d, r14d
0022D4BB  e9 6f f6 ff ff                                  jmp       0x22cb2f ; loc_22CB2F [RVA 0x22CB2F]
0022D4C0  45 85 ff                                        test      r15d, r15d
0022D4C3  0f 85 f3 f9 ff ff                               jne       0x22cebc ; loc_22CEBC [RVA 0x22CEBC]
0022D4C9  81 e2 09 00 00 80                               and       edx, 0x80000009
0022D4CF  81 fa 01 00 00 80                               cmp       edx, 0x80000001
0022D4D5  0f 85 e1 f9 ff ff                               jne       0x22cebc ; loc_22CEBC [RVA 0x22CEBC]
0022D4DB  33 c9                                           xor       ecx, ecx
0022D4DD  e8 12 50 10 00                                  call      0x3324f4 ; VslGetNestedPageProtectionFlags [RVA 0x3324F4]
0022D4E2  44 0f b6 44 24 40                               movzx     r8d, byte ptr [rsp + 0x40]
0022D4E8  0f ba e0 0e                                     bt        eax, 0xe
0022D4EC  0f 83 ca f9 ff ff                               jae       0x22cebc ; loc_22CEBC [RVA 0x22CEBC]
0022D4F2  45 33 c9                                        xor       r9d, r9d
0022D4F5  e9 38 fe ff ff                                  jmp       0x22d332 ; loc_22D332 [RVA 0x22D332]
0022D4FA  45 0f b6 c4                                     movzx     r8d, r12b
0022D4FE  44 88 64 24 40                                  mov       byte ptr [rsp + 0x40], r12b
0022D503  e9 7a f9 ff ff                                  jmp       0x22ce82 ; loc_22CE82 [RVA 0x22CE82]
0022D508  48 8b 05 f9 97 bd 00                            mov       rax, qword ptr [rip + 0xbd97f9]
0022D50F  4d 8b c5                                        mov       r8, r13
0022D512  8b d5                                           mov       edx, ebp
0022D514  49 8b c9                                        mov       rcx, r9
0022D517  48 8b 40 48                                     mov       rax, qword ptr [rax + 0x48]
0022D51B  e8 e0 6f 48 00                                  call      0x6b4500 ; guard_dispatch_icall/guard_dispatch_icall_no_overrides [RVA 0x6B4500]
0022D520  4d 85 ff                                        test      r15, r15
0022D523  0f 84 7b f6 ff ff                               je        0x22cba4 ; loc_22CBA4 [RVA 0x22CBA4]
0022D529  e9 a9 fd ff ff                                  jmp       0x22d2d7 ; loc_22D2D7 [RVA 0x22D2D7]
0022D52E  41 8b d1                                        mov       edx, r9d
0022D531  e8 1a 16 00 00                                  call      0x22eb50 ; KeFlushTb [RVA 0x22EB50]
0022D536  e9 b1 f6 ff ff                                  jmp       0x22cbec ; loc_22CBEC [RVA 0x22CBEC]
