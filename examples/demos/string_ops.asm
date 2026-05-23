INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
    src       BYTE "MASM", 0
    dst       BYTE 5 DUP(0)
    fill      BYTE 4 DUP(0)
    same      BYTE "MASM", 0
    equal_msg BYTE "equal", 0

.code
main PROC
    CLD

    MOV     esi, OFFSET src
    MOV     edi, OFFSET dst
    MOV     ecx, LENGTHOF src
    REP     MOVSB
    MOV     edx, OFFSET dst
    CALL    WriteString
    CALL    Crlf

    MOV     edi, OFFSET dst
    MOV     ecx, LENGTHOF dst
    MOV     al, 0
    REPNE   SCASB
    MOV     eax, LENGTHOF dst
    SUB     eax, ecx
    DEC     eax
    CALL    WriteDec
    CALL    Crlf

    MOV     edi, OFFSET fill
    MOV     ecx, 3
    MOV     al, 'X'
    REP     STOSB
    MOV     edx, OFFSET fill
    CALL    WriteString
    CALL    Crlf

    MOV     esi, OFFSET src
    LODSB
    CALL    WriteChar
    CALL    Crlf

    MOV     esi, OFFSET src
    MOV     edi, OFFSET same
    MOV     ecx, LENGTHOF src
    REPE    CMPSB
    JE      Equal
    exit

Equal:
    MOV     edx, OFFSET equal_msg
    CALL    WriteString
    CALL    Crlf
    exit
main ENDP
END main
