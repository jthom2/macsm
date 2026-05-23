INCLUDE Irvine32.inc
includelib Irvine32.lib


.data
    MyString BYTE "Spring is pleasant in Auburn", 0
    FoundChar BYTE "Found character '", 0
    FoundChar1 BYTE "' at index ", 0
    NoFoundChar BYTE "Character not found.", 0

    UserPrompt BYTE "Enter a character to search for: ", 0
    UserChar BYTE ?
    MyCount BYTE 1

.code
main PROC

; --------------------- Display Input Prompt, Gather & Store Input ------------------
    MOV edx, OFFSET UserPrompt
    CALL WriteString
    CALL ReadChar
    CALL WriteChar
    CALL Crlf
    CALL Crlf
    MOV UserChar, al            ; UserChar = inputted character



; ------------------------- Loop Logic & Exe --------------------------------


    MOV esi, OFFSET MyString    ; Search index starts at 1st char of MyString

SearchStr:
    
    MOV al, [esi]
    CMP al, 0
    JE NotFound                 ; if current char is null term, end loop, char not found

    CMP UserChar, al            ; current char = UserChar ?
    JE Found                    ; if =, found
    
    ADD MyCount, 1              ; index counter (starts at 1)
    ADD esi, TYPE MyString      ; fall-through logic, goes to next char
JMP SearchStr



; -------------------- After-Loop Logic ----------------------


; This prints found character '{char}' at index {index}
Found:
    MOV edx, OFFSET FoundChar
    CALL WriteString

    MOV al, UserChar
    CALL WriteChar

    MOV edx, OFFSET FoundChar1
    CALL WriteString

    MOVZX eax, MyCount
    CALL WriteDec

    CALL Crlf
    CALL Crlf                   

    JMP Done               ; avoids printing 'not found' when found




; This prints character not found
NotFound:
    MOV edx, OFFSET NoFoundChar
    CALL WriteString
    CALL Crlf

Done:
    exit
main ENDP
END main