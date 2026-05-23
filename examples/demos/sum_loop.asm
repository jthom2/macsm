INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
    numbers DWORD 2, 4, 6, 8
    total   DWORD ?

.code
main PROC
    MOV     ecx, LENGTHOF numbers
    MOV     esi, OFFSET numbers
    MOV     eax, 0

L1:
    ADD     eax, DWORD PTR [esi]
    ADD     esi, TYPE numbers
    LOOP    L1

    MOV     total, eax
    CALL    WriteDec
    CALL    Crlf
    exit
main ENDP
END main
