; VbsVtl0Stub.asm — #PF 向量的计数桩，仅为 VBS/VTL0 现场研究使用。
; 进入时 CPU 已压入 SS:RSP、RFLAGS、CS:RIP 以及 #PF 的 ErrorCode。
; 本桩不触碰栈、不保存寄存器、不开启中断，仅做两件事：
;   1) 原子增加 g_VbsPfCounter；lock inc 只扰动 RFLAGS 运算位，而 KiPageFault 的
;      trap-frame prologue 使用的是 CPU 压栈的 RFLAGS，所以当前寄存器被扰动安全。
;   2) 通过内存间接跳转把控制交回 g_VbsOriginalPfHandler，保持 CPU 压栈的中断帧原样。
; ENDBR64 位于入口处，启用 CET IBT 的间接跳转栈检查不会误伤本桩的任何复用路径。

EXTERN g_VbsPfCounter:QWORD
EXTERN g_VbsOriginalPfHandler:QWORD

_TEXT SEGMENT

PUBLIC VbsPfCountingStub
VbsPfCountingStub PROC
    db 0F3h, 0Fh, 1Eh, 0FAh                     ; endbr64：CET IBT 启用时的入口标记。
    lock inc QWORD PTR [g_VbsPfCounter]         ; 原子增加计数；RIP 相对寻址，无需外部重定位。
    jmp QWORD PTR [g_VbsOriginalPfHandler]      ; 间接跳回原始 #PF 处理器；KiPageFault 自带 ENDBR64。
VbsPfCountingStub ENDP

_TEXT ENDS
END
