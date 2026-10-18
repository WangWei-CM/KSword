; 专用检查点只有一个 INT3；VEH 必须核验该精确地址，不能认领其它断点。
PUBLIC KswordCooperativeCheckpointTrap
PUBLIC KswordCooperativeCheckpointAddress
.code
KswordCooperativeCheckpointTrap PROC
KswordCooperativeCheckpointInstruction LABEL BYTE
    int 3
    ret
KswordCooperativeCheckpointTrap ENDP
; 返回实际指令地址，不能把增量链接器生成的 ILT 地址当成 INT3。
KswordCooperativeCheckpointAddress PROC
    lea rax, KswordCooperativeCheckpointInstruction
    ret
KswordCooperativeCheckpointAddress ENDP
END
