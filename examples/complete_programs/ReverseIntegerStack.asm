INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
	PromptUser BYTE "Please enter a value:",0

.code
main PROC
	MOV edx, OFFSET PromptUser
	
	MOV ecx, 4
GetInts:
	CALL WriteString
	CALL ReadInt
	PUSH eax
	LOOP GetInts

	CALL Crlf
	MOV ecx, 4
Display:
	POP eax
	CALL WriteInt
	CALL Crlf
	LOOP Display

	

	exit
main ENDP
END main