INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
    prompt BYTE "Number: ", 0

.code
main PROC
    MOV     edx, OFFSET prompt
    CALL    WriteString
    CALL    ReadInt
    ADD     eax, 10
    CALL    WriteInt
    CALL    Crlf
    exit
main ENDP
END main
