INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
	bcd1	DWORD 90000000h
	bcd2	DWORD 00119999h
	sum		BYTE  5 DUP(0)

	msg1	BYTE  " First BCD:  ",  0
	msg2	BYTE  "Second BCD:  ", 0
	msg3	BYTE  "    Result:  ",	  0

	msg4	BYTE  " - Overflow! Result is 9 digits.", 0

.code
main PROC

	LEA		edx, msg1
	CALL	WriteString
	MOV		eax, bcd1
	CALL	WriteHex
	CALL	Crlf
	LEA		edx, msg2
	CALL	WriteString
	MOV		eax, bcd2
	CALL	WriteHex
	CALL	Crlf
	LEA		edx, msg3
	CALL	WriteString
	MOV		edx, 0
	; // Displays BCDs & "Result: "


	CLC								; // CF=0
	MOV		ecx, 4					; // Loop counter (4 bytes => 4 additions)
	MOV		esi, 0					; // index counter

add_loop:
	MOVZX	eax, BYTE PTR bcd1[esi] ; // al = 78
	MOVZX	ebx, BYTE PTR bcd2[esi] ; // bl = 11
	
	ADC		al, bl					; // adds current bytes together w/ CF
	DAA								; // & adjusts result to decimal

	MOV     BYTE PTR sum[esi], al	; // Moves result of current byte to corresponding sum location

	INC		esi						; // move to next byte
LOOP add_loop						; // if ecx>0 -> go again
	
	MOV		al, 0
	ADC		al, 0
	MOV		BYTE PTR sum[esi], al	; // move CF into digit 9

	CMP		sum[esi], 0
	JZ		NoCarry					; // if CF=0, don't print upper half of QWORD
	MOV		edx, 0
Carry:
	MOVZX	eax, BYTE PTR sum[4]	; // move CF into al
	Call	WriteDec				; // Display CF only (instead of 00000001)
	LEA		edx, msg4

NoCarry:
	MOV		eax, DWORD PTR sum		; // move first 8 digits into eax
	Call	WriteHex				; // display first 8 digits
	CMP		edx, 0
	JNZ		DisplayCarryMsg
	JMP		Done					; // Skip overflow message if edx is none

DisplayCarryMsg:
	Call WriteString
	Call Crlf						; // notify user over carryover into 9th digit

Done:
	

	
	exit
main ENDP
END main