INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

Item STRUCT
    id    DWORD ?
    score DWORD ?
Item ENDS

.data
    items Item <1, 100>, <2, 200>, <3, 300>
    total DWORD ?

.code
main PROC
    MOV     ecx, LENGTHOF items
    MOV     esi, OFFSET items
    MOV     eax, 0

L1:
    ADD     eax, [esi].score
    ADD     esi, TYPE Item
    LOOP    L1

    MOV     total, eax
    CALL    WriteDec
    CALL    Crlf
    exit
main ENDP
END main
