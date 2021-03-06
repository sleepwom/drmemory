/* **********************************************************
 * Copyright (c) 2012 Google, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Google, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL GOOGLE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#ifndef _INSTR_INLINE_H_
#define _INSTR_INLINE_H_ 1

/* DR_API EXPORT TOFILE dr_ir_instr.h */
/* DR_API EXPORT BEGIN */

#ifdef DR_FAST_IR

#ifdef AVOID_API_EXPORT
# define MAKE_OPNDS_VALID(instr) \
    (void)(TEST(INSTR_OPERANDS_VALID, (instr)->flags) ? \
           (instr) : instr_decode_with_current_dcontext(instr))
#endif

/* CLIENT_ASSERT with a trailing comma in a debug build, otherwise nothing. */
#define CLIENT_ASSERT_(cond, msg) IF_DEBUG_(CLIENT_ASSERT(cond, msg))

#ifdef API_EXPORT_ONLY
/* Internally DR has multiple levels of IR, but once it gets to a client, we
 * assume it's already level 3 or higher, and we don't need to do any checks.
 * Furthermore, instr_decode() and get_thread_private_dcontext() are not
 * exported.
 */
#define MAKE_OPNDS_VALID(instr) ((void)0)
/* Turn off checks if a client includes us with DR_FAST_IR.  We can't call the
 * internal routines we'd use for these checks anyway.
 */
#define CLIENT_ASSERT(cond, msg)
#define IF_DEBUG(stmt)
#define IF_DEBUG_(stmt)
#endif

/* Any function that takes or returns an opnd_t by value should be a macro,
 * *not* an inline function.  Most widely available versions of gcc have trouble
 * optimizing structs that have been passed by value, even after inlining.
 */

/* opnd_t predicates */

/* Simple predicates */
#define OPND_IS_NULL(op)        ((op).kind == NULL_kind)
#define OPND_IS_IMMED_INT(op)   ((op).kind == IMMED_INTEGER_kind)
#define OPND_IS_IMMED_FLOAT(op) ((op).kind == IMMED_FLOAT_kind)
#define OPND_IS_NEAR_PC(op)     ((op).kind == PC_kind)
#define OPND_IS_NEAR_INSTR(op)  ((op).kind == INSTR_kind)
#define OPND_IS_REG(op)         ((op).kind == REG_kind)
#define OPND_IS_BASE_DISP(op)   ((op).kind == BASE_DISP_kind)
#define OPND_IS_FAR_PC(op)      ((op).kind == FAR_PC_kind)
#define OPND_IS_FAR_INSTR(op)   ((op).kind == FAR_INSTR_kind)
#define OPND_IS_MEM_INSTR(op)   ((op).kind == MEM_INSTR_kind)
#define OPND_IS_VALID(op)       ((op).kind < LAST_kind)

#define opnd_is_null            OPND_IS_NULL
#define opnd_is_immed_int       OPND_IS_IMMED_INT
#define opnd_is_immed_float     OPND_IS_IMMED_FLOAT
#define opnd_is_near_pc         OPND_IS_NEAR_PC
#define opnd_is_near_instr      OPND_IS_NEAR_INSTR
#define opnd_is_reg             OPND_IS_REG
#define opnd_is_base_disp       OPND_IS_BASE_DISP
#define opnd_is_far_pc          OPND_IS_FAR_PC
#define opnd_is_far_instr       OPND_IS_FAR_INSTR
#define opnd_is_mem_instr       OPND_IS_MEM_INSTR
#define opnd_is_valid           OPND_IS_VALID

/* Compound predicates */
INSTR_INLINE
bool
opnd_is_immed(opnd_t op)
{
    return op.kind == IMMED_INTEGER_kind || op.kind == IMMED_FLOAT_kind;
}

INSTR_INLINE
bool
opnd_is_pc(opnd_t op) {
    return op.kind == PC_kind || op.kind == FAR_PC_kind;
}

INSTR_INLINE
bool
opnd_is_instr(opnd_t op)
{
    return op.kind == INSTR_kind || op.kind == FAR_INSTR_kind;
}

INSTR_INLINE
bool
opnd_is_near_base_disp(opnd_t op)
{
    return op.kind == BASE_DISP_kind && op.seg.segment == DR_REG_NULL;
}

INSTR_INLINE
bool
opnd_is_far_base_disp(opnd_t op)
{
    return op.kind == BASE_DISP_kind && op.seg.segment != DR_REG_NULL;
}


#ifdef X64
# define OPND_IS_REL_ADDR(op)   ((op).kind == REL_ADDR_kind)
# define opnd_is_rel_addr       OPND_IS_REL_ADDR

INSTR_INLINE
bool
opnd_is_near_rel_addr(opnd_t opnd)
{
    return opnd.kind == REL_ADDR_kind && opnd.seg.segment == DR_REG_NULL;
}

INSTR_INLINE
bool
opnd_is_far_rel_addr(opnd_t opnd)
{
    return opnd.kind == REL_ADDR_kind && opnd.seg.segment != DR_REG_NULL;
}
#endif /* X64 */

/* opnd_t constructors */

/* XXX: How can we macro-ify these?  We can use C99 initializers or a copy from
 * a constant, but that implies a full initialization, when we could otherwise
 * partially intialize.  Do we care?
 */
INSTR_INLINE
opnd_t
opnd_create_null(void)
{
    opnd_t opnd;
    opnd.kind = NULL_kind;
    return opnd;
}

INSTR_INLINE
opnd_t
opnd_create_reg(reg_id_t r)
{
    opnd_t opnd IF_DEBUG(= {0});  /* FIXME: Needed until i#417 is fixed. */
    CLIENT_ASSERT(r <= DR_REG_LAST_ENUM && r != DR_REG_INVALID,
                  "opnd_create_reg: invalid register");
    opnd.kind = REG_kind;
    opnd.value.reg = r;
    return opnd;
}

INSTR_INLINE
opnd_t
opnd_create_pc(app_pc pc)
{
    opnd_t opnd;
    opnd.kind = PC_kind;
    opnd.value.pc = pc;
    return opnd;
}

/* opnd_t accessors */

#define OPND_GET_REG(opnd) \
    (CLIENT_ASSERT_(opnd_is_reg(opnd), "opnd_get_reg called on non-reg opnd") \
     (opnd).value.reg)
#define opnd_get_reg OPND_GET_REG

#define GET_BASE_DISP(opnd) \
    (CLIENT_ASSERT_(opnd_is_base_disp(opnd), \
                    "opnd_get_disp called on invalid opnd type") \
     (opnd).value.base_disp)

#define OPND_GET_BASE(opnd)  (GET_BASE_DISP(opnd).base_reg)
#define OPND_GET_DISP(opnd)  (GET_BASE_DISP(opnd).disp)
#define OPND_GET_INDEX(opnd) (GET_BASE_DISP(opnd).index_reg)
#define OPND_GET_SCALE(opnd) (GET_BASE_DISP(opnd).scale)

#define opnd_get_base OPND_GET_BASE
#define opnd_get_disp OPND_GET_DISP
#define opnd_get_index OPND_GET_INDEX
#define opnd_get_scale OPND_GET_SCALE

#define OPND_GET_SEGMENT(opnd) \
    (CLIENT_ASSERT_(opnd_is_base_disp(opnd) \
                    IF_X64(|| opnd_is_abs_addr(opnd) || \
                           opnd_is_rel_addr(opnd)), \
                    "opnd_get_segment called on invalid opnd type") \
     (opnd).seg.segment)
#define opnd_get_segment OPND_GET_SEGMENT

/* instr_t accessors */

INSTR_INLINE
bool
instr_ok_to_mangle(instr_t *instr)
{
    CLIENT_ASSERT(instr != NULL, "instr_ok_to_mangle: passed NULL");
    return ((instr->flags & INSTR_DO_NOT_MANGLE) == 0);
}

#ifdef AVOID_API_EXPORT
/* This is hot internally, but unlikely to be used by clients. */
INSTR_INLINE
bool
instr_ok_to_emit(instr_t *instr)
{
    CLIENT_ASSERT(instr != NULL, "instr_ok_to_emit: passed NULL");
    return ((instr->flags & INSTR_DO_NOT_EMIT) == 0);
}
#endif

INSTR_INLINE
int
instr_num_srcs(instr_t *instr)
{
    MAKE_OPNDS_VALID(instr);
    return instr->num_srcs;
}

INSTR_INLINE
int
instr_num_dsts(instr_t *instr)
{
    MAKE_OPNDS_VALID(instr);
    return instr->num_dsts;
}

/* src0 is static, rest are dynamic. */
/* FIXME: Double evaluation. */
#define INSTR_GET_SRC(instr, pos)                                   \
    (MAKE_OPNDS_VALID(instr),                                       \
     CLIENT_ASSERT_(pos >= 0 && pos < (instr)->num_srcs,            \
                    "instr_get_src: ordinal invalid")               \
     ((pos) == 0 ? (instr)->src0 : (instr)->srcs[(pos) - 1]))

#define INSTR_GET_DST(instr, pos)                                   \
    (MAKE_OPNDS_VALID(instr),                                       \
     CLIENT_ASSERT_(pos >= 0 && pos < (instr)->num_dsts,            \
                    "instr_get_dst: ordinal invalid")               \
     (instr)->dsts[pos])

/* Assumes that if an instr has a jump target, it's stored in the 0th src
 * location.
 */
#define INSTR_GET_TARGET(instr)                                         \
    (MAKE_OPNDS_VALID(instr),                                           \
     CLIENT_ASSERT_(instr_is_cti(instr),                                \
                    "instr_get_target: called on non-cti")              \
     CLIENT_ASSERT_((instr)->num_srcs >= 1,                             \
                    "instr_get_target: instr has no sources")           \
     (instr)->src0)

#define instr_get_src INSTR_GET_SRC
#define instr_get_dst INSTR_GET_DST
#define instr_get_target INSTR_GET_TARGET

INSTR_INLINE
void
instr_set_note(instr_t *instr, void *value)
{
    instr->note = value;
}

INSTR_INLINE
void *
instr_get_note(instr_t *instr)
{
    return instr->note;
}

INSTR_INLINE
instr_t*
instr_get_next(instr_t *instr)
{
    return instr->next;
}

INSTR_INLINE
instr_t*
instr_get_prev(instr_t *instr)
{
    return instr->prev;
}

INSTR_INLINE
void
instr_set_next(instr_t *instr, instr_t *next)
{
    instr->next = next;
}

INSTR_INLINE
void
instr_set_prev(instr_t *instr, instr_t *prev)
{
    instr->prev = prev;
}

#endif /* DR_FAST_IR */

/* DR_API EXPORT END */

#endif /* _INSTR_INLINE_H_ */
