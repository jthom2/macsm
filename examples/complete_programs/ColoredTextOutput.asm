INCLUDE Irvine32.inc
includelib Irvine32.lib


.data
    PromptUser BYTE "Please enter a value:", 0
    MyName BYTE "Put your name here", 0
    ErrorMsg BYTE "ERROR: Enter 1 or 0 only.", 0

.code
main PROC
    MOV edx, OFFSET PromptUser
    call WriteString
    call ReadInt                ; Display message, user input = eax

    mov bl, al                  ; bl <-- userInput

    call DisplayName


    exit
main ENDP

DisplayName PROC

    test bl, bl
    jz SetBlue                  ; iff input=0

    cmp bl, 1                   ; iff input=1, fall through
    jnz Error                   ; input not 1 or 0, error

SetRed:
    mov eax, red + (white * 16)
    jmp Display                 ; jump over SetBlue to avoid overwrite

SetBlue:
    mov eax, blue + (white * 16)

Display:
    mov edx, OFFSET MyName
    call SetTextColor
    call WriteString
    mov eax, white + (black * 16)
    call SetTextColor
    ret

Error:
    mov edx, OFFSET ErrorMsg
    mov eax, black + (white * 16)
    call SetTextColor
    call WriteString
    mov eax, white + (black * 16)
    call SetTextColor
    ret
    

DisplayName ENDP

END main