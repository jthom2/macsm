INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.code
PrintLocalArray PROC
    LOCAL buf[8]:BYTE
    LEA     esi, buf
    MOV     BYTE PTR [esi],   'l'
    MOV     BYTE PTR [esi+1], 'o'
    MOV     BYTE PTR [esi+2], 'c'
    MOV     BYTE PTR [esi+3], 'a'
    MOV     BYTE PTR [esi+4], 'l'
    MOV     BYTE PTR [esi+5], 0
    MOV     buf[6], 'x'
    LEA     edx, buf
    CALL    WriteString
    CALL    Crlf
    RET
PrintLocalArray ENDP

main PROC
    CALL    PrintLocalArray
    exit
main ENDP
END main
