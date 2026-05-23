INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
    
    Msg1      BYTE "Before Encryption: ",0  
    Msg2      BYTE "Encrypted: ",0  
    Msg3      BYTE "Decrypted: ",0  
    
    MyName    BYTE "MyLastName",0
    Key       BYTE 78

.code
main PROC
    
    MOV     edx, OFFSET Msg1
    CALL    WriteString
    MOV     edx, OFFSET MyName
    CALL    WriteString             ;   Display "Before Encryption: {name}"
    CALL    Crlf

    MOV     edx, OFFSET Msg2
    CALL WriteString
; ----------------------------------------------------------------------------------



    MOV     al, Key                 ; load key into register
    MOV     esi, 0                  ; base address
    
Encrypt:
    
    CMP     MyName[esi], 0           ; current char = null term?
    je      Done                     ; terminates loop if hits null term


    XOR     MyName[esi], al          ; encrypting logic
    inc     esi                      ; move to next char


    jmp     Encrypt                  ; unconditional loop
 ; -------------------------------------------------------------------------------  



Done:
    MOV     edx, OFFSET MyName
    CALL    WriteString
    CALL    Crlf


    MOV     edx, OFFSET Msg3
    CALL    WriteString
; ---------------------------------------------------------------------------------


    MOV     esi, 0

Decrypt:
    CMP     MyName[esi], 0
    je      ReallyDone

    XOR     MyName[esi], al
    inc esi

    jmp Decrypt                     ; same basic logic as Encrypt loop
; ----------------------------------------------------------------------------------



ReallyDone:
    MOV     edx, OFFSET MyName
    CALL    WriteString
    CALL    Crlf
; ----------------------------------------------------------------------------------
  
    exit
main ENDP
END main