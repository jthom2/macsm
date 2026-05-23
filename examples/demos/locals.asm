INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.code
SumTo PROC
    LOCAL count:DWORD, total:DWORD
    MOV     count, ecx
    MOV     total, 0

L1:
    CMP     count, 0
    JE      Done
    MOV     eax, total
    ADD     eax, count
    MOV     total, eax
    DEC     count
    JMP     L1

Done:
    MOV     eax, total
    RET
SumTo ENDP

main PROC
    MOV     ecx, 5
    CALL    SumTo
    CALL    WriteDec
    CALL    Crlf
    exit
main ENDP
END main
