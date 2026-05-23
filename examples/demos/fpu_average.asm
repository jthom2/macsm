INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
    v1    REAL8  10.0
    v2    REAL8  20.0
    v3    REAL8  30.0
    denom REAL8   3.0
    msg   BYTE   "avg=", 0

.code
main PROC
    FLD     v1
    FADD    v2
    FADD    v3
    FDIV    denom
    MOV     edx, OFFSET msg
    CALL    WriteString
    CALL    WriteFloat
    CALL    Crlf
    exit
main ENDP
END main
