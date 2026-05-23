INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
    regs_ok  BYTE "regs-ok", 0
    flags_ok BYTE "flags-ok", 0
    bad      BYTE "bad", 0

.code
main PROC
    MOV     eax, 11
    MOV     ebx, 22
    MOV     ecx, 33
    MOV     edx, 44
    MOV     esi, 55
    MOV     edi, 66

    PUSHAD
    MOV     eax, 1
    MOV     ebx, 2
    MOV     ecx, 3
    MOV     edx, 4
    MOV     esi, 5
    MOV     edi, 6
    POPAD

    CMP     eax, 11
    JNE     Fail
    CMP     ebx, 22
    JNE     Fail
    CMP     ecx, 33
    JNE     Fail
    CMP     edx, 44
    JNE     Fail
    CMP     esi, 55
    JNE     Fail
    CMP     edi, 66
    JNE     Fail

    MOV     edx, OFFSET regs_ok
    CALL    WriteString
    CALL    Crlf

    CMP     eax, eax
    PUSHFD
    CMP     eax, ebx
    POPFD
    JE      FlagsOk
    JMP     Fail

FlagsOk:
    MOV     edx, OFFSET flags_ok
    CALL    WriteString
    CALL    Crlf
    exit

Fail:
    MOV     edx, OFFSET bad
    CALL    WriteString
    CALL    Crlf
    exit
main ENDP
END main
