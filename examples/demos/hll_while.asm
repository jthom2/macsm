INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.code
main PROC
    MOV     eax, 0
    MOV     ecx, 1

WhileLoop:
    CMP     ecx, 5
    JA      WhileDone
    ADD     eax, ecx
    INC     ecx
    JMP     WhileLoop

WhileDone:
    CALL    WriteDec
    CALL    Crlf
    exit
main ENDP
END main
