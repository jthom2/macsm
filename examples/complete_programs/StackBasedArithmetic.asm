INCLUDE Irvine32.inc
INCLUDELIB Irvine32.lib

.data
	valA DWORD 500
	valB DWORD 20
	valC DWORD 521

	X DWORD ?

.code

main PROC
	push    0				; // placeholder on stack for result [ebp + 20]
	push	valA			; // [ebp + 16]
	push	valB			; // [ebp + 12]
	push	valC			; // [ebp + 8]

	CALL	ArithmeticExpression


	POP		eax 			; // Retrieve result from top of stack
	MOV		X, eax			; // Store result in X

	CALL	WriteInt		; // print X (513)
	exit
main	ENDP

ArithmeticExpression PROC
	PUSH	ebp
	MOV		ebp, esp;
	
	MOV		eax, [ebp + 16]	; // eax = A
	ADD		eax, [ebp + 12]	; // eax = A + B
	SUB		eax, [ebp + 8]	; // eax = A + B - C


	MOV		[ebp + 20], eax	; // Store result at top of stack
	MOV		X, eax
	MOV		edi, OFFSET X	; // EDI <-- address of X

	POP		ebp

	ret		12				; // clean 3 params (12 bytes), result stays on top
ArithmeticExpression ENDP

END main