asm_from_c:
	add.q r1, r1, #1
	rts

asm_calls_c:
	jsr c_from_assembly
	rts
