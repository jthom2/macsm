INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.code
Sum PROC
    PUSH    ebp
    MOV     ebp, esp
    MOV     eax, [ebp + 8]      ; a (first param)
    ADD     eax, [ebp + 12]     ; b (second param)
    POP     ebp
    RET     8                   ; clean 2 params (8 bytes)
Sum ENDP

main PROC
    PUSH    8                   ; b
    PUSH    7                   ; a
    CALL    Sum
    CALL    WriteDec
    CALL    Crlf
    exit
main ENDP
END main
