002C5A30  488bc4                   mov      rax, rsp
002C5A33  44894820                 mov      dword ptr [rax + 0x20], r9d
002C5A37  4c894018                 mov      qword ptr [rax + 0x18], r8
002C5A3B  48894808                 mov      qword ptr [rax + 8], rcx
002C5A3F  53                       push     rbx
002C5A40  55                       push     rbp
002C5A41  56                       push     rsi
002C5A42  57                       push     rdi
002C5A43  4154                     push     r12
002C5A45  4155                     push     r13
002C5A47  4156                     push     r14
002C5A49  4157                     push     r15
002C5A4B  4881ec88000000           sub      rsp, 0x88
002C5A52  4533ed                   xor      r13d, r13d
002C5A55  418be9                   mov      ebp, r9d
002C5A58  4c896888                 mov      qword ptr [rax - 0x78], r13
002C5A5C  498bf8                   mov      rdi, r8
002C5A5F  488bf2                   mov      rsi, rdx
002C5A62  488bd9                   mov      rbx, rcx
002C5A65  458d7d01                 lea      r15d, [r13 + 1]
002C5A69  4183f918                 cmp      r9d, 0x18
002C5A6D  740a                     je       0x2c5a79
002C5A6F  40f6c510                 test     bpl, 0x10
002C5A73  0f8554040000             jne      0x2c5ecd
002C5A79  44896c2440               mov      dword ptr [rsp + 0x40], r13d
002C5A7E  4c8be6                   mov      r12, rsi
002C5A81  48b80000000080f6ffff     movabs   rax, 0xfffff68000000000
002C5A8B  49c1e419                 shl      r12, 0x19
002C5A8F  488bc8                   mov      rcx, rax
002C5A92  48c1e119                 shl      rcx, 0x19
002C5A96  4c2be1                   sub      r12, rcx
002C5A99  49c1fc10                 sar      r12, 0x10
002C5A9D  498bcc                   mov      rcx, r12
002C5AA0  e80b870300               call     0x2fe1b0 ; sub_2FE1B0
002C5AA5  85c0                     test     eax, eax
002C5AA7  0f857b020000             jne      0x2c5d28
002C5AAD  448bf5                   mov      r14d, ebp
002C5AB0  81fd00010000             cmp      ebp, 0x100
002C5AB6  0f8409030000             je       0x2c5dc5
002C5ABC  8bc5                     mov      eax, ebp
002C5ABE  48897c2458               mov      qword ptr [rsp + 0x58], rdi
002C5AC3  83e005                   and      eax, 5
002C5AC6  4c896c2468               mov      qword ptr [rsp + 0x68], r13
002C5ACB  4183e6fe                 and      r14d, 0xfffffffe
002C5ACF  4c896c2448               mov      qword ptr [rsp + 0x48], r13
002C5AD4  3c05                     cmp      al, 5
002C5AD6  4c8bc7                   mov      r8, rdi
002C5AD9  488bd6                   mov      rdx, rsi
002C5ADC  488bcb                   mov      rcx, rbx
002C5ADF  440f45f5                 cmovne   r14d, ebp
002C5AE3  458bce                   mov      r9d, r14d
002C5AE6  41d1e9                   shr      r9d, 1
002C5AE9  4183e102                 and      r9d, 2
002C5AED  e87e070000               call     0x2c6270 ; sub_2C6270
002C5AF2  85c0                     test     eax, eax
002C5AF4  0f88c4020000             js       0x2c5dbe
002C5AFA  33ff                     xor      edi, edi
002C5AFC  488d0d3d23b700           lea      rcx, [rip + 0xb7233d]
002C5B03  48897c2438               mov      qword ptr [rsp + 0x38], rdi
002C5B08  e8c3aefcff               call     0x2909d0 ; sub_2909D0
002C5B0D  888424d8000000           mov      byte ptr [rsp + 0xd8], al
002C5B14  e9d6000000               jmp      0x2c5bef
002C5B19  f0480fba6b183f           lock bts qword ptr [rbx + 0x18], 0x3f
002C5B20  0f8282010000             jb       0x2c5ca8
002C5B26  488b4310                 mov      rax, qword ptr [rbx + 0x10]
002C5B2A  8b542430                 mov      edx, dword ptr [rsp + 0x30]
002C5B2E  48c1e205                 shl      rdx, 5
002C5B32  488bca                   mov      rcx, rdx
002C5B35  4833c8                   xor      rcx, rax
002C5B38  4881e11ffcffff           and      rcx, 0xfffffffffffffc1f
002C5B3F  4833ca                   xor      rcx, rdx
002C5B42  837c244000               cmp      dword ptr [rsp + 0x40], 0
002C5B47  48894b10                 mov      qword ptr [rbx + 0x10], rcx
002C5B4B  740d                     je       0x2c5b5a
002C5B4D  ba04000000               mov      edx, 4
002C5B52  488bcb                   mov      rcx, rbx
002C5B55  e87aeeffff               call     0x2c49d4 ; sub_2C49D4
002C5B5A  33c0                     xor      eax, eax
002C5B5C  488bef                   mov      rbp, rdi
002C5B5F  83e542                   and      ebp, 0x42
002C5B62  0f8560010000             jne      0x2c5cc8
002C5B68  48b9ffffffffffffff7f     movabs   rcx, 0x7fffffffffffffff
002C5B72  f048214b18               lock and qword ptr [rbx + 0x18], rcx
002C5B77  4885c0                   test     rax, rax
002C5B7A  0f8525060000             jne      0x2c61a5
002C5B80  448b442430               mov      r8d, dword ptr [rsp + 0x30]
002C5B85  488bce                   mov      rcx, rsi
002C5B88  488b542460               mov      rdx, qword ptr [rsp + 0x60]
002C5B8D  e8be190a00               call     0x367550 ; sub_367550
002C5B92  488bd8                   mov      rbx, rax
002C5B95  8b442430                 mov      eax, dword ptr [rsp + 0x30]
002C5B99  a804                     test     al, 4
002C5B9B  0f852d060000             jne      0x2c61ce
002C5BA1  488bd3                   mov      rdx, rbx
002C5BA4  488bce                   mov      rcx, rsi
002C5BA7  e844fafcff               call     0x2955f0 ; sub_2955F0
002C5BAC  488b0515ffcf00           mov      rax, qword ptr [rip + 0xcfff15]
002C5BB3  480fbae008               bt       rax, 8
002C5BB8  720b                     jb       0x2c5bc5
002C5BBA  480fbae009               bt       rax, 9
002C5BBF  0f837b010000             jae      0x2c5d40
002C5BC5  488bd3                   mov      rdx, rbx
002C5BC8  488bcf                   mov      rcx, rdi
002C5BCB  e870060000               call     0x2c6240 ; sub_2C6240
002C5BD0  85c0                     test     eax, eax
002C5BD2  0f8568010000             jne      0x2c5d40
002C5BD8  8bac24e8000000           mov      ebp, dword ptr [rsp + 0xe8]
002C5BDF  4883c608                 add      rsi, 8
002C5BE3  4981c400100000           add      r12, 0x1000
002C5BEA  488b7c2438               mov      rdi, qword ptr [rsp + 0x38]
002C5BEF  483bb424e0000000         cmp      rsi, qword ptr [rsp + 0xe0]
002C5BF7  0f87f0000000             ja       0x2c5ced
002C5BFD  4885ff                   test     rdi, rdi
002C5C00  0f847b010000             je       0x2c5d81
002C5C06  48f7c6ff0f0000           test     rsi, 0xfff
002C5C0D  0f843f050000             je       0x2c6152
002C5C13  483b742458               cmp      rsi, qword ptr [rsp + 0x58]
002C5C18  0f87c4010000             ja       0x2c5de2
002C5C1E  83bc24f000000000         cmp      dword ptr [rsp + 0xf0], 0
002C5C26  488b3e                   mov      rdi, qword ptr [rsi]
002C5C29  0f8556050000             jne      0x2c6185
002C5C2F  418bc6                   mov      eax, r14d
002C5C32  89442430                 mov      dword ptr [rsp + 0x30], eax
002C5C36  4184ff                   test     r15b, dil
002C5C39  0f84b7020000             je       0x2c5ef6
002C5C3F  488bc7                   mov      rax, rdi
002C5C42  48b9ffffffffff000000     movabs   rcx, 0xffffffffff
002C5C4C  48c1e80c                 shr      rax, 0xc
002C5C50  4823c1                   and      rax, rcx
002C5C53  4889442460               mov      qword ptr [rsp + 0x60], rax
002C5C58  488d1c40                 lea      rbx, [rax + rax*2]
002C5C5C  48b80000000000deffff     movabs   rax, 0xffffde0000000000
002C5C66  48c1e304                 shl      rbx, 4
002C5C6A  488bc0                   mov      rax, rax
002C5C6D  4803d8                   add      rbx, rax
002C5C70  488b4328                 mov      rax, qword ptr [rbx + 0x28]
002C5C74  4885c0                   test     rax, rax
002C5C77  0f8862ffffff             js       0x2c5bdf
002C5C7D  4533c0                   xor      r8d, r8d
002C5C80  488bcb                   mov      rcx, rbx
002C5C83  418d5002                 lea      edx, [r8 + 2]
002C5C87  e87435fdff               call     0x299200 ; sub_299200
002C5C8C  a840                     test     al, 0x40
002C5C8E  0f8501050000             jne      0x2c6195
002C5C94  48f70529fecf0000800000   test     qword ptr [rip + 0xcffe29], 0x8000
002C5C9F  7534                     jne      0x2c5cd5
002C5CA1  33ed                     xor      ebp, ebp
002C5CA3  e971feffff               jmp      0x2c5b19
002C5CA8  4103ef                   add      ebp, r15d
002C5CAB  852dfbe5cf00             test     dword ptr [rip + 0xcfe5fb], ebp
002C5CB1  0f84ef010000             je       0x2c5ea6
002C5CB7  f390                     pause
002C5CB9  488b4318                 mov      rax, qword ptr [rbx + 0x18]
002C5CBD  4885c0                   test     rax, rax
002C5CC0  0f8953feffff             jns      0x2c5b19
002C5CC6  ebe0                     jmp      0x2c5ca8
002C5CC8  488bcb                   mov      rcx, rbx
002C5CCB  e870b7feff               call     0x2b1440 ; sub_2B1440
002C5CD0  e993feffff               jmp      0x2c5b68
002C5CD5  f644243004               test     byte ptr [rsp + 0x30], 4
002C5CDA  74c5                     je       0x2c5ca1
002C5CDC  488b4328                 mov      rax, qword ptr [rbx + 0x28]
002C5CE0  48c1e83c                 shr      rax, 0x3c
002C5CE4  2407                     and      al, 7
002C5CE6  3c03                     cmp      al, 3
002C5CE8  75b7                     jne      0x2c5ca1
002C5CEA  4533ff                   xor      r15d, r15d
002C5CED  4d85ed                   test     r13, r13
002C5CF0  740d                     je       0x2c5cff
002C5CF2  498bcd                   mov      rcx, r13
002C5CF5  e8d66cf6ff               call     0x22c9d0 ; sub_22C9D0
002C5CFA  e88189f6ff               call     0x22e680 ; sub_22E680
002C5CFF  488b542438               mov      rdx, qword ptr [rsp + 0x38]
002C5D04  4885d2                   test     rdx, rdx
002C5D07  740c                     je       0x2c5d15
002C5D09  488d0d3021b700           lea      rcx, [rip + 0xb72130]
002C5D10  e8eb92f5ff               call     0x21f000 ; sub_21F000
002C5D15  8a9424d8000000           mov      dl, byte ptr [rsp + 0xd8]
002C5D1C  488d0d1d21b700           lea      rcx, [rip + 0xb7211d]
002C5D23  e848b7fcff               call     0x291470 ; sub_291470
002C5D28  418bc7                   mov      eax, r15d
002C5D2B  4881c488000000           add      rsp, 0x88
002C5D32  415f                     pop      r15
002C5D34  415e                     pop      r14
002C5D36  415d                     pop      r13
002C5D38  415c                     pop      r12
002C5D3A  5f                       pop      rdi
002C5D3B  5e                       pop      rsi
002C5D3C  5d                       pop      rbp
002C5D3D  5b                       pop      rbx
002C5D3E  c3                       ret
002C5D3F  cc                       int3
002C5D40  4d85ed                   test     r13, r13
002C5D43  7526                     jne      0x2c5d6b
002C5D45  e806f90300               call     0x305650 ; sub_305650
002C5D4A  4533c9                   xor      r9d, r9d
002C5D4D  c744242020000000         mov      dword ptr [rsp + 0x20], 0x20
002C5D55  488d15e420b700           lea      rdx, [rip + 0xb720e4]
002C5D5C  488bc8                   mov      rcx, rax
002C5D5F  4c8be8                   mov      r13, rax
002C5D62  448b400c                 mov      r8d, dword ptr [rax + 0xc]
002C5D66  e8752ffeff               call     0x2a8ce0 ; sub_2A8CE0
002C5D6B  4533c9                   xor      r9d, r9d
002C5D6E  4d8bc7                   mov      r8, r15
002C5D71  498bd4                   mov      rdx, r12
002C5D74  498bcd                   mov      rcx, r13
002C5D77  e8d416feff               call     0x2a7450 ; sub_2A7450
002C5D7C  e957feffff               jmp      0x2c5bd8
002C5D81  48b8f8ffffff7f000000     movabs   rax, 0x7ffffffff8
002C5D8B  488bfe                   mov      rdi, rsi
002C5D8E  48c1ef09                 shr      rdi, 9
002C5D92  4823f8                   and      rdi, rax
002C5D95  48b80000000080f6ffff     movabs   rax, 0xfffff68000000000
002C5D9F  488bc0                   mov      rax, rax
002C5DA2  4803f8                   add      rdi, rax
002C5DA5  488d0d9420b700           lea      rcx, [rip + 0xb72094]
002C5DAC  488bd7                   mov      rdx, rdi
002C5DAF  48897c2438               mov      qword ptr [rsp + 0x38], rdi
002C5DB4  e8a704f4ff               call     0x206260 ; sub_206260
002C5DB9  e955feffff               jmp      0x2c5c13
002C5DBE  33c0                     xor      eax, eax
002C5DC0  e966ffffff               jmp      0x2c5d2b
002C5DC5  488b4b70                 mov      rcx, qword ptr [rbx + 0x70]
002C5DC9  e8c22cfeff               call     0x2a8a90 ; sub_2A8A90
002C5DCE  4889442468               mov      qword ptr [rsp + 0x68], rax
002C5DD3  4c896c2448               mov      qword ptr [rsp + 0x48], r13
002C5DD8  4c896c2458               mov      qword ptr [rsp + 0x58], r13
002C5DDD  e918fdffff               jmp      0x2c5afa
002C5DE2  488b8424d0000000         mov      rax, qword ptr [rsp + 0xd0]
002C5DEA  498bdc                   mov      rbx, r12
002C5DED  4c8b742448               mov      r14, qword ptr [rsp + 0x48]
002C5DF2  482b5830                 sub      rbx, qword ptr [rax + 0x30]
002C5DF6  4d85f6                   test     r14, r14
002C5DF9  0f85e2000000             jne      0x2c5ee1
002C5DFF  4d85ed                   test     r13, r13
002C5E02  0f8568030000             jne      0x2c6170
002C5E08  488bd7                   mov      rdx, rdi
002C5E0B  488d0d2e20b700           lea      rcx, [rip + 0xb7202e]
002C5E12  e8e991f5ff               call     0x21f000 ; sub_21F000
002C5E17  8a9424d8000000           mov      dl, byte ptr [rsp + 0xd8]
002C5E1E  488d0d1b20b700           lea      rcx, [rip + 0xb7201b]
002C5E25  e846b6fcff               call     0x291470 ; sub_291470
002C5E2A  488b4c2468               mov      rcx, qword ptr [rsp + 0x68]
002C5E2F  4c8d442450               lea      r8, [rsp + 0x50]
002C5E34  488bd3                   mov      rdx, rbx
002C5E37  e804a40400               call     0x310240 ; sub_310240
002C5E3C  488d0dfd1fb700           lea      rcx, [rip + 0xb71ffd]
002C5E43  4889442448               mov      qword ptr [rsp + 0x48], rax
002C5E48  4c8bf0                   mov      r14, rax
002C5E4B  e880abfcff               call     0x2909d0 ; sub_2909D0
002C5E50  488bd7                   mov      rdx, rdi
002C5E53  488d0de61fb700           lea      rcx, [rip + 0xb71fe6]
002C5E5A  e80104f4ff               call     0x206260 ; sub_206260
002C5E5F  488b542450               mov      rdx, qword ptr [rsp + 0x50]
002C5E64  418b4634                 mov      eax, dword ptr [r14 + 0x34]
002C5E68  418b4e2c                 mov      ecx, dword ptr [r14 + 0x2c]
002C5E6C  25ffffff3f               and      eax, 0x3fffffff
002C5E71  458b7620                 mov      r14d, dword ptr [r14 + 0x20]
002C5E75  482bc8                   sub      rcx, rax
002C5E78  482bca                   sub      rcx, rdx
002C5E7B  41d1ee                   shr      r14d, 1
002C5E7E  4183e61f                 and      r14d, 0x1f
002C5E82  488d41ff                 lea      rax, [rcx - 1]
002C5E86  488d04c6                 lea      rax, [rsi + rax*8]
002C5E8A  4889442458               mov      qword ptr [rsp + 0x58], rax
002C5E8F  418bc6                   mov      eax, r14d
002C5E92  83e005                   and      eax, 5
002C5E95  3c05                     cmp      al, 5
002C5E97  0f8581fdffff             jne      0x2c5c1e
002C5E9D  4183e6fe                 and      r14d, 0xfffffffe
002C5EA1  e978fdffff               jmp      0x2c5c1e
002C5EA6  8b054cfccf00             mov      eax, dword ptr [rip + 0xcffc4c]
002C5EAC  a840                     test     al, 0x40
002C5EAE  0f8403feffff             je       0x2c5cb7
002C5EB4  e8e73ff6ff               call     0x229ea0 ; sub_229EA0
002C5EB9  84c0                     test     al, al
002C5EBB  0f84f6fdffff             je       0x2c5cb7
002C5EC1  8bcd                     mov      ecx, ebp
002C5EC3  e8a83ff6ff               call     0x229e70 ; sub_229E70
002C5EC8  e9ecfdffff               jmp      0x2c5cb9
002C5ECD  83e5ef                   and      ebp, 0xffffffef
002C5ED0  44897c2440               mov      dword ptr [rsp + 0x40], r15d
002C5ED5  89ac24e8000000           mov      dword ptr [rsp + 0xe8], ebp
002C5EDC  e99dfbffff               jmp      0x2c5a7e
002C5EE1  4d8b7610                 mov      r14, qword ptr [r14 + 0x10]
002C5EE5  33d2                     xor      edx, edx
002C5EE7  4c89742448               mov      qword ptr [rsp + 0x48], r14
002C5EEC  4889542450               mov      qword ptr [rsp + 0x50], rdx
002C5EF1  e96effffff               jmp      0x2c5e64
002C5EF6  480fbae70a               bt       rdi, 0xa
002C5EFB  0f82defcffff             jb       0x2c5bdf
002C5F01  480fbae70b               bt       rdi, 0xb
002C5F06  0f83f2020000             jae      0x2c61fe
002C5F0C  488bce                   mov      rcx, rsi
002C5F0F  e86c07fdff               call     0x296680 ; sub_296680
002C5F14  8bac24e8000000           mov      ebp, dword ptr [rsp + 0xe8]
002C5F1B  488bd8                   mov      rbx, rax
002C5F1E  488b7c2438               mov      rdi, qword ptr [rsp + 0x38]
002C5F23  480fbae00b               bt       rax, 0xb
002C5F28  0f83c1fcffff             jae      0x2c5bef
002C5F2E  488bc8                   mov      rcx, rax
002C5F31  e84ad1feff               call     0x2b3080 ; sub_2B3080
002C5F36  85c0                     test     eax, eax
002C5F38  74d2                     je       0x2c5f0c
002C5F3A  488b053f7db600           mov      rax, qword ptr [rip + 0xb67d3f]
002C5F41  4885c0                   test     rax, rax
002C5F44  7412                     je       0x2c5f58
002C5F46  f6c310                   test     bl, 0x10
002C5F49  0f85f1010000             jne      0x2c6140
002C5F4F  488bc8                   mov      rcx, rax
002C5F52  48f7d1                   not      rcx
002C5F55  4823cb                   and      rcx, rbx
002C5F58  48b8ffffffffff000000     movabs   rax, 0xffffffffff
002C5F62  48c1e90c                 shr      rcx, 0xc
002C5F66  4823c8                   and      rcx, rax
002C5F69  48b80000000000deffff     movabs   rax, 0xffffde0000000000
002C5F73  488d1449                 lea      rdx, [rcx + rcx*2]
002C5F77  4803d2                   add      rdx, rdx
002C5F7A  488bc0                   mov      rax, rax
002C5F7D  483b0d5c7db600           cmp      rcx, qword ptr [rip + 0xb67d5c]
002C5F84  488d3cd0                 lea      rdi, [rax + rdx*8]
002C5F88  7782                     ja       0x2c5f0c
002C5F8A  48b82800000000deffff     movabs   rax, 0xffffde0000000028
002C5F94  488b04d0                 mov      rax, qword ptr [rax + rdx*8]
002C5F98  48c1e836                 shr      rax, 0x36
002C5F9C  4185c7                   test     r15d, eax
002C5F9F  0f8467ffffff             je       0x2c5f0c
002C5FA5  33ed                     xor      ebp, ebp
002C5FA7  f0480fba6f183f           lock bts qword ptr [rdi + 0x18], 0x3f
002C5FAE  0f825b010000             jb       0x2c610f
002C5FB4  488bce                   mov      rcx, rsi
002C5FB7  e8c406fdff               call     0x296680 ; sub_296680
002C5FBC  483bc3                   cmp      rax, rbx
002C5FBF  0f8508010000             jne      0x2c60cd
002C5FC5  488b4728                 mov      rax, qword ptr [rdi + 0x28]
002C5FC9  488b15b8a0b600           mov      rdx, qword ptr [rip + 0xb6a0b8]
002C5FD0  48c1e82b                 shr      rax, 0x2b
002C5FD4  25ff030000               and      eax, 0x3ff
002C5FD9  488b04c2                 mov      rax, qword ptr [rdx + rax*8]
002C5FDD  8b4004                   mov      eax, dword ptr [rax + 4]
002C5FE0  84c0                     test     al, al
002C5FE2  791d                     jns      0x2c6001
002C5FE4  33d2                     xor      edx, edx
002C5FE6  488bcf                   mov      rcx, rdi
002C5FE9  e87ee20c00               call     0x39426c ; sub_39426C
002C5FEE  8bac24e8000000           mov      ebp, dword ptr [rsp + 0xe8]
002C5FF5  488bf8                   mov      rdi, rax
002C5FF8  4885c0                   test     rax, rax
002C5FFB  0f84e9fbffff             je       0x2c5bea
002C6001  488b4708                 mov      rax, qword ptr [rdi + 8]
002C6005  48b90000000000000080     movabs   rcx, 0x8000000000000000
002C600F  480bc1                   or       rax, rcx
002C6012  483bc6                   cmp      rax, rsi
002C6015  0f85c6000000             jne      0x2c60e1
002C601B  8bac24e8000000           mov      ebp, dword ptr [rsp + 0xe8]
002C6022  4885ff                   test     rdi, rdi
002C6025  488b7c2438               mov      rdi, qword ptr [rsp + 0x38]
002C602A  0f84bffbffff             je       0x2c5bef
002C6030  488b0e                   mov      rcx, qword ptr [rsi]
002C6033  488b15467cb600           mov      rdx, qword ptr [rip + 0xb67c46]
002C603A  488bc1                   mov      rax, rcx
002C603D  4885d2                   test     rdx, rdx
002C6040  7412                     je       0x2c6054
002C6042  f6c110                   test     cl, 0x10
002C6045  0f85fe000000             jne      0x2c6149
002C604B  488bc2                   mov      rax, rdx
002C604E  48f7d0                   not      rax
002C6051  4823c1                   and      rax, rcx
002C6054  48c1e80c                 shr      rax, 0xc
002C6058  48baffffffffff000000     movabs   rdx, 0xffffffffff
002C6062  4823c2                   and      rax, rdx
002C6065  49bb1000000000deffff     movabs   r11, 0xffffde0000000010
002C606F  4c8d0c40                 lea      r9, [rax + rax*2]
002C6073  4d03c9                   add      r9, r9
002C6076  498bc3                   mov      rax, r11
002C6079  448b442430               mov      r8d, dword ptr [rsp + 0x30]
002C607E  49c7c21ffcffff           mov      r10, 0xfffffffffffffc1f
002C6085  4a8b04c8                 mov      rax, qword ptr [rax + r9*8]
002C6089  49c1e005                 shl      r8, 5
002C608D  498bd0                   mov      rdx, r8
002C6090  4833d0                   xor      rdx, rax
002C6093  4923d2                   and      rdx, r10
002C6096  4933d0                   xor      rdx, r8
002C6099  498bc3                   mov      rax, r11
002C609C  4a8914c8                 mov      qword ptr [rax + r9*8], rdx
002C60A0  498bc0                   mov      rax, r8
002C60A3  4833c1                   xor      rax, rcx
002C60A6  4923c2                   and      rax, r10
002C60A9  4933c0                   xor      rax, r8
002C60AC  488906                   mov      qword ptr [rsi], rax
002C60AF  48b81800000000deffff     movabs   rax, 0xffffde0000000018
002C60B9  48b9ffffffffffffff7f     movabs   rcx, 0x7fffffffffffffff
002C60C3  f04a210cc8               lock and qword ptr [rax + r9*8], rcx
002C60C8  e90bfbffff               jmp      0x2c5bd8
002C60CD  48b8ffffffffffffff7f     movabs   rax, 0x7fffffffffffffff
002C60D7  f048214718               lock and qword ptr [rdi + 0x18], rax
002C60DC  e92bfeffff               jmp      0x2c5f0c
002C60E1  488b4728                 mov      rax, qword ptr [rdi + 0x28]
002C60E5  480fbae028               bt       rax, 0x28
002C60EA  0f822bffffff             jb       0x2c601b
002C60F0  488b4708                 mov      rax, qword ptr [rdi + 8]
002C60F4  4c8bcb                   mov      r9, rbx
002C60F7  4c8bc6                   mov      r8, rsi
002C60FA  4889442420               mov      qword ptr [rsp + 0x20], rax
002C60FF  ba11040000               mov      edx, 0x411
002C6104  b91a000000               mov      ecx, 0x1a
002C6109  e8f2742300               call     0x4fd600 ; sub_4FD600
002C610E  cc                       int3
002C610F  4103ef                   add      ebp, r15d
002C6112  852d94e1cf00             test     dword ptr [rip + 0xcfe194], ebp
002C6118  0f84cd000000             je       0x2c61eb
002C611E  f390                     pause
002C6120  eb10                     jmp      0x2c6132
002C6122  e8793df6ff               call     0x229ea0 ; sub_229EA0
002C6127  84c0                     test     al, al
002C6129  74f3                     je       0x2c611e
002C612B  8bcd                     mov      ecx, ebp
002C612D  e83e3df6ff               call     0x229e70 ; sub_229E70
002C6132  488b4718                 mov      rax, qword ptr [rdi + 0x18]
002C6136  4885c0                   test     rax, rax
002C6139  78d4                     js       0x2c610f
002C613B  e967feffff               jmp      0x2c5fa7
002C6140  4883e1ef                 and      rcx, 0xffffffffffffffef
002C6144  e90ffeffff               jmp      0x2c5f58
002C6149  4883e0ef                 and      rax, 0xffffffffffffffef
002C614D  e902ffffff               jmp      0x2c6054
002C6152  4d85ed                   test     r13, r13
002C6155  0f8443e74000             je       0x6d489e
002C615B  498bcd                   mov      rcx, r13
002C615E  e86d68f6ff               call     0x22c9d0 ; sub_22C9D0
002C6163  e81885f6ff               call     0x22e680 ; sub_22E680
002C6168  4533ed                   xor      r13d, r13d
002C616B  e92ee74000               jmp      0x6d489e
002C6170  498bcd                   mov      rcx, r13
002C6173  e85868f6ff               call     0x22c9d0 ; sub_22C9D0
002C6178  e80385f6ff               call     0x22e680 ; sub_22E680
002C617D  4533ed                   xor      r13d, r13d
002C6180  e983fcffff               jmp      0x2c5e08
002C6185  418bd6                   mov      edx, r14d
002C6188  488bcf                   mov      rcx, rdi
002C618B  e858702300               call     0x4fd1e8 ; sub_4FD1E8
002C6190  e99dfaffff               jmp      0x2c5c32
002C6195  f644243006               test     byte ptr [rsp + 0x30], 6
002C619A  0f854afbffff             jne      0x2c5cea
002C61A0  e9fcfaffff               jmp      0x2c5ca1
002C61A5  488b4b28                 mov      rcx, qword ptr [rbx + 0x28]
002C61A9  488bd0                   mov      rdx, rax
002C61AC  488b05d59eb600           mov      rax, qword ptr [rip + 0xb69ed5]
002C61B3  458bc7                   mov      r8d, r15d
002C61B6  48c1e92b                 shr      rcx, 0x2b
002C61BA  81e1ff030000             and      ecx, 0x3ff
002C61C0  488b0cc8                 mov      rcx, qword ptr [rax + rcx*8]
002C61C4  e8e71bfdff               call     0x297db0 ; sub_297DB0
002C61C9  e9b2f9ffff               jmp      0x2c5b80
002C61CE  4885ed                   test     rbp, rbp
002C61D1  0f95c1                   setne    cl
002C61D4  4184c7                   test     r15b, al
002C61D7  0f94c0                   sete     al
002C61DA  84c8                     test     al, cl
002C61DC  0f84bff9ffff             je       0x2c5ba1
002C61E2  4883cb42                 or       rbx, 0x42
002C61E6  e9b6f9ffff               jmp      0x2c5ba1
002C61EB  8b0507f9cf00             mov      eax, dword ptr [rip + 0xcff907]
002C61F1  a840                     test     al, 0x40
002C61F3  0f8425ffffff             je       0x2c611e
002C61F9  e924ffffff               jmp      0x2c6122
002C61FE  4885ff                   test     rdi, rdi
002C6201  0f84d8f9ffff             je       0x2c5bdf
002C6207  81fd00010000             cmp      ebp, 0x100
002C620D  0f84ccf9ffff             je       0x2c5bdf
002C6213  8bc8                     mov      ecx, eax
002C6215  48c1e105                 shl      rcx, 5
002C6219  488bc1                   mov      rax, rcx
002C621C  4833c7                   xor      rax, rdi
002C621F  48251ffcffff             and      rax, 0xfffffffffffffc1f
002C6225  4833c1                   xor      rax, rcx
002C6228  488906                   mov      qword ptr [rsi], rax
002C622B  e9aff9ffff               jmp      0x2c5bdf
