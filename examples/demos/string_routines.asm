INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
    source  BYTE "AbC!!", 0
    dest    BYTE 16 DUP(0)
    same    BYTE "AbC!!", 0
    other   BYTE "zzz", 0
    trim_me BYTE "trim....", 0
    eq_msg  BYTE "eq", 0
    neq_msg BYTE "neq", 0

.code
main PROC
    MOV     edx, OFFSET source
    CALL    Str_length
    CALL    WriteDec
    CALL    Crlf

    MOV     esi, OFFSET source
    MOV     edi, OFFSET dest
    CALL    Str_copy
    MOV     edx, OFFSET dest
    CALL    WriteString
    CALL    Crlf

    MOV     edx, OFFSET source
    MOV     edi, OFFSET same
    CALL    Str_compare
    JE      Equal
    exit

Equal:
    MOV     edx, OFFSET eq_msg
    CALL    WriteString
    CALL    Crlf

    MOV     edx, OFFSET source
    MOV     edi, OFFSET other
    CALL    Str_compare
    JNE     NotEqual
    exit

NotEqual:
    MOV     edx, OFFSET neq_msg
    CALL    WriteString
    CALL    Crlf

    MOV     edx, OFFSET trim_me
    MOV     al, '.'
    CALL    Str_trim
    MOV     edx, OFFSET trim_me
    CALL    WriteString
    CALL    Crlf

    MOV     edx, OFFSET dest
    CALL    Str_ucase
    MOV     edx, OFFSET dest
    CALL    WriteString
    CALL    Crlf

    MOV     edx, OFFSET dest
    CALL    Str_lcase
    MOV     edx, OFFSET dest
    CALL    WriteString
    CALL    Crlf
    exit
main ENDP
END main
