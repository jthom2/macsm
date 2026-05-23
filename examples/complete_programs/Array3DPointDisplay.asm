INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
	points DWORD 10, 20, 30		; x=10, y=20, z=30
		   DWORD 40, 50, 60		; x=40, y=50, z=60
		   DWORD 70, 80, 90		; x=70, y=80, z=90

	NumPoints = ($ - points)/12 ; Total bytes / bytes per point

	msg1   BYTE "Z: ", 0
.code
main PROC

	MOV		ebx, OFFSET points	; base addr of array
	MOV		esi, 8				; offset

	MOV		ecx, NumPoints		; ecx=3
	MOV		edx, OFFSET msg1
	
DisplayZ:
	CALL	WriteString
	MOV		eax, [ebx+esi]		; eax=current z coord
	CALL	WriteDec
	CALL	Crlf
	ADD		esi, 12				; move to next z coord
LOOP DisplayZ
	exit
main ENDP
END main