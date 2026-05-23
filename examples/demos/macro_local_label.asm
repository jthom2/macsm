INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
    count DWORD 0

.code
CountTwice MACRO
    LOCAL Again, Done
    MOV     ecx, 2
Again:
    INC     count
    LOOP    Again
Done:
ENDM

main PROC
    CountTwice
    CountTwice
    MOV     eax, count
    CALL    WriteDec
    CALL    Crlf
    exit
main ENDP
END main
