INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.code
main PROC
    MOV     eax, 0
    MOV     al, 0FEh
    CBW
    CWDE
    CALL    WriteInt
    CALL    Crlf

    MOV     eax, 0
    MOV     ax, 0FF80h
    CWDE
    CALL    WriteInt
    CALL    Crlf

    MOV     edx, 12340000h
    MOV     eax, 0
    MOV     ax, 0FFFEh
    CWD
    MOV     eax, edx
    CALL    WriteHex
    CALL    Crlf

    exit
main ENDP
END main
