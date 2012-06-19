/* Subroutines used for MIPS code generation.
   Copyright (C) 1989, 1990, 1991, 1993, 1994, 1995, 1996, 1997, 1998,
   1999, 2000, 2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010,
   2011
   Free Software Foundation, Inc.
   Contributed by A. Lichnewsky, lich@inria.inria.fr.
   Changes by Michael Meissner, meissner@osf.org.
   64-bit r4000 support by Ian Lance Taylor, ian@cygnus.com, and
   Brendan Eich, brendan@microunity.com.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "rtl.h"
#include "regs.h"
#include "hard-reg-set.h"
#include "insn-config.h"
#include "conditions.h"
#include "insn-attr.h"
#include "recog.h"
#include "output.h"
#include "tree.h"
#include "function.h"
#include "expr.h"
#include "optabs.h"
#include "libfuncs.h"
#include "flags.h"
#include "reload.h"
#include "tm_p.h"
#include "ggc.h"
#include "gstab.h"
#include "hashtab.h"
#include "debug.h"
#include "target.h"
#include "target-def.h"
#include "integrate.h"
#include "langhooks.h"
#include "cfglayout.h"
#include "sched-int.h"
#include "gimple.h"
#include "bitmap.h"
#include "diagnostic.h"
#include "target-globals.h"

/* True if X is an UNSPEC wrapper around a SYMBOL_REF or LABEL_REF.  */
#define UNSPEC_ADDRESS_P(X)					\
  (GET_CODE (X) == UNSPEC					\
   && XINT (X, 1) >= UNSPEC_ADDRESS_FIRST			\
   && XINT (X, 1) < UNSPEC_ADDRESS_FIRST + NUM_SYMBOL_TYPES)

/* Extract the symbol or label from UNSPEC wrapper X.  */
#define UNSPEC_ADDRESS(X) \
  XVECEXP (X, 0, 0)

/* Extract the symbol type from UNSPEC wrapper X.  */
#define UNSPEC_ADDRESS_TYPE(X) \
  ((enum mips_symbol_type) (XINT (X, 1) - UNSPEC_ADDRESS_FIRST))

/* The maximum distance between the top of the stack frame and the
   value $sp has when we save and restore registers.

   The value for normal-mode code must be a SMALL_OPERAND and must
   preserve the maximum stack alignment.  We therefore use a value
   of 0x7ff0 in this case.

   MIPS16e SAVE and RESTORE instructions can adjust the stack pointer by
   up to 0x7f8 bytes and can usually save or restore all the registers
   that we need to save or restore.  (Note that we can only use these
   instructions for o32, for which the stack alignment is 8 bytes.)

   We use a maximum gap of 0x100 or 0x400 for MIPS16 code when SAVE and
   RESTORE are not available.  We can then use unextended instructions
   to save and restore registers, and to allocate and deallocate the top
   part of the frame.  */
#define MIPS_MAX_FIRST_STACK_STEP (RISCV_IMM_REACH/2 - 16)

/* True if INSN is a mips.md pattern or asm statement.  */
#define USEFUL_INSN_P(INSN)						\
  (NONDEBUG_INSN_P (INSN)						\
   && GET_CODE (PATTERN (INSN)) != USE					\
   && GET_CODE (PATTERN (INSN)) != CLOBBER				\
   && GET_CODE (PATTERN (INSN)) != ADDR_VEC				\
   && GET_CODE (PATTERN (INSN)) != ADDR_DIFF_VEC)

/* True if bit BIT is set in VALUE.  */
#define BITSET_P(VALUE, BIT) (((VALUE) & (1 << (BIT))) != 0)

/* Classifies an address.

   ADDRESS_REG
       A natural register + offset address.  The register satisfies
       mips_valid_base_register_p and the offset is a const_arith_operand.

   ADDRESS_LO_SUM
       A LO_SUM rtx.  The first operand is a valid base register and
       the second operand is a symbolic address.

   ADDRESS_CONST_INT
       A signed 16-bit constant address.

   ADDRESS_SYMBOLIC:
       A constant symbolic address.  */
enum mips_address_type {
  ADDRESS_REG,
  ADDRESS_LO_SUM,
  ADDRESS_CONST_INT,
  ADDRESS_SYMBOLIC
};

/* Macros to create an enumeration identifier for a function prototype.  */
#define MIPS_FTYPE_NAME1(A, B) MIPS_##A##_FTYPE_##B
#define MIPS_FTYPE_NAME2(A, B, C) MIPS_##A##_FTYPE_##B##_##C
#define MIPS_FTYPE_NAME3(A, B, C, D) MIPS_##A##_FTYPE_##B##_##C##_##D
#define MIPS_FTYPE_NAME4(A, B, C, D, E) MIPS_##A##_FTYPE_##B##_##C##_##D##_##E

/* Classifies the prototype of a built-in function.  */
enum mips_function_type {
#define DEF_MIPS_FTYPE(NARGS, LIST) MIPS_FTYPE_NAME##NARGS LIST,
#include "config/riscv/riscv-ftypes.def"
#undef DEF_MIPS_FTYPE
  MIPS_MAX_FTYPE_MAX
};

/* Specifies how a built-in function should be converted into rtl.  */
enum mips_builtin_type {
  /* The function corresponds directly to an .md pattern.  The return
     value is mapped to operand 0 and the arguments are mapped to
     operands 1 and above.  */
  MIPS_BUILTIN_DIRECT,

  /* The function corresponds directly to an .md pattern.  There is no return
     value and the arguments are mapped to operands 0 and above.  */
  MIPS_BUILTIN_DIRECT_NO_TARGET
};

/* Invoke MACRO (COND) for each C.cond.fmt condition.  */
#define MIPS_FP_CONDITIONS(MACRO) \
  MACRO (f),	\
  MACRO (un),	\
  MACRO (eq),	\
  MACRO (ueq),	\
  MACRO (olt),	\
  MACRO (ult),	\
  MACRO (ole),	\
  MACRO (ule),	\
  MACRO (sf),	\
  MACRO (ngle),	\
  MACRO (seq),	\
  MACRO (ngl),	\
  MACRO (lt),	\
  MACRO (nge),	\
  MACRO (le),	\
  MACRO (ngt)

/* Enumerates the codes above as MIPS_FP_COND_<X>.  */
#define DECLARE_MIPS_COND(X) MIPS_FP_COND_ ## X
enum mips_fp_condition {
  MIPS_FP_CONDITIONS (DECLARE_MIPS_COND)
};

/* Index X provides the string representation of MIPS_FP_COND_<X>.  */
#define STRINGIFY(X) #X
static const char *const mips_fp_conditions[] = {
  MIPS_FP_CONDITIONS (STRINGIFY)
};

/* Information about a function's frame layout.  */
struct GTY(())  mips_frame_info {
  /* The size of the frame in bytes.  */
  HOST_WIDE_INT total_size;

  /* The number of bytes allocated to variables.  */
  HOST_WIDE_INT var_size;

  /* The number of bytes allocated to outgoing function arguments.  */
  HOST_WIDE_INT args_size;

  /* Bit X is set if the function saves or restores GPR X.  */
  unsigned int mask;

  /* Likewise FPR X.  */
  unsigned int fmask;

  /* The number of GPRs, FPRs, doubleword accumulators and COP0
     registers saved.  */
  unsigned int num_gp;
  unsigned int num_fp;

  /* The offset of the topmost GPR, FPR, accumulator and COP0-register
     save slots from the top of the frame, or zero if no such slots are
     needed.  */
  HOST_WIDE_INT gp_save_offset;
  HOST_WIDE_INT fp_save_offset;

  /* Likewise, but giving offsets from the bottom of the frame.  */
  HOST_WIDE_INT gp_sp_offset;
  HOST_WIDE_INT fp_sp_offset;

  /* The offset of arg_pointer_rtx from the bottom of the frame.  */
  HOST_WIDE_INT arg_pointer_offset;
};

struct GTY(())  machine_function {
  /* The number of extra stack bytes taken up by register varargs.
     This area is allocated by the callee at the very top of the frame.  */
  int varargs_size;

  /* The current frame information, calculated by mips_compute_frame_info.  */
  struct mips_frame_info frame;

  /* The register to use as the function's global pointer, or INVALID_REGNUM
     if the function doesn't need one.  */
  unsigned int global_pointer;

  /* True if the function has "inflexible" and "flexible" references
     to the global pointer.  See mips_cfun_has_inflexible_gp_ref_p
     and mips_cfun_has_flexible_gp_ref_p for details.  */
  bool has_flexible_gp_insn_p;

  /* True if the function's prologue must load the global pointer
     value into pic_offset_table_rtx and store the same value in
     the function's cprestore slot (if any).  Even if this value
     is currently false, we may decide to set it to true later;
     see mips_must_initialize_gp_p () for details.  */
  bool must_initialize_gp_p;
};

/* Information about a single argument.  */
struct mips_arg_info {
  /* True if the argument is passed in a floating-point register, or
     would have been if we hadn't run out of registers.  */
  bool fpr_p;

  /* The number of words passed in registers, rounded up.  */
  unsigned int reg_words;

  /* For EABI, the offset of the first register from GP_ARG_FIRST or
     FP_ARG_FIRST.  For other ABIs, the offset of the first register from
     the start of the ABI's argument structure (see the CUMULATIVE_ARGS
     comment for details).

     The value is MAX_ARGS_IN_REGISTERS if the argument is passed entirely
     on the stack.  */
  unsigned int reg_offset;

  /* The number of words that must be passed on the stack, rounded up.  */
  unsigned int stack_words;

  /* The offset from the start of the stack overflow area of the argument's
     first stack word.  Only meaningful when STACK_WORDS is nonzero.  */
  unsigned int stack_offset;
};

/* Information about an address described by mips_address_type.

   ADDRESS_CONST_INT
       No fields are used.

   ADDRESS_REG
       REG is the base register and OFFSET is the constant offset.

   ADDRESS_LO_SUM
       REG and OFFSET are the operands to the LO_SUM and SYMBOL_TYPE
       is the type of symbol it references.

   ADDRESS_SYMBOLIC
       SYMBOL_TYPE is the type of symbol that the address references.  */
struct mips_address_info {
  enum mips_address_type type;
  rtx reg;
  rtx offset;
  enum mips_symbol_type symbol_type;
};

/* One stage in a constant building sequence.  These sequences have
   the form:

	A = VALUE[0]
	A = A CODE[1] VALUE[1]
	A = A CODE[2] VALUE[2]
	...

   where A is an accumulator, each CODE[i] is a binary rtl operation
   and each VALUE[i] is a constant integer.  CODE[0] is undefined.  */
struct mips_integer_op {
  enum rtx_code code;
  unsigned HOST_WIDE_INT value;
};

/* The largest number of operations needed to load an integer constant.
   The worst accepted case for 64-bit constants is LUI,ORI,SLL,ORI,SLL,ORI.
   When the lowest bit is clear, we can try, but reject a sequence with
   an extra SLL at the end.  */
#define MIPS_MAX_INTEGER_OPS 32

/* Costs of various operations on the different architectures.  */

struct mips_rtx_cost_data
{
  unsigned short fp_add;
  unsigned short fp_mult_sf;
  unsigned short fp_mult_df;
  unsigned short fp_div_sf;
  unsigned short fp_div_df;
  unsigned short int_mult_si;
  unsigned short int_mult_di;
  unsigned short int_div_si;
  unsigned short int_div_di;
  unsigned short branch_cost;
  unsigned short memory_latency;
};

/* Global variables for machine-dependent things.  */

/* The number of file directives written by mips_output_filename.  */
int num_source_filenames;

/* The name that appeared in the last .file directive written by
   mips_output_filename, or "" if mips_output_filename hasn't
   written anything yet.  */
const char *current_function_file = "";

/* A label counter used by PUT_SDB_BLOCK_START and PUT_SDB_BLOCK_END.  */
int sdb_label_count;

/* Arrays that map GCC register numbers to debugger register numbers.  */
int mips_dbx_regno[FIRST_PSEUDO_REGISTER];
int mips_dwarf_regno[FIRST_PSEUDO_REGISTER];

/* The current instruction-set architecture.  */
enum processor mips_arch;

/* The processor that we should tune the code for.  */
enum processor mips_tune;

/* The ISA level associated with mips_arch.  */
int mips_isa;

/* Which ABI to use.  */
int mips_abi = MIPS_ABI_DEFAULT;

/* Which cost information to use.  */
static const struct mips_rtx_cost_data *mips_cost;

/* Index [M][R] is true if register R is allowed to hold a value of mode M.  */
bool mips_hard_regno_mode_ok[(int) MAX_MACHINE_MODE][FIRST_PSEUDO_REGISTER];

static GTY (()) int mips_output_filename_first_time = 1;

/* mips_split_p[X] is true if symbols of type X can be split by
   mips_split_symbol.  */
bool mips_split_p[NUM_SYMBOL_TYPES];

/* mips_lo_relocs[X] is the relocation to use when a symbol of type X
   appears in a LO_SUM.  It can be null if such LO_SUMs aren't valid or
   if they are matched by a special .md file pattern.  */
static const char *mips_lo_relocs[NUM_SYMBOL_TYPES];

/* Likewise for HIGHs.  */
static const char *mips_hi_relocs[NUM_SYMBOL_TYPES];

/* Target state for MIPS16.  */
struct target_globals *mips16_globals;

/* Index R is the smallest register class that contains register R.  */
const enum reg_class mips_regno_to_class[FIRST_PSEUDO_REGISTER] = {
/*
yunsup: new register mapping
x0, ra, v0, v1,
a0, a1, a2, a3,
a4, a5, a6, a7,
t0, t1, t2, t3,
t4, t5, t6, t7,
s0, s1, s2, s3,
s4, s5, s6, s7,
s8, s9, sp, tp
*/
  LEA_REGS,	LEA_REGS,	LEA_REGS,	V1_REG,
  LEA_REGS,	LEA_REGS,	LEA_REGS,	LEA_REGS,
  LEA_REGS,	LEA_REGS,	LEA_REGS,	LEA_REGS,
  LEA_REGS,	LEA_REGS,	LEA_REGS,	LEA_REGS,
  LEA_REGS,	LEA_REGS,	LEA_REGS,	PIC_FN_ADDR_REG,
  LEA_REGS,	LEA_REGS,	LEA_REGS,	LEA_REGS,
  LEA_REGS,	LEA_REGS,	LEA_REGS,	LEA_REGS,
  LEA_REGS,	LEA_REGS,	LEA_REGS,	LEA_REGS,
  FP_REGS,	FP_REGS,	FP_REGS,	FP_REGS,
  FP_REGS,	FP_REGS,	FP_REGS,	FP_REGS,
  FP_REGS,	FP_REGS,	FP_REGS,	FP_REGS,
  FP_REGS,	FP_REGS,	FP_REGS,	FP_REGS,
  FP_REGS,	FP_REGS,	FP_REGS,	FP_REGS,
  FP_REGS,	FP_REGS,	FP_REGS,	FP_REGS,
  FP_REGS,	FP_REGS,	FP_REGS,	FP_REGS,
  FP_REGS,	FP_REGS,	FP_REGS,	FP_REGS,
  VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,
  VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,
  VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,
  VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,
  VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,
  VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,
  VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,
  VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,
  VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,
  VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,
  VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,
  VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,
  VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,
  VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,
  VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,
  VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,
  FRAME_REGS,	FRAME_REGS,	NO_REGS,	NO_REGS,
};

/* The value of TARGET_ATTRIBUTE_TABLE.  */
static const struct attribute_spec mips_attribute_table[] = {
  /* { name, min_len, max_len, decl_req, type_req, fn_type_req, handler } */
  { "long_call",   0, 0, false, true,  true,  NULL },
  { "far",     	   0, 0, false, true,  true,  NULL },
  { "near",        0, 0, false, true,  true,  NULL },
  { "utfunc",      0, 0, true,  false, false, NULL },
  { NULL,	   0, 0, false, false, false, NULL }
};

/* A table describing all the processors GCC knows about.  Names are
   matched in the order listed.  The first mention of an ISA level is
   taken as the canonical name for that ISA.

   To ease comparison, please keep this table in the same order
   as GAS's mips_cpu_info_table.  Please also make sure that
   MIPS_ISA_LEVEL_SPEC and MIPS_ARCH_FLOAT_SPEC handle all -march
   options correctly.  */
static const struct mips_cpu_info mips_cpu_info_table[] = {
  /* Entries for generic ISAs.  */
  { "rocket32", PROCESSOR_ROCKET32, ISA_RV32, 0 },
  { "rocket64", PROCESSOR_ROCKET64, ISA_RV64, 0 }
};

/* Default costs.  If these are used for a processor we should look
   up the actual costs.  */
#define DEFAULT_COSTS COSTS_N_INSNS (6),  /* fp_add */       \
                      COSTS_N_INSNS (7),  /* fp_mult_sf */   \
                      COSTS_N_INSNS (8),  /* fp_mult_df */   \
                      COSTS_N_INSNS (23), /* fp_div_sf */    \
                      COSTS_N_INSNS (36), /* fp_div_df */    \
                      COSTS_N_INSNS (10), /* int_mult_si */  \
                      COSTS_N_INSNS (10), /* int_mult_di */  \
                      COSTS_N_INSNS (69), /* int_div_si */   \
                      COSTS_N_INSNS (69), /* int_div_di */   \
                                       2, /* branch_cost */  \
                                       4  /* memory_latency */

/* Floating-point costs for processors without an FPU.  Just assume that
   all floating-point libcalls are very expensive.  */
#define SOFT_FP_COSTS COSTS_N_INSNS (256), /* fp_add */       \
                      COSTS_N_INSNS (256), /* fp_mult_sf */   \
                      COSTS_N_INSNS (256), /* fp_mult_df */   \
                      COSTS_N_INSNS (256), /* fp_div_sf */    \
                      COSTS_N_INSNS (256)  /* fp_div_df */

/* Costs to use when optimizing for size.  */
static const struct mips_rtx_cost_data mips_rtx_cost_optimize_size = {
  COSTS_N_INSNS (1),            /* fp_add */
  COSTS_N_INSNS (1),            /* fp_mult_sf */
  COSTS_N_INSNS (1),            /* fp_mult_df */
  COSTS_N_INSNS (1),            /* fp_div_sf */
  COSTS_N_INSNS (1),            /* fp_div_df */
  COSTS_N_INSNS (1),            /* int_mult_si */
  COSTS_N_INSNS (1),            /* int_mult_di */
  COSTS_N_INSNS (1),            /* int_div_si */
  COSTS_N_INSNS (1),            /* int_div_di */
		   2,           /* branch_cost */
		   4            /* memory_latency */
};

/* Costs to use when optimizing for speed, indexed by processor.  */
static const struct mips_rtx_cost_data
  mips_rtx_cost_data[NUM_PROCESSOR_VALUES] = {
  { /* Rocket */
    COSTS_N_INSNS (8),            /* fp_add */
    COSTS_N_INSNS (8),            /* fp_mult_sf */
    COSTS_N_INSNS (8),            /* fp_mult_df */
    COSTS_N_INSNS (20),           /* fp_div_sf */
    COSTS_N_INSNS (30),           /* fp_div_df */
    COSTS_N_INSNS (8),           /* int_mult_si */
    COSTS_N_INSNS (8),           /* int_mult_di */
    COSTS_N_INSNS (32),           /* int_div_si */
    COSTS_N_INSNS (64),           /* int_div_di */
		     2,           /* branch_cost */
		     7            /* memory_latency */
  }
};

static int mips_register_move_cost (enum machine_mode, reg_class_t,
				    reg_class_t);
static unsigned int mips_function_arg_boundary (enum machine_mode, const_tree);

/* Predicates to test for presence of "near" and "far"/"long_call"
   attributes on the given TYPE.  */

static bool
mips_near_type_p (const_tree type)
{
  return lookup_attribute ("near", TYPE_ATTRIBUTES (type)) != NULL;
}

static bool
mips_far_type_p (const_tree type)
{
  return (lookup_attribute ("long_call", TYPE_ATTRIBUTES (type)) != NULL
	  || lookup_attribute ("far", TYPE_ATTRIBUTES (type)) != NULL);
}

/* Implement TARGET_COMP_TYPE_ATTRIBUTES.  */

static int
mips_comp_type_attributes (const_tree type1, const_tree type2)
{
  /* Disallow mixed near/far attributes.  */
  if (mips_far_type_p (type1) && mips_near_type_p (type2))
    return 0;
  if (mips_near_type_p (type1) && mips_far_type_p (type2))
    return 0;
  return 1;
}

/* Fill CODES with a sequence of rtl operations to load VALUE.
   Return the number of operations needed.  */

static unsigned int
mips_build_integer (struct mips_integer_op *codes,
		    unsigned HOST_WIDE_INT value)
{
  unsigned HOST_WIDE_INT high_part = RISCV_CONST_HIGH_PART (value);
  unsigned HOST_WIDE_INT low_part = RISCV_CONST_LOW_PART (value);
  unsigned cost = UINT_MAX;

  if (SMALL_OPERAND (value) || LUI_OPERAND (value))
    {
      /* The value can be loaded with a single instruction.  */
      codes[0].code = UNKNOWN;
      codes[0].value = value;
      return 1;
    }

  if (LUI_OPERAND (high_part))
    {
      /* The value can be loaded with a LUI/ADDI combination. */
      codes[0].code = UNKNOWN;
      codes[0].value = high_part;
      codes[1].code = PLUS;
      codes[1].value = low_part;
      return 2;
    }

  if ((value & 1) == 0)
    {
      /* Try eliminating all trailing zeros by ending with SLL. */
      unsigned lshift = __builtin_ctzl (value);
      cost = mips_build_integer (codes, (int64_t)value >> lshift);
      codes[cost].code = ASHIFT;
      codes[cost].value = lshift;
      cost++;
    }

  if (low_part != 0)
    {
      struct mips_integer_op add_codes[MIPS_MAX_INTEGER_OPS];
      unsigned add_cost = mips_build_integer (add_codes, high_part);
      add_codes[add_cost].code = PLUS;
      add_codes[add_cost].value = low_part;
      add_cost++;

      if (add_cost < cost)
	{
	  memcpy (codes, add_codes, add_cost * sizeof (codes[0]));
          cost = add_cost;
	}
    }

  gcc_assert (cost != UINT_MAX);

  return cost;
}

/* Return true if X is a thread-local symbol.  */

static bool
mips_tls_symbol_p (const_rtx x)
{
  return GET_CODE (x) == SYMBOL_REF && SYMBOL_REF_TLS_MODEL (x) != 0;
}

/* Return the method that should be used to access SYMBOL_REF or
   LABEL_REF X in context CONTEXT.  */

static enum mips_symbol_type
mips_classify_symbol (const_rtx x)
{
  if (mips_tls_symbol_p (x))
    return SYMBOL_TLS;

  /* Don't use GOT accesses when -mno-shared is in effect.  */
  if (TARGET_ABICALLS && flag_pic)
    return SYMBOL_GOT_DISP;

  return SYMBOL_ABSOLUTE;
}

/* Classify the base of symbolic expression X, given that X appears in
   context CONTEXT.  */

static enum mips_symbol_type
mips_classify_symbolic_expression (rtx x)
{
  rtx offset;

  split_const (x, &x, &offset);
  if (UNSPEC_ADDRESS_P (x))
    return UNSPEC_ADDRESS_TYPE (x);

  return mips_classify_symbol (x);
}

/* Return true if OFFSET is within the range [0, ALIGN), where ALIGN
   is the alignment in bytes of SYMBOL_REF X.  */

static bool
mips_offset_within_alignment_p (rtx x, HOST_WIDE_INT offset)
{
  HOST_WIDE_INT align;

  align = SYMBOL_REF_DECL (x) ? DECL_ALIGN_UNIT (SYMBOL_REF_DECL (x)) : 1;
  return IN_RANGE (offset, 0, align - 1);
}

/* Return true if X is a symbolic constant that can be used in context
   CONTEXT.  If it is, store the type of the symbol in *SYMBOL_TYPE.  */

bool
mips_symbolic_constant_p (rtx x, enum mips_symbol_type *symbol_type)
{
  rtx offset;

  split_const (x, &x, &offset);
  if (UNSPEC_ADDRESS_P (x))
    {
      *symbol_type = UNSPEC_ADDRESS_TYPE (x);
      x = UNSPEC_ADDRESS (x);
    }
  else if (GET_CODE (x) == SYMBOL_REF || GET_CODE (x) == LABEL_REF)
    {
      *symbol_type = mips_classify_symbol (x);
      if (*symbol_type == SYMBOL_TLS)
	return false;
    }
  else
    return false;

  if (offset == const0_rtx)
    return true;

  /* Check whether a nonzero offset is valid for the underlying
     relocations.  */
  switch (*symbol_type)
    {
    case SYMBOL_ABSOLUTE:
    case SYMBOL_FORCE_TO_MEM:
    case SYMBOL_32_HIGH:
      /* If the target has 64-bit pointers and the object file only
	 supports 32-bit symbols, the values of those symbols will be
	 sign-extended.  In this case we can't allow an arbitrary offset
	 in case the 32-bit value X + OFFSET has a different sign from X.  */
      if (Pmode == DImode)
	return offset_within_block_p (x, INTVAL (offset));

      /* In other cases the relocations can handle any offset.  */
      return true;

    case SYMBOL_TPREL:
    case SYMBOL_DTPREL:
      /* There is no carry between the HI and LO REL relocations, so the
	 offset is only valid if we know it won't lead to such a carry.  */
      return mips_offset_within_alignment_p (x, INTVAL (offset));

    case SYMBOL_GOT_DISP:
    case SYMBOL_GOTOFF_DISP:
    case SYMBOL_GOTOFF_CALL:
    case SYMBOL_GOTOFF_LOADGP:
    case SYMBOL_TLSGD:
    case SYMBOL_TLSLDM:
    case SYMBOL_GOTTPREL:
    case SYMBOL_TLS:
      return false;
    }
  gcc_unreachable ();
}

/* Like mips_symbol_insns, but treat extended MIPS16 instructions as a
   single instruction.  We rely on the fact that, in the worst case,
   all instructions involved in a MIPS16 address calculation are usually
   extended ones.  */

static int
mips_symbol_insns (enum mips_symbol_type type, enum machine_mode mode)
{
  switch (type)
    {
    case SYMBOL_ABSOLUTE:
      return 2;

    case SYMBOL_FORCE_TO_MEM:
      /* LEAs will be converted into constant-pool references by
	 mips_reorg.  */
      if (mode == MAX_MACHINE_MODE)
	return 1;

      /* The constant must be loaded and then dereferenced.  */
      return 0;

    case SYMBOL_GOT_DISP:
      /* The constant will have to be loaded from the GOT before it
	 is used in an address.  */
      if (mode != MAX_MACHINE_MODE)
	return 0;

      /* Fall through.  */

    case SYMBOL_GOTOFF_DISP:
    case SYMBOL_GOTOFF_CALL:
    case SYMBOL_GOTOFF_LOADGP:
    case SYMBOL_32_HIGH:
    case SYMBOL_TLSGD:
    case SYMBOL_TLSLDM:
    case SYMBOL_DTPREL:
    case SYMBOL_GOTTPREL:
    case SYMBOL_TPREL:
      /* A 16-bit constant formed by a single relocation, or a 32-bit
	 constant formed from a high 16-bit relocation and a low 16-bit
	 relocation.  Use mips_split_p to determine which.  32-bit
	 constants need an "lui; addiu" sequence for normal mode and
	 an "li; sll; addiu" sequence for MIPS16 mode.  */
      return !mips_split_p[type] ? 1 : 2;

    case SYMBOL_TLS:
      /* We don't treat a bare TLS symbol as a constant.  */
      return 0;
    }
  gcc_unreachable ();
}

/* A for_each_rtx callback.  Stop the search if *X references a
   thread-local symbol.  */

static int
mips_tls_symbol_ref_1 (rtx *x, void *data ATTRIBUTE_UNUSED)
{
  return mips_tls_symbol_p (*x);
}

/* Implement TARGET_CANNOT_FORCE_CONST_MEM.  */

static bool
mips_cannot_force_const_mem (rtx x)
{
  enum mips_symbol_type type;
  rtx base, offset;

  /* There is no assembler syntax for expressing an address-sized
     high part.  */
  if (GET_CODE (x) == HIGH)
    return true;

  /* As an optimization, reject constants that mips_legitimize_move
     can expand inline.

     Suppose we have a multi-instruction sequence that loads constant C
     into register R.  If R does not get allocated a hard register, and
     R is used in an operand that allows both registers and memory
     references, reload will consider forcing C into memory and using
     one of the instruction's memory alternatives.  Returning false
     here will force it to use an input reload instead.  */
  if (CONST_INT_P (x) && LEGITIMATE_CONSTANT_P (x))
    return true;

  split_const (x, &base, &offset);
  if (mips_symbolic_constant_p (base, &type) && type != SYMBOL_FORCE_TO_MEM)
    {
      /* The same optimization as for CONST_INT.  */
      if (SMALL_INT (offset) && mips_symbol_insns (type, MAX_MACHINE_MODE) > 0)
	return true;
    }

  /* TLS symbols must be computed by mips_legitimize_move.  */
  if (for_each_rtx (&x, &mips_tls_symbol_ref_1, NULL))
    return true;

  return false;
}

/* Return true if register REGNO is a valid base register for mode MODE.
   STRICT_P is true if REG_OK_STRICT is in effect.  */

int
mips_regno_mode_ok_for_base_p (int regno, enum machine_mode mode ATTRIBUTE_UNUSED,
			       bool strict_p)
{
  if (!HARD_REGISTER_NUM_P (regno))
    {
      if (!strict_p)
	return true;
      regno = reg_renumber[regno];
    }

  /* These fake registers will be eliminated to either the stack or
     hard frame pointer, both of which are usually valid base registers.
     Reload deals with the cases where the eliminated form isn't valid.  */
  if (regno == ARG_POINTER_REGNUM || regno == FRAME_POINTER_REGNUM)
    return true;

  return GP_REG_P (regno);
}

/* Return true if X is a valid base register for mode MODE.
   STRICT_P is true if REG_OK_STRICT is in effect.  */

static bool
mips_valid_base_register_p (rtx x, enum machine_mode mode, bool strict_p)
{
  if (!strict_p && GET_CODE (x) == SUBREG)
    x = SUBREG_REG (x);

  return (REG_P (x)
	  && mips_regno_mode_ok_for_base_p (REGNO (x), mode, strict_p));
}

/* Return true if, for every base register BASE_REG, (plus BASE_REG X)
   can address a value of mode MODE.  */

static bool
mips_valid_offset_p (rtx x, enum machine_mode mode)
{
  /* Check that X is a signed 12-bit number.  */
  if (!const_arith_operand (x, Pmode))
    return false;

  /* We may need to split multiword moves, so make sure that every word
     is accessible.  */
  if (GET_MODE_SIZE (mode) > UNITS_PER_WORD
      && !SMALL_OPERAND (INTVAL (x) + GET_MODE_SIZE (mode) - UNITS_PER_WORD))
    return false;

  return true;
}

/* Return true if a LO_SUM can address a value of mode MODE when the
   LO_SUM symbol has type SYMBOL_TYPE.  */

static bool
mips_valid_lo_sum_p (enum mips_symbol_type symbol_type, enum machine_mode mode)
{
  /* Check that symbols of type SYMBOL_TYPE can be used to access values
     of mode MODE.  */
  if (mips_symbol_insns (symbol_type, mode) == 0)
    return false;

  /* Check that there is a known low-part relocation.  */
  if (mips_lo_relocs[symbol_type] == NULL)
    return false;

  /* We may need to split multiword moves, so make sure that each word
     can be accessed without inducing a carry.  This is mainly needed
     for o64, which has historically only guaranteed 64-bit alignment
     for 128-bit types.  */
  if (GET_MODE_SIZE (mode) > UNITS_PER_WORD
      && GET_MODE_BITSIZE (mode) > GET_MODE_ALIGNMENT (mode))
    return false;

  return true;
}

/* Return true if X is a valid address for machine mode MODE.  If it is,
   fill in INFO appropriately.  STRICT_P is true if REG_OK_STRICT is in
   effect.  */

static bool
mips_classify_address (struct mips_address_info *info, rtx x,
		       enum machine_mode mode, bool strict_p)
{
  switch (GET_CODE (x))
    {
    case REG:
    case SUBREG:
      info->type = ADDRESS_REG;
      info->reg = x;
      info->offset = const0_rtx;
      return mips_valid_base_register_p (info->reg, mode, strict_p);

    case PLUS:
      info->type = ADDRESS_REG;
      info->reg = XEXP (x, 0);
      info->offset = XEXP (x, 1);
      return (mips_valid_base_register_p (info->reg, mode, strict_p)
	      && mips_valid_offset_p (info->offset, mode));

    case LO_SUM:
      info->type = ADDRESS_LO_SUM;
      info->reg = XEXP (x, 0);
      info->offset = XEXP (x, 1);
      /* We have to trust the creator of the LO_SUM to do something vaguely
	 sane.  Target-independent code that creates a LO_SUM should also
	 create and verify the matching HIGH.  Target-independent code that
	 adds an offset to a LO_SUM must prove that the offset will not
	 induce a carry.  Failure to do either of these things would be
	 a bug, and we are not required to check for it here.  The MIPS
	 backend itself should only create LO_SUMs for valid symbolic
	 constants, with the high part being either a HIGH or a copy
	 of _gp. */
      info->symbol_type
	= mips_classify_symbolic_expression (info->offset);
      return (mips_valid_base_register_p (info->reg, mode, strict_p)
	      && mips_valid_lo_sum_p (info->symbol_type, mode));

    case CONST_INT:
      /* Small-integer addresses don't occur very often, but they
	 are legitimate if $0 is a valid base register.  */
      info->type = ADDRESS_CONST_INT;
      return SMALL_INT (x);

    case CONST:
    case LABEL_REF:
    case SYMBOL_REF:
      info->type = ADDRESS_SYMBOLIC;
      return (mips_symbolic_constant_p (x, &info->symbol_type)
	      && mips_symbol_insns (info->symbol_type, mode) > 0
	      && !mips_split_p[info->symbol_type]);

    default:
      return false;
    }
}

/* Implement TARGET_LEGITIMATE_ADDRESS_P.  */

static bool
mips_legitimate_address_p (enum machine_mode mode, rtx x, bool strict_p)
{
  struct mips_address_info addr;

  return mips_classify_address (&addr, x, mode, strict_p);
}

/* Return the number of instructions needed to load or store a value
   of mode MODE at address X.  Return 0 if X isn't valid for MODE.
   Assume that multiword moves may need to be split into word moves
   if MIGHT_SPLIT_P, otherwise assume that a single load or store is
   enough.

   For MIPS16 code, count extended instructions as two instructions.  */

int
mips_address_insns (rtx x, enum machine_mode mode, bool might_split_p)
{
  struct mips_address_info addr;

  if (mips_classify_address (&addr, x, mode, false))
    {
      int factor = 1;

      /* BLKmode is used for single unaligned loads and stores and should
         not count as a multiword mode. */
      if (mode != BLKmode && might_split_p)
        factor = (GET_MODE_SIZE (mode) + UNITS_PER_WORD - 1) / UNITS_PER_WORD;

      if (addr.type == ADDRESS_SYMBOLIC)
	factor *= mips_symbol_insns (addr.symbol_type, mode);

      return factor;
    }

  return 0;
}

/* Return the number of instructions needed to load constant X.
   Return 0 if X isn't a valid constant.  */

int
mips_const_insns (rtx x)
{
  struct mips_integer_op codes[MIPS_MAX_INTEGER_OPS];
  enum mips_symbol_type symbol_type;
  rtx offset;

  switch (GET_CODE (x))
    {
    case HIGH:
      if (!mips_symbolic_constant_p (XEXP (x, 0), &symbol_type)
	  || !mips_split_p[symbol_type])
	return 0;

      /* This is simply an LUI. */
      return 1;

    case CONST_INT:
      return mips_build_integer (codes, INTVAL (x));

    case CONST_DOUBLE:
    case CONST_VECTOR:
      /* Allow zeros for normal mode, where we can use $0.  */
      return x == CONST0_RTX (GET_MODE (x)) ? 1 : 0;

    case CONST:
      /* See if we can refer to X directly.  */
      if (mips_symbolic_constant_p (x, &symbol_type))
	return mips_symbol_insns (symbol_type, MAX_MACHINE_MODE);

      /* Otherwise try splitting the constant into a base and offset.
	 If the offset is a 16-bit value, we can load the base address
	 into a register and then use (D)ADDIU to add in the offset.
	 If the offset is larger, we can load the base and offset
	 into separate registers and add them together with (D)ADDU.
	 However, the latter is only possible before reload; during
	 and after reload, we must have the option of forcing the
	 constant into the pool instead.  */
      split_const (x, &x, &offset);
      if (offset != 0)
	{
	  int n = mips_const_insns (x);
	  if (n != 0)
	    {
	      if (SMALL_INT (offset))
		return n + 1;
	      else if (!targetm.cannot_force_const_mem (x))
		return n + 1 + mips_build_integer (codes, INTVAL (offset));
	    }
	}
      return 0;

    case SYMBOL_REF:
    case LABEL_REF:
      return mips_symbol_insns (mips_classify_symbol (x), MAX_MACHINE_MODE);

    default:
      return 0;
    }
}

/* X is a doubleword constant that can be handled by splitting it into
   two words and loading each word separately.  Return the number of
   instructions required to do this.  */

int
mips_split_const_insns (rtx x)
{
  unsigned int low, high;

  low = mips_const_insns (mips_subword (x, false));
  high = mips_const_insns (mips_subword (x, true));
  gcc_assert (low > 0 && high > 0);
  return low + high;
}

/* Return the number of instructions needed to implement INSN,
   given that it loads from or stores to MEM.  Count extended
   MIPS16 instructions as two instructions.  */

int
mips_load_store_insns (rtx mem, rtx insn)
{
  enum machine_mode mode;
  bool might_split_p;
  rtx set;

  gcc_assert (MEM_P (mem));
  mode = GET_MODE (mem);

  /* Try to prove that INSN does not need to be split.  */
  might_split_p = true;
  if (GET_MODE_BITSIZE (mode) == 64)
    {
      set = single_set (insn);
      if (set && !mips_split_64bit_move_p (SET_DEST (set), SET_SRC (set)))
	might_split_p = false;
    }

  return mips_address_insns (XEXP (mem, 0), mode, might_split_p);
}

/* Emit a move from SRC to DEST.  Assume that the move expanders can
   handle all moves if !can_create_pseudo_p ().  The distinction is
   important because, unlike emit_move_insn, the move expanders know
   how to force Pmode objects into the constant pool even when the
   constant pool address is not itself legitimate.  */

rtx
mips_emit_move (rtx dest, rtx src)
{
  return (can_create_pseudo_p ()
	  ? emit_move_insn (dest, src)
	  : emit_move_insn_1 (dest, src));
}

/* Emit an instruction of the form (set TARGET (CODE OP0 OP1)).  */

static void
mips_emit_binary (enum rtx_code code, rtx target, rtx op0, rtx op1)
{
  emit_insn (gen_rtx_SET (VOIDmode, target,
			  gen_rtx_fmt_ee (code, GET_MODE (target), op0, op1)));
}

/* Compute (CODE OP0 OP1) and store the result in a new register
   of mode MODE.  Return that new register.  */

static rtx
mips_force_binary (enum machine_mode mode, enum rtx_code code, rtx op0, rtx op1)
{
  rtx reg;

  reg = gen_reg_rtx (mode);
  mips_emit_binary (code, reg, op0, op1);
  return reg;
}

/* Copy VALUE to a register and return that register.  If new pseudos
   are allowed, copy it into a new register, otherwise use DEST.  */

static rtx
mips_force_temporary (rtx dest, rtx value)
{
  if (can_create_pseudo_p ())
    return force_reg (Pmode, value);
  else
    {
      mips_emit_move (dest, value);
      return dest;
    }
}

/* Emit a call sequence with call pattern PATTERN and return the call
   instruction itself (which is not necessarily the last instruction
   emitted).  ORIG_ADDR is the original, unlegitimized address,
   ADDR is the legitimized form, and LAZY_P is true if the call
   address is lazily-bound.  */

static rtx
mips_emit_call_insn (rtx pattern, bool lazy_p)
{
  rtx insn;

  insn = emit_call_insn (pattern);

  if (lazy_p)
    /* Lazy-binding stubs require $gp to be valid on entry.  */
    use_reg (&CALL_INSN_FUNCTION_USAGE (insn), pic_offset_table_rtx);

  if (TARGET_USE_GOT)
    {
      /* See the comment above load_call<mode> for details.  */
      use_reg (&CALL_INSN_FUNCTION_USAGE (insn),
	       gen_rtx_REG (Pmode, GOT_VERSION_REGNUM));
      emit_insn (gen_update_got_version ());
    }
  return insn;
}

/* Wrap symbol or label BASE in an UNSPEC address of type SYMBOL_TYPE,
   then add CONST_INT OFFSET to the result.  */

static rtx
mips_unspec_address_offset (rtx base, rtx offset,
			    enum mips_symbol_type symbol_type)
{
  base = gen_rtx_UNSPEC (Pmode, gen_rtvec (1, base),
			 UNSPEC_ADDRESS_FIRST + symbol_type);
  if (offset != const0_rtx)
    base = gen_rtx_PLUS (Pmode, base, offset);
  return gen_rtx_CONST (Pmode, base);
}

/* Return an UNSPEC address with underlying address ADDRESS and symbol
   type SYMBOL_TYPE.  */

rtx
mips_unspec_address (rtx address, enum mips_symbol_type symbol_type)
{
  rtx base, offset;

  split_const (address, &base, &offset);
  return mips_unspec_address_offset (base, offset, symbol_type);
}

/* If OP is an UNSPEC address, return the address to which it refers,
   otherwise return OP itself.  */

static rtx
mips_strip_unspec_address (rtx op)
{
  rtx base, offset;

  split_const (op, &base, &offset);
  if (UNSPEC_ADDRESS_P (base))
    op = plus_constant (UNSPEC_ADDRESS (base), INTVAL (offset));
  return op;
}

/* If mips_unspec_address (ADDR, SYMBOL_TYPE) is a 32-bit value, add the
   high part to BASE and return the result.  Just return BASE otherwise.
   TEMP is as for mips_force_temporary.

   The returned expression can be used as the first operand to a LO_SUM.  */

static rtx
mips_unspec_offset_high (rtx temp, rtx base, rtx addr,
			 enum mips_symbol_type symbol_type)
{
  if (mips_split_p[symbol_type])
    {
      addr = gen_rtx_HIGH (Pmode, mips_unspec_address (addr, symbol_type));
      addr = mips_force_temporary (temp, addr);
      base = mips_force_temporary (temp, gen_rtx_PLUS (Pmode, addr, base));
    }
  return base;
}

/* Return the RHS of a load_call<mode> insn.  */

static rtx
mips_unspec_call (rtx reg, rtx symbol)
{
  rtvec vec;

  vec = gen_rtvec (3, reg, symbol, gen_rtx_REG (SImode, GOT_VERSION_REGNUM));
  return gen_rtx_UNSPEC (Pmode, vec, UNSPEC_LOAD_CALL);
}

/* Create and return a GOT reference of type TYPE for address ADDR.
   TEMP, if nonnull, is a scratch Pmode base register.  */

rtx
mips_got_load (rtx temp, rtx addr, enum mips_symbol_type type)
{
  rtx base, high, lo_sum_symbol;

  base = pic_offset_table_rtx;

  /* If we used the temporary register to load $gp, we can't use
     it for the high part as well.  */
  if (temp != NULL && reg_overlap_mentioned_p (base, temp))
    temp = NULL;

  high = mips_unspec_offset_high (temp, base, addr, type);
  lo_sum_symbol = mips_unspec_address (addr, type);

  if (type == SYMBOL_GOTOFF_CALL)
    return mips_unspec_call (high, lo_sum_symbol);
  else
    return (Pmode == SImode
	    ? gen_unspec_gotsi (high, lo_sum_symbol)
	    : gen_unspec_gotdi (high, lo_sum_symbol));
}

/* If MODE is MAX_MACHINE_MODE, ADDR appears as a move operand, otherwise
   it appears in a MEM of that mode.  Return true if ADDR is a legitimate
   constant in that context and can be split into high and low parts.
   If so, and if LOW_OUT is nonnull, emit the high part and store the
   low part in *LOW_OUT.  Leave *LOW_OUT unchanged otherwise.

   TEMP is as for mips_force_temporary and is used to load the high
   part into a register.

   When MODE is MAX_MACHINE_MODE, the low part is guaranteed to be
   a legitimize SET_SRC for an .md pattern, otherwise the low part
   is guaranteed to be a legitimate address for mode MODE.  */

bool
mips_split_symbol (rtx temp, rtx addr, enum machine_mode mode, rtx *low_out)
{
  enum mips_symbol_type symbol_type;
  rtx high;

  if (!(GET_CODE (addr) == HIGH && mode == MAX_MACHINE_MODE))
    {
      if (mips_symbolic_constant_p (addr, &symbol_type)
	  && mips_symbol_insns (symbol_type, mode) > 0
	  && mips_split_p[symbol_type])
	{
	  if (low_out)
	    switch (symbol_type)
	      {
	      case SYMBOL_GOT_DISP:
		/* SYMBOL_GOT_DISP symbols are loaded from the GOT.  */
		*low_out = mips_got_load (temp, addr, SYMBOL_GOTOFF_DISP);
		break;

	      default:
		high = gen_rtx_HIGH (Pmode, copy_rtx (addr));
		high = mips_force_temporary (temp, high);
		*low_out = gen_rtx_LO_SUM (Pmode, high, addr);
		break;
	      }
	  return true;
	}
    }
  return false;
}

/* Return a legitimate address for REG + OFFSET.  TEMP is as for
   mips_force_temporary; it is only needed when OFFSET is not a
   SMALL_OPERAND.  */

static rtx
mips_add_offset (rtx temp, rtx reg, HOST_WIDE_INT offset)
{
  if (!SMALL_OPERAND (offset))
    {
      rtx high;

      /* Leave OFFSET as a 16-bit offset and put the excess in HIGH.
         The addition inside the macro CONST_HIGH_PART may cause an
         overflow, so we need to force a sign-extension check.  */
      high = gen_int_mode (RISCV_CONST_HIGH_PART (offset), Pmode);
      offset = RISCV_CONST_LOW_PART (offset);
      high = mips_force_temporary (temp, high);
      reg = mips_force_temporary (temp, gen_rtx_PLUS (Pmode, high, reg));
    }
  return plus_constant (reg, offset);
}

/* The __tls_get_attr symbol.  */
static GTY(()) rtx mips_tls_symbol;

/* Return an instruction sequence that calls __tls_get_addr.  SYM is
   the TLS symbol we are referencing and TYPE is the symbol type to use
   (either global dynamic or local dynamic).  V0 is an RTX for the
   return value location.  */

static rtx
mips_call_tls_get_addr (rtx sym, enum mips_symbol_type type, rtx v0)
{
  rtx insn, loc, a0, temp;

  a0 = gen_rtx_REG (Pmode, GP_ARG_FIRST);

  if (!mips_tls_symbol)
    mips_tls_symbol = init_one_libfunc ("__tls_get_addr");

  temp = gen_reg_rtx (Pmode);
  loc = mips_unspec_offset_high (temp, pic_offset_table_rtx, sym, type);
  loc = gen_rtx_LO_SUM (Pmode, loc,
			mips_unspec_address (sym, type));

  start_sequence ();

  emit_insn (gen_rtx_SET (Pmode, a0, loc));
  insn = mips_expand_call (MIPS_CALL_NORMAL, v0, mips_tls_symbol,
			   const0_rtx, NULL_RTX, false);
  RTL_CONST_CALL_P (insn) = 1;
  use_reg (&CALL_INSN_FUNCTION_USAGE (insn), a0);
  insn = get_insns ();

  end_sequence ();

  return insn;
}

/* Generate the code to access LOC, a thread-local SYMBOL_REF, and return
   its address.  The return value will be both a valid address and a valid
   SET_SRC (either a REG or a LO_SUM).  */

static rtx
mips_legitimize_tls_address (rtx loc)
{
  rtx dest, insn, v0, tp, tmp1, tmp2, eqv;
  enum tls_model model;

  model = SYMBOL_REF_TLS_MODEL (loc);
  /* Only TARGET_ABICALLS code can have more than one module; other
     code must be be static and should not use a GOT.  All TLS models
     reduce to local exec in this situation.  */
  if (!TARGET_ABICALLS)
    model = TLS_MODEL_LOCAL_EXEC;

  switch (model)
    {
    case TLS_MODEL_GLOBAL_DYNAMIC:
      v0 = gen_rtx_REG (Pmode, GP_RETURN);
      insn = mips_call_tls_get_addr (loc, SYMBOL_TLSGD, v0);
      dest = gen_reg_rtx (Pmode);
      emit_libcall_block (insn, dest, v0, loc);
      break;

    case TLS_MODEL_LOCAL_DYNAMIC:
      v0 = gen_rtx_REG (Pmode, GP_RETURN);
      insn = mips_call_tls_get_addr (loc, SYMBOL_TLSLDM, v0);
      tmp1 = gen_reg_rtx (Pmode);

      /* Attach a unique REG_EQUIV, to allow the RTL optimizers to
	 share the LDM result with other LD model accesses.  */
      eqv = gen_rtx_UNSPEC (Pmode, gen_rtvec (1, const0_rtx),
			    UNSPEC_TLS_LDM);
      emit_libcall_block (insn, tmp1, v0, eqv);

      tmp2 = mips_unspec_offset_high (NULL, tmp1, loc, SYMBOL_DTPREL);
      dest = gen_rtx_LO_SUM (Pmode, tmp2,
			     mips_unspec_address (loc, SYMBOL_DTPREL));
      break;

    case TLS_MODEL_INITIAL_EXEC:
      tp = gen_rtx_REG (Pmode, THREAD_POINTER_REGNUM);
      tmp1 = gen_reg_rtx (Pmode);
      tmp2 = mips_got_load (tmp1, loc, SYMBOL_GOTTPREL);
      emit_insn (gen_rtx_SET (VOIDmode, tmp1, tmp2));
      dest = gen_reg_rtx (Pmode);
      emit_insn (gen_add3_insn (dest, tmp1, tp));
      break;

    case TLS_MODEL_LOCAL_EXEC:
      tp = gen_rtx_REG (Pmode, THREAD_POINTER_REGNUM);
      tmp1 = mips_unspec_offset_high (NULL, tp, loc, SYMBOL_TPREL);
      dest = gen_rtx_LO_SUM (Pmode, tmp1,
			     mips_unspec_address (loc, SYMBOL_TPREL));
      break;

    default:
      gcc_unreachable ();
    }
  return dest;
}

/* If X is not a valid address for mode MODE, force it into a register.  */

static rtx
mips_force_address (rtx x, enum machine_mode mode)
{
  if (!mips_legitimate_address_p (mode, x, false))
    x = force_reg (Pmode, x);
  return x;
}

/* This function is used to implement LEGITIMIZE_ADDRESS.  If X can
   be legitimized in a way that the generic machinery might not expect,
   return a new address, otherwise return NULL.  MODE is the mode of
   the memory being accessed.  */

static rtx
mips_legitimize_address (rtx x, rtx oldx ATTRIBUTE_UNUSED,
			 enum machine_mode mode)
{
  rtx addr;

  if (mips_tls_symbol_p (x))
    return mips_legitimize_tls_address (x);

  /* See if the address can split into a high part and a LO_SUM.  */
  if (mips_split_symbol (NULL, x, mode, &addr))
    return mips_force_address (addr, mode);

  /* Handle BASE + OFFSET using mips_add_offset.  */
  if (GET_CODE (x) == PLUS && CONST_INT_P (XEXP (x, 1))
      && INTVAL (XEXP (x, 1)) != 0)
    {
      rtx base = XEXP (x, 0);
      HOST_WIDE_INT offset = INTVAL (XEXP (x, 1));

      if (!mips_valid_base_register_p (base, mode, false))
	base = copy_to_mode_reg (Pmode, base);
      addr = mips_add_offset (NULL, base, offset);
      return mips_force_address (addr, mode);
    }

  return x;
}

/* Load VALUE into DEST.  TEMP is as for mips_force_temporary.  */

void
mips_move_integer (rtx temp, rtx dest, unsigned HOST_WIDE_INT value)
{
  struct mips_integer_op codes[MIPS_MAX_INTEGER_OPS];
  enum machine_mode mode;
  unsigned int i, num_ops;
  rtx x;

  mode = GET_MODE (dest);
  num_ops = mips_build_integer (codes, value);

  /* Apply each binary operation to X.  Invariant: X is a legitimate
     source operand for a SET pattern.  */
  x = GEN_INT (codes[0].value);

  if(mode == HImode && num_ops == 2)
    {
      /* Some HImode constants are loaded with a LUI/ADDI[W] pair, whose
         result would ordinarily be SImode.  For the ADDI[W], we invoke a
         special addsihi instruction to mark the output as HImode instead.
         The instruction doesn't actually canonicalize the result to 16 bits,
	 as it's really just ADDI[W], but that's OK here because any HImode
	 constant is already canonical. */
      gcc_assert(codes[1].code == PLUS);
      gcc_assert((codes[0].value + codes[1].value) == value);

      if (!can_create_pseudo_p ())
	{
      	  emit_insn (gen_rtx_SET (SImode, temp, x));
      	  x = temp;
      	}
      else
        x = force_reg (SImode, x);

      emit_insn (gen_bullshit_addsihi(dest, x, GEN_INT (codes[1].value)));
    }
  else
    {
      for (i = 1; i < num_ops; i++)
        {
          if (!can_create_pseudo_p ())
            {
              emit_insn (gen_rtx_SET (VOIDmode, temp, x));
              x = temp;
            }
          else
            x = force_reg (mode, x);

          x = gen_rtx_fmt_ee (codes[i].code, mode, x, GEN_INT (codes[i].value));
        }

      emit_insn (gen_rtx_SET (VOIDmode, dest, x));
    }
}

/* Subroutine of mips_legitimize_move.  Move constant SRC into register
   DEST given that SRC satisfies immediate_operand but doesn't satisfy
   move_operand.  */

static void
mips_legitimize_const_move (enum machine_mode mode, rtx dest, rtx src)
{
  rtx base, offset;

  /* Split moves of big integers into smaller pieces.  */
  if (splittable_const_int_operand (src, mode))
    {
      mips_move_integer (dest, dest, INTVAL (src));
      return;
    }

  /* Split moves of symbolic constants into high/low pairs.  */
  if (mips_split_symbol (dest, src, MAX_MACHINE_MODE, &src))
    {
      emit_insn (gen_rtx_SET (VOIDmode, dest, src));
      return;
    }

  /* Generate the appropriate access sequences for TLS symbols.  */
  if (mips_tls_symbol_p (src))
    {
      mips_emit_move (dest, mips_legitimize_tls_address (src));
      return;
    }

  /* If we have (const (plus symbol offset)), and that expression cannot
     be forced into memory, load the symbol first and add in the offset.
     In non-MIPS16 mode, prefer to do this even if the constant _can_ be
     forced into memory, as it usually produces better code.  */
  split_const (src, &base, &offset);
  if (offset != const0_rtx
      && (targetm.cannot_force_const_mem (src)
	  || can_create_pseudo_p ()))
    {
      base = mips_force_temporary (dest, base);
      mips_emit_move (dest, mips_add_offset (NULL, base, INTVAL (offset)));
      return;
    }

  src = force_const_mem (mode, src);

  /* When using explicit relocs, constant pool references are sometimes
     not legitimate addresses.  */
  mips_split_symbol (dest, XEXP (src, 0), mode, &XEXP (src, 0));
  mips_emit_move (dest, src);
}

/* If (set DEST SRC) is not a valid move instruction, emit an equivalent
   sequence that is valid.  */

bool
mips_legitimize_move (enum machine_mode mode, rtx dest, rtx src)
{
  if (!register_operand (dest, mode) && !reg_or_0_operand (src, mode))
    {
      mips_emit_move (dest, force_reg (mode, src));
      return true;
    }

  /* We need to deal with constants that would be legitimate
     immediate_operands but aren't legitimate move_operands.  */
  if (CONSTANT_P (src) && !move_operand (src, mode))
    {
      mips_legitimize_const_move (mode, dest, src);
      set_unique_reg_note (get_last_insn (), REG_EQUAL, copy_rtx (src));
      return true;
    }
  return false;
}

/* The cost of loading values from the constant pool.  It should be
   larger than the cost of any constant we want to synthesize inline.  */
#define CONSTANT_POOL_COST COSTS_N_INSNS (8)

/* Return true if there is a non-MIPS16 instruction that implements CODE
   and if that instruction accepts X as an immediate operand.  */

static int
mips_immediate_operand_p (int code, HOST_WIDE_INT x)
{
  switch (code)
    {
    case ASHIFT:
    case ASHIFTRT:
    case LSHIFTRT:
      /* All shift counts are truncated to a valid constant.  */
      return true;

    case AND:
    case IOR:
    case XOR:
    case PLUS:
    case LT:
    case LTU:
      /* These instructions take 12-bit signed immediates.  */
      return SMALL_OPERAND (x);

    case LE:
      /* We add 1 to the immediate and use SLT.  */
      return SMALL_OPERAND (x + 1);

    case LEU:
      /* Likewise SLTU, but reject the always-true case.  */
      return SMALL_OPERAND (x + 1) && x + 1 != 0;

    case GE:
    case GEU:
      /* We can emulate an immediate of 1 by using GT/GTU against x0. */
      return x == 1;

    default:
      /* By default assume that x0 can be used for 0.  */
      return x == 0;
    }
}

/* Return the cost of binary operation X, given that the instruction
   sequence for a word-sized or smaller operation has cost SINGLE_COST
   and that the sequence of a double-word operation has cost DOUBLE_COST.
   If SPEED is true, optimize for speed otherwise optimize for size.  */

static int
mips_binary_cost (rtx x, int single_cost, int double_cost, bool speed)
{
  if (GET_MODE_SIZE (GET_MODE (x)) == UNITS_PER_WORD * 2)
    single_cost = double_cost;

  return (single_cost
	  + rtx_cost (XEXP (x, 0), SET, speed)
	  + rtx_cost (XEXP (x, 1), GET_CODE (x), speed));
}

/* Return the cost of floating-point multiplications of mode MODE.  */

static int
mips_fp_mult_cost (enum machine_mode mode)
{
  return mode == DFmode ? mips_cost->fp_mult_df : mips_cost->fp_mult_sf;
}

/* Return the cost of floating-point divisions of mode MODE.  */

static int
mips_fp_div_cost (enum machine_mode mode)
{
  return mode == DFmode ? mips_cost->fp_div_df : mips_cost->fp_div_sf;
}

/* Return the cost of sign-extending OP to mode MODE, not including the
   cost of OP itself.  */

static int
mips_sign_extend_cost (enum machine_mode mode, rtx op)
{
  if (MEM_P (op))
    /* Extended loads are as cheap as unextended ones.  */
    return 0;

  if (TARGET_64BIT && mode == DImode && GET_MODE (op) == SImode)
    /* A sign extension from SImode to DImode in 64-bit mode is free.  */
    return 0;

  /* We need to use a shift left and a shift right.  */
  return COSTS_N_INSNS (2);
}

/* Return the cost of zero-extending OP to mode MODE, not including the
   cost of OP itself.  */

static int
mips_zero_extend_cost (enum machine_mode mode, rtx op)
{
  if (MEM_P (op))
    /* Extended loads are as cheap as unextended ones.  */
    return 0;

  if ((TARGET_64BIT && mode == DImode && GET_MODE (op) == SImode) ||
      ((mode == DImode || mode == SImode) && GET_MODE (op) == HImode))
    /* We need a shift left by 32 bits and a shift right by 32 bits.  */
    return COSTS_N_INSNS (2);

  /* We can use ANDI.  */
  return COSTS_N_INSNS (1);
}

/* Implement TARGET_RTX_COSTS.  */

static bool
mips_rtx_costs (rtx x, int code, int outer_code, int *total, bool speed)
{
  enum machine_mode mode = GET_MODE (x);
  bool float_mode_p = FLOAT_MODE_P (mode);
  int cost;
  rtx addr;

  /* The cost of a COMPARE is hard to define for MIPS.  COMPAREs don't
     appear in the instruction stream, and the cost of a comparison is
     really the cost of the branch or scc condition.  At the time of
     writing, GCC only uses an explicit outer COMPARE code when optabs
     is testing whether a constant is expensive enough to force into a
     register.  We want optabs to pass such constants through the MIPS
     expanders instead, so make all constants very cheap here.  */
  if (outer_code == COMPARE)
    {
      gcc_assert (CONSTANT_P (x));
      *total = 0;
      return true;
    }

  switch (code)
    {
    case CONST_INT:
      /* Treat *clear_upper32-style ANDs as having zero cost in the
	 second operand.  The cost is entirely in the first operand.

	 ??? This is needed because we would otherwise try to CSE
	 the constant operand.  Although that's the right thing for
	 instructions that continue to be a register operation throughout
	 compilation, it is disastrous for instructions that could
	 later be converted into a memory operation.  */
      if (TARGET_64BIT
	  && outer_code == AND
	  && UINTVAL (x) == 0xffffffff)
	{
	  *total = 0;
	  return true;
	}

      /* When not optimizing for size, we care more about the cost
         of hot code, and hot code is often in a loop.  If a constant
         operand needs to be forced into a register, we will often be
         able to hoist the constant load out of the loop, so the load
         should not contribute to the cost.  */
      if (speed || mips_immediate_operand_p (outer_code, INTVAL (x)))
        {
          *total = 0;
          return true;
        }
      /* Fall through.  */

    case CONST:
    case SYMBOL_REF:
    case LABEL_REF:
    case CONST_DOUBLE:
      if (force_to_mem_operand (x, VOIDmode))
	{
	  *total = COSTS_N_INSNS (1);
	  return true;
	}
      cost = mips_const_insns (x);
      if (cost > 0)
	{
	  /* If the constant is likely to be stored in a GPR, SETs of
	     single-insn constants are as cheap as register sets; we
	     never want to CSE them.

	     Don't reduce the cost of storing a floating-point zero in
	     FPRs.  If we have a zero in an FPR for other reasons, we
	     can get better cfg-cleanup and delayed-branch results by
	     using it consistently, rather than using $0 sometimes and
	     an FPR at other times.  Also, moves between floating-point
	     registers are sometimes cheaper than (D)MTC1 $0.  */
	  if (cost == 1
	      && outer_code == SET
	      && !(float_mode_p && TARGET_HARD_FLOAT))
	    cost = 0;
	  /* When non-MIPS16 code loads a constant N>1 times, we rarely
	     want to CSE the constant itself.  It is usually better to
	     have N copies of the last operation in the sequence and one
	     shared copy of the other operations.  (Note that this is
	     not true for MIPS16 code, where the final operation in the
	     sequence is often an extended instruction.)

	     Also, if we have a CONST_INT, we don't know whether it is
	     for a word or doubleword operation, so we cannot rely on
	     the result of mips_build_integer.  */
	  else if (outer_code == SET || mode == VOIDmode)
	    cost = 1;
	  *total = COSTS_N_INSNS (cost);
	  return true;
	}
      /* The value will need to be fetched from the constant pool.  */
      *total = CONSTANT_POOL_COST;
      return true;

    case MEM:
      /* If the address is legitimate, return the number of
	 instructions it needs.  */
      addr = XEXP (x, 0);
      cost = mips_address_insns (addr, mode, true);
      if (cost > 0)
	{
	  *total = COSTS_N_INSNS (cost + 1);
	  return true;
	}
      /* Otherwise use the default handling.  */
      return false;

    case FFS:
      *total = COSTS_N_INSNS (6);
      return false;

    case NOT:
      *total = COSTS_N_INSNS (GET_MODE_SIZE (mode) > UNITS_PER_WORD ? 2 : 1);
      return false;

    case AND:
      /* Check for a *clear_upper32 pattern and treat it like a zero
	 extension.  See the pattern's comment for details.  */
      if (TARGET_64BIT
	  && mode == DImode
	  && CONST_INT_P (XEXP (x, 1))
	  && UINTVAL (XEXP (x, 1)) == 0xffffffff)
	{
	  *total = (mips_zero_extend_cost (mode, XEXP (x, 0))
		    + rtx_cost (XEXP (x, 0), SET, speed));
	  return true;
	}
      /* Fall through.  */

    case IOR:
    case XOR:
      /* Double-word operations use two single-word operations.  */
      *total = mips_binary_cost (x, COSTS_N_INSNS (1), COSTS_N_INSNS (2),
				 speed);
      return true;

    case ASHIFT:
    case ASHIFTRT:
    case LSHIFTRT:
    case ROTATE:
    case ROTATERT:
      if (CONSTANT_P (XEXP (x, 1)))
	*total = mips_binary_cost (x, COSTS_N_INSNS (1), COSTS_N_INSNS (4),
				   speed);
      else
	*total = mips_binary_cost (x, COSTS_N_INSNS (1), COSTS_N_INSNS (12),
				   speed);
      return true;

    case ABS:
      if (float_mode_p)
        *total = mips_cost->fp_add;
      else
        *total = COSTS_N_INSNS (4);
      return false;

    case LO_SUM:
      /* Low-part immediates need an extended MIPS16 instruction.  */
      *total = (COSTS_N_INSNS (1)
		+ rtx_cost (XEXP (x, 0), SET, speed));
      return true;

    case LT:
    case LTU:
    case LE:
    case LEU:
    case GT:
    case GTU:
    case GE:
    case GEU:
    case EQ:
    case NE:
    case UNORDERED:
    case LTGT:
      /* Branch comparisons have VOIDmode, so use the first operand's
	 mode instead.  */
      mode = GET_MODE (XEXP (x, 0));
      if (FLOAT_MODE_P (mode))
	{
	  *total = mips_cost->fp_add;
	  return false;
	}
      *total = mips_binary_cost (x, COSTS_N_INSNS (1), COSTS_N_INSNS (4),
				 speed);
      return true;

    case MINUS:
      if (float_mode_p
	  && !HONOR_NANS (mode)
	  && !HONOR_SIGNED_ZEROS (mode))
	{
	  /* See if we can use NMADD or NMSUB.  See mips.md for the
	     associated patterns.  */
	  rtx op0 = XEXP (x, 0);
	  rtx op1 = XEXP (x, 1);
	  if (GET_CODE (op0) == MULT && GET_CODE (XEXP (op0, 0)) == NEG)
	    {
	      *total = (mips_fp_mult_cost (mode)
			+ rtx_cost (XEXP (XEXP (op0, 0), 0), SET, speed)
			+ rtx_cost (XEXP (op0, 1), SET, speed)
			+ rtx_cost (op1, SET, speed));
	      return true;
	    }
	  if (GET_CODE (op1) == MULT)
	    {
	      *total = (mips_fp_mult_cost (mode)
			+ rtx_cost (op0, SET, speed)
			+ rtx_cost (XEXP (op1, 0), SET, speed)
			+ rtx_cost (XEXP (op1, 1), SET, speed));
	      return true;
	    }
	}
      /* Fall through.  */

    case PLUS:
      if (float_mode_p)
	{
	  /* If this is part of a MADD or MSUB, treat the PLUS as
	     being free.  */
	  if (GET_CODE (XEXP (x, 0)) == MULT)
	    *total = 0;
	  else
	    *total = mips_cost->fp_add;
	  return false;
	}

      /* Double-word operations require three single-word operations and
	 an SLTU.  The MIPS16 version then needs to move the result of
	 the SLTU from $24 to a MIPS16 register.  */
      *total = mips_binary_cost (x, COSTS_N_INSNS (1),
				 COSTS_N_INSNS (4),
				 speed);
      return true;

    case NEG:
      if (float_mode_p
	  && !HONOR_NANS (mode)
	  && HONOR_SIGNED_ZEROS (mode))
	{
	  /* See if we can use NMADD or NMSUB.  See mips.md for the
	     associated patterns.  */
	  rtx op = XEXP (x, 0);
	  if ((GET_CODE (op) == PLUS || GET_CODE (op) == MINUS)
	      && GET_CODE (XEXP (op, 0)) == MULT)
	    {
	      *total = (mips_fp_mult_cost (mode)
			+ rtx_cost (XEXP (XEXP (op, 0), 0), SET, speed)
			+ rtx_cost (XEXP (XEXP (op, 0), 1), SET, speed)
			+ rtx_cost (XEXP (op, 1), SET, speed));
	      return true;
	    }
	}

      if (float_mode_p)
	*total = mips_cost->fp_add;
      else
	*total = COSTS_N_INSNS (GET_MODE_SIZE (mode) > UNITS_PER_WORD ? 4 : 1);
      return false;

    case MULT:
      if (float_mode_p)
	*total = mips_fp_mult_cost (mode);
      else if (mode == DImode && !TARGET_64BIT)
	/* We use a MUL and a MULH[[S]U]. */
	*total = mips_cost->int_mult_si * 2;
      else if (!speed)
	*total = 1;
      else if (mode == DImode)
	*total = mips_cost->int_mult_di;
      else
	*total = mips_cost->int_mult_si;
      return false;

    case DIV:
      /* Check for a reciprocal.  */
      if (float_mode_p
	  && flag_unsafe_math_optimizations
	  && XEXP (x, 0) == CONST1_RTX (mode))
	{
	  if (outer_code == SQRT || GET_CODE (XEXP (x, 1)) == SQRT)
	    /* An rsqrt<mode>a or rsqrt<mode>b pattern.  Count the
	       division as being free.  */
	    *total = rtx_cost (XEXP (x, 1), SET, speed);
	  else
	    *total = (mips_fp_div_cost (mode)
		      + rtx_cost (XEXP (x, 1), SET, speed));
	  return true;
	}
      /* Fall through.  */

    case SQRT:
    case MOD:
      if (float_mode_p)
	{
	  *total = mips_fp_div_cost (mode);
	  return false;
	}
      /* Fall through.  */

    case UDIV:
    case UMOD:
      if (!speed)
	*total = 1;
      else if (mode == DImode)
        *total = mips_cost->int_div_di;
      else
	*total = mips_cost->int_div_si;
      return false;

    case SIGN_EXTEND:
      *total = mips_sign_extend_cost (mode, XEXP (x, 0));
      return false;

    case ZERO_EXTEND:
      *total = mips_zero_extend_cost (mode, XEXP (x, 0));
      return false;

    case FLOAT:
    case UNSIGNED_FLOAT:
    case FIX:
    case FLOAT_EXTEND:
    case FLOAT_TRUNCATE:
      *total = mips_cost->fp_add;
      return false;

    default:
      return false;
    }
}

/* Implement TARGET_ADDRESS_COST.  */

static int
mips_address_cost (rtx addr, bool speed ATTRIBUTE_UNUSED)
{
  return mips_address_insns (addr, SImode, false);
}

/* Return one word of double-word value OP, taking into account the fixed
   endianness of certain registers.  HIGH_P is true to select the high part,
   false to select the low part.  */

rtx
mips_subword (rtx op, bool high_p)
{
  unsigned int byte, offset;
  enum machine_mode mode;

  mode = GET_MODE (op);
  if (mode == VOIDmode)
    mode = TARGET_64BIT ? TImode : DImode;

  if (TARGET_BIG_ENDIAN ? !high_p : high_p)
    byte = UNITS_PER_WORD;
  else
    byte = 0;

  if (FP_REG_RTX_P (op))
    {
      /* Paired FPRs are always ordered little-endian.  */
      offset = (UNITS_PER_WORD < UNITS_PER_HWFPVALUE ? high_p : byte != 0);
      return gen_rtx_REG (word_mode, REGNO (op) + offset);
    }

  if (MEM_P (op))
    return adjust_address (op, word_mode, byte);

  return simplify_gen_subreg (word_mode, op, mode, byte);
}

/* Return true if a 64-bit move from SRC to DEST should be split into two.  */

bool
mips_split_64bit_move_p (rtx dest, rtx src)
{
  /* All 64b moves are legal in 64b mode.  All 64b FPR <-> FPR and
     FPR <-> MEM moves are legal in 32b mode, too.  Although
     FPR <-> GPR moves are not available in general in 32b mode,
     we can at least load 0 into an FPR with fcvt.d.w fpr, x0. */
  return !(TARGET_64BIT
	   || (FP_REG_RTX_P (src) && FP_REG_RTX_P (dest))
	   || (FP_REG_RTX_P (dest) && MEM_P (src))
	   || (FP_REG_RTX_P (src) && MEM_P (dest))
	   || (FP_REG_RTX_P(dest) && src == CONST0_RTX(GET_MODE(src))));
}

/* Split a doubleword move from SRC to DEST.  On 32-bit targets,
   this function handles 64-bit moves for which mips_split_64bit_move_p
   holds.  For 64-bit targets, this function handles 128-bit moves.  */

void
mips_split_doubleword_move (rtx dest, rtx src)
{
  rtx low_dest;

   /* The operation can be split into two normal moves.  Decide in
      which order to do them.  */
   low_dest = mips_subword (dest, false);
   if (REG_P (low_dest) && reg_overlap_mentioned_p (low_dest, src))
     {
       mips_emit_move (mips_subword (dest, true), mips_subword (src, true));
       mips_emit_move (low_dest, mips_subword (src, false));
     }
   else
     {
       mips_emit_move (low_dest, mips_subword (src, false));
       mips_emit_move (mips_subword (dest, true), mips_subword (src, true));
     }
}

/* Return the appropriate instructions to move SRC into DEST.  Assume
   that SRC is operand 1 and DEST is operand 0.  */

const char *
mips_output_move (rtx dest, rtx src)
{
  enum rtx_code dest_code, src_code;
  enum machine_mode mode;
  enum mips_symbol_type symbol_type;
  bool dbl_p;

  dest_code = GET_CODE (dest);
  src_code = GET_CODE (src);
  mode = GET_MODE (dest);
  dbl_p = (GET_MODE_SIZE (mode) == 8);

  if (dbl_p && mips_split_64bit_move_p (dest, src))
    return "#";

  if ((src_code == REG && GP_REG_P (REGNO (src)))
      || (src == CONST0_RTX (mode)))
    {
      if (dest_code == REG)
	{
	  if (GP_REG_P (REGNO (dest)))
	    return "move\t%0,%z1";

	  if (FP_REG_P (REGNO (dest)))
	    {
	      if (!dbl_p)
		return "mxtf.s\t%0,%z1";
	      if (TARGET_64BIT)
		return "mxtf.d\t%0,%z1";
	      /* in RV32, we can emulate mxtf.d %0, x0 using fcvt.d.w */
	      gcc_assert (src == CONST0_RTX (mode));
	      return "fcvt.d.w\t%0,x0";
	    }
	}
      if (dest_code == MEM)
	switch (GET_MODE_SIZE (mode))
	  {
	  case 1: return "sb\t%z1,%0";
	  case 2: return "sh\t%z1,%0";
	  case 4: return "sw\t%z1,%0";
	  case 8: return "sd\t%z1,%0";
	  }
    }
  if (dest_code == REG && GP_REG_P (REGNO (dest)))
    {
      if (src_code == REG)
	{
	  if (FP_REG_P (REGNO (src)))
	    return dbl_p ? "mftx.d\t%0,%1" : "mftx.s\t%0,%1";
	}

      if (src_code == MEM)
	switch (GET_MODE_SIZE (mode))
	  {
	  case 1: return "lbu\t%0,%1";
	  case 2: return "lhu\t%0,%1";
	  case 4: return "lw\t%0,%1";
	  case 8: return "ld\t%0,%1";
	  }

      if (src_code == CONST_INT)
	return "li\t%0,%1\t\t\t# %X1";

      if (src_code == HIGH)
	return "lui\t%0,%h1";

      if (mips_symbolic_constant_p (src, &symbol_type)
	  && mips_lo_relocs[symbol_type] != 0)
	{
	  /* A signed 16-bit constant formed by applying a relocation
	     operator to a symbolic address.  */
	  gcc_assert (!mips_split_p[symbol_type]);
	  return "li\t%0,%R1";
	}
    }
  if (src_code == REG && FP_REG_P (REGNO (src)))
    {
      if (dest_code == REG && FP_REG_P (REGNO (dest)))
	return dbl_p ? "fsgnj.d\t%0,%1,%1" : "fsgnj.s\t%0,%1,%1";

      if (dest_code == MEM)
	return dbl_p ? "fsd\t%1,%0" : "fsw\t%1,%0";
    }
  if (dest_code == REG && FP_REG_P (REGNO (dest)))
    {
      if (src_code == MEM)
	return dbl_p ? "fld\t%0,%1" : "flw\t%0,%1";
    }
  gcc_unreachable ();
}

/* Return true if CMP1 is a suitable second operand for integer ordering
   test CODE.  See also the *sCC patterns in mips.md.  */

static bool
mips_int_order_operand_ok_p (enum rtx_code code, rtx cmp1)
{
  switch (code)
    {
    case GT:
    case GTU:
      return reg_or_0_operand (cmp1, VOIDmode);

    case GE:
    case GEU:
      return cmp1 == const1_rtx;

    case LT:
    case LTU:
      return arith_operand (cmp1, VOIDmode);

    case LE:
      return sle_operand (cmp1, VOIDmode);

    case LEU:
      return sleu_operand (cmp1, VOIDmode);

    default:
      gcc_unreachable ();
    }
}

/* Return true if *CMP1 (of mode MODE) is a valid second operand for
   integer ordering test *CODE, or if an equivalent combination can
   be formed by adjusting *CODE and *CMP1.  When returning true, update
   *CODE and *CMP1 with the chosen code and operand, otherwise leave
   them alone.  */

static bool
mips_canonicalize_int_order_test (enum rtx_code *code, rtx *cmp1,
				  enum machine_mode mode)
{
  HOST_WIDE_INT plus_one;

  if (mips_int_order_operand_ok_p (*code, *cmp1))
    return true;

  if (CONST_INT_P (*cmp1))
    switch (*code)
      {
      case LE:
	plus_one = trunc_int_for_mode (UINTVAL (*cmp1) + 1, mode);
	if (INTVAL (*cmp1) < plus_one)
	  {
	    *code = LT;
	    *cmp1 = force_reg (mode, GEN_INT (plus_one));
	    return true;
	  }
	break;

      case LEU:
	plus_one = trunc_int_for_mode (UINTVAL (*cmp1) + 1, mode);
	if (plus_one != 0)
	  {
	    *code = LTU;
	    *cmp1 = force_reg (mode, GEN_INT (plus_one));
	    return true;
	  }
	break;

      default:
	break;
      }
  return false;
}

/* Compare CMP0 and CMP1 using ordering test CODE and store the result
   in TARGET.  CMP0 and TARGET are register_operands.  If INVERT_PTR
   is nonnull, it's OK to set TARGET to the inverse of the result and
   flip *INVERT_PTR instead.  */

static void
mips_emit_int_order_test (enum rtx_code code, bool *invert_ptr,
			  rtx target, rtx cmp0, rtx cmp1)
{
  enum machine_mode mode;

  /* First see if there is a MIPS instruction that can do this operation.
     If not, try doing the same for the inverse operation.  If that also
     fails, force CMP1 into a register and try again.  */
  mode = GET_MODE (cmp0);
  if (mips_canonicalize_int_order_test (&code, &cmp1, mode))
    mips_emit_binary (code, target, cmp0, cmp1);
  else
    {
      enum rtx_code inv_code = reverse_condition (code);
      if (!mips_canonicalize_int_order_test (&inv_code, &cmp1, mode))
	{
	  cmp1 = force_reg (mode, cmp1);
	  mips_emit_int_order_test (code, invert_ptr, target, cmp0, cmp1);
	}
      else if (invert_ptr == 0)
	{
	  rtx inv_target;

	  inv_target = mips_force_binary (GET_MODE (target),
					  inv_code, cmp0, cmp1);
	  mips_emit_binary (XOR, target, inv_target, const1_rtx);
	}
      else
	{
	  *invert_ptr = !*invert_ptr;
	  mips_emit_binary (inv_code, target, cmp0, cmp1);
	}
    }
}

/* Return a register that is zero iff CMP0 and CMP1 are equal.
   The register will have the same mode as CMP0.  */

static rtx
mips_zero_if_equal (rtx cmp0, rtx cmp1)
{
  if (cmp1 == const0_rtx)
    return cmp0;

  return expand_binop (GET_MODE (cmp0), sub_optab,
		       cmp0, cmp1, 0, 0, OPTAB_DIRECT);
}

/* Convert *CODE into a code that can be used in a floating-point
   scc instruction (C.cond.fmt).  Return true if the values of
   the condition code registers will be inverted, with 0 indicating
   that the condition holds.  */

static bool
mips_reversed_fp_cond (enum rtx_code *code)
{
  switch (*code)
    {
    case NE:
    case LTGT:
    case ORDERED:
      *code = reverse_condition_maybe_unordered (*code);
      return true;

    default:
      return false;
    }
}

/* Convert a comparison into something that can be used in a branch or
   conditional move.  On entry, *OP0 and *OP1 are the values being
   compared and *CODE is the code used to compare them.

   Update *CODE, *OP0 and *OP1 so that they describe the final comparison.
   If NEED_EQ_NE_P, then only EQ or NE comparisons against zero are possible,
   otherwise any standard branch condition can be used.  The standard branch
   conditions are:

      - EQ or NE between two registers.
      - any comparison between a register and zero.  */

static void
mips_emit_compare (enum rtx_code *code, rtx *op0, rtx *op1, bool need_eq_ne_p)
{
  rtx cmp_op0 = *op0;
  rtx cmp_op1 = *op1;

  if (GET_MODE_CLASS (GET_MODE (*op0)) == MODE_INT)
    {
      if (!need_eq_ne_p && *op1 == const0_rtx)
	;
      else if (*code == EQ || *code == NE)
	{
	  if (need_eq_ne_p)
	    {
	      *op0 = mips_zero_if_equal (cmp_op0, cmp_op1);
	      *op1 = const0_rtx;
	    }
	  else
	    *op1 = force_reg (GET_MODE (cmp_op0), cmp_op1);
	}
      else
	{
	  /* The comparison needs a separate scc instruction.  Store the
	     result of the scc in *OP0 and compare it against zero.  */
	  bool invert = false;
	  *op0 = gen_reg_rtx (GET_MODE (cmp_op0));
	  mips_emit_int_order_test (*code, &invert, *op0, cmp_op0, cmp_op1);
	  *code = (invert ? EQ : NE);
	  *op1 = const0_rtx;
	}
    }
  else
    {
      enum rtx_code cmp_code;

      /* Floating-point tests use a separate C.cond.fmt comparison to
	 set a condition code register.  The branch or conditional move
	 will then compare that register against zero.

	 Set CMP_CODE to the code of the comparison instruction and
	 *CODE to the code that the branch or move should use.  */
      cmp_code = *code;
      *code = mips_reversed_fp_cond (&cmp_code) ? EQ : NE;
      *op0 = gen_reg_rtx (SImode);
      *op1 = const0_rtx;
      mips_emit_binary (cmp_code, *op0, cmp_op0, cmp_op1);
    }
}

/* Try performing the comparison in OPERANDS[1], whose arms are OPERANDS[2]
   and OPERAND[3].  Store the result in OPERANDS[0].

   On 64-bit targets, the mode of the comparison and target will always be
   SImode, thus possibly narrower than that of the comparison's operands.  */

void
mips_expand_scc (rtx operands[])
{
  rtx target = operands[0];
  enum rtx_code code = GET_CODE (operands[1]);
  rtx op0 = operands[2];
  rtx op1 = operands[3];

  gcc_assert (GET_MODE_CLASS (GET_MODE (op0)) == MODE_INT);

  if (code == EQ || code == NE)
    {
      rtx zie = mips_zero_if_equal (op0, op1);
      mips_emit_binary (code, target, zie, const0_rtx);
    }
  else
    mips_emit_int_order_test (code, 0, target, op0, op1);
}

/* Compare OPERANDS[1] with OPERANDS[2] using comparison code
   CODE and jump to OPERANDS[3] if the condition holds.  */

void
mips_expand_conditional_branch (rtx *operands)
{
  enum rtx_code code = GET_CODE (operands[0]);
  rtx op0 = operands[1];
  rtx op1 = operands[2];
  rtx condition;

  mips_emit_compare (&code, &op0, &op1, false);
  condition = gen_rtx_fmt_ee (code, VOIDmode, op0, op1);
  emit_jump_insn (gen_condjump (condition, operands[3]));
}

/* Perform the comparison in OPERANDS[1].  Move OPERANDS[2] into OPERANDS[0]
   if the condition holds, otherwise move OPERANDS[3] into OPERANDS[0].  */

void
mips_expand_conditional_move (rtx *operands)
{
  rtx cond;
  enum rtx_code code = GET_CODE (operands[1]);
  rtx op0 = XEXP (operands[1], 0);
  rtx op1 = XEXP (operands[1], 1);

  mips_emit_compare (&code, &op0, &op1, true);
  cond = gen_rtx_fmt_ee (code, GET_MODE (op0), op0, op1);
  emit_insn (gen_rtx_SET (VOIDmode, operands[0],
			  gen_rtx_IF_THEN_ELSE (GET_MODE (operands[0]), cond,
						operands[2], operands[3])));
}

/* Initialize *CUM for a call to a function of type FNTYPE.  */

void
mips_init_cumulative_args (CUMULATIVE_ARGS *cum, tree fntype ATTRIBUTE_UNUSED)
{
  memset (cum, 0, sizeof (*cum));
}

/* Fill INFO with information about a single argument.  CUM is the
   cumulative state for earlier arguments.  MODE is the mode of this
   argument and TYPE is its type (if known).  NAMED is true if this
   is a named (fixed) argument rather than a variable one.  */

static void
mips_get_arg_info (struct mips_arg_info *info, const CUMULATIVE_ARGS *cum,
		   enum machine_mode mode, const_tree type, bool named)
{
  bool doubleword_aligned_p;
  unsigned int num_bytes, num_words, max_regs;

  /* Work out the size of the argument.  */
  num_bytes = type ? int_size_in_bytes (type) : GET_MODE_SIZE (mode);
  num_words = (num_bytes + UNITS_PER_WORD - 1) / UNITS_PER_WORD;

  /* Scalar, complex and vector floating-point types are passed in
     floating-point registers, as long as this is a named rather
     than a variable argument.  */
  info->fpr_p = (named
		 && (type == 0 || FLOAT_TYPE_P (type))
		 && (GET_MODE_CLASS (mode) == MODE_FLOAT
		     || GET_MODE_CLASS (mode) == MODE_COMPLEX_FLOAT
		     || GET_MODE_CLASS (mode) == MODE_VECTOR_FLOAT)
		 && GET_MODE_UNIT_SIZE (mode) <= UNITS_PER_FPVALUE);

  /* ??? According to the ABI documentation, the real and imaginary
     parts of complex floats should be passed in individual registers.
     The real and imaginary parts of stack arguments are supposed
     to be contiguous and there should be an extra word of padding
     at the end.

     This has two problems.  First, it makes it impossible to use a
     single "void *" va_list type, since register and stack arguments
     are passed differently.  (At the time of writing, MIPSpro cannot
     handle complex float varargs correctly.)  Second, it's unclear
     what should happen when there is only one register free.

     For now, we assume that named complex floats should go into FPRs
     if there are two FPRs free, otherwise they should be passed in the
     same way as a struct containing two floats.  */
  if (info->fpr_p
      && GET_MODE_CLASS (mode) == MODE_COMPLEX_FLOAT
      && GET_MODE_UNIT_SIZE (mode) < UNITS_PER_FPVALUE)
    {
      if (cum->num_gprs >= MAX_ARGS_IN_REGISTERS - 1)
        info->fpr_p = false;
      else
        num_words = 2;
    }

  /* See whether the argument has doubleword alignment.  */
  doubleword_aligned_p = (mips_function_arg_boundary (mode, type)
			  > BITS_PER_WORD);

  /* Set REG_OFFSET to the register count we're interested in.
     The EABI allocates the floating-point registers separately,
     but the other ABIs allocate them like integer registers.  */
  info->reg_offset = cum->num_gprs;

  /* Advance to an even register if the argument is doubleword-aligned.  */
  if (doubleword_aligned_p)
    info->reg_offset += info->reg_offset & 1;

  /* Work out the offset of a stack argument.  */
  info->stack_offset = cum->stack_words;
  if (doubleword_aligned_p)
    info->stack_offset += info->stack_offset & 1;

  max_regs = MAX_ARGS_IN_REGISTERS - info->reg_offset;

  /* Partition the argument between registers and stack.  */
  info->reg_words = MIN (num_words, max_regs);
  info->stack_words = num_words - info->reg_words;
}

/* INFO describes a register argument that has the normal format for the
   argument's mode.  Return the register it uses, assuming that FPRs are
   available if HARD_FLOAT_P.  */

static unsigned int
mips_arg_regno (const struct mips_arg_info *info, bool hard_float_p)
{
  if (!info->fpr_p || !hard_float_p)
    return GP_ARG_FIRST + info->reg_offset;
  else
    return FP_ARG_FIRST + info->reg_offset;
}

/* Implement TARGET_FUNCTION_ARG.  */

static rtx
mips_function_arg (CUMULATIVE_ARGS *cum, enum machine_mode mode,
		   const_tree type, bool named)
{
  struct mips_arg_info info;

  if (mode == VOIDmode)
    return NULL;

  mips_get_arg_info (&info, cum, mode, type, named);

  /* Return straight away if the whole argument is passed on the stack.  */
  if (info.reg_offset == MAX_ARGS_IN_REGISTERS)
    return NULL;

  /* The n32 and n64 ABIs say that if any 64-bit chunk of the structure
     contains a double in its entirety, then that 64-bit chunk is passed
     in a floating-point register.  */
  if (TARGET_HARD_FLOAT
      && named
      && type != 0
      && TREE_CODE (type) == RECORD_TYPE
      && TYPE_SIZE_UNIT (type)
      && host_integerp (TYPE_SIZE_UNIT (type), 1))
    {
      tree field;

      /* First check to see if there is any such field.  */
      for (field = TYPE_FIELDS (type); field; field = DECL_CHAIN (field))
	if (TREE_CODE (field) == FIELD_DECL
	    && SCALAR_FLOAT_TYPE_P (TREE_TYPE (field))
	    && TYPE_PRECISION (TREE_TYPE (field)) == BITS_PER_WORD
	    && host_integerp (bit_position (field), 0)
	    && int_bit_position (field) % BITS_PER_WORD == 0)
	  break;

      if (field != 0)
	{
	  /* Now handle the special case by returning a PARALLEL
	     indicating where each 64-bit chunk goes.  INFO.REG_WORDS
	     chunks are passed in registers.  */
	  unsigned int i;
	  HOST_WIDE_INT bitpos;
	  rtx ret;

	  /* assign_parms checks the mode of ENTRY_PARM, so we must
	     use the actual mode here.  */
	  ret = gen_rtx_PARALLEL (mode, rtvec_alloc (info.reg_words));

	  bitpos = 0;
	  field = TYPE_FIELDS (type);
	  for (i = 0; i < info.reg_words; i++)
	    {
	      rtx reg;

	      for (; field; field = DECL_CHAIN (field))
		if (TREE_CODE (field) == FIELD_DECL
		    && int_bit_position (field) >= bitpos)
		  break;

	      if (field
		  && int_bit_position (field) == bitpos
		  && SCALAR_FLOAT_TYPE_P (TREE_TYPE (field))
		  && TYPE_PRECISION (TREE_TYPE (field)) == BITS_PER_WORD)
		reg = gen_rtx_REG (DFmode, FP_ARG_FIRST + info.reg_offset + i);
	      else
		reg = gen_rtx_REG (DImode, GP_ARG_FIRST + info.reg_offset + i);

	      XVECEXP (ret, 0, i)
		= gen_rtx_EXPR_LIST (VOIDmode, reg,
				     GEN_INT (bitpos / BITS_PER_UNIT));

	      bitpos += BITS_PER_WORD;
	    }
	  return ret;
	}
    }

  /* Handle the n32/n64 conventions for passing complex floating-point
     arguments in FPR pairs.  The real part goes in the lower register
     and the imaginary part goes in the upper register.  */
  if (info.fpr_p
      && GET_MODE_CLASS (mode) == MODE_COMPLEX_FLOAT)
    {
      rtx real, imag;
      enum machine_mode inner;
      unsigned int regno;

      inner = GET_MODE_INNER (mode);
      regno = FP_ARG_FIRST + info.reg_offset;
      if (info.reg_words * UNITS_PER_WORD == GET_MODE_SIZE (inner))
	{
	  /* Real part in registers, imaginary part on stack.  */
	  gcc_assert (info.stack_words == info.reg_words);
	  return gen_rtx_REG (inner, regno);
	}
      else
	{
	  gcc_assert (info.stack_words == 0);
	  real = gen_rtx_EXPR_LIST (VOIDmode,
				    gen_rtx_REG (inner, regno),
				    const0_rtx);
	  imag = gen_rtx_EXPR_LIST (VOIDmode,
				    gen_rtx_REG (inner,
						 regno + info.reg_words / 2),
				    GEN_INT (GET_MODE_SIZE (inner)));
	  return gen_rtx_PARALLEL (mode, gen_rtvec (2, real, imag));
	}
    }

  return gen_rtx_REG (mode, mips_arg_regno (&info, TARGET_HARD_FLOAT));
}

/* Implement TARGET_FUNCTION_ARG_ADVANCE.  */

static void
mips_function_arg_advance (CUMULATIVE_ARGS *cum, enum machine_mode mode,
			   const_tree type, bool named)
{
  struct mips_arg_info info;

  mips_get_arg_info (&info, cum, mode, type, named);

  /* Advance the register count.  This has the effect of setting
     num_gprs to MAX_ARGS_IN_REGISTERS if a doubleword-aligned
     argument required us to skip the final GPR and pass the whole
     argument on the stack.  */
  cum->num_gprs = info.reg_offset + info.reg_words;

  /* Advance the stack word count.  */
  if (info.stack_words > 0)
    cum->stack_words = info.stack_offset + info.stack_words;
}

/* Implement TARGET_ARG_PARTIAL_BYTES.  */

static int
mips_arg_partial_bytes (CUMULATIVE_ARGS *cum,
			enum machine_mode mode, tree type, bool named)
{
  struct mips_arg_info info;

  mips_get_arg_info (&info, cum, mode, type, named);
  return info.stack_words > 0 ? info.reg_words * UNITS_PER_WORD : 0;
}

/* Implement TARGET_FUNCTION_ARG_BOUNDARY.  Every parameter gets at
   least PARM_BOUNDARY bits of alignment, but will be given anything up
   to STACK_BOUNDARY bits if the type requires it.  */

static unsigned int
mips_function_arg_boundary (enum machine_mode mode, const_tree type)
{
  unsigned int alignment;

  alignment = type ? TYPE_ALIGN (type) : GET_MODE_ALIGNMENT (mode);
  if (alignment < PARM_BOUNDARY)
    alignment = PARM_BOUNDARY;
  if (alignment > STACK_BOUNDARY)
    alignment = STACK_BOUNDARY;
  return alignment;
}

/* Return true if FUNCTION_ARG_PADDING (MODE, TYPE) should return
   upward rather than downward.  In other words, return true if the
   first byte of the stack slot has useful data, false if the last
   byte does.  */

bool
mips_pad_arg_upward (enum machine_mode mode, const_tree type)
{
  /* On little-endian targets, the first byte of every stack argument
     is passed in the first byte of the stack slot.  */
  if (!BYTES_BIG_ENDIAN)
    return true;

  /* Otherwise, integral types are padded downward: the last byte of a
     stack argument is passed in the last byte of the stack slot.  */
  if (type != 0
      ? (INTEGRAL_TYPE_P (type)
	 || POINTER_TYPE_P (type)
	 || FIXED_POINT_TYPE_P (type))
      : (SCALAR_INT_MODE_P (mode)
	 || ALL_SCALAR_FIXED_POINT_MODE_P (mode)))
    return false;

  return true;
}

/* Likewise BLOCK_REG_PADDING (MODE, TYPE, ...).  Return !BYTES_BIG_ENDIAN
   if the least significant byte of the register has useful data.  Return
   the opposite if the most significant byte does.  */

bool
mips_pad_reg_upward (enum machine_mode mode, tree type)
{
  /* No shifting is required for floating-point arguments.  */
  if (type != 0 ? FLOAT_TYPE_P (type) : GET_MODE_CLASS (mode) == MODE_FLOAT)
    return !BYTES_BIG_ENDIAN;

  /* Otherwise, apply the same padding to register arguments as we do
     to stack arguments.  */
  return mips_pad_arg_upward (mode, type);
}

/* See whether VALTYPE is a record whose fields should be returned in
   floating-point registers.  If so, return the number of fields and
   list them in FIELDS (which should have two elements).  Return 0
   otherwise.

   For n32 & n64, a structure with one or two fields is returned in
   floating-point registers as long as every field has a floating-point
   type.  */

static int
mips_fpr_return_fields (const_tree valtype, tree *fields)
{
  tree field;
  int i;

  if (TREE_CODE (valtype) != RECORD_TYPE)
    return 0;

  i = 0;
  for (field = TYPE_FIELDS (valtype); field != 0; field = DECL_CHAIN (field))
    {
      if (TREE_CODE (field) != FIELD_DECL)
	continue;

      if (!SCALAR_FLOAT_TYPE_P (TREE_TYPE (field)))
	return 0;

      if (i == 2)
	return 0;

      fields[i++] = field;
    }
  return i;
}

/* Implement TARGET_RETURN_IN_MSB.  For n32 & n64, we should return
   a value in the most significant part of $2/$3 if:

      - the target is big-endian;

      - the value has a structure or union type (we generalize this to
	cover aggregates from other languages too); and

      - the structure is not returned in floating-point registers.  */

static bool
mips_return_in_msb (const_tree valtype)
{
  tree fields[2];

  return (TARGET_BIG_ENDIAN
	  && AGGREGATE_TYPE_P (valtype)
	  && mips_fpr_return_fields (valtype, fields) == 0);
}

/* Return true if the function return value MODE will get returned in a
   floating-point register.  */

static bool
mips_return_mode_in_fpr_p (enum machine_mode mode)
{
  return ((GET_MODE_CLASS (mode) == MODE_FLOAT
	   || GET_MODE_CLASS (mode) == MODE_VECTOR_FLOAT
	   || GET_MODE_CLASS (mode) == MODE_COMPLEX_FLOAT)
	  && GET_MODE_UNIT_SIZE (mode) <= UNITS_PER_HWFPVALUE);
}

/* Return the representation of an FPR return register when the
   value being returned in FP_RETURN has mode VALUE_MODE and the
   return type itself has mode TYPE_MODE.  On NewABI targets,
   the two modes may be different for structures like:

       struct __attribute__((packed)) foo { float f; }

   where we return the SFmode value of "f" in FP_RETURN, but where
   the structure itself has mode BLKmode.  */

static rtx
mips_return_fpr_single (enum machine_mode type_mode,
			enum machine_mode value_mode)
{
  rtx x;

  x = gen_rtx_REG (value_mode, FP_RETURN);
  if (type_mode != value_mode)
    {
      x = gen_rtx_EXPR_LIST (VOIDmode, x, const0_rtx);
      x = gen_rtx_PARALLEL (type_mode, gen_rtvec (1, x));
    }
  return x;
}

/* Return a composite value in a pair of floating-point registers.
   MODE1 and OFFSET1 are the mode and byte offset for the first value,
   likewise MODE2 and OFFSET2 for the second.  MODE is the mode of the
   complete value.

   For n32 & n64, $f0 always holds the first value and $f2 the second.
   Otherwise the values are packed together as closely as possible.  */

static rtx
mips_return_fpr_pair (enum machine_mode mode,
		      enum machine_mode mode1, HOST_WIDE_INT offset1,
		      enum machine_mode mode2, HOST_WIDE_INT offset2)
{
  return gen_rtx_PARALLEL
    (mode,
     gen_rtvec (2,
		gen_rtx_EXPR_LIST (VOIDmode,
				   gen_rtx_REG (mode1, FP_RETURN),
				   GEN_INT (offset1)),
		gen_rtx_EXPR_LIST (VOIDmode,
				   gen_rtx_REG (mode2, FP_RETURN + 1),
				   GEN_INT (offset2))));

}

/* Implement FUNCTION_VALUE and LIBCALL_VALUE.  For normal calls,
   VALTYPE is the return type and MODE is VOIDmode.  For libcalls,
   VALTYPE is null and MODE is the mode of the return value.  */

rtx
mips_function_value (const_tree valtype, const_tree func, enum machine_mode mode)
{
  if (valtype)
    {
      tree fields[2];
      int unsigned_p;

      mode = TYPE_MODE (valtype);
      unsigned_p = TYPE_UNSIGNED (valtype);

      /* Since TARGET_PROMOTE_FUNCTION_MODE unconditionally promotes,
	 return values, promote the mode here too.  */
      mode = promote_function_mode (valtype, mode, &unsigned_p, func, 1);

      /* Handle structures whose fields are returned in $f0/$f2.  */
      switch (mips_fpr_return_fields (valtype, fields))
	{
	case 1:
	  return mips_return_fpr_single (mode,
					 TYPE_MODE (TREE_TYPE (fields[0])));

	case 2:
	  return mips_return_fpr_pair (mode,
				       TYPE_MODE (TREE_TYPE (fields[0])),
				       int_byte_position (fields[0]),
				       TYPE_MODE (TREE_TYPE (fields[1])),
				       int_byte_position (fields[1]));
	}

      /* If a value is passed in the most significant part of a register, see
	 whether we have to round the mode up to a whole number of words.  */
      if (mips_return_in_msb (valtype))
	{
	  HOST_WIDE_INT size = int_size_in_bytes (valtype);
	  if (size % UNITS_PER_WORD != 0)
	    {
	      size += UNITS_PER_WORD - size % UNITS_PER_WORD;
	      mode = mode_for_size (size * BITS_PER_UNIT, MODE_INT, 0);
	    }
	}

      /* Only use FPRs for scalar, complex or vector types.  */
      if (!FLOAT_TYPE_P (valtype))
	return gen_rtx_REG (mode, GP_RETURN);
    }

  /* Handle long doubles for n32 & n64.  */
  if (mode == TFmode)
    return mips_return_fpr_pair (mode,
    			     DImode, 0,
    			     DImode, GET_MODE_SIZE (mode) / 2);

  if (mips_return_mode_in_fpr_p (mode))
    {
      if (GET_MODE_CLASS (mode) == MODE_COMPLEX_FLOAT)
        return mips_return_fpr_pair (mode,
    				 GET_MODE_INNER (mode), 0,
    				 GET_MODE_INNER (mode),
    				 GET_MODE_SIZE (mode) / 2);
      else
        return gen_rtx_REG (mode, FP_RETURN);
    }

  return gen_rtx_REG (mode, GP_RETURN);
}

/* Implement TARGET_RETURN_IN_MEMORY.  Scalars and small structures
   that fit in two registers are returned in v0/v1. */

static bool
mips_return_in_memory (const_tree type, const_tree fndecl ATTRIBUTE_UNUSED)
{
  return !IN_RANGE (int_size_in_bytes (type), 0, 2 * UNITS_PER_WORD);
}

/* Implement TARGET_SETUP_INCOMING_VARARGS.  */

static void
mips_setup_incoming_varargs (CUMULATIVE_ARGS *cum, enum machine_mode mode,
			     tree type, int *pretend_size ATTRIBUTE_UNUSED,
			     int no_rtl)
{
  CUMULATIVE_ARGS local_cum;
  int gp_saved;

  /* The caller has advanced CUM up to, but not beyond, the last named
     argument.  Advance a local copy of CUM past the last "real" named
     argument, to find out how many registers are left over.  */
  local_cum = *cum;
  mips_function_arg_advance (&local_cum, mode, type, true);

  /* Found out how many registers we need to save.  */
  gp_saved = MAX_ARGS_IN_REGISTERS - local_cum.num_gprs;

  if (!no_rtl && gp_saved > 0)
    {
      rtx ptr, mem;

      ptr = plus_constant (virtual_incoming_args_rtx,
			   REG_PARM_STACK_SPACE (cfun->decl)
			   - gp_saved * UNITS_PER_WORD);
      mem = gen_frame_mem (BLKmode, ptr);
      set_mem_alias_set (mem, get_varargs_alias_set ());

      move_block_from_reg (local_cum.num_gprs + GP_ARG_FIRST,
			   mem, gp_saved);
    }
  if (REG_PARM_STACK_SPACE (cfun->decl) == 0)
    cfun->machine->varargs_size = gp_saved * UNITS_PER_WORD;
}

/* Implement TARGET_EXPAND_BUILTIN_VA_START.  */

static void
mips_va_start (tree valist, rtx nextarg)
{
  nextarg = plus_constant (nextarg, -cfun->machine->varargs_size);
  std_expand_builtin_va_start (valist, nextarg);
}

/* Start a definition of function NAME. */

static void
mips_start_function_definition (const char *name)
{
  if (!flag_inhibit_size_directive)
    {
      fputs ("\t.ent\t", asm_out_file);
      assemble_name (asm_out_file, name);
      fputs ("\n", asm_out_file);
    }

  ASM_OUTPUT_TYPE_DIRECTIVE (asm_out_file, name, "function");

  /* Start the definition proper.  */
  assemble_name (asm_out_file, name);
  fputs (":\n", asm_out_file);
}

/* End a function definition started by mips_start_function_definition.  */

static void
mips_end_function_definition (const char *name)
{
  if (!flag_inhibit_size_directive)
    {
      fputs ("\t.end\t", asm_out_file);
      assemble_name (asm_out_file, name);
      fputs ("\n", asm_out_file);
    }
}

/* Return true if SYMBOL_REF X binds locally.  */

static bool
mips_symbol_binds_local_p (const_rtx x)
{
  return (SYMBOL_REF_DECL (x)
	  ? targetm.binds_local_p (SYMBOL_REF_DECL (x))
	  : SYMBOL_REF_LOCAL_P (x));
}

/* Return true if calls to X can use R_MIPS_CALL* relocations.  */

static bool
mips_ok_for_lazy_binding_p (rtx x)
{
  return (TARGET_USE_GOT
	  && GET_CODE (x) == SYMBOL_REF
	  && !SYMBOL_REF_BIND_NOW_P (x)
	  && !mips_symbol_binds_local_p (x));
}

/* Load function address ADDR into register DEST.  TYPE is as for
   mips_expand_call.  Return true if we used an explicit lazy-binding
   sequence.  */

static bool
mips_load_call_address (enum mips_call_type type, rtx dest, rtx addr)
{
  /* If we're generating PIC, and this call is to a global function,
     try to allow its address to be resolved lazily.  This isn't
     possible for sibcalls when $gp is call-saved because the value
     of $gp on entry to the stub would be our caller's gp, not ours.  */
  if (type != MIPS_CALL_SIBCALL && mips_ok_for_lazy_binding_p (addr))
    {
      addr = mips_got_load (dest, addr, SYMBOL_GOTOFF_CALL);
      emit_insn (gen_rtx_SET (VOIDmode, dest, addr));
      return true;
    }
  else
    {
      mips_emit_move (dest, addr);
      return false;
    }
}

/* Expand a call of type TYPE.  RESULT is where the result will go (null
   for "call"s and "sibcall"s), ADDR is the address of the function,
   ARGS_SIZE is the size of the arguments and AUX is the value passed
   to us by mips_function_arg.  LAZY_P is true if this call already
   involves a lazily-bound function address (such as when calling
   functions through a MIPS16 hard-float stub).

   Return the call itself.  */

rtx
mips_expand_call (enum mips_call_type type, rtx result, rtx addr,
		  rtx args_size, rtx aux, bool lazy_p)
{
  rtx orig_addr, pattern;

  orig_addr = addr;
  if (!call_insn_operand (addr, VOIDmode))
    {
      if (type == MIPS_CALL_EPILOGUE)
	addr = MIPS_EPILOGUE_TEMP (Pmode);
      else
	addr = gen_reg_rtx (Pmode);
      lazy_p |= mips_load_call_address (type, addr, orig_addr);
    }

  if (result == 0)
    {
      rtx (*fn) (rtx, rtx);

      if (type == MIPS_CALL_SIBCALL)
	fn = gen_sibcall_internal;
      else
	fn = gen_call_internal;

      pattern = fn (addr, args_size);
    }
  else if (GET_CODE (result) == PARALLEL && XVECLEN (result, 0) == 2)
    {
      /* Handle return values created by mips_return_fpr_pair.  */
      rtx (*fn) (rtx, rtx, rtx, rtx);
      rtx reg1, reg2;

      if (type == MIPS_CALL_SIBCALL)
	fn = gen_sibcall_value_multiple_internal;
      else
	fn = gen_call_value_multiple_internal;

      reg1 = XEXP (XVECEXP (result, 0, 0), 0);
      reg2 = XEXP (XVECEXP (result, 0, 1), 0);
      pattern = fn (reg1, addr, args_size, reg2);
    }
  else
    {
      rtx (*fn) (rtx, rtx, rtx);

      if (type == MIPS_CALL_SIBCALL)
	fn = gen_sibcall_value_internal;
      else
	fn = gen_call_value_internal;

      /* Handle return values created by mips_return_fpr_single.  */
      if (GET_CODE (result) == PARALLEL && XVECLEN (result, 0) == 1)
	result = XEXP (XVECEXP (result, 0, 0), 0);
      pattern = fn (result, addr, args_size);
    }

  return mips_emit_call_insn (pattern, lazy_p);
}

/* Emit straight-line code to move LENGTH bytes from SRC to DEST.
   Assume that the areas do not overlap.  */

static void
mips_block_move_straight (rtx dest, rtx src, HOST_WIDE_INT length)
{
  HOST_WIDE_INT offset, delta;
  unsigned HOST_WIDE_INT bits;
  int i;
  enum machine_mode mode;
  rtx *regs;

  bits = MAX( BITS_PER_UNIT,
             MIN( BITS_PER_WORD, MIN( MEM_ALIGN(src),MEM_ALIGN(dest) ) ) );

  mode = mode_for_size (bits, MODE_INT, 0);
  delta = bits / BITS_PER_UNIT;

  /* Allocate a buffer for the temporary registers.  */
  regs = XALLOCAVEC (rtx, length / delta);

  /* Load as many BITS-sized chunks as possible.  Use a normal load if
     the source has enough alignment, otherwise use left/right pairs.  */
  for (offset = 0, i = 0; offset + delta <= length; offset += delta, i++)
    {
      regs[i] = gen_reg_rtx (mode);
	mips_emit_move (regs[i], adjust_address (src, mode, offset));
    }

  /* Copy the chunks to the destination.  */
  for (offset = 0, i = 0; offset + delta <= length; offset += delta, i++)
      mips_emit_move (adjust_address (dest, mode, offset), regs[i]);

  /* Mop up any left-over bytes.  */
  if (offset < length)
    {
      src = adjust_address (src, BLKmode, offset);
      dest = adjust_address (dest, BLKmode, offset);
      move_by_pieces (dest, src, length - offset,
		      MIN (MEM_ALIGN (src), MEM_ALIGN (dest)), 0);
    }
}

/* Helper function for doing a loop-based block operation on memory
   reference MEM.  Each iteration of the loop will operate on LENGTH
   bytes of MEM.

   Create a new base register for use within the loop and point it to
   the start of MEM.  Create a new memory reference that uses this
   register.  Store them in *LOOP_REG and *LOOP_MEM respectively.  */

static void
mips_adjust_block_mem (rtx mem, HOST_WIDE_INT length,
		       rtx *loop_reg, rtx *loop_mem)
{
  *loop_reg = copy_addr_to_reg (XEXP (mem, 0));

  /* Although the new mem does not refer to a known location,
     it does keep up to LENGTH bytes of alignment.  */
  *loop_mem = change_address (mem, BLKmode, *loop_reg);
  set_mem_align (*loop_mem, MIN (MEM_ALIGN (mem), length * BITS_PER_UNIT));
}

/* Move LENGTH bytes from SRC to DEST using a loop that moves BYTES_PER_ITER
   bytes at a time.  LENGTH must be at least BYTES_PER_ITER.  Assume that
   the memory regions do not overlap.  */

static void
mips_block_move_loop (rtx dest, rtx src, HOST_WIDE_INT length,
		      HOST_WIDE_INT bytes_per_iter)
{
  rtx label, src_reg, dest_reg, final_src, test;
  HOST_WIDE_INT leftover;

  leftover = length % bytes_per_iter;
  length -= leftover;

  /* Create registers and memory references for use within the loop.  */
  mips_adjust_block_mem (src, bytes_per_iter, &src_reg, &src);
  mips_adjust_block_mem (dest, bytes_per_iter, &dest_reg, &dest);

  /* Calculate the value that SRC_REG should have after the last iteration
     of the loop.  */
  final_src = expand_simple_binop (Pmode, PLUS, src_reg, GEN_INT (length),
				   0, 0, OPTAB_WIDEN);

  /* Emit the start of the loop.  */
  label = gen_label_rtx ();
  emit_label (label);

  /* Emit the loop body.  */
  mips_block_move_straight (dest, src, bytes_per_iter);

  /* Move on to the next block.  */
  mips_emit_move (src_reg, plus_constant (src_reg, bytes_per_iter));
  mips_emit_move (dest_reg, plus_constant (dest_reg, bytes_per_iter));

  /* Emit the loop condition.  */
  test = gen_rtx_NE (VOIDmode, src_reg, final_src);
  if (Pmode == DImode)
    emit_jump_insn (gen_cbranchdi4 (test, src_reg, final_src, label));
  else
    emit_jump_insn (gen_cbranchsi4 (test, src_reg, final_src, label));

  /* Mop up any left-over bytes.  */
  if (leftover)
    mips_block_move_straight (dest, src, leftover);
}

/* Expand a movmemsi instruction, which copies LENGTH bytes from
   memory reference SRC to memory reference DEST.  */

bool
mips_expand_block_move (rtx dest, rtx src, rtx length)
{
  if (CONST_INT_P (length))
    {
      HOST_WIDE_INT factor, align;
      
      align = MIN (MIN (MEM_ALIGN (src), MEM_ALIGN (dest)), BITS_PER_WORD);
      factor = BITS_PER_WORD / align;

      if (INTVAL (length) <= MIPS_MAX_MOVE_BYTES_STRAIGHT / factor)
	{
	  mips_block_move_straight (dest, src, INTVAL (length));
	  return true;
	}
      else if (optimize && align >= BITS_PER_WORD)
	{
	  mips_block_move_loop (dest, src, INTVAL (length),
				MIPS_MAX_MOVE_BYTES_PER_LOOP_ITER / factor);
	  return true;
	}
    }
  return false;
}

/* Return true if X is a MEM with the same size as MODE.  */

bool
mips_mem_fits_mode_p (enum machine_mode mode, rtx x)
{
  rtx size;

  if (!MEM_P (x))
    return false;

  size = MEM_SIZE (x);
  return size && INTVAL (size) == GET_MODE_SIZE (mode);
}

/* (Re-)Initialize mips_split_p, mips_lo_relocs and mips_hi_relocs.  */

static void
mips_init_relocs (void)
{
  memset (mips_split_p, '\0', sizeof (mips_split_p));
  memset (mips_hi_relocs, '\0', sizeof (mips_hi_relocs));
  memset (mips_lo_relocs, '\0', sizeof (mips_lo_relocs));

  mips_split_p[SYMBOL_ABSOLUTE] = true;
  mips_hi_relocs[SYMBOL_ABSOLUTE] = "%hi(";
  mips_lo_relocs[SYMBOL_ABSOLUTE] = "%lo(";

  mips_lo_relocs[SYMBOL_32_HIGH] = "%hi(";

  if (TARGET_XGOT)
    {
      /* The HIGH and LO_SUM are matched by special .md patterns.  */
      mips_split_p[SYMBOL_GOT_DISP] = true;

      mips_split_p[SYMBOL_GOTOFF_DISP] = true;
      mips_hi_relocs[SYMBOL_GOTOFF_DISP] = "%got_hi(";
      mips_lo_relocs[SYMBOL_GOTOFF_DISP] = "%got_lo(";

      mips_split_p[SYMBOL_GOTOFF_CALL] = true;
      mips_hi_relocs[SYMBOL_GOTOFF_CALL] = "%call_hi(";
      mips_lo_relocs[SYMBOL_GOTOFF_CALL] = "%call_lo(";

      mips_split_p[SYMBOL_GOTTPREL] = true;
      mips_hi_relocs[SYMBOL_GOTTPREL] = "%gottp_hi(";
      mips_lo_relocs[SYMBOL_GOTTPREL] = "%gottp_lo(";

      mips_split_p[SYMBOL_TLSGD] = true;
      mips_hi_relocs[SYMBOL_TLSGD] = "%tlsgd_hi(";
      mips_lo_relocs[SYMBOL_TLSGD] = "%tlsgd_lo(";

      mips_split_p[SYMBOL_TLSLDM] = true;
      mips_hi_relocs[SYMBOL_TLSLDM] = "%tlsldm_hi(";
      mips_lo_relocs[SYMBOL_TLSLDM] = "%tlsldm_lo(";
    }
  else
    {
      mips_lo_relocs[SYMBOL_GOTOFF_DISP] = "%got_disp(";
      mips_lo_relocs[SYMBOL_GOTOFF_CALL] = "%call16(";
      mips_lo_relocs[SYMBOL_GOTTPREL] = "%gottprel(";
      mips_lo_relocs[SYMBOL_TLSGD] = "%tlsgd(";
      mips_lo_relocs[SYMBOL_TLSLDM] = "%tlsldm(";
    }

  mips_split_p[SYMBOL_GOTOFF_LOADGP] = true;
  mips_hi_relocs[SYMBOL_GOTOFF_LOADGP] = "%hi(%neg(%gp_rel(";
  mips_lo_relocs[SYMBOL_GOTOFF_LOADGP] = "%lo(%neg(%gp_rel(";

  mips_split_p[SYMBOL_DTPREL] = true;
  mips_hi_relocs[SYMBOL_DTPREL] = "%dtprel_hi(";
  mips_lo_relocs[SYMBOL_DTPREL] = "%dtprel_lo(";

  mips_split_p[SYMBOL_TPREL] = true;
  mips_hi_relocs[SYMBOL_TPREL] = "%tprel_hi(";
  mips_lo_relocs[SYMBOL_TPREL] = "%tprel_lo(";
}

/* Print symbolic operand OP, which is part of a HIGH or LO_SUM
   in context CONTEXT.  RELOCS is the array of relocations to use.  */

static void
mips_print_operand_reloc (FILE *file, rtx op, const char **relocs)
{
  enum mips_symbol_type symbol_type;
  const char *p;

  symbol_type = mips_classify_symbolic_expression (op);
  gcc_assert (relocs[symbol_type]);

  fputs (relocs[symbol_type], file);
  output_addr_const (file, mips_strip_unspec_address (op));
  for (p = relocs[symbol_type]; *p != 0; p++)
    if (*p == '(')
      fputc (')', file);
}

/* PRINT_OPERAND prefix LETTER refers to the integer branch instruction
   associated with condition CODE.  Print the condition part of the
   opcode to FILE.  */

static void
mips_print_int_branch_condition (FILE *file, enum rtx_code code, int letter)
{
  switch (code)
    {
    case EQ:
    case NE:
    case GT:
    case GE:
    case LT:
    case LE:
    case GTU:
    case GEU:
    case LTU:
    case LEU:
      /* Conveniently, the MIPS names for these conditions are the same
	 as their RTL equivalents.  */
      fputs (GET_RTX_NAME (code), file);
      break;

    default:
      output_operand_lossage ("'%%%c' is not a valid operand prefix", letter);
      break;
    }
}

/* Implement TARGET_PRINT_OPERAND.  The MIPS-specific operand codes are:

   'X'	Print CONST_INT OP in hexadecimal format.
   'x'	Print the low 16 bits of CONST_INT OP in hexadecimal format.
   'd'	Print CONST_INT OP in decimal.
   'm'	Print one less than CONST_INT OP in decimal.
   'h'	Print the high-part relocation associated with OP, after stripping
	  any outermost HIGH.
   'R'	Print the low-part relocation associated with OP.
   'C'	Print the integer branch condition for comparison OP.
   'N'	Print the inverse of the integer branch condition for comparison OP.
   'T'	Print 'f' for (eq:CC ...), 't' for (ne:CC ...),
	      'z' for (eq:?I ...), 'n' for (ne:?I ...).
   't'	Like 'T', but with the EQ/NE cases reversed
   'Y'	Print mips_fp_conditions[INTVAL (OP)]
   'Z'	Print OP and a comma for ISA_HAS_8CC, otherwise print nothing.
   'D'	Print the second part of a double-word register or memory operand.
   'L'	Print the low-order register in a double-word register operand.
   'M'	Print high-order register in a double-word register operand.
   'z'	Print $0 if OP is zero, otherwise print OP normally.  */

static void
mips_print_operand (FILE *file, rtx op, int letter)
{
  enum rtx_code code;

  gcc_assert (op);
  code = GET_CODE (op);

  switch (letter)
    {
    case 'X':
      if (CONST_INT_P (op))
	fprintf (file, HOST_WIDE_INT_PRINT_HEX, INTVAL (op));
      else
	output_operand_lossage ("invalid use of '%%%c'", letter);
      break;

    case 'x':
      if (CONST_INT_P (op))
	fprintf (file, HOST_WIDE_INT_PRINT_HEX, INTVAL (op) & 0xffff);
      else
	output_operand_lossage ("invalid use of '%%%c'", letter);
      break;

    case 'd':
      if (CONST_INT_P (op))
	fprintf (file, HOST_WIDE_INT_PRINT_DEC, INTVAL (op));
      else
	output_operand_lossage ("invalid use of '%%%c'", letter);
      break;

    case 'm':
      if (CONST_INT_P (op))
	fprintf (file, HOST_WIDE_INT_PRINT_DEC, INTVAL (op) - 1);
      else
	output_operand_lossage ("invalid use of '%%%c'", letter);
      break;

    case 'h':
      if (code == HIGH)
	op = XEXP (op, 0);
      mips_print_operand_reloc (file, op, mips_hi_relocs);
      break;

    case 'R':
      mips_print_operand_reloc (file, op, mips_lo_relocs);
      break;

    case 'C':
      mips_print_int_branch_condition (file, code, letter);
      break;

    case 'N':
      mips_print_int_branch_condition (file, reverse_condition (code), letter);
      break;

    case 'S':
      mips_print_int_branch_condition (file, swap_condition (code), letter);
      break;

    case 'T':
    case 't':
      {
	int truth = (code == NE) == (letter == 'T');
	fputc ("zfnt"[truth * 2 + (GET_MODE (op) == CCmode)], file);
      }
      break;

    case 'Y':
      if (code == CONST_INT && UINTVAL (op) < ARRAY_SIZE (mips_fp_conditions))
	fputs (mips_fp_conditions[UINTVAL (op)], file);
      else
	output_operand_lossage ("'%%%c' is not a valid operand prefix",
				letter);
      break;

    case 'Z':
      mips_print_operand (file, op, 0);
      fputc (',', file);
      break;

    default:
      switch (code)
	{
	case REG:
	  {
	    unsigned int regno = REGNO (op);
	    if ((letter == 'M' && TARGET_LITTLE_ENDIAN)
		|| (letter == 'L' && TARGET_BIG_ENDIAN)
		|| letter == 'D')
	      regno++;
	    else if (letter && letter != 'z' && letter != 'M' && letter != 'L')
	      output_operand_lossage ("invalid use of '%%%c'", letter);
	    fprintf (file, "%s", reg_names[regno]);
	  }
	  break;

	case MEM:
	  if (letter == 'D')
	    output_address (plus_constant (XEXP (op, 0), 4));
	  else if (letter && letter != 'z')
	    output_operand_lossage ("invalid use of '%%%c'", letter);
	  else
	    output_address (XEXP (op, 0));
	  break;

	default:
	  if (letter == 'z' && op == CONST0_RTX (GET_MODE (op)))
	    fputs (reg_names[GP_REG_FIRST], file);
	  else if (letter && letter != 'z')
	    output_operand_lossage ("invalid use of '%%%c'", letter);
	  else
	    output_addr_const (file, mips_strip_unspec_address (op));
	  break;
	}
    }
}

/* Implement TARGET_PRINT_OPERAND_ADDRESS.  */

static void
mips_print_operand_address (FILE *file, rtx x)
{
  struct mips_address_info addr;

  if (mips_classify_address (&addr, x, word_mode, true))
    switch (addr.type)
      {
      case ADDRESS_REG:
	mips_print_operand (file, addr.offset, 0);
	fprintf (file, "(%s)", reg_names[REGNO (addr.reg)]);
	return;

      case ADDRESS_LO_SUM:
	mips_print_operand_reloc (file, addr.offset, mips_lo_relocs);
	fprintf (file, "(%s)", reg_names[REGNO (addr.reg)]);
	return;

      case ADDRESS_CONST_INT:
	output_addr_const (file, x);
	fprintf (file, "(%s)", reg_names[GP_REG_FIRST]);
	return;

      case ADDRESS_SYMBOLIC:
	output_addr_const (file, mips_strip_unspec_address (x));
	return;
      }
  gcc_unreachable ();
}

/* Implement TARGET_ENCODE_SECTION_INFO.  */

static void
mips_encode_section_info (tree decl, rtx rtl, int first)
{
  default_encode_section_info (decl, rtl, first);

  if (TREE_CODE (decl) == FUNCTION_DECL)
    {
      rtx symbol = XEXP (rtl, 0);
      tree type = TREE_TYPE (decl);

      /* Encode whether the symbol is short or long.  */
      if ((TARGET_LONG_CALLS && !mips_near_type_p (type))
	  || mips_far_type_p (type))
	SYMBOL_REF_FLAGS (symbol) |= SYMBOL_FLAG_LONG_CALL;
    }
}

/* The MIPS debug format wants all automatic variables and arguments
   to be in terms of the virtual frame pointer (stack pointer before
   any adjustment in the function), while the MIPS 3.0 linker wants
   the frame pointer to be the stack pointer after the initial
   adjustment.  So, we do the adjustment here.  The arg pointer (which
   is eliminated) points to the virtual frame pointer, while the frame
   pointer (which may be eliminated) points to the stack pointer after
   the initial adjustments.  */

HOST_WIDE_INT
mips_debugger_offset (rtx addr, HOST_WIDE_INT offset)
{
  rtx offset2 = const0_rtx;
  rtx reg = eliminate_constant_term (addr, &offset2);

  gcc_assert (reg == stack_pointer_rtx
              || reg == frame_pointer_rtx
              || reg == hard_frame_pointer_rtx);

  if (offset == 0)
    offset = INTVAL (offset2);
  
  offset -= cfun->machine->frame.total_size;

  return offset;
}

/* Implement TARGET_ASM_OUTPUT_SOURCE_FILENAME.  */

static void
mips_output_filename (FILE *stream, const char *name)
{
  /* If we are emitting DWARF-2, let dwarf2out handle the ".file"
     directives.  */
  if (write_symbols == DWARF2_DEBUG)
    return;
  else if (mips_output_filename_first_time)
    {
      mips_output_filename_first_time = 0;
      num_source_filenames += 1;
      current_function_file = name;
      fprintf (stream, "\t.file\t%d ", num_source_filenames);
      output_quoted_string (stream, name);
      putc ('\n', stream);
    }
  /* If we are emitting stabs, let dbxout.c handle this (except for
     the mips_output_filename_first_time case).  */
  else if (write_symbols == DBX_DEBUG)
    return;
  else if (name != current_function_file
	   && strcmp (name, current_function_file) != 0)
    {
      num_source_filenames += 1;
      current_function_file = name;
      fprintf (stream, "\t.file\t%d ", num_source_filenames);
      output_quoted_string (stream, name);
      putc ('\n', stream);
    }
}

/* Implement TARGET_ASM_OUTPUT_DWARF_DTPREL.  */

static void ATTRIBUTE_UNUSED
mips_output_dwarf_dtprel (FILE *file, int size, rtx x)
{
  switch (size)
    {
    case 4:
      fputs ("\t.dtprelword\t", file);
      break;

    case 8:
      fputs ("\t.dtpreldword\t", file);
      break;

    default:
      gcc_unreachable ();
    }
  output_addr_const (file, x);
  fputs ("+0x8000", file);
}

/* Implement ASM_OUTPUT_ASCII.  */

void
mips_output_ascii (FILE *stream, const char *string, size_t len)
{
  size_t i;
  int cur_pos;

  cur_pos = 17;
  fprintf (stream, "\t.ascii\t\"");
  for (i = 0; i < len; i++)
    {
      int c;

      c = (unsigned char) string[i];
      if (ISPRINT (c))
	{
	  if (c == '\\' || c == '\"')
	    {
	      putc ('\\', stream);
	      cur_pos++;
	    }
	  putc (c, stream);
	  cur_pos++;
	}
      else
	{
	  fprintf (stream, "\\%03o", c);
	  cur_pos += 4;
	}

      if (cur_pos > 72 && i+1 < len)
	{
	  cur_pos = 17;
	  fprintf (stream, "\"\n\t.ascii\t\"");
	}
    }
  fprintf (stream, "\"\n");
}

/* Emit either a label, .comm, or .lcomm directive.  When using assembler
   macros, mark the symbol as written so that mips_asm_output_external
   won't emit an .extern for it.  STREAM is the output file, NAME is the
   name of the symbol, INIT_STRING is the string that should be written
   before the symbol and FINAL_STRING is the string that should be
   written after it.  FINAL_STRING is a printf format that consumes the
   remaining arguments.  */

void
mips_declare_object (FILE *stream, const char *name, const char *init_string,
		     const char *final_string, ...)
{
  va_list ap;

  fputs (init_string, stream);
  assemble_name (stream, name);
  va_start (ap, final_string);
  vfprintf (stream, final_string, ap);
  va_end (ap);
}

/* Declare a common object of SIZE bytes using asm directive INIT_STRING.
   NAME is the name of the object and ALIGN is the required alignment
   in bytes.  TAKES_ALIGNMENT_P is true if the directive takes a third
   alignment argument.  */

void
mips_declare_common_object (FILE *stream, const char *name,
			    const char *init_string,
			    unsigned HOST_WIDE_INT size,
			    unsigned int align, bool takes_alignment_p)
{
  if (!takes_alignment_p)
    {
      size += (align / BITS_PER_UNIT) - 1;
      size -= size % (align / BITS_PER_UNIT);
      mips_declare_object (stream, name, init_string,
			   "," HOST_WIDE_INT_PRINT_UNSIGNED "\n", size);
    }
  else
    mips_declare_object (stream, name, init_string,
			 "," HOST_WIDE_INT_PRINT_UNSIGNED ",%u\n",
			 size, align / BITS_PER_UNIT);
}

/* Implement ASM_OUTPUT_ALIGNED_DECL_COMMON.  This is usually the same as the
   elfos.h version, but we also need to handle -muninit-const-in-rodata.  */

void
mips_output_aligned_decl_common (FILE *stream, tree decl ATTRIBUTE_UNUSED,
				 const char *name,
				 unsigned HOST_WIDE_INT size,
				 unsigned int align)
{
  mips_declare_common_object (stream, name, "\n\t.comm\t", size, align, true);
}

#ifdef ASM_OUTPUT_SIZE_DIRECTIVE
extern int size_directive_output;

/* Implement ASM_DECLARE_OBJECT_NAME.  This is like most of the standard ELF
   definitions except that it uses mips_declare_object to emit the label.  */

void
mips_declare_object_name (FILE *stream, const char *name,
			  tree decl ATTRIBUTE_UNUSED)
{
#ifdef ASM_OUTPUT_TYPE_DIRECTIVE
  ASM_OUTPUT_TYPE_DIRECTIVE (stream, name, "object");
#endif

  size_directive_output = 0;
  if (!flag_inhibit_size_directive && DECL_SIZE (decl))
    {
      HOST_WIDE_INT size;

      size_directive_output = 1;
      size = int_size_in_bytes (TREE_TYPE (decl));
      ASM_OUTPUT_SIZE_DIRECTIVE (stream, name, size);
    }

  mips_declare_object (stream, name, "", ":\n");
}

/* Implement ASM_FINISH_DECLARE_OBJECT.  This is generic ELF stuff.  */

void
mips_finish_declare_object (FILE *stream, tree decl, int top_level, int at_end)
{
  const char *name;

  name = XSTR (XEXP (DECL_RTL (decl), 0), 0);
  if (!flag_inhibit_size_directive
      && DECL_SIZE (decl) != 0
      && !at_end
      && top_level
      && DECL_INITIAL (decl) == error_mark_node
      && !size_directive_output)
    {
      HOST_WIDE_INT size;

      size_directive_output = 1;
      size = int_size_in_bytes (TREE_TYPE (decl));
      ASM_OUTPUT_SIZE_DIRECTIVE (stream, name, size);
    }
}
#endif

/* Return the FOO in the name of the ".mdebug.FOO" section associated
   with the current ABI.  */

static const char *
mips_mdebug_abi_name (void)
{
  switch (mips_abi)
    {
    case ABI_32:
      return "abi32";
    case ABI_64:
      return "abi64";
    default:
      gcc_unreachable ();
    }
}

/* Implement TARGET_ASM_FILE_START.  */

static void
mips_file_start (void)
{
  default_file_start ();

  /* Generate a special section to describe the ABI switches used to
     produce the resultant binary.  This is unnecessary on IRIX and
     causes unwanted warnings from the native linker.  */
  if (!TARGET_IRIX6)
    {
      /* Record the ABI itself.  Modern versions of binutils encode
	 this information in the ELF header flags, but GDB needs the
	 information in order to correctly debug binaries produced by
	 older binutils.  See the function mips_gdbarch_init in
	 gdb/mips-tdep.c.  */
      fprintf (asm_out_file, "\t.section .mdebug.%s\n\t.previous\n",
	       mips_mdebug_abi_name ());

#ifdef HAVE_AS_GNU_ATTRIBUTE
      {
	int attr;

	/* Soft-float code, -msoft-float.  */
	if (!TARGET_HARD_FLOAT_ABI)
	  attr = 3;
	/* 64-bit FP registers on a 32-bit target. */
	else if (!TARGET_64BIT)
	  attr = 4;
	/* Regular FP code, FP regs same size as GP regs, -mdouble-float.  */
	else
	  attr = 1;

	fprintf (asm_out_file, "\t.gnu_attribute 4, %d\n", attr);
      }
#endif
    }

  /* If TARGET_ABICALLS, tell GAS to generate -KPIC code.  */
  if (TARGET_ABICALLS)
    {
      fprintf (asm_out_file, "\t.abicalls\n");
      if (!flag_pic)
	fprintf (asm_out_file, "\t.option\tpic0\n");
    }
}

/* Make the last instruction frame-related and note that it performs
   the operation described by FRAME_PATTERN.  */

static void
mips_set_frame_expr (rtx frame_pattern)
{
  rtx insn;

  insn = get_last_insn ();
  RTX_FRAME_RELATED_P (insn) = 1;
  REG_NOTES (insn) = alloc_EXPR_LIST (REG_FRAME_RELATED_EXPR,
				      frame_pattern,
				      REG_NOTES (insn));
}

/* Return a frame-related rtx that stores REG at MEM.
   REG must be a single register.  */

static rtx
mips_frame_set (rtx mem, rtx reg)
{
  rtx set;

  /* If we're saving the return address register and the DWARF return
     address column differs from the hard register number, adjust the
     note reg to refer to the former.  */
  if (REGNO (reg) == RETURN_ADDR_REGNUM
      && DWARF_FRAME_RETURN_COLUMN != RETURN_ADDR_REGNUM)
    reg = gen_rtx_REG (GET_MODE (reg), DWARF_FRAME_RETURN_COLUMN);

  set = gen_rtx_SET (VOIDmode, mem, reg);
  RTX_FRAME_RELATED_P (set) = 1;

  return set;
}

/* Return true if predicate PRED is true for at least one instruction.
   Cache the result in *CACHE, and assume that the result is true
   if *CACHE is already true.  */

static bool
mips_find_gp_ref (bool *cache, bool (*pred) (rtx))
{
  rtx insn;

  if (!*cache)
    {
      push_topmost_sequence ();
      for (insn = get_insns (); insn; insn = NEXT_INSN (insn))
	if (USEFUL_INSN_P (insn) && pred (insn))
	  {
	    *cache = true;
	    break;
	  }
      pop_topmost_sequence ();
    }
  return *cache;
}

/* Return true if INSN refers to the global pointer in a "flexible" way.
   See mips_cfun_has_flexible_gp_ref_p for details.  */

static bool
mips_insn_has_flexible_gp_ref_p (rtx insn)
{
  return (get_attr_got (insn) != GOT_UNSET
	  || reg_overlap_mentioned_p (pic_offset_table_rtx, PATTERN (insn)));
}

/* Return true if the current function references the global pointer,
   but if those references do not inherently require the global pointer
   to be $28.  Assume !mips_cfun_has_inflexible_gp_ref_p ().  */

static bool
mips_cfun_has_flexible_gp_ref_p (void)
{
  return mips_find_gp_ref (&cfun->machine->has_flexible_gp_insn_p,
			   mips_insn_has_flexible_gp_ref_p);
}

/* Return true if the current function's prologue must load the global
   pointer value into pic_offset_table_rtx and store the same value in
   the function's cprestore slot (if any).

   One problem we have to deal with is that, when emitting GOT-based
   position independent code, long-branch sequences will need to load
   the address of the branch target from the GOT.  We don't know until
   the very end of compilation whether (and where) the function needs
   long branches, so we must ensure that _any_ branch can access the
   global pointer in some form.  However, we do not want to pessimize
   the usual case in which all branches are short.

   We handle this as follows:

   (1) During reload, we set cfun->machine->global_pointer to
       INVALID_REGNUM if we _know_ that the current function
       doesn't need a global pointer.  This is only valid if
       long branches don't need the GOT.

       Otherwise, we assume that we might need a global pointer
       and pick an appropriate register.

   (2) If cfun->machine->global_pointer != INVALID_REGNUM,
       we ensure that the global pointer is available at every
       block boundary bar entry and exit.  We do this in one of two ways:

       - If the function has a cprestore slot, we ensure that this
	 slot is valid at every branch.  However, as explained in
	 point (6) below, there is no guarantee that pic_offset_table_rtx
	 itself is valid if new uses of the global pointer are introduced
	 after the first post-epilogue split.

	 We guarantee that the cprestore slot is valid by loading it
	 into a fake register, CPRESTORE_SLOT_REGNUM.  We then make
	 this register live at every block boundary bar function entry
	 and exit.  It is then invalid to move the load (and thus the
	 preceding store) across a block boundary.

       - If the function has no cprestore slot, we guarantee that
	 pic_offset_table_rtx itself is valid at every branch.

   (3) During prologue and epilogue generation, we emit "ghost"
       placeholder instructions to manipulate the global pointer.

   (4) During prologue generation, we set cfun->machine->must_initialize_gp_p
       and cfun->machine->must_restore_gp_when_clobbered_p if we already know
       that the function needs a global pointer.  (There is no need to set
       them earlier than this, and doing it as late as possible leads to
       fewer false positives.)

   (5) If cfun->machine->must_initialize_gp_p is true during a
       split_insns pass, we split the ghost instructions into real
       instructions.  These split instructions can then be optimized in
       the usual way.  Otherwise, we keep the ghost instructions intact,
       and optimize for the case where they aren't needed.  We still
       have the option of splitting them later, if we need to introduce
       new uses of the global pointer.

       For example, the scheduler ignores a ghost instruction that
       stores $28 to the stack, but it handles the split form of
       the ghost instruction as an ordinary store.

   Note that the ghost instructions must have a zero length for three reasons:

   - Giving the length of the underlying $gp sequence might cause
     us to use long branches in cases where they aren't really needed.

   - They would perturb things like alignment calculations.

   - More importantly, the hazard detection in md_reorg relies on
     empty instructions having a zero length.

   If we find a long branch and split the ghost instructions at the
   end of md_reorg, the split could introduce more long branches.
   That isn't a problem though, because we still do the split before
   the final shorten_branches pass.

   This is extremely ugly, but it seems like the best compromise between
   correctness and efficiency.  */

bool
mips_must_initialize_gp_p (void)
{
  return cfun->machine->must_initialize_gp_p;
}

/* Return true if the current function must save register REGNO.  */

static bool
mips_save_reg_p (unsigned int regno)
{
  bool call_saved = !global_regs[regno] && !call_really_used_regs[regno];
  bool might_clobber = crtl->saves_all_registers
		       || df_regs_ever_live_p (regno)
		       || cfun->machine->global_pointer == regno
		       || (regno == HARD_FRAME_POINTER_REGNUM
			   && frame_pointer_needed);

  return (call_saved && might_clobber)
	 || (regno == RETURN_ADDR_REGNUM && crtl->calls_eh_return);
}

/* Return the register that should be used as the global pointer
   within this function.  Return INVALID_REGNUM if the function
   doesn't need a global pointer.  */

static unsigned int
mips_global_pointer (void)
{
  /* $gp is always available unless we're using a GOT.  */
  if (!TARGET_USE_GOT)
    return GLOBAL_POINTER_REGNUM;

  /* If there are no current references to $gp, then the only uses
     we can introduce later are those involved in long branches.  */
  if (!mips_cfun_has_flexible_gp_ref_p ())
    return INVALID_REGNUM;

  /* For non-leaf functions, use a saved register. */
  if (!current_function_is_leaf)
    return GLOBAL_POINTER_REGNUM_NONLEAF;

  return GLOBAL_POINTER_REGNUM;
}

/* Populate the current function's mips_frame_info structure.

   MIPS stack frames look like:

	+-------------------------------+
	|                               |
	|  incoming stack arguments     |
	|                               |
	+-------------------------------+
	|                               |
	|  caller-allocated save area   |
      A |  for register arguments       |
	|                               |
	+-------------------------------+ <-- incoming stack pointer
	|                               |
	|  callee-allocated save area   |
      B |  for arguments that are       |
	|  split between registers and  |
	|  the stack                    |
	|                               |
	+-------------------------------+ <-- arg_pointer_rtx
	|                               |
      C |  callee-allocated save area   |
	|  for register varargs         |
	|                               |
	+-------------------------------+ <-- stack_pointer_rtx + fp_sp_offset
	|                               |       + UNITS_PER_HWFPVALUE
	|  FPR save area                |
	|                               |
	+-------------------------------+ <-- stack_pointer_rtx + gp_sp_offset
	|                               |       + UNITS_PER_WORD
	|  GPR save area                |
	|                               |
	+-------------------------------+ <-- frame_pointer_rtx with
	|                               | \     -fstack-protector
	|  local variables              |  | var_size
	|                               | /
      P +-------------------------------+ <-- hard_frame_pointer_rtx for
	|                               | \     MIPS16 code
	|  outgoing stack arguments     |  |
	|                               |  |
	+-------------------------------+  | args_size
	|                               |  |
	|  caller-allocated save area   |  |
	|  for register arguments       |  |
	|                               | /
	+-------------------------------+ <-- stack_pointer_rtx
					      frame_pointer_rtx without
					        -fstack-protector
					      hard_frame_pointer_rtx for
						non-MIPS16 code.

   At least two of A, B and C will be empty.

   Dynamic stack allocations such as alloca insert data at point P.
   They decrease stack_pointer_rtx but leave frame_pointer_rtx and
   hard_frame_pointer_rtx unchanged.  */

static void
mips_compute_frame_info (void)
{
  struct mips_frame_info *frame;
  HOST_WIDE_INT offset, size;
  unsigned int regno, i;

  frame = &cfun->machine->frame;
  memset (frame, 0, sizeof (*frame));
  size = get_frame_size ();

  cfun->machine->global_pointer = mips_global_pointer ();

  /* The first two blocks contain the outgoing argument area and the $gp save
     slot.  This area isn't needed in leaf functions, but if the
     target-independent frame size is nonzero, we have already committed to
     allocating these in STARTING_FRAME_OFFSET for !FRAME_GROWS_DOWNWARD.  */
  if ((size == 0 || FRAME_GROWS_DOWNWARD) && current_function_is_leaf)
    {
      /* The MIPS 3.0 linker does not like functions that dynamically
	 allocate the stack and have 0 for STACK_DYNAMIC_OFFSET, since it
	 looks like we are trying to create a second frame pointer to the
	 function, so allocate some stack space to make it happy.  */
      if (cfun->calls_alloca)
	frame->args_size = REG_PARM_STACK_SPACE (cfun->decl);
      else
	frame->args_size = 0;
    }
  else
    {
      frame->args_size = crtl->outgoing_args_size;
    }
  offset = frame->args_size;

  /* Move above the local variables.  */
  frame->var_size = MIPS_STACK_ALIGN (size);
  offset += frame->var_size;

  /* Find out which GPRs we need to save.  */
  for (regno = GP_REG_FIRST; regno <= GP_REG_LAST; regno++)
    if (mips_save_reg_p (regno))
      {
	frame->num_gp++;
	frame->mask |= 1 << (regno - GP_REG_FIRST);
      }

  /* If this function calls eh_return, we must also save and restore the
     EH data registers.  */
  if (crtl->calls_eh_return)
    for (i = 0; EH_RETURN_DATA_REGNO (i) != INVALID_REGNUM; i++)
      {
	frame->num_gp++;
	frame->mask |= 1 << (EH_RETURN_DATA_REGNO (i) - GP_REG_FIRST);
      }

  /* Move above the GPR save area.  */
  if (frame->num_gp > 0)
    {
      offset += MIPS_STACK_ALIGN (frame->num_gp * UNITS_PER_WORD);
      frame->gp_sp_offset = offset - UNITS_PER_WORD;
    }

  /* Find out which FPRs we need to save.  This loop must iterate over
     the same space as its companion in mips_for_each_saved_gpr_and_fpr.  */
  if (TARGET_HARD_FLOAT)
    for (regno = FP_REG_FIRST; regno <= FP_REG_LAST; regno++)
      if (mips_save_reg_p (regno))
	{
	  frame->num_fp++;
	  frame->fmask |= 1 << (regno - FP_REG_FIRST);
	}

  /* Move above the FPR save area.  */
  if (frame->num_fp > 0)
    {
      offset += MIPS_STACK_ALIGN (frame->num_fp * UNITS_PER_FPREG);
      frame->fp_sp_offset = offset - UNITS_PER_HWFPVALUE;
    }

  /* Move above the callee-allocated varargs save area.  */
  offset += MIPS_STACK_ALIGN (cfun->machine->varargs_size);
  frame->arg_pointer_offset = offset;

  /* Move above the callee-allocated area for pretend stack arguments.  */
  offset += crtl->args.pretend_args_size;
  frame->total_size = offset;

  /* Work out the offsets of the save areas from the top of the frame.  */
  if (frame->gp_sp_offset > 0)
    frame->gp_save_offset = frame->gp_sp_offset - offset;
  if (frame->fp_sp_offset > 0)
    frame->fp_save_offset = frame->fp_sp_offset - offset;
}

/* Return the style of GP load sequence that is being used for the
   current function.  */

enum mips_loadgp_style
mips_current_loadgp_style (void)
{
  if (!TARGET_USE_GOT || cfun->machine->global_pointer == INVALID_REGNUM)
    return LOADGP_NONE;

  if (TARGET_ABICALLS && !flag_pic)
    return LOADGP_ABSOLUTE;

  return LOADGP_NEWABI;
}

/* Implement TARGET_FRAME_POINTER_REQUIRED.  */

static bool
mips_frame_pointer_required (void)
{
  return cfun->calls_alloca;
}

/* Make sure that we're not trying to eliminate to the wrong hard frame
   pointer.  */

static bool
mips_can_eliminate (const int from ATTRIBUTE_UNUSED, const int to)
{
  return (to == HARD_FRAME_POINTER_REGNUM || to == STACK_POINTER_REGNUM);
}

/* Implement INITIAL_ELIMINATION_OFFSET.  FROM is either the frame pointer
   or argument pointer.  TO is either the stack pointer or hard frame
   pointer.  */

HOST_WIDE_INT
mips_initial_elimination_offset (int from)
{
  HOST_WIDE_INT offset;

  mips_compute_frame_info ();

  /* Set OFFSET to the offset from the end-of-prologue stack pointer.  */
  switch (from)
    {
    case FRAME_POINTER_REGNUM:
      if (FRAME_GROWS_DOWNWARD)
	offset = (cfun->machine->frame.args_size
		  + cfun->machine->frame.var_size);
      else
	offset = 0;
      break;

    case ARG_POINTER_REGNUM:
      offset = cfun->machine->frame.arg_pointer_offset;
      break;

    default:
      gcc_unreachable ();
    }

  return offset;
}

/* Implement TARGET_EXTRA_LIVE_ON_ENTRY.  */

static void
mips_extra_live_on_entry (bitmap regs)
{
  if (TARGET_USE_GOT)
    {
      /* PIC_FUNCTION_ADDR_REGNUM is live if we need it to set up
	 the global pointer.   */
      if (flag_pic)
	bitmap_set_bit (regs, PIC_FUNCTION_ADDR_REGNUM);

      /* See the comment above load_call<mode> for details.  */
      bitmap_set_bit (regs, GOT_VERSION_REGNUM);
    }
}

/* Implement RETURN_ADDR_RTX.  We do not support moving back to a
   previous frame.  */

rtx
mips_return_addr (int count, rtx frame ATTRIBUTE_UNUSED)
{
  if (count != 0)
    return const0_rtx;

  return get_hard_reg_initial_val (Pmode, RETURN_ADDR_REGNUM);
}

/* Emit code to change the current function's return address to
   ADDRESS.  SCRATCH is available as a scratch register, if needed.
   ADDRESS and SCRATCH are both word-mode GPRs.  */

void
mips_set_return_address (rtx address, rtx scratch)
{
  rtx slot_address;

  gcc_assert (BITSET_P (cfun->machine->frame.mask, RETURN_ADDR_REGNUM));
  slot_address = mips_add_offset (scratch, stack_pointer_rtx,
				  cfun->machine->frame.gp_sp_offset);
  mips_emit_move (gen_frame_mem (GET_MODE (address), slot_address), address);
}

/* A function to save or store a register.  The first argument is the
   register and the second is the stack slot.  */
typedef void (*mips_save_restore_fn) (rtx, rtx);

/* Use FN to save or restore register REGNO.  MODE is the register's
   mode and OFFSET is the offset of its save slot from the current
   stack pointer.  */

static void
mips_save_restore_reg (enum machine_mode mode, int regno,
		       HOST_WIDE_INT offset, mips_save_restore_fn fn)
{
  rtx mem;

  mem = gen_frame_mem (mode, plus_constant (stack_pointer_rtx, offset));
  fn (gen_rtx_REG (mode, regno), mem);
}

/* Call FN for each register that is saved by the current function.
   SP_OFFSET is the offset of the current stack pointer from the start
   of the frame.  */

static void
mips_for_each_saved_gpr_and_fpr (HOST_WIDE_INT sp_offset,
				 mips_save_restore_fn fn)
{
  HOST_WIDE_INT offset;
  int regno;

  /* Save registers starting from high to low.  The debuggers prefer at least
     the return register be stored at func+4, and also it allows us not to
     need a nop in the epilogue if at least one register is reloaded in
     addition to return address.  */
  offset = cfun->machine->frame.gp_sp_offset - sp_offset;
  for (regno = GP_REG_LAST; regno >= GP_REG_FIRST; regno--)
    if (BITSET_P (cfun->machine->frame.mask, regno - GP_REG_FIRST))
      {
	mips_save_restore_reg (word_mode, regno, offset, fn);
	offset -= UNITS_PER_WORD;
      }

  /* This loop must iterate over the same space as its companion in
     mips_compute_frame_info.  */
  offset = cfun->machine->frame.fp_sp_offset - sp_offset;
  for (regno = FP_REG_LAST - 1;
       regno >= FP_REG_FIRST;
       regno --)
    if (BITSET_P (cfun->machine->frame.fmask, regno - FP_REG_FIRST))
      {
	mips_save_restore_reg (DFmode, regno, offset, fn);
	offset -= GET_MODE_SIZE (DFmode);
      }
}

/* Return true if a move between register REGNO and its save slot (MEM)
   can be done in a single move.  LOAD_P is true if we are loading
   from the slot, false if we are storing to it.  */

static bool
mips_direct_save_slot_move_p (unsigned int regno, rtx mem, bool load_p)
{
  return mips_secondary_reload_class (REGNO_REG_CLASS (regno),
				      GET_MODE (mem), mem, load_p) == NO_REGS;
}

/* Emit a move from SRC to DEST, given that one of them is a register
   save slot and that the other is a register.  TEMP is a temporary
   GPR of the same mode that is available if need be.  */

void
mips_emit_save_slot_move (rtx dest, rtx src, rtx temp)
{
  unsigned int regno;
  rtx mem;

  if (REG_P (src))
    {
      regno = REGNO (src);
      mem = dest;
    }
  else
    {
      regno = REGNO (dest);
      mem = src;
    }

  if (regno == cfun->machine->global_pointer && !mips_must_initialize_gp_p ())
    {
      /* We don't yet know whether we'll need this instruction or not.
	 Postpone the decision by emitting a ghost move.  This move
	 is specifically not frame-related; only the split version is.  */
      if (TARGET_64BIT)
	emit_insn (gen_move_gpdi (dest, src));
      else
	emit_insn (gen_move_gpsi (dest, src));
      return;
    }

  if (mips_direct_save_slot_move_p (regno, mem, mem == src))
    mips_emit_move (dest, src);
  else
    {
      gcc_assert (!reg_overlap_mentioned_p (dest, temp));
      mips_emit_move (temp, src);
      mips_emit_move (dest, temp);
    }
  if (MEM_P (dest))
    mips_set_frame_expr (mips_frame_set (dest, src));
}

/* Implement TARGET_OUTPUT_FUNCTION_PROLOGUE.  */

static void
mips_output_function_prologue (FILE *file, HOST_WIDE_INT size ATTRIBUTE_UNUSED)
{
  const char *fnname;

#ifdef SDB_DEBUGGING_INFO
  if (debug_info_level != DINFO_LEVEL_TERSE && write_symbols == SDB_DEBUG)
    SDB_OUTPUT_SOURCE_LINE (file, DECL_SOURCE_LINE (current_function_decl));
#endif

  /* Get the function name the same way that toplev.c does before calling
     assemble_start_function.  This is needed so that the name used here
     exactly matches the name used in ASM_DECLARE_FUNCTION_NAME.  */
  fnname = XSTR (XEXP (DECL_RTL (current_function_decl), 0), 0);
  mips_start_function_definition (fnname);

  /* Output MIPS-specific frame information.  */
  if (!flag_inhibit_size_directive)
    {
      const struct mips_frame_info *frame;

      frame = &cfun->machine->frame;

      /* .frame FRAMEREG, FRAMESIZE, RETREG.  */
      fprintf (file,
	       "\t.frame\t%s," HOST_WIDE_INT_PRINT_DEC ",%s\t\t"
	       "# vars= " HOST_WIDE_INT_PRINT_DEC
	       ", regs= %d/%d"
	       ", args= " HOST_WIDE_INT_PRINT_DEC "\n",
	       reg_names[frame_pointer_needed
			 ? HARD_FRAME_POINTER_REGNUM
			 : STACK_POINTER_REGNUM],
	       frame->total_size,
	       reg_names[RETURN_ADDR_REGNUM],
	       frame->var_size,
	       frame->num_gp, frame->num_fp,
	       frame->args_size);

      /* .mask MASK, OFFSET.  */
      fprintf (file, "\t.mask\t0x%08x," HOST_WIDE_INT_PRINT_DEC "\n",
	       frame->mask, frame->gp_save_offset);

      /* .fmask MASK, OFFSET.  */
      fprintf (file, "\t.fmask\t0x%08x," HOST_WIDE_INT_PRINT_DEC "\n",
	       frame->fmask, frame->fp_save_offset);
    }
}

/* Implement TARGET_OUTPUT_FUNCTION_EPILOGUE.  */

static void
mips_output_function_epilogue (FILE *file ATTRIBUTE_UNUSED,
			       HOST_WIDE_INT size ATTRIBUTE_UNUSED)
{
  const char *fnname;

  /* Reinstate the normal $gp.  */
  SET_REGNO (pic_offset_table_rtx, GLOBAL_POINTER_REGNUM);

  /* Get the function name the same way that toplev.c does before calling
     assemble_start_function.  This is needed so that the name used here
     exactly matches the name used in ASM_DECLARE_FUNCTION_NAME.  */
  fnname = XSTR (XEXP (DECL_RTL (current_function_decl), 0), 0);
  mips_end_function_definition (fnname);
}

/* Save register REG to MEM.  Make the instruction frame-related.  */

static void
mips_save_reg (rtx reg, rtx mem)
{
  mips_emit_save_slot_move (mem, reg, MIPS_PROLOGUE_TEMP (GET_MODE (reg)));
}

/* The __gnu_local_gp symbol.  */

static GTY(()) rtx mips_gnu_local_gp;

/* If we're generating n32 or n64 abicalls, emit instructions
   to set up the global pointer.  */

static void
mips_emit_loadgp (void)
{
  rtx addr, offset, incoming_address, pic_reg;

  pic_reg = pic_offset_table_rtx;
  switch (mips_current_loadgp_style ())
    {
    case LOADGP_ABSOLUTE:
      if (mips_gnu_local_gp == NULL)
	{
	  mips_gnu_local_gp = gen_rtx_SYMBOL_REF (Pmode, "__gnu_local_gp");
	  SYMBOL_REF_FLAGS (mips_gnu_local_gp) |= SYMBOL_FLAG_LOCAL;
	}
      emit_insn (Pmode == SImode
		 ? gen_loadgp_absolute_si (pic_reg, mips_gnu_local_gp)
		 : gen_loadgp_absolute_di (pic_reg, mips_gnu_local_gp));
      break;

    case LOADGP_NEWABI:
      addr = XEXP (DECL_RTL (current_function_decl), 0);
      offset = mips_unspec_address (addr, SYMBOL_GOTOFF_LOADGP);
      incoming_address = gen_rtx_REG (Pmode, PIC_FUNCTION_ADDR_REGNUM);
      emit_insn (Pmode == SImode
		 ? gen_loadgp_newabi_si (pic_reg, offset, incoming_address)
		 : gen_loadgp_newabi_di (pic_reg, offset, incoming_address));
      break;

    default:
      return;
    }
}

/* Expand the "prologue" pattern.  */

void
mips_expand_prologue (void)
{
  const struct mips_frame_info *frame;
  HOST_WIDE_INT size;
  rtx insn;

  if (cfun->machine->global_pointer != INVALID_REGNUM)
    {
      /* Check whether an insn uses pic_offset_table_rtx, either explicitly
	 or implicitly.  If so, we can commit to using a global pointer
	 straight away, otherwise we need to defer the decision.  */
      if (mips_cfun_has_flexible_gp_ref_p ())
	cfun->machine->must_initialize_gp_p = true;

      SET_REGNO (pic_offset_table_rtx, cfun->machine->global_pointer);
    }

  frame = &cfun->machine->frame;
  size = frame->total_size;

  if (flag_stack_usage)
    current_function_static_stack_size = size;

  /* Save the registers.  Allocate up to MIPS_MAX_FIRST_STACK_STEP
     bytes beforehand; this is enough to cover the register save area
     without going out of range.  */
  if ((frame->mask | frame->fmask) != 0)
    {
      HOST_WIDE_INT step1;

      step1 = MIN (size, MIPS_MAX_FIRST_STACK_STEP);
      insn = gen_add3_insn (stack_pointer_rtx,
			    stack_pointer_rtx,
			    GEN_INT (-step1));
      RTX_FRAME_RELATED_P (emit_insn (insn)) = 1;
      size -= step1;
      mips_for_each_saved_gpr_and_fpr (size, mips_save_reg);
    }

  /* Allocate the rest of the frame.  */
  if (size > 0)
    {
      if (SMALL_OPERAND (-size))
	RTX_FRAME_RELATED_P (emit_insn (gen_add3_insn (stack_pointer_rtx,
						       stack_pointer_rtx,
						       GEN_INT (-size)))) = 1;
      else
	{
	  mips_emit_move (MIPS_PROLOGUE_TEMP (Pmode), GEN_INT (size));
	  emit_insn (gen_sub3_insn (stack_pointer_rtx,
				    stack_pointer_rtx,
				    MIPS_PROLOGUE_TEMP (Pmode)));

	  /* Describe the combined effect of the previous instructions.  */
	  mips_set_frame_expr
	    (gen_rtx_SET (VOIDmode, stack_pointer_rtx,
			  plus_constant (stack_pointer_rtx, -size)));
	}
    }

  /* Set up the frame pointer, if we're using one.  */
  if (frame_pointer_needed)
    {
      insn = mips_emit_move (hard_frame_pointer_rtx, stack_pointer_rtx);
      RTX_FRAME_RELATED_P (insn) = 1;
    }

  mips_emit_loadgp ();
}

/* Emit instructions to restore register REG from slot MEM.  */

static void
mips_restore_reg (rtx reg, rtx mem)
{
  mips_emit_save_slot_move (reg, mem, MIPS_EPILOGUE_TEMP (GET_MODE (reg)));
}

/* Expand an "epilogue" or "sibcall_epilogue" pattern; SIBCALL_P
   says which.  */

void
mips_expand_epilogue (bool sibcall_p)
{
  const struct mips_frame_info *frame;
  HOST_WIDE_INT step1, step2;
  rtx base, target;

  if (!sibcall_p && mips_can_use_return_insn ())
    {
      emit_jump_insn (gen_return ());
      return;
    }

  /* Split the frame into two.  STEP1 is the amount of stack we should
     deallocate before restoring the registers.  STEP2 is the amount we
     should deallocate afterwards.

     Start off by assuming that no registers need to be restored.  */
  frame = &cfun->machine->frame;
  step1 = frame->total_size;
  step2 = 0;

  /* If we need to restore registers, deallocate as much stack as
     possible in the second step without going out of range.  */
  if ((frame->mask | frame->fmask) != 0)
    {
      step2 = MIN (step1, MIPS_MAX_FIRST_STACK_STEP);
      step1 -= step2;
    }

  /* Set TARGET to BASE + STEP1.  */
  base = frame_pointer_needed ? hard_frame_pointer_rtx : stack_pointer_rtx;
  target = base;
  if (step1 > 0)
    {
      rtx adjust;

      /* Get an rtx for STEP1 that we can add to BASE.  */
      adjust = GEN_INT (step1);
      if (!SMALL_OPERAND (step1))
	{
	  mips_emit_move (MIPS_EPILOGUE_TEMP (Pmode), adjust);
	  adjust = MIPS_EPILOGUE_TEMP (Pmode);
	}

      /* Normal mode code can copy the result straight into $sp.  */
      target = stack_pointer_rtx;

      emit_insn (gen_add3_insn (target, base, adjust));
    }

  /* Copy TARGET into the stack pointer.  */
  if (target != stack_pointer_rtx)
    mips_emit_move (stack_pointer_rtx, target);

  /* Restore the registers.  */
  mips_for_each_saved_gpr_and_fpr (frame->total_size - step2,
		       mips_restore_reg);

	  /* Deallocate the final bit of the frame.  */
  if (step2 > 0)
    emit_insn (gen_add3_insn (stack_pointer_rtx,
			      stack_pointer_rtx,
			      GEN_INT (step2)));

  /* Add in the __builtin_eh_return stack adjustment.  We need to
     use a temporary in MIPS16 code.  */
  if (crtl->calls_eh_return)
    {
      emit_insn (gen_add3_insn (stack_pointer_rtx,
				stack_pointer_rtx,
				EH_RETURN_STACKADJ_RTX));
    }

  if (!sibcall_p)
    emit_jump_insn (gen_return_internal (gen_rtx_REG (Pmode, RETURN_ADDR_REGNUM)));
}

/* Return nonzero if this function is known to have a null epilogue.
   This allows the optimizer to omit jumps to jumps if no stack
   was created.  */

bool
mips_can_use_return_insn (void)
{
  return reload_completed && cfun->machine->frame.total_size == 0;
}

/* Return true if register REGNO can store a value of mode MODE.
   The result of this function is cached in mips_hard_regno_mode_ok.  */

static bool
mips_hard_regno_mode_ok_p (unsigned int regno, enum machine_mode mode)
{
  unsigned int size;
  enum mode_class mclass;

  if (mode == CCmode)
    return GP_REG_P (regno);

  size = GET_MODE_SIZE (mode);
  mclass = GET_MODE_CLASS (mode);

  if (GP_REG_P (regno))
    return ((regno - GP_REG_FIRST) & 1) == 0 || size <= UNITS_PER_WORD;

  if (FP_REG_P (regno))
    {
      /* Allow TFmode for CCmode reloads.  */
      if (mode == TFmode)
	return true;

      if (mclass == MODE_FLOAT
	  || mclass == MODE_COMPLEX_FLOAT
	  || mclass == MODE_VECTOR_FLOAT)
	return size <= UNITS_PER_FPVALUE;

      /* Allow integer modes that fit into a single register.  We need
	 to put integers into FPRs when using instructions like CVT
	 and TRUNC.  There's no point allowing sizes smaller than a word,
	 because the FPU has no appropriate load/store instructions.  */
      if (mclass == MODE_INT)
	return size >= MIN_UNITS_PER_WORD && size <= UNITS_PER_FPREG;
    }

  if (regno == GOT_VERSION_REGNUM)
    return mode == SImode;

  return false;
}

/* Implement HARD_REGNO_NREGS.  */

unsigned int
mips_hard_regno_nregs (int regno, enum machine_mode mode)
{
  if (VECTOR_MODE_P(mode))
    return 1;

  if (VEC_GP_REG_P(regno) || VEC_FP_REG_P(regno))
    return 1;

  if (FP_REG_P (regno))
    return (GET_MODE_SIZE (mode) + UNITS_PER_FPREG - 1) / UNITS_PER_FPREG;

  /* All other registers are word-sized.  */
  return (GET_MODE_SIZE (mode) + UNITS_PER_WORD - 1) / UNITS_PER_WORD;
}

/* Implement CLASS_MAX_NREGS, taking the maximum of the cases
   in mips_hard_regno_nregs.  */

int
mips_class_max_nregs (enum reg_class rclass, enum machine_mode mode)
{
  int size;
  HARD_REG_SET left;

  if (VECTOR_MODE_P(mode))
    return 1;

  if ((rclass == VEC_GR_REGS) || (rclass == VEC_FP_REGS))
    return 1;

  size = 0x8000;
  COPY_HARD_REG_SET (left, reg_class_contents[(int) rclass]);
  if (hard_reg_set_intersect_p (left, reg_class_contents[(int) FP_REGS]))
    {
      size = MIN (size, UNITS_PER_FPREG);
      AND_COMPL_HARD_REG_SET (left, reg_class_contents[(int) FP_REGS]);
    }
  if (!hard_reg_set_empty_p (left))
    size = MIN (size, UNITS_PER_WORD);
  return (GET_MODE_SIZE (mode) + size - 1) / size;
}

/* Implement CANNOT_CHANGE_MODE_CLASS.  */

bool
mips_cannot_change_mode_class (enum machine_mode from ATTRIBUTE_UNUSED,
			       enum machine_mode to ATTRIBUTE_UNUSED,
			       enum reg_class rclass)
{
  /* There are several problems with changing the modes of values
     in floating-point registers:

     - When a multi-word value is stored in paired floating-point
       registers, the first register always holds the low word.
       We therefore can't allow FPRs to change between single-word
       and multi-word modes on big-endian targets.

     - GCC assumes that each word of a multiword register can be accessed
       individually using SUBREGs.  This is not true for floating-point
       registers if they are bigger than a word.

     - Loading a 32-bit value into a 64-bit floating-point register
       will not sign-extend the value, despite what LOAD_EXTEND_OP says.
       We can't allow FPRs to change from SImode to to a wider mode on
       64-bit targets.

     - If the FPU has already interpreted a value in one format, we must
       not ask it to treat the value as having a different format.

     We therefore disallow all mode changes involving FPRs.  */
  return reg_classes_intersect_p (FP_REGS, rclass);
}

/* Return true if moves in mode MODE can use the FPU's mov.fmt instruction.  */

static bool
mips_mode_ok_for_mov_fmt_p (enum machine_mode mode)
{
  switch (mode)
    {
    case SFmode:
    case DFmode:
      return TARGET_HARD_FLOAT;

    default:
      return false;
    }
}

/* Implement MODES_TIEABLE_P.  */

bool
mips_modes_tieable_p (enum machine_mode mode1, enum machine_mode mode2)
{
  /* FPRs allow no mode punning, so it's not worth tying modes if we'd
     prefer to put one of them in FPRs.  */
  return (mode1 == mode2
	  || (!mips_mode_ok_for_mov_fmt_p (mode1)
	      && !mips_mode_ok_for_mov_fmt_p (mode2)));
}

/* Implement TARGET_PREFERRED_RELOAD_CLASS.  */

static reg_class_t
mips_preferred_reload_class (rtx x, reg_class_t rclass)
{
  if (reg_class_subset_p (FP_REGS, rclass)
      && mips_mode_ok_for_mov_fmt_p (GET_MODE (x)))
    return FP_REGS;

  if (reg_class_subset_p (GR_REGS, rclass))
    rclass = GR_REGS;

  return rclass;
}

/* RCLASS is a class involved in a REGISTER_MOVE_COST calculation.
   Return a "canonical" class to represent it in later calculations.  */

static reg_class_t
mips_canonicalize_move_class (reg_class_t rclass)
{
  if (reg_class_subset_p (rclass, GENERAL_REGS))
    rclass = GENERAL_REGS;

  return rclass;
}

/* Return the cost of moving a value of mode MODE from a register of
   class FROM to a GPR.  Return 0 for classes that are unions of other
   classes handled by this function.  */

static int
mips_move_to_gpr_cost (reg_class_t from)
{
  switch (from)
    {
    case GENERAL_REGS:
      return 1;

    case FP_REGS:
      /* FP->int moves can cause recoupling on decoupled implementations */
      return 4;

    default:
      return 0;
    }
}

/* Return the cost of moving a value of mode MODE from a GPR to a
   register of class TO.  Return 0 for classes that are unions of
   other classes handled by this function.  */

static int
mips_move_from_gpr_cost (reg_class_t to)
{
  switch (to)
    {
    case GENERAL_REGS:
    case FP_REGS:
      return 1;

    default:
      return 0;
    }
}

/* Implement TARGET_REGISTER_MOVE_COST.  Return 0 for classes that are the
   maximum of the move costs for subclasses; regclass will work out
   the maximum for us.  */

static int
mips_register_move_cost (enum machine_mode mode,
			 reg_class_t from, reg_class_t to)
{
  int cost1, cost2;

  from = mips_canonicalize_move_class (from);
  to = mips_canonicalize_move_class (to);

  /* Handle moves that can be done without using general-purpose registers.  */
  if (from == FP_REGS)
    if (to == FP_REGS && mips_mode_ok_for_mov_fmt_p (mode))
      /* fsgnj.fmt.  */
      return 1;

  /* Handle cases in which only one class deviates from the ideal.  */
  if (from == GENERAL_REGS)
    return mips_move_from_gpr_cost (to);
  if (to == GENERAL_REGS)
    return mips_move_to_gpr_cost (from);

  /* Handles cases that require a GPR temporary.  */
  cost1 = mips_move_to_gpr_cost (from);
  if (cost1 != 0)
    {
      cost2 = mips_move_from_gpr_cost (to);
      if (cost2 != 0)
	return cost1 + cost2;
    }

  return 0;
}

/* Implement TARGET_MEMORY_MOVE_COST.  */

static int
mips_memory_move_cost (enum machine_mode mode, reg_class_t rclass, bool in)
{
  return (mips_cost->memory_latency
	  + memory_move_secondary_cost (mode, rclass, in));
} 

/* Implement TARGET_IRA_COVER_CLASSES.  */

static const reg_class_t *
mips_ira_cover_classes (void)
{
  static const reg_class_t no_acc_classes[] = {
    GR_REGS, FP_REGS, VEC_GR_REGS, VEC_FP_REGS,
    LIM_REG_CLASSES
  };

  return no_acc_classes;
}

/* Return the register class required for a secondary register when
   copying between one of the registers in RCLASS and value X, which
   has mode MODE.  X is the source of the move if IN_P, otherwise it
   is the destination.  Return NO_REGS if no secondary register is
   needed.  */

enum reg_class
mips_secondary_reload_class (enum reg_class rclass,
			     enum machine_mode mode, rtx x,
			     bool in_p ATTRIBUTE_UNUSED)
{
  int regno;

  regno = true_regnum (x);

  if (reg_class_subset_p (rclass, FP_REGS))
    {
      if (MEM_P (x)
	  && (GET_MODE_SIZE (mode) == 4 || GET_MODE_SIZE (mode) == 8))
	/* In this case we can use lwc1, swc1, ldc1 or sdc1.  We'll use
	   pairs of lwc1s and swc1s if ldc1 and sdc1 are not supported.  */
	return NO_REGS;

      if (GP_REG_P (regno) || x == CONST0_RTX (mode))
	/* In this case we can use mtc1, mfc1, dmtc1 or dmfc1.  */
	return NO_REGS;

      if (CONSTANT_P (x) && !targetm.cannot_force_const_mem (x))
	/* We can force the constant to memory and use lwc1
	   and ldc1.  As above, we will use pairs of lwc1s if
	   ldc1 is not supported.  */
	return NO_REGS;

      if (FP_REG_P (regno) && mips_mode_ok_for_mov_fmt_p (mode))
	/* In this case we can use mov.fmt.  */
	return NO_REGS;

      /* Otherwise, we need to reload through an integer register.  */
      return GR_REGS;
    }
  if (FP_REG_P (regno))
    return reg_class_subset_p (rclass, GR_REGS) ? NO_REGS : GR_REGS;

  return NO_REGS;
}

/* Implement TARGET_MODE_REP_EXTENDED.  */

static int
mips_mode_rep_extended (enum machine_mode mode, enum machine_mode mode_rep)
{
  /* On 64-bit targets, SImode register values are sign-extended to DImode.  */
  if (TARGET_64BIT && mode == SImode && mode_rep == DImode)
    return SIGN_EXTEND;

  return UNKNOWN;
}

/* Implement TARGET_VALID_POINTER_MODE.  */

static bool
mips_valid_pointer_mode (enum machine_mode mode)
{
  return mode == SImode || (TARGET_64BIT && mode == DImode);
}

/* Implement TARGET_VECTOR_MODE_SUPPORTED_P.  */

static bool
mips_vector_mode_supported_p (enum machine_mode mode ATTRIBUTE_UNUSED)
{
  return false;
}

/* Implement TARGET_SCALAR_MODE_SUPPORTED_P.  */

static bool
mips_scalar_mode_supported_p (enum machine_mode mode)
{
  if (ALL_FIXED_POINT_MODE_P (mode)
      && GET_MODE_PRECISION (mode) <= 2 * BITS_PER_WORD)
    return true;

  return default_scalar_mode_supported_p (mode);
}

/* Implement TARGET_VECTORIZE_PREFERRED_SIMD_MODE.  */

static enum machine_mode
mips_preferred_simd_mode (enum machine_mode mode ATTRIBUTE_UNUSED)
{
  return word_mode;
}

/* Implement TARGET_INIT_LIBFUNCS.  */

static void
mips_init_libfuncs (void)
{
}

/* Return the assembly code for INSN, which has the operands given by
   OPERANDS, and which branches to OPERANDS[0] if some condition is true.
   BRANCH_IF_TRUE is the asm template that should be used if OPERANDS[0]
   is in range of a direct branch.  BRANCH_IF_FALSE is an inverted
   version of BRANCH_IF_TRUE.  */

const char *
mips_output_conditional_branch (rtx insn, rtx *operands,
				const char *branch_if_true,
				const char *branch_if_false)
{
  unsigned int length;
  rtx taken, not_taken;

  gcc_assert (LABEL_P (operands[0]));

  length = get_attr_length (insn);
  if (length <= 4)
    return branch_if_true;

  /* Generate a reversed branch around a direct jump.  This fallback does
     not use branch-likely instructions.  */
  not_taken = gen_label_rtx ();
  taken = operands[0];

  /* Generate the reversed branch to NOT_TAKEN.  */
  operands[0] = not_taken;
  output_asm_insn (branch_if_false, operands);

  /* Output the unconditional branch to TAKEN.  */
  output_asm_insn ("j\t%0", &taken);

  /* Output NOT_TAKEN.  */
  targetm.asm_out.internal_label (asm_out_file, "L",
				  CODE_LABEL_NUMBER (not_taken));
  return "";
}

/* Implement TARGET_SCHED_ADJUST_COST.  We assume that anti and output
   dependencies have no cost. */

static int
mips_adjust_cost (rtx insn ATTRIBUTE_UNUSED, rtx link,
		  rtx dep ATTRIBUTE_UNUSED, int cost)
{
  if (REG_NOTE_KIND (link) != 0)
    return 0;
  return cost;
}

/* Return the number of instructions that can be issued per cycle.  */

static int
mips_issue_rate (void)
{
  switch (mips_tune)
    {
    case PROCESSOR_ROCKET32:
    case PROCESSOR_ROCKET64:
      return 1;

    default:
      return 1;
    }
}

/* This structure describes a single built-in function.  */
struct mips_builtin_description {
  /* The code of the main .md file instruction.  See mips_builtin_type
     for more information.  */
  enum insn_code icode;

  /* The floating-point comparison code to use with ICODE, if any.  */
  enum mips_fp_condition cond;

  /* The name of the built-in function.  */
  const char *name;

  /* Specifies how the function should be expanded.  */
  enum mips_builtin_type builtin_type;

  /* The function's prototype.  */
  enum mips_function_type function_type;

  /* Whether the function is available.  */
  unsigned int (*avail) (void);
};

static unsigned int
mips_builtin_avail_cache (void)
{
  return 0;
}

/* Construct a mips_builtin_description from the given arguments.

   INSN is the name of the associated instruction pattern, without the
   leading CODE_FOR_mips_.

   CODE is the floating-point condition code associated with the
   function.  It can be 'f' if the field is not applicable.

   NAME is the name of the function itself, without the leading
   "__builtin_mips_".

   BUILTIN_TYPE and FUNCTION_TYPE are mips_builtin_description fields.

   AVAIL is the name of the availability predicate, without the leading
   mips_builtin_avail_.  */
#define MIPS_BUILTIN(INSN, COND, NAME, BUILTIN_TYPE,			\
		     FUNCTION_TYPE, AVAIL)				\
  { CODE_FOR_mips_ ## INSN, MIPS_FP_COND_ ## COND,			\
    "__builtin_mips_" NAME, BUILTIN_TYPE, FUNCTION_TYPE,		\
    mips_builtin_avail_ ## AVAIL }

/* Define __builtin_mips_<INSN>, which is a MIPS_BUILTIN_DIRECT function
   mapped to instruction CODE_FOR_mips_<INSN>,  FUNCTION_TYPE and AVAIL
   are as for MIPS_BUILTIN.  */
#define DIRECT_BUILTIN(INSN, FUNCTION_TYPE, AVAIL)			\
  MIPS_BUILTIN (INSN, f, #INSN, MIPS_BUILTIN_DIRECT, FUNCTION_TYPE, AVAIL)

/* Define __builtin_mips_<INSN>, which is a MIPS_BUILTIN_DIRECT_NO_TARGET
   function mapped to instruction CODE_FOR_mips_<INSN>,  FUNCTION_TYPE
   and AVAIL are as for MIPS_BUILTIN.  */
#define DIRECT_NO_TARGET_BUILTIN(INSN, FUNCTION_TYPE, AVAIL)		\
  MIPS_BUILTIN (INSN, f, #INSN,	MIPS_BUILTIN_DIRECT_NO_TARGET,		\
		FUNCTION_TYPE, AVAIL)

static const struct mips_builtin_description mips_builtins[] = {
  DIRECT_NO_TARGET_BUILTIN (cache, MIPS_VOID_FTYPE_SI_CVPOINTER, cache),
};

/* Index I is the function declaration for mips_builtins[I], or null if the
   function isn't defined on this target.  */
static GTY(()) tree mips_builtin_decls[ARRAY_SIZE (mips_builtins)];

/* MODE is a vector mode whose elements have type TYPE.  Return the type
   of the vector itself.  */

static tree
mips_builtin_vector_type (tree type, enum machine_mode mode)
{
  static tree types[2 * (int) MAX_MACHINE_MODE];
  int mode_index;

  mode_index = (int) mode;

  if (TREE_CODE (type) == INTEGER_TYPE && TYPE_UNSIGNED (type))
    mode_index += MAX_MACHINE_MODE;

  if (types[mode_index] == NULL_TREE)
    types[mode_index] = build_vector_type_for_mode (type, mode);
  return types[mode_index];
}

/* Return a type for 'const volatile void *'.  */

static tree
mips_build_cvpointer_type (void)
{
  static tree cache;

  if (cache == NULL_TREE)
    cache = build_pointer_type (build_qualified_type
				(void_type_node,
				 TYPE_QUAL_CONST | TYPE_QUAL_VOLATILE));
  return cache;
}

/* Source-level argument types.  */
#define MIPS_ATYPE_VOID void_type_node
#define MIPS_ATYPE_INT integer_type_node
#define MIPS_ATYPE_POINTER ptr_type_node
#define MIPS_ATYPE_CVPOINTER mips_build_cvpointer_type ()

/* Standard mode-based argument types.  */
#define MIPS_ATYPE_UQI unsigned_intQI_type_node
#define MIPS_ATYPE_SI intSI_type_node
#define MIPS_ATYPE_USI unsigned_intSI_type_node
#define MIPS_ATYPE_DI intDI_type_node
#define MIPS_ATYPE_UDI unsigned_intDI_type_node
#define MIPS_ATYPE_SF float_type_node
#define MIPS_ATYPE_DF double_type_node

/* Vector argument types.  */
#define MIPS_ATYPE_V2SF mips_builtin_vector_type (float_type_node, V2SFmode)
#define MIPS_ATYPE_V2HI mips_builtin_vector_type (intHI_type_node, V2HImode)
#define MIPS_ATYPE_V2SI mips_builtin_vector_type (intSI_type_node, V2SImode)
#define MIPS_ATYPE_V4QI mips_builtin_vector_type (intQI_type_node, V4QImode)
#define MIPS_ATYPE_V4HI mips_builtin_vector_type (intHI_type_node, V4HImode)
#define MIPS_ATYPE_V8QI mips_builtin_vector_type (intQI_type_node, V8QImode)
#define MIPS_ATYPE_UV2SI					\
  mips_builtin_vector_type (unsigned_intSI_type_node, V2SImode)
#define MIPS_ATYPE_UV4HI					\
  mips_builtin_vector_type (unsigned_intHI_type_node, V4HImode)
#define MIPS_ATYPE_UV8QI					\
  mips_builtin_vector_type (unsigned_intQI_type_node, V8QImode)

/* MIPS_FTYPE_ATYPESN takes N MIPS_FTYPES-like type codes and lists
   their associated MIPS_ATYPEs.  */
#define MIPS_FTYPE_ATYPES1(A, B) \
  MIPS_ATYPE_##A, MIPS_ATYPE_##B

#define MIPS_FTYPE_ATYPES2(A, B, C) \
  MIPS_ATYPE_##A, MIPS_ATYPE_##B, MIPS_ATYPE_##C

#define MIPS_FTYPE_ATYPES3(A, B, C, D) \
  MIPS_ATYPE_##A, MIPS_ATYPE_##B, MIPS_ATYPE_##C, MIPS_ATYPE_##D

#define MIPS_FTYPE_ATYPES4(A, B, C, D, E) \
  MIPS_ATYPE_##A, MIPS_ATYPE_##B, MIPS_ATYPE_##C, MIPS_ATYPE_##D, \
  MIPS_ATYPE_##E

/* Return the function type associated with function prototype TYPE.  */

static tree
mips_build_function_type (enum mips_function_type type)
{
  static tree types[(int) MIPS_MAX_FTYPE_MAX];

  if (types[(int) type] == NULL_TREE)
    switch (type)
      {
#define DEF_MIPS_FTYPE(NUM, ARGS)					\
  case MIPS_FTYPE_NAME##NUM ARGS:					\
    types[(int) type]							\
      = build_function_type_list (MIPS_FTYPE_ATYPES##NUM ARGS,		\
				  NULL_TREE);				\
    break;
#include "config/riscv/riscv-ftypes.def"
#undef DEF_MIPS_FTYPE
      default:
	gcc_unreachable ();
      }

  return types[(int) type];
}

/* Implement TARGET_INIT_BUILTINS.  */

static void
mips_init_builtins (void)
{
  const struct mips_builtin_description *d;
  unsigned int i;

  /* Iterate through all of the bdesc arrays, initializing all of the
     builtin functions.  */
  for (i = 0; i < ARRAY_SIZE (mips_builtins); i++)
    {
      d = &mips_builtins[i];
      if (d->avail ())
	mips_builtin_decls[i]
	  = add_builtin_function (d->name,
				  mips_build_function_type (d->function_type),
				  i, BUILT_IN_MD, NULL, NULL);
    }
}

/* Implement TARGET_BUILTIN_DECL.  */

static tree
mips_builtin_decl (unsigned int code, bool initialize_p ATTRIBUTE_UNUSED)
{
  if (code >= ARRAY_SIZE (mips_builtins))
    return error_mark_node;
  return mips_builtin_decls[code];
}

/* Take argument ARGNO from EXP's argument list and convert it into a
   form suitable for input operand OPNO of instruction ICODE.  Return the
   value.  */

static rtx
mips_prepare_builtin_arg (enum insn_code icode,
			  unsigned int opno, tree exp, unsigned int argno)
{
  tree arg;
  rtx value;
  enum machine_mode mode;

  arg = CALL_EXPR_ARG (exp, argno);
  value = expand_normal (arg);
  mode = insn_data[icode].operand[opno].mode;
  if (!insn_data[icode].operand[opno].predicate (value, mode))
    {
      /* We need to get the mode from ARG for two reasons:

	   - to cope with address operands, where MODE is the mode of the
	     memory, rather than of VALUE itself.

	   - to cope with special predicates like pmode_register_operand,
	     where MODE is VOIDmode.  */
      value = copy_to_mode_reg (TYPE_MODE (TREE_TYPE (arg)), value);

      /* Check the predicate again.  */
      if (!insn_data[icode].operand[opno].predicate (value, mode))
	{
	  error ("invalid argument to built-in function");
	  return const0_rtx;
	}
    }

  return value;
}

/* Return an rtx suitable for output operand OP of instruction ICODE.
   If TARGET is non-null, try to use it where possible.  */

static rtx
mips_prepare_builtin_target (enum insn_code icode, unsigned int op, rtx target)
{
  enum machine_mode mode;

  mode = insn_data[icode].operand[op].mode;
  if (target == 0 || !insn_data[icode].operand[op].predicate (target, mode))
    target = gen_reg_rtx (mode);

  return target;
}

/* Expand a MIPS_BUILTIN_DIRECT or MIPS_BUILTIN_DIRECT_NO_TARGET function;
   HAS_TARGET_P says which.  EXP is the CALL_EXPR that calls the function
   and ICODE is the code of the associated .md pattern.  TARGET, if nonnull,
   suggests a good place to put the result.  */

static rtx
mips_expand_builtin_direct (enum insn_code icode, rtx target, tree exp,
			    bool has_target_p)
{
  rtx ops[MAX_RECOG_OPERANDS];
  int opno, argno;

  /* Map any target to operand 0.  */
  opno = 0;
  if (has_target_p)
    {
      target = mips_prepare_builtin_target (icode, opno, target);
      ops[opno] = target;
      opno++;
    }

  /* Map the arguments to the other operands.  The n_operands value
     for an expander includes match_dups and match_scratches as well as
     match_operands, so n_operands is only an upper bound on the number
     of arguments to the expander function.  */
  gcc_assert (opno + call_expr_nargs (exp) <= insn_data[icode].n_operands);
  for (argno = 0; argno < call_expr_nargs (exp); argno++, opno++)
    ops[opno] = mips_prepare_builtin_arg (icode, opno, exp, argno);

  switch (opno)
    {
    case 2:
      emit_insn (GEN_FCN (icode) (ops[0], ops[1]));
      break;

    case 3:
      emit_insn (GEN_FCN (icode) (ops[0], ops[1], ops[2]));
      break;

    case 4:
      emit_insn (GEN_FCN (icode) (ops[0], ops[1], ops[2], ops[3]));
      break;

    default:
      gcc_unreachable ();
    }
  return target;
}

/* Implement TARGET_EXPAND_BUILTIN.  */

static rtx
mips_expand_builtin (tree exp, rtx target, rtx subtarget ATTRIBUTE_UNUSED,
		     enum machine_mode mode ATTRIBUTE_UNUSED,
		     int ignore ATTRIBUTE_UNUSED)
{
  tree fndecl;
  unsigned int fcode, avail;
  const struct mips_builtin_description *d;

  fndecl = TREE_OPERAND (CALL_EXPR_FN (exp), 0);
  fcode = DECL_FUNCTION_CODE (fndecl);
  gcc_assert (fcode < ARRAY_SIZE (mips_builtins));
  d = &mips_builtins[fcode];
  avail = d->avail ();
  gcc_assert (avail != 0);
  switch (d->builtin_type)
    {
    case MIPS_BUILTIN_DIRECT:
      return mips_expand_builtin_direct (d->icode, target, exp, true);

    case MIPS_BUILTIN_DIRECT_NO_TARGET:
      return mips_expand_builtin_direct (d->icode, target, exp, false);
    }
  gcc_unreachable ();
}

/* This structure records that the current function has a LO_SUM
   involving SYMBOL_REF or LABEL_REF BASE and that MAX_OFFSET is
   the largest offset applied to BASE by all such LO_SUMs.  */
struct mips_lo_sum_offset {
  rtx base;
  HOST_WIDE_INT offset;
};

/* Return a hash value for SYMBOL_REF or LABEL_REF BASE.  */

static hashval_t
mips_hash_base (rtx base)
{
  int do_not_record_p;

  return hash_rtx (base, GET_MODE (base), &do_not_record_p, NULL, false);
}

/* Hash-table callbacks for mips_lo_sum_offsets.  */

static hashval_t
mips_lo_sum_offset_hash (const void *entry)
{
  return mips_hash_base (((const struct mips_lo_sum_offset *) entry)->base);
}

static int
mips_lo_sum_offset_eq (const void *entry, const void *value)
{
  return rtx_equal_p (((const struct mips_lo_sum_offset *) entry)->base,
		      (const_rtx) value);
}

/* Look up symbolic constant X in HTAB, which is a hash table of
   mips_lo_sum_offsets.  If OPTION is NO_INSERT, return true if X can be
   paired with a recorded LO_SUM, otherwise record X in the table.  */

static bool
mips_lo_sum_offset_lookup (htab_t htab, rtx x, enum insert_option option)
{
  rtx base, offset;
  void **slot;
  struct mips_lo_sum_offset *entry;

  /* Split X into a base and offset.  */
  split_const (x, &base, &offset);
  if (UNSPEC_ADDRESS_P (base))
    base = UNSPEC_ADDRESS (base);

  /* Look up the base in the hash table.  */
  slot = htab_find_slot_with_hash (htab, base, mips_hash_base (base), option);
  if (slot == NULL)
    return false;

  entry = (struct mips_lo_sum_offset *) *slot;
  if (option == INSERT)
    {
      if (entry == NULL)
	{
	  entry = XNEW (struct mips_lo_sum_offset);
	  entry->base = base;
	  entry->offset = INTVAL (offset);
	  *slot = entry;
	}
      else
	{
	  if (INTVAL (offset) > entry->offset)
	    entry->offset = INTVAL (offset);
	}
    }
  return INTVAL (offset) <= entry->offset;
}

/* A for_each_rtx callback for which DATA is a mips_lo_sum_offset hash table.
   Record every LO_SUM in *LOC.  */

static int
mips_record_lo_sum (rtx *loc, void *data)
{
  if (GET_CODE (*loc) == LO_SUM)
    mips_lo_sum_offset_lookup ((htab_t) data, XEXP (*loc, 1), INSERT);
  return 0;
}

/* Return true if INSN is a SET of an orphaned high-part relocation.
   HTAB is a hash table of mips_lo_sum_offsets that describes all the
   LO_SUMs in the current function.  */

static bool
mips_orphaned_high_part_p (htab_t htab, rtx insn)
{
  rtx x, set;

  set = single_set (insn);
  if (set)
    {
      /* Check for %his.  */
      x = SET_SRC (set);
      if (GET_CODE (x) == HIGH
	  && absolute_symbolic_operand (XEXP (x, 0), VOIDmode))
	return !mips_lo_sum_offset_lookup (htab, XEXP (x, 0), NO_INSERT);
    }
  return false;
}

/* Delete any high-part relocations whose partnering low parts are dead. */

static void
mips_reorg_process_insns (void)
{
  rtx insn, next_insn;
  htab_t htab;

  /* Force all instructions to be split into their final form.  */
  split_all_insns_noflow ();

  /* Recalculate instruction lengths without taking nops into account.  */
  shorten_branches (get_insns ());

  htab = htab_create (37, mips_lo_sum_offset_hash,
		      mips_lo_sum_offset_eq, free);

  /* Make a first pass over the instructions, recording all the LO_SUMs.  */
  for (insn = get_insns (); insn != 0; insn = NEXT_INSN (insn))
    if (USEFUL_INSN_P (insn))
      for_each_rtx (&PATTERN (insn), mips_record_lo_sum, htab);

  /* Make a second pass over the instructions.  Delete orphaned
     high-part relocations or turn them into NOPs.  Avoid hazards
     by inserting NOPs.  */
  for (insn = get_insns (); insn != 0; insn = next_insn)
    {
      next_insn = NEXT_INSN (insn);
      if (USEFUL_INSN_P (insn))
	{
	  /* INSN is a single instruction.  Delete it if it's an
	     orphaned high-part relocation.  */
	  if (mips_orphaned_high_part_p (htab, insn))
	    delete_insn (insn);
	}
    }

  htab_delete (htab);
}

/* Implement TARGET_MACHINE_DEPENDENT_REORG.  */

static void
mips_reorg (void)
{
  mips_reorg_process_insns ();
}

/* Implement TARGET_ASM_OUTPUT_MI_THUNK.  Generate rtl rather than asm text
   in order to avoid duplicating too much logic from elsewhere.  */

static void
mips_output_mi_thunk (FILE *file, tree thunk_fndecl ATTRIBUTE_UNUSED,
		      HOST_WIDE_INT delta, HOST_WIDE_INT vcall_offset,
		      tree function)
{
  rtx this_rtx, temp1, temp2, insn, fnaddr;
  bool use_sibcall_p;

  /* Pretend to be a post-reload pass while generating rtl.  */
  reload_completed = 1;

  /* Mark the end of the (empty) prologue.  */
  emit_note (NOTE_INSN_PROLOGUE_END);

  /* Determine if we can use a sibcall to call FUNCTION directly.  */
  fnaddr = XEXP (DECL_RTL (function), 0);
  use_sibcall_p = const_call_insn_operand (fnaddr, Pmode);

  /* Determine if we need to load FNADDR from the GOT.  */
  if (!use_sibcall_p && mips_classify_symbol (fnaddr) == SYMBOL_GOT_DISP)
    {
      cfun->machine->global_pointer = GLOBAL_POINTER_REGNUM - GP_REG_FIRST;
      cfun->machine->must_initialize_gp_p = true;
      SET_REGNO (pic_offset_table_rtx, cfun->machine->global_pointer);

      /* Set up the global pointer for n32 or n64 abicalls.  */
      mips_emit_loadgp ();
    }

  /* We need two temporary registers in some cases.  */
  temp1 = gen_rtx_REG (Pmode, 2);
  temp2 = gen_rtx_REG (Pmode, 3);

  /* Find out which register contains the "this" pointer.  */
  if (aggregate_value_p (TREE_TYPE (TREE_TYPE (function)), function))
    this_rtx = gen_rtx_REG (Pmode, GP_ARG_FIRST + 1);
  else
    this_rtx = gen_rtx_REG (Pmode, GP_ARG_FIRST);

  /* Add DELTA to THIS_RTX.  */
  if (delta != 0)
    {
      rtx offset = GEN_INT (delta);
      if (!SMALL_OPERAND (delta))
	{
	  mips_emit_move (temp1, offset);
	  offset = temp1;
	}
      emit_insn (gen_add3_insn (this_rtx, this_rtx, offset));
    }

  /* If needed, add *(*THIS_RTX + VCALL_OFFSET) to THIS_RTX.  */
  if (vcall_offset != 0)
    {
      rtx addr;

      /* Set TEMP1 to *THIS_RTX.  */
      mips_emit_move (temp1, gen_rtx_MEM (Pmode, this_rtx));

      /* Set ADDR to a legitimate address for *THIS_RTX + VCALL_OFFSET.  */
      addr = mips_add_offset (temp2, temp1, vcall_offset);

      /* Load the offset and add it to THIS_RTX.  */
      mips_emit_move (temp1, gen_rtx_MEM (Pmode, addr));
      emit_insn (gen_add3_insn (this_rtx, this_rtx, temp1));
    }

  /* Jump to the target function.  Use a sibcall if direct jumps are
     allowed, otherwise load the address into a register first.  */
  if (use_sibcall_p)
    {
      insn = emit_call_insn (gen_sibcall_internal (fnaddr, const0_rtx));
      SIBLING_CALL_P (insn) = 1;
    }
  else
    {
      if (TARGET_ABICALLS)
	temp1 = gen_rtx_REG (Pmode, PIC_FUNCTION_ADDR_REGNUM);
      mips_load_call_address (MIPS_CALL_SIBCALL, temp1, fnaddr);
      emit_jump_insn (gen_indirect_jump (temp1));
    }

  /* Run just enough of rest_of_compilation.  This sequence was
     "borrowed" from alpha.c.  */
  insn = get_insns ();
  insn_locators_alloc ();
  split_all_insns_noflow ();
  shorten_branches (insn);
  final_start_function (insn, file, 1);
  final (insn, file, 1);
  final_end_function ();

  /* Clean up the vars set above.  Note that final_end_function resets
     the global pointer for us.  */
  reload_completed = 0;
}

/* Allocate a chunk of memory for per-function machine-dependent data.  */

static struct machine_function *
mips_init_machine_status (void)
{
  return ggc_alloc_cleared_machine_function ();
}

/* Return the processor associated with the given ISA level, or null
   if the ISA isn't valid.  */

static const struct mips_cpu_info *
mips_cpu_info_from_isa (int isa)
{
  unsigned int i;

  for (i = 0; i < ARRAY_SIZE (mips_cpu_info_table); i++)
    if (mips_cpu_info_table[i].isa == isa)
      return mips_cpu_info_table + i;

  return NULL;
}

/* Return true if GIVEN is the same as CANONICAL, or if it is CANONICAL
   with a final "000" replaced by "k".  Ignore case.

   Note: this function is shared between GCC and GAS.  */

static bool
mips_strict_matching_cpu_name_p (const char *canonical, const char *given)
{
  while (*given != 0 && TOLOWER (*given) == TOLOWER (*canonical))
    given++, canonical++;

  return ((*given == 0 && *canonical == 0)
	  || (strcmp (canonical, "000") == 0 && strcasecmp (given, "k") == 0));
}

/* Return true if GIVEN matches CANONICAL, where GIVEN is a user-supplied
   CPU name.  We've traditionally allowed a lot of variation here.

   Note: this function is shared between GCC and GAS.  */

static bool
mips_matching_cpu_name_p (const char *canonical, const char *given)
{
  /* First see if the name matches exactly, or with a final "000"
     turned into "k".  */
  if (mips_strict_matching_cpu_name_p (canonical, given))
    return true;

  /* If not, try comparing based on numerical designation alone.
     See if GIVEN is an unadorned number, or 'r' followed by a number.  */
  if (TOLOWER (*given) == 'r')
    given++;
  if (!ISDIGIT (*given))
    return false;

  /* Skip over some well-known prefixes in the canonical name,
     hoping to find a number there too.  */
  if (TOLOWER (canonical[0]) == 'v' && TOLOWER (canonical[1]) == 'r')
    canonical += 2;
  else if (TOLOWER (canonical[0]) == 'r' && TOLOWER (canonical[1]) == 'm')
    canonical += 2;
  else if (TOLOWER (canonical[0]) == 'r')
    canonical += 1;

  return mips_strict_matching_cpu_name_p (canonical, given);
}

/* Return the mips_cpu_info entry for the processor or ISA given
   by CPU_STRING.  Return null if the string isn't recognized.

   A similar function exists in GAS.  */

static const struct mips_cpu_info *
mips_parse_cpu (const char *cpu_string)
{
  unsigned int i;
  const char *s;

  /* In the past, we allowed upper-case CPU names, but it doesn't
     work well with the multilib machinery.  */
  for (s = cpu_string; *s != 0; s++)
    if (ISUPPER (*s))
      {
	warning (0, "CPU names must be lower case");
	break;
      }

  /* 'from-abi' selects the most compatible architecture for the given
     ABI: MIPS I for 32-bit ABIs and MIPS III for 64-bit ABIs.  For the
     EABIs, we have to decide whether we're using the 32-bit or 64-bit
     version.  */
  if (strcasecmp (cpu_string, "from-abi") == 0)
    return mips_cpu_info_from_isa (TARGET_64BIT ? ISA_RV32 : ISA_RV64);

  for (i = 0; i < ARRAY_SIZE (mips_cpu_info_table); i++)
    if (mips_matching_cpu_name_p (mips_cpu_info_table[i].name, cpu_string))
      return mips_cpu_info_table + i;

  return NULL;
}

/* Set up globals to generate code for the ISA or processor
   described by INFO.  */

static void
mips_set_architecture (const struct mips_cpu_info *info)
{
  if (info != 0)
    {
      mips_tune = mips_arch = info->cpu;
      mips_isa = info->isa;
    }
}

/* Likewise for tuning.  */

static void
mips_set_tune (const struct mips_cpu_info *info)
{
  if (info != 0)
    mips_tune = info->cpu;
}

/* Implement TARGET_HANDLE_OPTION.  */

static bool
mips_handle_option (size_t code, const char *arg, int value ATTRIBUTE_UNUSED)
{
  switch (code)
    {
    case OPT_mabi_:
      if (strcmp (arg, "32") == 0)
	mips_abi = ABI_32;
      else if (strcmp (arg, "64") == 0)
	mips_abi = ABI_64;
      else
	return false;
      return true;

    case OPT_march_:
    case OPT_mtune_:
      return mips_parse_cpu (arg) != 0;

    default:
      return true;
    }
}

/* Implement TARGET_OPTION_OVERRIDE.  */

static void
mips_option_override (void)
{
  int i, start, regno, mode;
  const struct mips_cpu_info *info = 0;

#ifdef SUBTARGET_OVERRIDE_OPTIONS
  SUBTARGET_OVERRIDE_OPTIONS;
#endif

  /* The following code determines the architecture and register size.
     Similar code was added to GAS 2.14 (see tc-mips.c:md_after_parse_args()).
     The GAS and GCC code should be kept in sync as much as possible.  */

  if (mips_arch_string != 0)
    info = mips_parse_cpu (mips_arch_string);
  if (info == 0)
    info = mips_parse_cpu (MIPS_CPU_STRING_DEFAULT);
  gcc_assert (info);
  mips_set_architecture (info);

  /* Optimize for mips_arch, unless -mtune selects a different processor.  */
  if (mips_tune_string != 0)
    mips_set_tune (mips_parse_cpu (mips_tune_string));

  /* End of code shared with GAS.  */

  flag_pcc_struct_return = 0;

  /* Decide which rtx_costs structure to use.  */
  if (optimize_size)
    mips_cost = &mips_rtx_cost_optimize_size;
  else
    mips_cost = &mips_rtx_cost_data[mips_tune];

  /* If the user hasn't specified a branch cost, use the processor's
     default.  */
  if (mips_branch_cost == 0)
    mips_branch_cost = mips_cost->branch_cost;

  if (flag_pic)
    target_flags |= MASK_ABICALLS;

  /* Prefer a call to memcpy over inline code when optimizing for size,
     though see MOVE_RATIO in mips.h.  */
  if (optimize_size && (target_flags_explicit & MASK_MEMCPY) == 0)
    target_flags |= MASK_MEMCPY;

#ifdef MIPS_TFMODE_FORMAT
  REAL_MODE_FORMAT (TFmode) = &MIPS_TFMODE_FORMAT;
#endif

  /* .cfi_* directives generate a read-only section, so fall back on
     manual .eh_frame creation if we need the section to be writable.  */
  if (TARGET_WRITABLE_EH_FRAME)
    flag_dwarf2_cfi_asm = 0;

  /* Set up array to map GCC register number to debug register number.
     Ignore the special purpose register numbers.  */

  for (i = 0; i < FIRST_PSEUDO_REGISTER; i++)
    {
      mips_dbx_regno[i] = INVALID_REGNUM;
      if (GP_REG_P (i) || FP_REG_P (i))
	mips_dwarf_regno[i] = i;
      else
	mips_dwarf_regno[i] = INVALID_REGNUM;
    }

  start = GP_DBX_FIRST - GP_REG_FIRST;
  for (i = GP_REG_FIRST; i <= GP_REG_LAST; i++)
    mips_dbx_regno[i] = i + start;

  start = FP_DBX_FIRST - FP_REG_FIRST;
  for (i = FP_REG_FIRST; i <= FP_REG_LAST; i++)
    mips_dbx_regno[i] = i + start;

  /* Set up mips_hard_regno_mode_ok.  */
  for (mode = 0; mode < MAX_MACHINE_MODE; mode++)
    for (regno = 0; regno < FIRST_PSEUDO_REGISTER; regno++)
      mips_hard_regno_mode_ok[mode][regno]
	= mips_hard_regno_mode_ok_p (regno, (enum machine_mode) mode);

  /* Function to allocate machine-dependent function status.  */
  init_machine_status = &mips_init_machine_status;

  targetm.min_anchor_offset = -RISCV_IMM_REACH/2;
  targetm.max_anchor_offset = RISCV_IMM_REACH/2-1;

  targetm.const_anchor = RISCV_IMM_REACH/2;

  mips_init_relocs ();

  restore_target_globals (&default_target_globals);
}

/* Implement TARGET_OPTION_OPTIMIZATION_TABLE.  */
static const struct default_options mips_option_optimization_table[] =
  {
    { OPT_LEVELS_1_PLUS, OPT_fomit_frame_pointer, NULL, 1 },
    { OPT_LEVELS_NONE, 0, NULL, 0 }
  };

/* Implement TARGET_CONDITIONAL_REGISTER_USAGE.  */

static void
mips_conditional_register_usage (void)
{
  int regno;

  if (!TARGET_HARD_FLOAT)
    {
      for (regno = FP_REG_FIRST; regno <= FP_REG_LAST; regno++)
	fixed_regs[regno] = call_used_regs[regno] = 1;
    }
}

/* Initialize vector TARGET to VALS.  */

void
mips_expand_vector_init (rtx target, rtx vals)
{
  enum machine_mode mode;
  enum machine_mode inner;
  unsigned int i, n_elts;
  rtx mem;

  mode = GET_MODE (target);
  inner = GET_MODE_INNER (mode);
  n_elts = GET_MODE_NUNITS (mode);

  gcc_assert (VECTOR_MODE_P (mode));

  mem = assign_stack_temp (mode, GET_MODE_SIZE (mode), 0);
  for (i = 0; i < n_elts; i++)
    emit_move_insn (adjust_address_nv (mem, inner, i * GET_MODE_SIZE (inner)),
                    XVECEXP (vals, 0, i));

  emit_move_insn (target, mem);
}

/* Implement EPILOGUE_USES.  */

bool
mips_epilogue_uses (unsigned int regno)
{
  /* Say that the epilogue uses the return address register.  Note that
     in the case of sibcalls, the values "used by the epilogue" are
     considered live at the start of the called function.  */
  if (regno == RETURN_ADDR_REGNUM)
    return true;

  /* If using a GOT, say that the epilogue also uses GOT_VERSION_REGNUM.
     See the comment above load_call<mode> for details.  */
  if (TARGET_USE_GOT && (regno) == GOT_VERSION_REGNUM)
    return true;

  return false;
}

/* Implement TARGET_TRAMPOLINE_INIT.  */

static void
mips_trampoline_init (rtx m_tramp, tree fndecl, rtx chain_value)
{
  rtx addr, end_addr, mem;
  rtx trampoline[8];
  unsigned int i, j, gp;
  HOST_WIDE_INT static_chain_offset, target_function_offset;

  /* Work out the offsets of the pointers from the start of the
     trampoline code.  */
  static_chain_offset = TRAMPOLINE_CODE_SIZE;
  target_function_offset = static_chain_offset + GET_MODE_SIZE (ptr_mode);

  /* Get pointers to the beginning and end of the code block.  */
  addr = force_reg (Pmode, XEXP (m_tramp, 0));
  end_addr = mips_force_binary (Pmode, PLUS, addr, GEN_INT (TRAMPOLINE_CODE_SIZE));

#define OP(X) gen_int_mode (X, SImode)
#define MATCH_LREG ((Pmode) == DImode ? MATCH_LD : MATCH_LW)

  /* We want:

        rdnpc   t6
     1: l[wd]   t7, target_function_offset - 4(t6)
	l[wd]   $static_chain, static_chain_offset - 4(t6)
	jr      t7

     where 4 is the offset of "1:" from the start of the code block.  */

  i = 0;
  gp = GLOBAL_POINTER_REGNUM - GP_REG_FIRST;

  trampoline[i++] = OP (RISCV_ITYPE (RDNPC, gp, 0, 0));
  trampoline[i++] = OP (RISCV_ITYPE (LREG, PIC_FUNCTION_ADDR_REGNUM,
    				   gp, target_function_offset - 4));
  trampoline[i++] = OP (RISCV_ITYPE (LREG, STATIC_CHAIN_REGNUM,
    				   gp, static_chain_offset - 4));
  trampoline[i++] = OP (RISCV_ITYPE (JALR_J, 0, PIC_FUNCTION_ADDR_REGNUM, 0));

  gcc_assert (i * 4 == TRAMPOLINE_CODE_SIZE);

#undef MATCH_LREG
#undef OP

  /* Copy the trampoline code.  Leave any padding uninitialized.  */
  for (j = 0; j < i; j++)
    {
      mem = adjust_address (m_tramp, SImode, j * GET_MODE_SIZE (SImode));
      mips_emit_move (mem, trampoline[j]);
    }

  /* Set up the static chain pointer field.  */
  mem = adjust_address (m_tramp, ptr_mode, static_chain_offset);
  mips_emit_move (mem, chain_value);

  /* Set up the target function field.  */
  mem = adjust_address (m_tramp, ptr_mode, target_function_offset);
  mips_emit_move (mem, XEXP (DECL_RTL (fndecl), 0));

  /* Flush the code part of the trampoline.  */
  emit_insn (gen_add3_insn (end_addr, addr, GEN_INT (TRAMPOLINE_SIZE)));
  emit_insn (gen_clear_cache (addr, end_addr));
}

/* Implement TARGET_SHIFT_TRUNCATION_MASK. */

static unsigned HOST_WIDE_INT
mips_shift_truncation_mask (enum machine_mode mode)
{
  return GET_MODE_BITSIZE (mode) - 1;
}


/* Initialize the GCC target structure.  */
#undef TARGET_ASM_ALIGNED_HI_OP
#define TARGET_ASM_ALIGNED_HI_OP "\t.half\t"
#undef TARGET_ASM_ALIGNED_SI_OP
#define TARGET_ASM_ALIGNED_SI_OP "\t.word\t"
#undef TARGET_ASM_ALIGNED_DI_OP
#define TARGET_ASM_ALIGNED_DI_OP "\t.dword\t"

#undef TARGET_OPTION_OVERRIDE
#define TARGET_OPTION_OVERRIDE mips_option_override
#undef TARGET_OPTION_OPTIMIZATION_TABLE
#define TARGET_OPTION_OPTIMIZATION_TABLE mips_option_optimization_table

#undef TARGET_LEGITIMIZE_ADDRESS
#define TARGET_LEGITIMIZE_ADDRESS mips_legitimize_address

#undef TARGET_ASM_FUNCTION_PROLOGUE
#define TARGET_ASM_FUNCTION_PROLOGUE mips_output_function_prologue
#undef TARGET_ASM_FUNCTION_EPILOGUE
#define TARGET_ASM_FUNCTION_EPILOGUE mips_output_function_epilogue

#undef TARGET_SCHED_ADJUST_COST
#define TARGET_SCHED_ADJUST_COST mips_adjust_cost
#undef TARGET_SCHED_ISSUE_RATE
#define TARGET_SCHED_ISSUE_RATE mips_issue_rate

#undef TARGET_DEFAULT_TARGET_FLAGS
#define TARGET_DEFAULT_TARGET_FLAGS		\
  (TARGET_DEFAULT				\
   | TARGET_CPU_DEFAULT				\
   | TARGET_ENDIAN_DEFAULT)
#undef TARGET_HANDLE_OPTION
#define TARGET_HANDLE_OPTION mips_handle_option

#undef TARGET_FUNCTION_OK_FOR_SIBCALL
#define TARGET_FUNCTION_OK_FOR_SIBCALL hook_bool_tree_tree_true

#undef TARGET_VALID_POINTER_MODE
#define TARGET_VALID_POINTER_MODE mips_valid_pointer_mode
#undef TARGET_REGISTER_MOVE_COST
#define TARGET_REGISTER_MOVE_COST mips_register_move_cost
#undef TARGET_MEMORY_MOVE_COST
#define TARGET_MEMORY_MOVE_COST mips_memory_move_cost
#undef TARGET_RTX_COSTS
#define TARGET_RTX_COSTS mips_rtx_costs
#undef TARGET_ADDRESS_COST
#define TARGET_ADDRESS_COST mips_address_cost

#undef TARGET_MACHINE_DEPENDENT_REORG
#define TARGET_MACHINE_DEPENDENT_REORG mips_reorg

#undef  TARGET_PREFERRED_RELOAD_CLASS
#define TARGET_PREFERRED_RELOAD_CLASS mips_preferred_reload_class

#undef TARGET_ASM_FILE_START
#define TARGET_ASM_FILE_START mips_file_start
#undef TARGET_ASM_FILE_START_FILE_DIRECTIVE
#define TARGET_ASM_FILE_START_FILE_DIRECTIVE true

#undef TARGET_INIT_LIBFUNCS
#define TARGET_INIT_LIBFUNCS mips_init_libfuncs

#undef TARGET_EXPAND_BUILTIN_VA_START
#define TARGET_EXPAND_BUILTIN_VA_START mips_va_start

#undef  TARGET_PROMOTE_FUNCTION_MODE
#define TARGET_PROMOTE_FUNCTION_MODE default_promote_function_mode_always_promote
#undef TARGET_PROMOTE_PROTOTYPES
#define TARGET_PROMOTE_PROTOTYPES hook_bool_const_tree_true

#undef TARGET_RETURN_IN_MEMORY
#define TARGET_RETURN_IN_MEMORY mips_return_in_memory
#undef TARGET_RETURN_IN_MSB
#define TARGET_RETURN_IN_MSB mips_return_in_msb

#undef TARGET_ASM_OUTPUT_MI_THUNK
#define TARGET_ASM_OUTPUT_MI_THUNK mips_output_mi_thunk
#undef TARGET_ASM_CAN_OUTPUT_MI_THUNK
#define TARGET_ASM_CAN_OUTPUT_MI_THUNK hook_bool_const_tree_hwi_hwi_const_tree_true

#undef TARGET_PRINT_OPERAND
#define TARGET_PRINT_OPERAND mips_print_operand
#undef TARGET_PRINT_OPERAND_ADDRESS
#define TARGET_PRINT_OPERAND_ADDRESS mips_print_operand_address

#undef TARGET_SETUP_INCOMING_VARARGS
#define TARGET_SETUP_INCOMING_VARARGS mips_setup_incoming_varargs
#undef TARGET_STRICT_ARGUMENT_NAMING
#define TARGET_STRICT_ARGUMENT_NAMING hook_bool_CUMULATIVE_ARGS_true
#undef TARGET_MUST_PASS_IN_STACK
#define TARGET_MUST_PASS_IN_STACK must_pass_in_stack_var_size
#undef TARGET_PASS_BY_REFERENCE
#define TARGET_PASS_BY_REFERENCE hook_pass_by_reference_must_pass_in_stack
#undef TARGET_ARG_PARTIAL_BYTES
#define TARGET_ARG_PARTIAL_BYTES mips_arg_partial_bytes
#undef TARGET_FUNCTION_ARG
#define TARGET_FUNCTION_ARG mips_function_arg
#undef TARGET_FUNCTION_ARG_ADVANCE
#define TARGET_FUNCTION_ARG_ADVANCE mips_function_arg_advance
#undef TARGET_FUNCTION_ARG_BOUNDARY
#define TARGET_FUNCTION_ARG_BOUNDARY mips_function_arg_boundary

#undef TARGET_MODE_REP_EXTENDED
#define TARGET_MODE_REP_EXTENDED mips_mode_rep_extended

#undef TARGET_VECTOR_MODE_SUPPORTED_P
#define TARGET_VECTOR_MODE_SUPPORTED_P mips_vector_mode_supported_p

#undef TARGET_SCALAR_MODE_SUPPORTED_P
#define TARGET_SCALAR_MODE_SUPPORTED_P mips_scalar_mode_supported_p

#undef TARGET_VECTORIZE_PREFERRED_SIMD_MODE
#define TARGET_VECTORIZE_PREFERRED_SIMD_MODE mips_preferred_simd_mode

#undef TARGET_INIT_BUILTINS
#define TARGET_INIT_BUILTINS mips_init_builtins
#undef TARGET_BUILTIN_DECL
#define TARGET_BUILTIN_DECL mips_builtin_decl
#undef TARGET_EXPAND_BUILTIN
#define TARGET_EXPAND_BUILTIN mips_expand_builtin

#undef TARGET_HAVE_TLS
#define TARGET_HAVE_TLS HAVE_AS_TLS

#undef TARGET_CANNOT_FORCE_CONST_MEM
#define TARGET_CANNOT_FORCE_CONST_MEM mips_cannot_force_const_mem

#undef TARGET_ENCODE_SECTION_INFO
#define TARGET_ENCODE_SECTION_INFO mips_encode_section_info

#undef TARGET_ATTRIBUTE_TABLE
#define TARGET_ATTRIBUTE_TABLE mips_attribute_table
/* All our function attributes are related to how out-of-line copies should
   be compiled or called.  They don't in themselves prevent inlining.  */
#undef TARGET_FUNCTION_ATTRIBUTE_INLINABLE_P
#define TARGET_FUNCTION_ATTRIBUTE_INLINABLE_P hook_bool_const_tree_true

#undef TARGET_EXTRA_LIVE_ON_ENTRY
#define TARGET_EXTRA_LIVE_ON_ENTRY mips_extra_live_on_entry

#undef TARGET_USE_BLOCKS_FOR_CONSTANT_P
#define TARGET_USE_BLOCKS_FOR_CONSTANT_P hook_bool_mode_const_rtx_true

#undef  TARGET_COMP_TYPE_ATTRIBUTES
#define TARGET_COMP_TYPE_ATTRIBUTES mips_comp_type_attributes

#ifdef HAVE_AS_DTPRELWORD
#undef TARGET_ASM_OUTPUT_DWARF_DTPREL
#define TARGET_ASM_OUTPUT_DWARF_DTPREL mips_output_dwarf_dtprel
#endif

#undef TARGET_IRA_COVER_CLASSES
#define TARGET_IRA_COVER_CLASSES mips_ira_cover_classes

#undef TARGET_LEGITIMATE_ADDRESS_P
#define TARGET_LEGITIMATE_ADDRESS_P	mips_legitimate_address_p

#undef TARGET_FRAME_POINTER_REQUIRED
#define TARGET_FRAME_POINTER_REQUIRED mips_frame_pointer_required

#undef TARGET_CAN_ELIMINATE
#define TARGET_CAN_ELIMINATE mips_can_eliminate

#undef TARGET_CONDITIONAL_REGISTER_USAGE
#define TARGET_CONDITIONAL_REGISTER_USAGE mips_conditional_register_usage

#undef TARGET_TRAMPOLINE_INIT
#define TARGET_TRAMPOLINE_INIT mips_trampoline_init

#undef TARGET_ASM_OUTPUT_SOURCE_FILENAME
#define TARGET_ASM_OUTPUT_SOURCE_FILENAME mips_output_filename

#undef TARGET_SHIFT_TRUNCATION_MASK
#define TARGET_SHIFT_TRUNCATION_MASK mips_shift_truncation_mask

struct gcc_target targetm = TARGET_INITIALIZER;

#include "gt-riscv.h"