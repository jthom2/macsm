INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
    message BYTE "Hello from MASM", 0

.code
main PROC
    MOV     edx, OFFSET message
    CALL    WriteString
    CALL    Crlf
    exit
main ENDP
END main
