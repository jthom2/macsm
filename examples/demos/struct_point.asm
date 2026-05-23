INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

Point STRUCT
    x DWORD ?
    y DWORD ?
Point ENDS

.data
    p1 Point <1, 2>
    p2 Point <10, 20>

.code
main PROC
    MOV     eax, p1.x
    ADD     eax, p1.y
    CALL    WriteDec
    CALL    Crlf

    MOV     eax, p2.x
    ADD     eax, p2.y
    CALL    WriteDec
    CALL    Crlf
    exit
main ENDP
END main
