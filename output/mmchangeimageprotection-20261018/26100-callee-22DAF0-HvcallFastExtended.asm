HvcallFastExtended at RVA 0x22DAF0
extent source: next public symbol (first .pdata end 0x22DDC8); RVA 0x22DAF0..0x22DDD0 (exclusive)
shown: 200 instruction(s), max 200
All addresses are RVAs.

0022DAF0  40 55                                           push      rbp
0022DAF2  53                                              push      rbx
0022DAF3  56                                              push      rsi
0022DAF4  57                                              push      rdi
0022DAF5  41 54                                           push      r12
0022DAF7  41 55                                           push      r13
0022DAF9  41 56                                           push      r14
0022DAFB  41 57                                           push      r15
0022DAFD  48 8d 6c 24 e9                                  lea       rbp, [rsp - 0x17]
0022DB02  48 81 ec 98 00 00 00                            sub       rsp, 0x98
0022DB09  48 8b 05 30 cc bd 00                            mov       rax, qword ptr [rip + 0xbdcc30]
0022DB10  48 33 c4                                        xor       rax, rsp
0022DB13  48 89 45 ff                                     mov       qword ptr [rbp - 1], rax
0022DB17  8b 05 37 80 d9 00                               mov       eax, dword ptr [rip + 0xd98037]
0022DB1D  0f 57 c0                                        xorps     xmm0, xmm0
0022DB20  45 33 ed                                        xor       r13d, r13d
0022DB23  4d 8b e1                                        mov       r12, r9
0022DB26  45 8b f0                                        mov       r14d, r8d
0022DB29  4c 8b fa                                        mov       r15, rdx
0022DB2C  48 8b d9                                        mov       rbx, rcx
0022DB2F  0f 11 45 bf                                     movups    xmmword ptr [rbp - 0x41], xmm0
0022DB33  0f 11 45 cf                                     movups    xmmword ptr [rbp - 0x31], xmm0
0022DB37  a8 10                                           test      al, 0x10
0022DB39  0f 85 d2 01 00 00                               jne       0x22dd11 ; loc_22DD11 [RVA 0x22DD11]
0022DB3F  40 32 f6                                        xor       sil, sil
0022DB42  8b 45 7f                                        mov       eax, dword ptr [rbp + 0x7f]
0022DB45  45 8d 46 0f                                     lea       r8d, [r14 + 0xf]
0022DB49  41 c1 e8 04                                     shr       r8d, 4
0022DB4D  49 8b d7                                        mov       rdx, r15
0022DB50  48 8b cb                                        mov       rcx, rbx
0022DB53  85 c0                                           test      eax, eax
0022DB55  0f 85 e5 00 00 00                               jne       0x22dc40 ; loc_22DC40 [RVA 0x22DC40]
0022DB5B  45 8b c6                                        mov       r8d, r14d
0022DB5E  e8 0d 7d 48 00                                  call      0x6b5870 ; HvcallpExtendedFastHypercall [RVA 0x6B5870]
0022DB63  48 8b f8                                        mov       rdi, rax
0022DB66  40 84 f6                                        test      sil, sil
0022DB69  0f 84 ad 00 00 00                               je        0x22dc1c ; loc_22DC1C [RVA 0x22DC1C]
0022DB6F  0f b7 c3                                        movzx     eax, bx
0022DB72  4c 89 6d af                                     mov       qword ptr [rbp - 0x51], r13
0022DB76  89 45 af                                        mov       dword ptr [rbp - 0x51], eax
0022DB79  8b c3                                           mov       eax, ebx
0022DB7B  c1 eb 1f                                        shr       ebx, 0x1f
0022DB7E  c1 e8 10                                        shr       eax, 0x10
0022DB81  24 01                                           and       al, 1
0022DB83  88 5d b4                                        mov       byte ptr [rbp - 0x4c], bl
0022DB86  48 8b 1d 5b 7f d9 00                            mov       rbx, qword ptr [rip + 0xd97f5b]
0022DB8D  88 45 b3                                        mov       byte ptr [rbp - 0x4d], al
0022DB90  48 8d 45 af                                     lea       rax, [rbp - 0x51]
0022DB94  48 89 45 ef                                     mov       qword ptr [rbp - 0x11], rax
0022DB98  48 c7 45 f7 08 00 00 00                         mov       qword ptr [rbp - 9], 8
0022DBA0  8b 9b a8 11 00 00                               mov       ebx, dword ptr [rbx + 0x11a8]
0022DBA6  0f bc cb                                        bsf       ecx, ebx
0022DBA9  74 71                                           je        0x22dc1c ; loc_22DC1C [RVA 0x22DC1C]
0022DBAB  0f 1f 44 00 00                                  nop       dword ptr [rax + rax]
0022DBB0  48 8b 15 31 7f d9 00                            mov       rdx, qword ptr [rip + 0xd97f31]
0022DBB7  8d 43 ff                                        lea       eax, [rbx - 1]
0022DBBA  23 d8                                           and       ebx, eax
0022DBBC  44 8b c1                                        mov       r8d, ecx
0022DBBF  8b c1                                           mov       eax, ecx
0022DBC1  48 c1 e0 05                                     shl       rax, 5
0022DBC5  48 05 cc 11 00 00                               add       rax, 0x11cc
0022DBCB  48 03 c2                                        add       rax, rdx
0022DBCE  74 47                                           je        0x22dc17 ; loc_22DC17 [RVA 0x22DC17]
0022DBD0  8b 40 14                                        mov       eax, dword ptr [rax + 0x14]
0022DBD3  a8 10                                           test      al, 0x10
0022DBD5  74 40                                           je        0x22dc17 ; loc_22DC17 [RVA 0x22DC17]
0022DBD7  0f b6 84 4a 99 11 00 00                         movzx     eax, byte ptr [rdx + rcx*2 + 0x1199]
0022DBDF  41 b9 02 00 00 00                               mov       r9d, 2
0022DBE5  48 c7 45 e7 08 00 00 00                         mov       qword ptr [rbp - 0x19], 8
0022DBED  c7 44 24 28 02 0e 50 00                         mov       dword ptr [rsp + 0x28], 0x500e02
0022DBF5  66 c7 44 24 20 72 0f                            mov       word ptr [rsp + 0x20], 0xf72
0022DBFC  48 8d 4c c5 b7                                  lea       rcx, [rbp + rax*8 - 0x49]
0022DC01  48 89 4d df                                     mov       qword ptr [rbp - 0x21], rcx
0022DC05  48 8d 4d df                                     lea       rcx, [rbp - 0x21]
0022DC09  46 0f b6 84 42 98 11 00 00                      movzx     r8d, byte ptr [rdx + r8*2 + 0x1198]
0022DC12  e8 19 45 04 00                                  call      0x272130 ; EtwpLogKernelEvent [RVA 0x272130]
0022DC17  0f bc cb                                        bsf       ecx, ebx
0022DC1A  75 94                                           jne       0x22dbb0 ; loc_22DBB0 [RVA 0x22DBB0]
0022DC1C  48 8b c7                                        mov       rax, rdi
0022DC1F  48 8b 4d ff                                     mov       rcx, qword ptr [rbp - 1]
0022DC23  48 33 cc                                        xor       rcx, rsp
0022DC26  e8 65 84 47 00                                  call      0x6a6090 ; _security_check_cookie [RVA 0x6A6090]
0022DC2B  48 81 c4 98 00 00 00                            add       rsp, 0x98
0022DC32  41 5f                                           pop       r15
0022DC34  41 5e                                           pop       r14
0022DC36  41 5d                                           pop       r13
0022DC38  41 5c                                           pop       r12
0022DC3A  5f                                              pop       rdi
0022DC3B  5e                                              pop       rsi
0022DC3C  5b                                              pop       rbx
0022DC3D  5d                                              pop       rbp
0022DC3E  c3                                              ret
0022DC3F  cc                                              int3
0022DC40  c1 e8 04                                        shr       eax, 4
0022DC43  45 33 c9                                        xor       r9d, r9d
0022DC46  89 44 24 28                                     mov       dword ptr [rsp + 0x28], eax
0022DC4A  4c 89 64 24 20                                  mov       qword ptr [rsp + 0x20], r12
0022DC4F  e8 7c 7c 48 00                                  call      0x6b58d0 ; HvcallpExtendedFastHypercallWithOutput [RVA 0x6B58D0]
0022DC54  48 8b f8                                        mov       rdi, rax
0022DC57  40 84 f6                                        test      sil, sil
0022DC5A  74 c0                                           je        0x22dc1c ; loc_22DC1C [RVA 0x22DC1C]
0022DC5C  0f b7 c3                                        movzx     eax, bx
0022DC5F  4c 89 6d af                                     mov       qword ptr [rbp - 0x51], r13
0022DC63  89 45 af                                        mov       dword ptr [rbp - 0x51], eax
0022DC66  8b c3                                           mov       eax, ebx
0022DC68  c1 e8 10                                        shr       eax, 0x10
0022DC6B  24 01                                           and       al, 1
0022DC6D  c1 eb 1f                                        shr       ebx, 0x1f
0022DC70  88 45 b3                                        mov       byte ptr [rbp - 0x4d], al
0022DC73  48 8d 45 af                                     lea       rax, [rbp - 0x51]
0022DC77  48 89 45 ef                                     mov       qword ptr [rbp - 0x11], rax
0022DC7B  48 8b 05 66 7e d9 00                            mov       rax, qword ptr [rip + 0xd97e66]
0022DC82  88 5d b4                                        mov       byte ptr [rbp - 0x4c], bl
0022DC85  48 c7 45 f7 08 00 00 00                         mov       qword ptr [rbp - 9], 8
0022DC8D  8b 98 a8 11 00 00                               mov       ebx, dword ptr [rax + 0x11a8]
0022DC93  0f bc cb                                        bsf       ecx, ebx
0022DC96  74 84                                           je        0x22dc1c ; loc_22DC1C [RVA 0x22DC1C]
0022DC98  0f 1f 84 00 00 00 00 00                         nop       dword ptr [rax + rax]
0022DCA0  48 8b 15 41 7e d9 00                            mov       rdx, qword ptr [rip + 0xd97e41]
0022DCA7  8d 43 ff                                        lea       eax, [rbx - 1]
0022DCAA  23 d8                                           and       ebx, eax
0022DCAC  44 8b c1                                        mov       r8d, ecx
0022DCAF  8b c1                                           mov       eax, ecx
0022DCB1  48 c1 e0 05                                     shl       rax, 5
0022DCB5  48 05 cc 11 00 00                               add       rax, 0x11cc
0022DCBB  48 03 c2                                        add       rax, rdx
0022DCBE  74 47                                           je        0x22dd07 ; loc_22DD07 [RVA 0x22DD07]
0022DCC0  8b 40 14                                        mov       eax, dword ptr [rax + 0x14]
0022DCC3  a8 10                                           test      al, 0x10
0022DCC5  74 40                                           je        0x22dd07 ; loc_22DD07 [RVA 0x22DD07]
0022DCC7  0f b6 84 4a 99 11 00 00                         movzx     eax, byte ptr [rdx + rcx*2 + 0x1199]
0022DCCF  41 b9 02 00 00 00                               mov       r9d, 2
0022DCD5  48 c7 45 e7 08 00 00 00                         mov       qword ptr [rbp - 0x19], 8
0022DCDD  c7 44 24 28 02 0e 50 00                         mov       dword ptr [rsp + 0x28], 0x500e02
0022DCE5  66 c7 44 24 20 72 0f                            mov       word ptr [rsp + 0x20], 0xf72
0022DCEC  48 8d 4c c5 b7                                  lea       rcx, [rbp + rax*8 - 0x49]
0022DCF1  48 89 4d df                                     mov       qword ptr [rbp - 0x21], rcx
0022DCF5  48 8d 4d df                                     lea       rcx, [rbp - 0x21]
0022DCF9  46 0f b6 84 42 98 11 00 00                      movzx     r8d, byte ptr [rdx + r8*2 + 0x1198]
0022DD02  e8 29 44 04 00                                  call      0x272130 ; EtwpLogKernelEvent [RVA 0x272130]
0022DD07  0f bc cb                                        bsf       ecx, ebx
0022DD0A  75 94                                           jne       0x22dca0 ; loc_22DCA0 [RVA 0x22DCA0]
0022DD0C  e9 0b ff ff ff                                  jmp       0x22dc1c ; loc_22DC1C [RVA 0x22DC1C]
0022DD11  4c 8b 05 68 0f da 00                            mov       r8, qword ptr [rip + 0xda0f68]
0022DD18  40 b6 01                                        mov       sil, 1
0022DD1B  41 8b fd                                        mov       edi, r13d
0022DD1E  4d 85 c0                                        test      r8, r8
0022DD21  74 7a                                           je        0x22dd9d ; loc_22DD9D [RVA 0x22DD9D]
0022DD23  41 8b 90 a8 11 00 00                            mov       edx, dword ptr [r8 + 0x11a8]
0022DD2A  0f bc ca                                        bsf       ecx, edx
0022DD2D  74 36                                           je        0x22dd65 ; loc_22DD65 [RVA 0x22DD65]
0022DD2F  90                                              nop
0022DD30  8d 42 ff                                        lea       eax, [rdx - 1]
0022DD33  23 d0                                           and       edx, eax
0022DD35  48 8b c1                                        mov       rax, rcx
0022DD38  48 c1 e0 05                                     shl       rax, 5
0022DD3C  48 05 cc 11 00 00                               add       rax, 0x11cc
0022DD42  49 03 c0                                        add       rax, r8
0022DD45  74 19                                           je        0x22dd60 ; loc_22DD60 [RVA 0x22DD60]
0022DD47  8b 40 14                                        mov       eax, dword ptr [rax + 0x14]
0022DD4A  a8 10                                           test      al, 0x10
0022DD4C  74 12                                           je        0x22dd60 ; loc_22DD60 [RVA 0x22DD60]
0022DD4E  41 0f b6 8c 48 99 11 00 00                      movzx     ecx, byte ptr [r8 + rcx*2 + 0x1199]
0022DD57  b8 01 00 00 00                                  mov       eax, 1
0022DD5C  d3 e0                                           shl       eax, cl
0022DD5E  0b f8                                           or        edi, eax
0022DD60  0f bc ca                                        bsf       ecx, edx
0022DD63  75 cb                                           jne       0x22dd30 ; loc_22DD30 [RVA 0x22DD30]
0022DD65  40 f6 c7 02                                     test      dil, 2
0022DD69  75 37                                           jne       0x22dda2 ; loc_22DDA2 [RVA 0x22DDA2]
0022DD6B  40 f6 c7 04                                     test      dil, 4
0022DD6F  75 3e                                           jne       0x22ddaf ; loc_22DDAF [RVA 0x22DDAF]
0022DD71  4c 89 6d c7                                     mov       qword ptr [rbp - 0x39], r13
0022DD75  40 f6 c7 08                                     test      dil, 8
0022DD79  74 1c                                           je        0x22dd97 ; loc_22DD97 [RVA 0x22DD97]
0022DD7B  0f 31                                           rdtsc
0022DD7D  48 c1 e2 20                                     shl       rdx, 0x20
0022DD81  48 0b c2                                        or        rax, rdx
0022DD84  48 89 45 cf                                     mov       qword ptr [rbp - 0x31], rax
0022DD88  40 f6 c7 10                                     test      dil, 0x10
0022DD8C  75 2c                                           jne       0x22ddba ; loc_22DDBA [RVA 0x22DDBA]
0022DD8E  4c 89 6d d7                                     mov       qword ptr [rbp - 0x29], r13
0022DD92  e9 ab fd ff ff                                  jmp       0x22db42 ; loc_22DB42 [RVA 0x22DB42]
0022DD97  4c 89 6d cf                                     mov       qword ptr [rbp - 0x31], r13
0022DD9B  eb eb                                           jmp       0x22dd88 ; loc_22DD88 [RVA 0x22DD88]
0022DD9D  bf 1e 00 00 00                                  mov       edi, 0x1e
0022DDA2  33 c9                                           xor       ecx, ecx
0022DDA4  e8 47 20 09 00                                  call      0x2bfdf0 ; KeQueryPerformanceCounter [RVA 0x2BFDF0]
0022DDA9  48 89 45 bf                                     mov       qword ptr [rbp - 0x41], rax
0022DDAD  eb bc                                           jmp       0x22dd6b ; loc_22DD6B [RVA 0x22DD6B]
0022DDAF  e8 2c 26 09 00                                  call      0x2c03e0 ; RtlGetSystemTimePrecise [RVA 0x2C03E0]
0022DDB4  48 89 45 c7                                     mov       qword ptr [rbp - 0x39], rax
0022DDB8  eb bb                                           jmp       0x22dd75 ; loc_22DD75 [RVA 0x22DD75]
0022DDBA  e8 b5 88 0f 00                                  call      0x326674 ; EtwpGetHostPerfCounter [RVA 0x326674]
0022DDBF  48 89 45 d7                                     mov       qword ptr [rbp - 0x29], rax
0022DDC3  e9 7a fd ff ff                                  jmp       0x22db42 ; loc_22DB42 [RVA 0x22DB42]
0022DDC8  cc                                              int3
0022DDC9  cc                                              int3
0022DDCA  cc                                              int3
0022DDCB  cc                                              int3
