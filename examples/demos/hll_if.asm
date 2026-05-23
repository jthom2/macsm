INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
    negMsg   BYTE "negative", 0
    twoMsg   BYTE "two", 0
    otherMsg BYTE "other", 0

.code
main PROC
    MOV     eax, 2
    CMP     eax, 0
    JGE     NotNeg
    MOV     edx, OFFSET negMsg
    JMP     Done

NotNeg:
    CMP     eax, 2
    JNE     Other
    MOV     edx, OFFSET twoMsg
    JMP     Done

Other:
    MOV     edx, OFFSET otherMsg

Done:
    CALL    WriteString
    CALL    Crlf
    exit
main ENDP
END main
