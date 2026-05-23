INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
    filename BYTE "masmrun_file_io_test.tmp", 0
    payload  BYTE "hello-file", 0
    buffer   BYTE 64 DUP(0)

.code
main PROC
    MOV     edx, OFFSET filename
    CALL    CreateOutputFile
    MOV     ebx, eax            ; save handle
    MOV     eax, ebx
    MOV     edx, OFFSET payload
    MOV     ecx, 11
    CALL    WriteToFile
    MOV     eax, ebx
    CALL    CloseFile

    MOV     edx, OFFSET filename
    CALL    OpenInputFile
    MOV     ebx, eax
    MOV     eax, ebx
    MOV     edx, OFFSET buffer
    MOV     ecx, 32
    CALL    ReadFromFile
    MOV     eax, ebx
    CALL    CloseFile

    MOV     edx, OFFSET buffer
    CALL    WriteString
    CALL    Crlf
    exit
main ENDP
END main
