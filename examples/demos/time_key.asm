INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
    prompt   BYTE "key=", 0
    ms_label BYTE "ms=", 0

.code
main PROC
    CALL    GetMseconds
    MOV     edx, OFFSET ms_label
    CALL    WriteString
    CALL    WriteDec
    CALL    Crlf

    MOV     eax, 0
    CALL    Delay

    MOV     edx, OFFSET prompt
    CALL    WriteString
    CALL    ReadKey
    CALL    WriteChar
    CALL    Crlf
    exit
main ENDP
END main
