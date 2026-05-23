INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data

    Value DWORD 0B56CA2E9h

.code
main PROC
    MOV     edx, 0
    MOV     eax, Value          ; eax=B56CA2E9h


    SHR     eax, 20             ; eax=00000B56h --> al=56h
    MOV     dh, al              ; dh=56h
    


    MOV     eax, Value          ; al = 0 E9h
    AND     al, 09h             ; al = 09h

    MOV     dl, al              ; dl=09h

    
    MOV     eax, Value      
    SHR     eax, 4              ; al=2Eh
    AND     al, 20h             ; al=20h
    OR      dl, al              ; OR 09h, 20h = 29h = dl

                                ; so dh=56h, dl=29h, dx=5629h

    call dumpregs


       
    exit
main ENDP
END main