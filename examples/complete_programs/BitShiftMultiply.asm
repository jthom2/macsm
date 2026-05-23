INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data

.code
main PROC
    MOV     ebx, 9      ; ebx=(ebx * 1)
    MOV     eax, ebx
    MOV     ecx, ebx

    SHL     eax, 3      ; eax=(ebx * 8)
    SHL     ecx, 1      ; ecx=(ebx * 2)

    ADD     ebx, eax    ; ebx*1 + ebx*8 (= ebx*9)
    ADD     ebx, ecx    ; ebx*9 + ebx*2 (= ebx*11)



    MOV     eax, ebx
    CALL    WriteDec
       
    exit
main ENDP
END main