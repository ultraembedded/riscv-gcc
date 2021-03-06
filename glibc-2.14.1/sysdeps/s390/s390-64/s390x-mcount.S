/* 64 bit S/390-specific implemetation of profiling support.
   Copyright (C) 2001 Free Software Foundation, Inc.
   Contributed by Martin Schwidefsky (schwidefsky@de.ibm.com)
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307 USA.  */

#include <sysdep.h>

/* How profiling works on 64 bit S/390:
   On the start of each function _mcount is called with the address of a
   data word in %r1 (initialized to 0, used for counting). The compiler
   with the option -p generates code of the form:
  
           STM    6,15,24(15)
           BRAS   13,.LTN0_0
   .LT0_0: 
   .LC13:  .long  .LP0
           .data
           .align 4
   .LP0:   .long  0
           .text
   # function profiler
           stg    14,4(15)
           lg     1,.LC13-.LT0_0(13)
           brasl  14,_mcount
           lg     14,4(15)
  
   The _mcount implementation now has to call __mcount_internal with the
   address of .LP0 as first parameter and the return address as second
   parameter. &.LP0 was loaded to %r1 and the return address is in %r14.
   _mcount may not modify any register.  */

	ASM_GLOBAL_DIRECTIVE C_SYMBOL_NAME(_mcount)
	ASM_TYPE_DIRECTIVE(C_SYMBOL_NAME(_mcount), @function)
	cfi_startproc
	.align ALIGNARG(4)
C_LABEL(_mcount)
	/* Save the caller-clobbered registers.  */
        aghi  %r15,-224
	cfi_adjust_cfa_offset (224)
        stmg  %r14,%r5,160(%r15)
	cfi_offset (r14, 0)
	cfi_offset (r15, 8)
        lg    %r2,232(%r15)       # callers address  = first parameter
        la    %r2,0(%r2)          # clear bit 0
        la    %r3,0(%r14)         # callees address  = second parameter

#ifdef PIC
	brasl %r14,__mcount_internal@PLT
#else
	brasl %r14,__mcount_internal
#endif

	/* Pop the saved registers.  Please note that `mcount' has no
	   return value.  */
	lmg   %r14,%r5,160(%r15)
        aghi   %r15,224
	cfi_adjust_cfa_offset (-224)
        br    %r14
	cfi_endproc
	ASM_SIZE_DIRECTIVE(C_SYMBOL_NAME(_mcount))

#undef mcount
weak_alias (_mcount, mcount)
