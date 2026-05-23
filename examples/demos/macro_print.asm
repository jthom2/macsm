INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
    message BYTE "macro-ok", 0

.code
mPrint MACRO text:=message
    MOV     edx, OFFSET text
    CALL    WriteString
ENDM

main PROC
    mPrint
    CALL    Crlf
    exit
main ENDP
END main
