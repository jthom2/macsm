INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.code
main PROC
    MOV     eax, 0
    MOV     ebx, 0

RepeatLoop:
    INC     eax
    CMP     eax, 2
    JE      RepeatContinue      ; skip add, go to UNTIL check
    CMP     eax, 4
    JA      RepeatBreak         ; exit loop entirely
    ADD     ebx, eax

RepeatContinue:
    CMP     eax, 10
    JL      RepeatLoop          ; loop while eax < 10

RepeatBreak:
    MOV     eax, ebx
    CALL    WriteDec
    CALL    Crlf
    exit
main ENDP
END main
