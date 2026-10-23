/*
 * PPTEST.COM: tiny386's information port (perfport.h) read from DOS, for
 * the PC harness.  It looks for the port, then four times, about a second
 * apart: takes a snapshot and says every value on the BIOS's debug port
 * (0402h), a line each, "@pp ROUND INDEX VALUE" in hex.  Each round uses
 * other widths, so that pc.c's every way in is gone through:
 *
 *   round 1  snapshot by out 0F1FAh, ax; each value by in eax, 0F1FCh
 *   round 2  snapshot by out 0F1F8h, eax (index 0, command 1 above it);
 *            each value by two in ax (0F1FCh and 0F1FEh)
 *   round 3  snapshot by out 0F1FAh, al; the index by out 0F1F8h, al;
 *            each value by four in al (0F1FCh to 0F1FFh)
 *   round 4  as round 1, after a moment of writing to the text screen
 *
 * Then "@done" (bios.inc), which stops the harness.  Under Windows 95 it
 * runs in a DOS box, so the same lines say whether a program in a V86
 * virtual machine reaches the port.
 *
 * Build:
 *   as --32 -I DIR_WITH_BIOS_INC -o pptest.o pptest.S
 *   ld -m elf_i386 -Ttext 0x100 --oformat binary -o PPTEST.COM pptest.o
 *
 * Offered under the licence of tiny386 (BSD 3-clause).
 */
	.intel_syntax noprefix
	.code16
	.text
	.globl	_start
_start:
	jmp	main
	.include "bios.inc"

PP_INDEX = 0xf1f8
PP_CMD	= 0xf1fa
PP_DATA	= 0xf1fc

/* eax: the value at index bx, read 32 bits at a time */
get32:
	push	dx
	mov	dx, PP_INDEX
	mov	ax, bx
	out	dx, ax
	mov	dx, PP_DATA
	in	eax, dx
	pop	dx
	ret

/* the same through two 16-bit reads */
get16:
	push	dx
	mov	dx, PP_INDEX
	mov	ax, bx
	out	dx, ax
	mov	dx, PP_DATA + 2
	in	ax, dx
	shl	eax, 16
	mov	dx, PP_DATA
	in	ax, dx
	pop	dx
	ret

/* the same through four 8-bit reads, the index by an 8-bit write (bx below
   256) */
get8:
	push	dx
	push	cx
	mov	dx, PP_INDEX
	mov	al, bl
	out	dx, al
	mov	dx, PP_DATA + 3
	mov	cx, 4
1:	shl	eax, 8
	in	al, dx
	dec	dx
	loop	1b
	pop	cx
	pop	dx
	ret

/* every value of round cl through the routine at si */
dump:
	push	bx
	mov	bx, 2			/* PP_I_COUNT */
	call	get32
	mov	di, ax
	xor	bx, bx
1:	cmp	bx, di
	jae	2f
	call	si
	PUTS	"@pp "
	push	eax
	mov	al, cl
	call	puthex8
	call	putsp
	mov	ax, bx
	call	puthex8
	call	putsp
	pop	eax
	call	puthex32
	call	putnl
	inc	bx
	jmp	1b
2:	pop	bx
	ret

main:
	push	cs
	pop	ds
	/* found? */
	xor	bx, bx
	call	get32
	CK32	magic, eax, 0x36383354
	jnz	notfound
	mov	bx, 1
	call	get32
	INFO32	version, eax
	mov	bx, 2
	call	get32
	INFO32	count, eax
	/* the index reads back */
	mov	dx, PP_INDEX
	mov	ax, 0x1234
	out	dx, ax
	in	ax, dx
	CK16	readback, ax, 0x1234

	/* round 1 */
	mov	dx, PP_CMD
	mov	ax, 1
	out	dx, ax
	mov	cl, 1
	mov	si, offset get32
	call	dump
	mov	cx, 18
	call	ticks

	/* round 2 */
	mov	dx, PP_INDEX
	mov	eax, 0x00010000
	out	dx, eax
	mov	cl, 2
	mov	si, offset get16
	call	dump
	mov	cx, 18
	call	ticks

	/* round 3 */
	mov	dx, PP_CMD
	mov	al, 1
	out	dx, al
	mov	cl, 3
	mov	si, offset get8
	call	dump

	/* a moment of writing to the text screen, then round 4 */
	push	es
	push	0xb800
	pop	es
	xor	di, di
	mov	cx, 2000
	mov	ax, 0x1f2a
	rep	stosw
	pop	es
	mov	cx, 18
	call	ticks
	mov	dx, PP_CMD
	mov	ax, 1
	out	dx, ax
	mov	cl, 4
	mov	si, offset get32
	call	dump
	DONE

notfound:
	PUTS	"@bad perfport not found\n"
	DONE
