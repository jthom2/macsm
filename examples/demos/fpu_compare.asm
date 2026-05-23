INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
    smaller REAL8   5.0
    bigger  REAL8  10.0
    msg_lt  BYTE   "less", 0
    msg_gt  BYTE   "greater", 0

.code
main PROC
    FLD     smaller
    FCOMP   bigger
    FNSTSW  ax
    SAHF
    JB      is_less
    MOV     edx, OFFSET msg_gt
    CALL    WriteString
    JMP     done

is_less:
    MOV     edx, OFFSET msg_lt
    CALL    WriteString

done:
    CALL    Crlf
    exit
main ENDP
END main
