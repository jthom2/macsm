INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
	EmptyStr BYTE 0
	Str1	 BYTE "Look forward to enjoying summer.", 0
	Str2	 BYTE "Homework 10 Test", 0

	Msg		 BYTE "Number of characters in string: ", 0
	Msg1	 BYTE "No characters in string", 0

	Count	 DWORD 0

.code
main PROC
				; set string to be counted
	MOV		ebx, OFFSET Str2	; base
	MOV		esi, 0				; offset
	MOV		eax, 0				; char counter


CountChars:
	MOV		dl, [ebx+esi]		; load char
	CMP		dl, 0				; char=nullTerm?
	JZ		CheckEmpty

	INC		eax					; counter++
	INC		esi					; offset++
	JMP		CountChars			; loop til char=nullTerm

CheckEmpty:
	CMP		eax, 0				; counter=0?
	JZ		Empty

Display:						; if counter NE 0, display count
	LEA		edx, Msg
	CALL	WriteString			
	CALL	WriteDec
	JMP		Quit				; jump over Empty message	


Empty:							; if counter=0, display No characters message
	LEA		edx, Msg1
	CALL	WriteString


Quit:

exit
main ENDP
END main