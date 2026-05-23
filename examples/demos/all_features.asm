INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
    src     BYTE "Hi", 0
    dst     BYTE 3 DUP(0)
    same    BYTE "Hi", 0
    ok      BYTE "all-ok", 0
    bad_msg BYTE "bad", 0

.code
Work PROC
    LOCAL localbuf[4]:BYTE, total:DWORD
    PUSHAD
    PUSHFD
    CLD

    MOV     esi, OFFSET src
    MOV     edi, OFFSET dst
    MOV     ecx, LENGTHOF src
    REP     MOVSB

    MOV     edx, OFFSET dst
    MOV     edi, OFFSET same
    CALL    Str_compare
    JNE     Fail

    MOV     eax, 0
    MOV     al, 0FEh
    CBW
    CWDE
    MOV     total, eax
    MOV     localbuf[0], 'O'
    MOV     localbuf[1], 'K'
    MOV     localbuf[2], 0

    POPFD
    POPAD
    MOV     edx, OFFSET ok
    RET

Fail:
    POPFD
    POPAD
    MOV     edx, OFFSET bad_msg
    RET
Work ENDP

main PROC
    CALL    Work
    CALL    WriteString
    CALL    Crlf
    exit
main ENDP
END main
