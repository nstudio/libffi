/* -----------------------------------------------------------------------
   ffi64.c - Copyright (c) 2013  The Written Word, Inc.
             Copyright (c) 2011  Anthony Green
             Copyright (c) 2008, 2010  Red Hat, Inc.
             Copyright (c) 2002, 2007  Bo Thorsen <bo@suse.de>

   x86-64 Foreign Function Interface

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   ``Software''), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be included
   in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED ``AS IS'', WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
   HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
   WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS IN THE SOFTWARE.
   ----------------------------------------------------------------------- */

#include <ffi.h>
#include <ffi_common.h>

#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include "internal64.h"

#ifdef __x86_64__

#define MAX_GPR_REGS 6
#define MAX_SSE_REGS 8

#if defined(__INTEL_COMPILER)
#include "xmmintrin.h"
#define UINT128 __m128
#else
#if defined(__SUNPRO_C)
#include <sunmedia_types.h>
#define UINT128 __m128i
#else
#define UINT128 __int128_t
#endif
#endif

union big_int_union
{
  UINT32 i32;
  UINT64 i64;
  UINT128 i128;
};

struct register_args
{
  /* Registers for argument passing.  */
  UINT64 gpr[MAX_GPR_REGS];
  union big_int_union sse[MAX_SSE_REGS];
  UINT64 rax;	/* ssecount */
  UINT64 r10;	/* static chain */
};

extern void ffi_call_unix64 (void *args, unsigned long bytes, unsigned flags,
			     void *raddr, void (*fnaddr)(void)) FFI_HIDDEN;

/* All reference to register classes here is identical to the code in
   gcc/config/i386/i386.c. Do *not* change one without the other.  */

/* Register class used for passing given 64bit part of the argument.
   These represent classes as documented by the PS ABI, with the
   exception of SSESF, SSEDF classes, that are basically SSE class,
   just gcc will use SF or DFmode move instead of DImode to avoid
   reformatting penalties.

   Similary we play games with INTEGERSI_CLASS to use cheaper SImode moves
   whenever possible (upper half does contain padding).  */
enum x86_64_reg_class
  {
    X86_64_NO_CLASS,
    X86_64_INTEGER_CLASS,
    X86_64_SSE_CLASS,
    X86_64_SSEUP_CLASS,
    X86_64_X87_CLASS,
    X86_64_X87UP_CLASS,
    X86_64_COMPLEX_X87_CLASS,
    X86_64_MEMORY_CLASS
  };

#define MAX_CLASSES 4

#define SSE_CLASS_P(X)	((X) >= X86_64_SSE_CLASS && X <= X86_64_SSEUP_CLASS)

/* A subroutine of is_vfp_type. Given a structure type,
 return the type code of the first non - structure element.Recurse
 for structure elements.
 Return - 1
 if the structure is in fact empty, i.e.no nested elements.*/

static int
is_hfa0(const ffi_type* ty) {
  ffi_type** elements = ty->elements;
  int i, ret = -1;

  if (elements != NULL)
    for (i = 0; elements[i]; ++i) {
      ret = elements[i]->type;
      if (ret == FFI_TYPE_STRUCT || ret == FFI_TYPE_EXT_VECTOR || ret == FFI_TYPE_COMPLEX) {
        ret = is_hfa0(elements[i]);
        if (ret < 0)
          continue;
      }
      break;
    }

  return ret;
}

/* A subroutine of is_vfp_type. Given a structure type, return true if all
 of the non-structure elements are the same as CANDIDATE.  */

static int
is_hfa1(const ffi_type* ty, int candidate) {
  ffi_type** elements = ty->elements;
  int i;

  if (elements != NULL)
    for (i = 0; elements[i]; ++i) {
      int t = elements[i]->type;
      if (t == FFI_TYPE_STRUCT || t == FFI_TYPE_COMPLEX || t == FFI_TYPE_EXT_VECTOR) {
        if (!is_hfa1(elements[i], candidate))
          return 0;
      } else if (t != candidate)
        return 0;
    }

  return 1;
}

static size_t is_simd(const ffi_type* ty) // return 0 if no SIMD elements
{
  if (ty->type == FFI_TYPE_EXT_VECTOR || ty->type == FFI_TYPE_COMPLEX) {
    return ty->size;
  }
  ffi_type ** elements = ty->elements;
  int i;

  if (elements != NULL) {
    for (i = 0; elements[i]; ++i) {
      int element_type = elements[i]->type;
      if (element_type == FFI_TYPE_STRUCT || element_type == FFI_TYPE_COMPLEX || element_type == FFI_TYPE_EXT_VECTOR) {
        return is_simd(elements[i]);
      }
    }
  }

  return 0;

}

int num_registers(ffi_type* type) {
  int candidate;
  size_t size, ele_count, simd_size;

  ffi_type** elements = type->elements;
  size = type->size;
  candidate = type->elements[0]->type;
  simd_size = is_simd(type);
  if (candidate == FFI_TYPE_STRUCT || candidate == FFI_TYPE_COMPLEX || candidate == FFI_TYPE_EXT_VECTOR) {
    for (int i = 0;; ++i) {
      candidate = is_hfa0(elements[i]);
      if (candidate >= 0)
        break;
    }
  }

  switch (candidate) {
    case FFI_TYPE_FLOAT:
      ele_count = size / sizeof(float);
      if (size != ele_count * sizeof(float))
        return 0;
      break;
    case FFI_TYPE_DOUBLE:
      ele_count = size / sizeof(double);
      if (size != ele_count * sizeof(double))
        return 0;
      break;
    case FFI_TYPE_LONGDOUBLE:
      ele_count = size / sizeof(long double);
      if (size != ele_count * sizeof(long double))
        return 0;
      break;
    default:
        return 0;
  }
  if ((ele_count > 4 && !simd_size) || (simd_size && ele_count > 16))
    return 0;

  /* Finally, make sure that all scalar elements are the same type.  */
  for (int i = 0; elements[i]; ++i) {
    int t = elements[i]->type;
    if (t == FFI_TYPE_STRUCT || t == FFI_TYPE_COMPLEX || t == FFI_TYPE_EXT_VECTOR) {
      if (!is_hfa1(elements[i], candidate))
        return 0;
    } else if (t != candidate)
        return 0;
  }

  if (simd_size) {
	/* Third element in double3 vectors is in ST0.  */
    if (candidate == FFI_TYPE_DOUBLE && ele_count == 3) {
      return 3;
    }
    size_t regSize = simd_size > 16 ? 16 : simd_size;
    return (int) size / regSize + (size % regSize ? 1 : 0);
  }
  return 0;
}

static enum x86_64_reg_class normalize_sse_class(enum x86_64_reg_class class, int byte_offset, _Bool is_vector) {
  if (SSE_CLASS_P(class)) {
    if (is_vector) {
      return byte_offset >= 8 ? X86_64_SSEUP_CLASS : X86_64_SSE_CLASS;
    }
    return X86_64_SSE_CLASS;
  }

  return class;
}

/* x86-64 register passing implementation.  See x86-64 ABI for details.  Goal
   of this code is to classify each 8bytes of incoming argument by the register
   class and assign registers accordingly.  */

/* Return the union class of CLASS1 and CLASS2.
   See the x86-64 PS ABI for details.  */

static enum x86_64_reg_class
merge_classes (enum x86_64_reg_class class1, enum x86_64_reg_class class2, int byte_offset, _Bool is_vector)
{
  /* Rule #1: If both classes are equal, this is the resulting class.  */
  if (class1 == class2)
    return normalize_sse_class(class1, byte_offset, is_vector);

  /* Rule #2: If one of the classes is NO_CLASS, the resulting class is
     the other class.  */
  if (class1 == X86_64_NO_CLASS)
    return normalize_sse_class(class2, byte_offset, is_vector);
  if (class2 == X86_64_NO_CLASS)
    return normalize_sse_class(class1, byte_offset, is_vector);

  /* Rule #3: If one of the classes is MEMORY, the result is MEMORY.  */
  if (class1 == X86_64_MEMORY_CLASS || class2 == X86_64_MEMORY_CLASS)
    return X86_64_MEMORY_CLASS;

  /* Rule #4: If one of the classes is INTEGER, the result is INTEGER.  */
  if (class1 == X86_64_INTEGER_CLASS || class2 == X86_64_INTEGER_CLASS)
      return X86_64_INTEGER_CLASS;

  /* Rule #5: If one of the classes is X87, X87UP, or COMPLEX_X87 class,
     MEMORY is used.  */
  if (class1 == X86_64_X87_CLASS
      || class1 == X86_64_X87UP_CLASS
      || class1 == X86_64_COMPLEX_X87_CLASS
      || class2 == X86_64_X87_CLASS
      || class2 == X86_64_X87UP_CLASS
      || class2 == X86_64_COMPLEX_X87_CLASS)
    return X86_64_MEMORY_CLASS;
  
  return byte_offset >= 8 ? X86_64_SSEUP_CLASS : X86_64_SSE_CLASS;
}

/* Classify the argument of type TYPE and mode MODE.
   CLASSES will be filled by the register class used to pass each word
   of the operand.  The number of words is returned.  In case the parameter
   should be passed in memory, 0 is returned. As a special case for zero
   sized containers, classes[0] will be NO_CLASS and 1 is returned.

   See the x86-64 PS ABI for details.
*/
static size_t
classify_argument (ffi_type *type, enum x86_64_reg_class classes[],
		   size_t byte_offset, _Bool is_vector)
{
  switch (type->type)
    {
    case FFI_TYPE_UINT8:
    case FFI_TYPE_SINT8:
    case FFI_TYPE_UINT16:
    case FFI_TYPE_SINT16:
    case FFI_TYPE_UINT32:
    case FFI_TYPE_SINT32:
    case FFI_TYPE_UINT64:
    case FFI_TYPE_SINT64:
    case FFI_TYPE_POINTER:
    do_integer:
      {
	size_t size = byte_offset + type->size;

	if (size <= 4)
	  {
	    classes[0] = X86_64_INTEGER_CLASS;
	    return 1;
	  }
	else if (size <= 8)
	  {
	    classes[0] = X86_64_INTEGER_CLASS;
	    return 1;
	  }
	else if (size <= 12)
	  {
	    classes[0] = X86_64_INTEGER_CLASS;
	    classes[1] = X86_64_INTEGER_CLASS;
	    return 2;
	  }
	else if (size <= 16)
	  {
	    classes[0] = classes[1] = X86_64_INTEGER_CLASS;
	    return 2;
	  }
	else
	  FFI_ASSERT (0);
      }
    case FFI_TYPE_FLOAT:
      classes[0] = X86_64_SSE_CLASS;
      return 1;
    case FFI_TYPE_DOUBLE:
      classes[0] = X86_64_SSE_CLASS;
      return 1;
#if FFI_TYPE_LONGDOUBLE != FFI_TYPE_DOUBLE
    case FFI_TYPE_LONGDOUBLE:
      classes[0] = X86_64_X87_CLASS;
      classes[1] = X86_64_X87UP_CLASS;
      return 2;
#endif
    case FFI_TYPE_STRUCT:
      // If the size of an object is larger than two eightbytes, or in C++, is a nonPOD
      // structure or union type, or contains unaligned fields, it has class
      // MEMORY.
      if (type->size > 16) {
          return 0;
      }
      // fallthrough case
    case FFI_TYPE_EXT_VECTOR:
      {
	const size_t UNITS_PER_WORD = 8;
	size_t words = (type->size + UNITS_PER_WORD - 1) / UNITS_PER_WORD;
	ffi_type **ptr;
	unsigned int i;
	enum x86_64_reg_class subclasses[MAX_CLASSES];

	/* If the struct is larger than 32 bytes, pass it on the stack.  */
	if (type->size > 32)
	  return 0;

	for (i = 0; i < words; i++)
	  classes[i] = X86_64_NO_CLASS;

	/* Zero sized arrays or structures are NO_CLASS.  We return 0 to
	   signalize memory class, so handle it as special case.  */
	if (!words)
	  {
    case FFI_TYPE_VOID:
	    classes[0] = X86_64_NO_CLASS;
	    return 1;
	  }

	/* Merge the fields of structure.  */
	for (ptr = type->elements; *ptr != NULL; ptr++)
	  {
	    size_t num;

	    byte_offset = FFI_ALIGN (byte_offset, (*ptr)->alignment);

	    num = classify_argument (*ptr, subclasses, byte_offset % 8, is_vector);
	    if (num == 0)
	      return 0;
	    for (i = 0; i < num; i++)
	      {
		size_t pos = byte_offset / 8;
		classes[i + pos] =
		  merge_classes (subclasses[i], classes[i + pos], (int)byte_offset, is_vector);
	      }

	    byte_offset += (*ptr)->size;
	  }

	if (words > 2)
	  {
	    /* When size > 16 bytes, if the first one isn't
	       X86_64_SSE_CLASS or any other ones aren't
	       X86_64_SSEUP_CLASS, everything should be passed in
	       memory.  */
	    if (classes[0] != X86_64_SSE_CLASS)
	      return 0;

	    for (i = 1; i < words; i++)
	      if (classes[i] != X86_64_SSEUP_CLASS)
		return 0;
	  }

	/* Final merger cleanup.  */
	for (i = 0; i < words; i++)
	  {
	    /* If one class is MEMORY, everything should be passed in
	       memory.  */
	    if (classes[i] == X86_64_MEMORY_CLASS)
	      return 0;

	    /* The X86_64_SSEUP_CLASS should be always preceded by
	       X86_64_SSE_CLASS or X86_64_SSEUP_CLASS.  */
	    if (classes[i] == X86_64_SSEUP_CLASS
		&& classes[i - 1] != X86_64_SSE_CLASS
		&& classes[i - 1] != X86_64_SSEUP_CLASS)
	      {
		/* The first one should never be X86_64_SSEUP_CLASS.  */
		FFI_ASSERT (i != 0);
		classes[i] = X86_64_SSE_CLASS;
	      }

	    /*  If X86_64_X87UP_CLASS isn't preceded by X86_64_X87_CLASS,
		everything should be passed in memory.  */
	    if (classes[i] == X86_64_X87UP_CLASS
		&& (classes[i - 1] != X86_64_X87_CLASS))
	      {
		/* The first one should never be X86_64_X87UP_CLASS.  */
		FFI_ASSERT (i != 0);
		return 0;
	      }
	  }
	return words;
      }
    case FFI_TYPE_COMPLEX:
      {
	ffi_type *inner = type->elements[0];
	switch (inner->type)
	  {
	  case FFI_TYPE_INT:
	  case FFI_TYPE_UINT8:
	  case FFI_TYPE_SINT8:
	  case FFI_TYPE_UINT16:
	  case FFI_TYPE_SINT16:
	  case FFI_TYPE_UINT32:
	  case FFI_TYPE_SINT32:
	  case FFI_TYPE_UINT64:
	  case FFI_TYPE_SINT64:
	    goto do_integer;

	  case FFI_TYPE_FLOAT:
	    classes[0] = X86_64_SSE_CLASS;
      return 1;
	  case FFI_TYPE_DOUBLE:
	    classes[0] = classes[1] = X86_64_SSE_CLASS;
	    return 2;
#if FFI_TYPE_LONGDOUBLE != FFI_TYPE_DOUBLE
	  case FFI_TYPE_LONGDOUBLE:
	    classes[0] = X86_64_COMPLEX_X87_CLASS;
	    return 1;
#endif
	  }
      }
    }
  abort();
}

/* Examine the argument and return set number of register required in each
   class.  Return zero iff parameter should be passed in memory, otherwise
   the number of registers.  */

static size_t
examine_argument (ffi_type *type, enum x86_64_reg_class classes[MAX_CLASSES],
		  _Bool in_return, int *pngpr, int *pnsse, _Bool is_vector)
{
  size_t n;
  unsigned int i;
  int ngpr, nsse;

  n = classify_argument (type, classes, 0, is_vector);
  if (n == 0)
    return 0;

  ngpr = nsse = 0;
  for (i = 0; i < n; ++i)
    switch (classes[i])
      {
      case X86_64_INTEGER_CLASS:
	ngpr++;
	break;
      case X86_64_SSE_CLASS:
      case X86_64_SSEUP_CLASS:
	nsse++;
	break;
      case X86_64_NO_CLASS:
	break;
      case X86_64_X87_CLASS:
      case X86_64_X87UP_CLASS:
      case X86_64_COMPLEX_X87_CLASS:
	return in_return != 0;
      default:
	abort ();
      }

  *pngpr = ngpr;
  *pnsse = nsse;

  return n;
}

/* Perform machine dependent cif processing.  */

#ifndef __ILP32__
extern ffi_status
ffi_prep_cif_machdep_efi64(ffi_cif *cif);
#endif

ffi_status
ffi_prep_cif_machdep (ffi_cif *cif)
{
  int gprcount, ssecount, i, avn, ngpr, nsse;
  unsigned flags;
  enum x86_64_reg_class classes[MAX_CLASSES];
  size_t bytes, n, rtype_size;
  ffi_type *rtype;

#ifndef __ILP32__
  if (cif->abi == FFI_EFI64)
    return ffi_prep_cif_machdep_efi64(cif);
#endif
  if (cif->abi != FFI_UNIX64)
    return FFI_BAD_ABI;

  gprcount = ssecount = 0;

  rtype = cif->rtype;
  rtype_size = rtype->size;
  switch (rtype->type)
    {
    case FFI_TYPE_VOID:
      flags = UNIX64_RET_VOID;
      break;
    case FFI_TYPE_UINT8:
      flags = UNIX64_RET_UINT8;
      break;
    case FFI_TYPE_SINT8:
      flags = UNIX64_RET_SINT8;
      break;
    case FFI_TYPE_UINT16:
      flags = UNIX64_RET_UINT16;
      break;
    case FFI_TYPE_SINT16:
      flags = UNIX64_RET_SINT16;
      break;
    case FFI_TYPE_UINT32:
      flags = UNIX64_RET_UINT32;
      break;
    case FFI_TYPE_INT:
    case FFI_TYPE_SINT32:
      flags = UNIX64_RET_SINT32;
      break;
    case FFI_TYPE_UINT64:
    case FFI_TYPE_SINT64:
      flags = UNIX64_RET_INT64;
      break;
    case FFI_TYPE_POINTER:
      flags = (sizeof(void *) == 4 ? UNIX64_RET_UINT32 : UNIX64_RET_INT64);
      break;
    case FFI_TYPE_FLOAT:
      flags = UNIX64_RET_XMM32;
      break;
    case FFI_TYPE_DOUBLE:
      flags = UNIX64_RET_XMM64;
      break;
    case FFI_TYPE_LONGDOUBLE:
      flags = UNIX64_RET_X87;
      break;
    case FFI_TYPE_STRUCT:
    case FFI_TYPE_EXT_VECTOR:
      n = examine_argument (cif->rtype, classes, 1, &ngpr, &nsse, rtype->type == FFI_TYPE_EXT_VECTOR);
      if (n == 0)
	{
	  /* The return value is passed in memory.  A pointer to that
	     memory is the first argument.  Allocate a register for it.  */
	  gprcount++;
	  /* We don't have to do anything in asm for the return.  */
	  flags = UNIX64_RET_VOID | UNIX64_FLAG_RET_IN_MEM;
	}
      else
	{
	  _Bool sse0 = SSE_CLASS_P (classes[0]);

	  if (rtype_size == 4 && sse0)
	    flags = UNIX64_RET_XMM32;
	  else if (rtype_size == 8)
	    flags = sse0 ? UNIX64_RET_XMM64 : UNIX64_RET_INT64;
	  else
	    {
	    _Bool sse1 = n == 2 && SSE_CLASS_P (classes[1]);
	    if (sse0) {
          int num_regs = num_registers(cif->rtype);
          if (num_regs == 1) {
            flags = UNIX64_RET_ST_XMM0;
          } else if (num_regs == 2) {
			/* matrix_float2x2 needs all 128 bits of the registers */
            flags = rtype_size > 16 ? UNIX64_RET_ST_XMM0_XMM1_128 : UNIX64_RET_ST_XMM0_XMM1_64;
          } else if (num_regs == 3) {
			/* third element in double3 vector is in ST0 */
            flags = UNIX64_RET_X86_ST0;
          } else if (sse1) {
			/* if num_regs == 0 && sse0 && sse1 => we have a struct with size < 16 */
            flags = UNIX64_RET_ST_XMM0_XMM1_64;
          } else {
            flags = UNIX64_RET_ST_XMM0_RAX;
          }
        } else if (sse1) {
          flags = UNIX64_RET_ST_RAX_XMM0;
        } else {
          flags = UNIX64_RET_ST_RAX_RDX;
        }
	      flags |= rtype_size << UNIX64_SIZE_SHIFT;
	    }
	}
      break;
    case FFI_TYPE_COMPLEX:
      switch (rtype->elements[0]->type)
	{
	case FFI_TYPE_UINT8:
	case FFI_TYPE_SINT8:
	case FFI_TYPE_UINT16:
	case FFI_TYPE_SINT16:
	case FFI_TYPE_INT:
	case FFI_TYPE_UINT32:
	case FFI_TYPE_SINT32:
	case FFI_TYPE_UINT64:
	case FFI_TYPE_SINT64:
	  flags = UNIX64_RET_ST_RAX_RDX | ((unsigned) rtype_size << UNIX64_SIZE_SHIFT);
	  break;
	case FFI_TYPE_FLOAT:
	  flags = UNIX64_RET_XMM64;
	  break;
	case FFI_TYPE_DOUBLE:
	  flags = UNIX64_RET_ST_XMM0_XMM1_64 | (16 << UNIX64_SIZE_SHIFT);
	  break;
#if FFI_TYPE_LONGDOUBLE != FFI_TYPE_DOUBLE
	case FFI_TYPE_LONGDOUBLE:
	  flags = UNIX64_RET_X87_2;
	  break;
#endif
	default:
	  return FFI_BAD_TYPEDEF;
	}
      break;
    default:
      return FFI_BAD_TYPEDEF;
    }

  /* Go over all arguments and determine the way they should be passed.
     If it's in a register and there is space for it, let that be so. If
     not, add it's size to the stack byte count.  */
  for (bytes = 0, i = 0, avn = cif->nargs; i < avn; i++)
    {
      if (examine_argument (cif->arg_types[i], classes, 0, &ngpr, &nsse, rtype->type == FFI_TYPE_EXT_VECTOR) == 0
	  || gprcount + ngpr > MAX_GPR_REGS
	  || ssecount + nsse > MAX_SSE_REGS)
	{
	  long align = cif->arg_types[i]->alignment;

	  if (align < 8)
	    align = 8;

	  bytes = FFI_ALIGN (bytes, align);
	  bytes += cif->arg_types[i]->size;
	}
      else
	{
	  gprcount += ngpr;
	  ssecount += nsse;
	}
    }
  if (ssecount)
    flags |= UNIX64_FLAG_XMM_ARGS;

  cif->flags = flags;
  cif->bytes = (unsigned) FFI_ALIGN (bytes, 8);

  return FFI_OK;
}

static void
ffi_call_int (ffi_cif *cif, void (*fn)(void), void *rvalue,
	      void **avalue, void *closure)
{
  enum x86_64_reg_class classes[MAX_CLASSES];
  char *stack, *argp;
  ffi_type **arg_types;
  int gprcount, ssecount, ngpr, nsse, i, avn, flags;
  struct register_args *reg_args;

  /* Can't call 32-bit mode from 64-bit mode.  */
  FFI_ASSERT (cif->abi == FFI_UNIX64);

  /* If the return value is a struct and we don't have a return value
     address then we need to make one.  Otherwise we can ignore it.  */
  flags = cif->flags;
  if (rvalue == NULL)
    {
      if (flags & UNIX64_FLAG_RET_IN_MEM)
	rvalue = alloca (cif->rtype->size);
      else
	flags = UNIX64_RET_VOID;
    }

  /* Allocate the space for the arguments, plus 4 words of temp space.  */
  stack = alloca (sizeof (struct register_args) + cif->bytes + 4*8);
  reg_args = (struct register_args *) stack;
  argp = stack + sizeof (struct register_args);

  reg_args->r10 = (uintptr_t) closure;

  gprcount = ssecount = 0;

  /* If the return value is passed in memory, add the pointer as the
     first integer argument.  */
  if (flags & UNIX64_FLAG_RET_IN_MEM)
    reg_args->gpr[gprcount++] = (unsigned long) rvalue;

  avn = cif->nargs;
  arg_types = cif->arg_types;

  for (i = 0; i < avn; ++i)
    {
      size_t n, size = arg_types[i]->size;

      n = examine_argument (arg_types[i], classes, 0, &ngpr, &nsse, cif->rtype->type == FFI_TYPE_EXT_VECTOR);
      if (n == 0
	  || gprcount + ngpr > MAX_GPR_REGS
	  || ssecount + nsse > MAX_SSE_REGS)
	{
	  long align = arg_types[i]->alignment;

	  /* Stack arguments are *always* at least 8 byte aligned.  */
	  if (align < 8)
	    align = 8;

	  /* Pass this argument in memory.  */
	  argp = (void *) FFI_ALIGN (argp, align);
	  memcpy (argp, avalue[i], size);
	  argp += size;
	}
      else
	{
	  /* The argument is passed entirely in registers.  */
	  char *a = (char *) avalue[i];
	  unsigned int j;

	  for (j = 0; j < n; j++, a += 8, size -= 8)
	    {
	      switch (classes[j])
		{
		case X86_64_NO_CLASS:
		  break;
		case X86_64_INTEGER_CLASS:
		  /* Sign-extend integer arguments passed in general
		     purpose registers, to cope with the fact that
		     LLVM incorrectly assumes that this will be done
		     (the x86-64 PS ABI does not specify this). */
		  switch (arg_types[i]->type)
		    {
		    case FFI_TYPE_SINT8:
		      reg_args->gpr[gprcount] = (SINT64) *((SINT8 *) a);
		      break;
		    case FFI_TYPE_SINT16:
		      reg_args->gpr[gprcount] = (SINT64) *((SINT16 *) a);
		      break;
		    case FFI_TYPE_SINT32:
		      reg_args->gpr[gprcount] = (SINT64) *((SINT32 *) a);
		      break;
		    default:
		      reg_args->gpr[gprcount] = 0;
		      memcpy (&reg_args->gpr[gprcount], a, size);
		    }
		    gprcount++;
		    break;
		case X86_64_SSE_CLASS:
          if (size > 4) {
            memcpy (&reg_args->sse[ssecount++].i128, a, sizeof(UINT64));
          } else {
            memcpy (&reg_args->sse[ssecount++].i32, a, sizeof(UINT32));
          }
          break;
		case X86_64_SSEUP_CLASS:
          if (j%2) {
            memcpy (&reg_args->sse[ssecount-1].i128 + 1, a, sizeof(UINT64));
          } else {
            memcpy (&reg_args->sse[ssecount++].i128, a, sizeof(UINT64));
          }
		  break;
		default:
		  abort();
		}
	    }
	}
    }
  reg_args->rax = ssecount;

  ffi_call_unix64 (stack, cif->bytes + sizeof (struct register_args),
		   flags, rvalue, fn);
}

#ifndef __ILP32__
extern void
ffi_call_efi64(ffi_cif *cif, void (*fn)(void), void *rvalue, void **avalue);
#endif

void
ffi_call (ffi_cif *cif, void (*fn)(void), void *rvalue, void **avalue)
{
#ifndef __ILP32__
  if (cif->abi == FFI_EFI64)
    return ffi_call_efi64(cif, fn, rvalue, avalue);
#endif
  ffi_call_int (cif, fn, rvalue, avalue, NULL);
}

#ifndef __ILP32__
extern void
ffi_call_go_efi64(ffi_cif *cif, void (*fn)(void), void *rvalue,
		  void **avalue, void *closure);
#endif

void
ffi_call_go (ffi_cif *cif, void (*fn)(void), void *rvalue,
	     void **avalue, void *closure)
{
#ifndef __ILP32__
  if (cif->abi == FFI_EFI64)
    ffi_call_go_efi64(cif, fn, rvalue, avalue, closure);
#endif
  ffi_call_int (cif, fn, rvalue, avalue, closure);
}


extern void ffi_closure_unix64(void) FFI_HIDDEN;
extern void ffi_closure_unix64_sse(void) FFI_HIDDEN;

#ifndef __ILP32__
extern ffi_status
ffi_prep_closure_loc_efi64(ffi_closure* closure,
			   ffi_cif* cif,
			   void (*fun)(ffi_cif*, void*, void**, void*),
			   void *user_data,
			   void *codeloc);
#endif

ffi_status
ffi_prep_closure_loc (ffi_closure* closure,
		      ffi_cif* cif,
		      void (*fun)(ffi_cif*, void*, void**, void*),
		      void *user_data,
		      void *codeloc)
{
  static const unsigned char trampoline[16] = {
    /* leaq  -0x7(%rip),%r10   # 0x0  */
    0x4c, 0x8d, 0x15, 0xf9, 0xff, 0xff, 0xff,
    /* jmpq  *0x3(%rip)        # 0x10 */
    0xff, 0x25, 0x03, 0x00, 0x00, 0x00,
    /* nopl  (%rax) */
    0x0f, 0x1f, 0x00
  };
  void (*dest)(void);
  char *tramp = closure->tramp;

#ifndef __ILP32__
  if (cif->abi == FFI_EFI64)
    return ffi_prep_closure_loc_efi64(closure, cif, fun, user_data, codeloc);
#endif
  if (cif->abi != FFI_UNIX64)
    return FFI_BAD_ABI;

  if (cif->flags & UNIX64_FLAG_XMM_ARGS)
    dest = ffi_closure_unix64_sse;
  else
    dest = ffi_closure_unix64;

  memcpy (tramp, trampoline, sizeof(trampoline));
  *(UINT64 *)(tramp + 16) = (uintptr_t)dest;

  closure->cif = cif;
  closure->fun = fun;
  closure->user_data = user_data;

  return FFI_OK;
}

int FFI_HIDDEN
ffi_closure_unix64_inner(ffi_cif *cif,
			 void (*fun)(ffi_cif*, void*, void**, void*),
			 void *user_data,
			 void *rvalue,
			 struct register_args *reg_args,
			 char *argp)
{
  void **avalue;
  ffi_type **arg_types;
  long i, avn;
  int gprcount, ssecount, ngpr, nsse;
  int flags;

  avn = cif->nargs;
  flags = cif->flags;
  avalue = alloca(avn * sizeof(void *));
  gprcount = ssecount = 0;

  if (flags & UNIX64_FLAG_RET_IN_MEM)
    {
      /* On return, %rax will contain the address that was passed
	 by the caller in %rdi.  */
      void *r = (void *)(uintptr_t)reg_args->gpr[gprcount++];
      *(void **)rvalue = r;
      rvalue = r;
      flags = (sizeof(void *) == 4 ? UNIX64_RET_UINT32 : UNIX64_RET_INT64);
    }

  arg_types = cif->arg_types;
  for (i = 0; i < avn; ++i)
    {
      enum x86_64_reg_class classes[MAX_CLASSES];
      size_t n;

      n = examine_argument (arg_types[i], classes, 0, &ngpr, &nsse, cif->rtype == FFI_TYPE_EXT_VECTOR);
      if (n == 0
	  || gprcount + ngpr > MAX_GPR_REGS
	  || ssecount + nsse > MAX_SSE_REGS)
	{
	  long align = arg_types[i]->alignment;

	  /* Stack arguments are *always* at least 8 byte aligned.  */
	  if (align < 8)
	    align = 8;

	  /* Pass this argument in memory.  */
	  argp = (void *) FFI_ALIGN (argp, align);
	  avalue[i] = argp;
	  argp += arg_types[i]->size;
	}
      /* If the argument is in a single register, or two consecutive
	 integer registers, then we can use that address directly.  */
      else if (n == 1
	       || (n == 2 && !(SSE_CLASS_P (classes[0])
			       || SSE_CLASS_P (classes[1]))))
	{
	  /* The argument is in a single register.  */
	  if (SSE_CLASS_P (classes[0]))
	    {
	      avalue[i] = &reg_args->sse[ssecount];
	      ssecount += n;
	    }
	  else
	    {
	      avalue[i] = &reg_args->gpr[gprcount];
	      gprcount += n;
	    }
	}
      /* Otherwise, allocate space to make them consecutive.  */
      else
	{
	  char *a = alloca (16);
	  unsigned int j;

	  avalue[i] = a;
	  for (j = 0; j < n; j++, a += 8)
	    {
	      if (SSE_CLASS_P (classes[j]))
		memcpy (a, &reg_args->sse[ssecount++], 8);
	      else
		memcpy (a, &reg_args->gpr[gprcount++], 8);
	    }
	}
    }

  /* Invoke the closure.  */
  fun (cif, rvalue, avalue, user_data);

  /* Tell assembly how to perform return type promotions.  */
  return flags;
}

extern void ffi_go_closure_unix64(void) FFI_HIDDEN;
extern void ffi_go_closure_unix64_sse(void) FFI_HIDDEN;

#ifndef __ILP32__
extern ffi_status
ffi_prep_go_closure_efi64(ffi_go_closure* closure, ffi_cif* cif,
			  void (*fun)(ffi_cif*, void*, void**, void*));
#endif

ffi_status
ffi_prep_go_closure (ffi_go_closure* closure, ffi_cif* cif,
		     void (*fun)(ffi_cif*, void*, void**, void*))
{
#ifndef __ILP32__
  if (cif->abi == FFI_EFI64)
    return ffi_prep_go_closure_efi64(closure, cif, fun);
#endif
  if (cif->abi != FFI_UNIX64)
    return FFI_BAD_ABI;

  closure->tramp = (cif->flags & UNIX64_FLAG_XMM_ARGS
		    ? ffi_go_closure_unix64_sse
		    : ffi_go_closure_unix64);
  closure->cif = cif;
  closure->fun = fun;

  return FFI_OK;
}

#endif /* __x86_64__ */
