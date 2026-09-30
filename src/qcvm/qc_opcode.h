// qc_opcode.h -- the QuakeC instruction set: every opcode FTE executes
//
// One table lists each opcode (0-281) with how it uses its operands a, b and
// c. It drives decoding, the loader's operand sanitizer, relocation, the
// disassembler and the tests' assembler, so the knowledge lives in one place.
#pragma once

#include <stdint.h>

// how an instruction uses one of its operands
typedef enum
{
	QC_OPND_G,		// a global, read and/or written; range-checked at load, relocated
	QC_OPND_A,		// a global index used as a number (the base of LOADA_*, FETCH_GBL_*,
					// GLOBALADDRESS); range-checked like a global, not relocated
	QC_OPND_J,		// a signed jump offset relative to the instruction
	QC_OPND_I,		// a raw immediate (the bounds of BOUNDCHECK), not checked
	QC_OPND_U		// unused; checked like a global, as FTE does
} qc_operand_t;

// X(number, name, a, b, c) for every opcode that can appear in a progs file,
// with the operand kinds as the letters above
#define QC_OPCODES(X) \
	X(  0, DONE,           G, U, U) \
	X(  1, MUL_F,          G, G, G) \
	X(  2, MUL_V,          G, G, G) \
	X(  3, MUL_FV,         G, G, G) \
	X(  4, MUL_VF,         G, G, G) \
	X(  5, DIV_F,          G, G, G) \
	X(  6, ADD_F,          G, G, G) \
	X(  7, ADD_V,          G, G, G) \
	X(  8, SUB_F,          G, G, G) \
	X(  9, SUB_V,          G, G, G) \
	X( 10, EQ_F,           G, G, G) \
	X( 11, EQ_V,           G, G, G) \
	X( 12, EQ_S,           G, G, G) \
	X( 13, EQ_E,           G, G, G) \
	X( 14, EQ_FNC,         G, G, G) \
	X( 15, NE_F,           G, G, G) \
	X( 16, NE_V,           G, G, G) \
	X( 17, NE_S,           G, G, G) \
	X( 18, NE_E,           G, G, G) \
	X( 19, NE_FNC,         G, G, G) \
	X( 20, LE_F,           G, G, G) \
	X( 21, GE_F,           G, G, G) \
	X( 22, LT_F,           G, G, G) \
	X( 23, GT_F,           G, G, G) \
	X( 24, LOAD_F,         G, G, G) \
	X( 25, LOAD_V,         G, G, G) \
	X( 26, LOAD_S,         G, G, G) \
	X( 27, LOAD_ENT,       G, G, G) \
	X( 28, LOAD_FLD,       G, G, G) \
	X( 29, LOAD_FNC,       G, G, G) \
	X( 30, ADDRESS,        G, G, G) \
	X( 31, STORE_F,        G, G, U) \
	X( 32, STORE_V,        G, G, U) \
	X( 33, STORE_S,        G, G, U) \
	X( 34, STORE_ENT,      G, G, U) \
	X( 35, STORE_FLD,      G, G, U) \
	X( 36, STORE_FNC,      G, G, U) \
	X( 37, STOREP_F,       G, G, G) \
	X( 38, STOREP_V,       G, G, G) \
	X( 39, STOREP_S,       G, G, G) \
	X( 40, STOREP_ENT,     G, G, G) \
	X( 41, STOREP_FLD,     G, G, G) \
	X( 42, STOREP_FNC,     G, G, G) \
	X( 43, RETURN,         G, U, U) \
	X( 44, NOT_F,          G, U, G) \
	X( 45, NOT_V,          G, U, G) \
	X( 46, NOT_S,          G, U, G) \
	X( 47, NOT_ENT,        G, U, G) \
	X( 48, NOT_FNC,        G, U, G) \
	X( 49, IF_I,           G, J, U) \
	X( 50, IFNOT_I,        G, J, U) \
	X( 51, CALL0,          G, U, U) \
	X( 52, CALL1,          G, U, U) \
	X( 53, CALL2,          G, U, U) \
	X( 54, CALL3,          G, U, U) \
	X( 55, CALL4,          G, U, U) \
	X( 56, CALL5,          G, U, U) \
	X( 57, CALL6,          G, U, U) \
	X( 58, CALL7,          G, U, U) \
	X( 59, CALL8,          G, U, U) \
	X( 60, STATE,          G, G, U) \
	X( 61, GOTO,           J, U, U) \
	X( 62, AND_F,          G, G, G) \
	X( 63, OR_F,           G, G, G) \
	X( 64, BITAND_F,       G, G, G) \
	X( 65, BITOR_F,        G, G, G) \
	X( 66, MULSTORE_F,     G, G, G) \
	X( 67, MULSTORE_VF,    G, G, G) \
	X( 68, MULSTOREP_F,    G, G, G) \
	X( 69, MULSTOREP_VF,   G, G, G) \
	X( 70, DIVSTORE_F,     G, G, G) \
	X( 71, DIVSTOREP_F,    G, G, G) \
	X( 72, ADDSTORE_F,     G, G, G) \
	X( 73, ADDSTORE_V,     G, G, G) \
	X( 74, ADDSTOREP_F,    G, G, G) \
	X( 75, ADDSTOREP_V,    G, G, G) \
	X( 76, SUBSTORE_F,     G, G, G) \
	X( 77, SUBSTORE_V,     G, G, G) \
	X( 78, SUBSTOREP_F,    G, G, G) \
	X( 79, SUBSTOREP_V,    G, G, G) \
	X( 80, FETCH_GBL_F,    A, G, G) \
	X( 81, FETCH_GBL_V,    A, G, G) \
	X( 82, FETCH_GBL_S,    A, G, G) \
	X( 83, FETCH_GBL_E,    A, G, G) \
	X( 84, FETCH_GBL_FNC,  A, G, G) \
	X( 85, CSTATE,         G, G, U) \
	X( 86, CWSTATE,        G, G, U) \
	X( 87, THINKTIME,      G, G, U) \
	X( 88, BITSETSTORE_F,  G, G, G) \
	X( 89, BITSETSTOREP_F, G, G, G) \
	X( 90, BITCLRSTORE_F,  G, G, G) \
	X( 91, BITCLRSTOREP_F, G, G, G) \
	X( 92, RAND0,          U, U, G) \
	X( 93, RAND1,          G, U, G) \
	X( 94, RAND2,          G, G, G) \
	X( 95, RANDV0,         U, U, G) \
	X( 96, RANDV1,         G, U, G) \
	X( 97, RANDV2,         G, G, G) \
	X( 98, SWITCH_F,       G, J, U) \
	X( 99, SWITCH_V,       G, J, U) \
	X(100, SWITCH_S,       G, J, U) \
	X(101, SWITCH_E,       G, J, U) \
	X(102, SWITCH_FNC,     G, J, U) \
	X(103, CASE,           G, J, U) \
	X(104, CASERANGE,      G, G, J) \
	X(105, CALL1H,         G, G, G) \
	X(106, CALL2H,         G, G, G) \
	X(107, CALL3H,         G, G, G) \
	X(108, CALL4H,         G, G, G) \
	X(109, CALL5H,         G, G, G) \
	X(110, CALL6H,         G, G, G) \
	X(111, CALL7H,         G, G, G) \
	X(112, CALL8H,         G, G, G) \
	X(113, STORE_I,        G, G, U) \
	X(114, STORE_IF,       G, G, U) \
	X(115, STORE_FI,       G, G, U) \
	X(116, ADD_I,          G, G, G) \
	X(117, ADD_FI,         G, G, G) \
	X(118, ADD_IF,         G, G, G) \
	X(119, SUB_I,          G, G, G) \
	X(120, SUB_FI,         G, G, G) \
	X(121, SUB_IF,         G, G, G) \
	X(122, CONV_ITOF,      G, U, G) \
	X(123, CONV_FTOI,      G, U, G) \
	X(124, LOADP_ITOF,     G, U, G) \
	X(125, LOADP_FTOI,     G, U, G) \
	X(126, LOAD_I,         G, G, G) \
	X(127, STOREP_I,       G, G, G) \
	X(128, STOREP_IF,      G, G, G) \
	X(129, STOREP_FI,      G, G, G) \
	X(130, BITAND_I,       G, G, G) \
	X(131, BITOR_I,        G, G, G) \
	X(132, MUL_I,          G, G, G) \
	X(133, DIV_I,          G, G, G) \
	X(134, EQ_I,           G, G, G) \
	X(135, NE_I,           G, G, G) \
	X(136, IFNOT_S,        G, J, U) \
	X(137, IF_S,           G, J, U) \
	X(138, NOT_I,          G, U, G) \
	X(139, DIV_VF,         G, G, G) \
	X(140, BITXOR_I,       G, G, G) \
	X(141, RSHIFT_I,       G, G, G) \
	X(142, LSHIFT_I,       G, G, G) \
	X(143, GLOBALADDRESS,  A, G, G) \
	X(144, ADD_PIW,        G, G, G) \
	X(145, LOADA_F,        A, G, G) \
	X(146, LOADA_V,        A, G, G) \
	X(147, LOADA_S,        A, G, G) \
	X(148, LOADA_ENT,      A, G, G) \
	X(149, LOADA_FLD,      A, G, G) \
	X(150, LOADA_FNC,      A, G, G) \
	X(151, LOADA_I,        A, G, G) \
	X(152, STORE_P,        G, G, U) \
	X(153, LOAD_P,         G, G, G) \
	X(154, LOADP_F,        G, G, G) \
	X(155, LOADP_V,        G, G, G) \
	X(156, LOADP_S,        G, G, G) \
	X(157, LOADP_ENT,      G, G, G) \
	X(158, LOADP_FLD,      G, G, G) \
	X(159, LOADP_FNC,      G, G, G) \
	X(160, LOADP_I,        G, G, G) \
	X(161, LE_I,           G, G, G) \
	X(162, GE_I,           G, G, G) \
	X(163, LT_I,           G, G, G) \
	X(164, GT_I,           G, G, G) \
	X(165, LE_IF,          G, G, G) \
	X(166, GE_IF,          G, G, G) \
	X(167, LT_IF,          G, G, G) \
	X(168, GT_IF,          G, G, G) \
	X(169, LE_FI,          G, G, G) \
	X(170, GE_FI,          G, G, G) \
	X(171, LT_FI,          G, G, G) \
	X(172, GT_FI,          G, G, G) \
	X(173, EQ_IF,          G, G, G) \
	X(174, EQ_FI,          G, G, G) \
	X(175, ADD_SF,         G, G, G) \
	X(176, SUB_S,          G, G, G) \
	X(177, STOREP_C,       G, G, G) \
	X(178, LOADP_C,        G, G, G) \
	X(179, MUL_IF,         G, G, G) \
	X(180, MUL_FI,         G, G, G) \
	X(181, MUL_VI,         G, G, G) \
	X(182, MUL_IV,         G, G, G) \
	X(183, DIV_IF,         G, G, G) \
	X(184, DIV_FI,         G, G, G) \
	X(185, BITAND_IF,      G, G, G) \
	X(186, BITOR_IF,       G, G, G) \
	X(187, BITAND_FI,      G, G, G) \
	X(188, BITOR_FI,       G, G, G) \
	X(189, AND_I,          G, G, G) \
	X(190, OR_I,           G, G, G) \
	X(191, AND_IF,         G, G, G) \
	X(192, OR_IF,          G, G, G) \
	X(193, AND_FI,         G, G, G) \
	X(194, OR_FI,          G, G, G) \
	X(195, NE_IF,          G, G, G) \
	X(196, NE_FI,          G, G, G) \
	X(197, GSTOREP_I,      G, G, U) \
	X(198, GSTOREP_F,      G, G, U) \
	X(199, GSTOREP_ENT,    G, G, U) \
	X(200, GSTOREP_FLD,    G, G, U) \
	X(201, GSTOREP_S,      G, G, U) \
	X(202, GSTOREP_FNC,    G, G, U) \
	X(203, GSTOREP_V,      G, G, U) \
	X(204, GADDRESS,       G, G, G) \
	X(205, GLOAD_I,        G, U, G) \
	X(206, GLOAD_F,        G, U, G) \
	X(207, GLOAD_FLD,      G, U, G) \
	X(208, GLOAD_ENT,      G, U, G) \
	X(209, GLOAD_S,        G, U, G) \
	X(210, GLOAD_FNC,      G, U, G) \
	X(211, BOUNDCHECK,     G, I, I) \
	X(212, UNUSED,         U, U, U) \
	X(213, PUSH,           G, U, G) \
	X(214, POP,            U, U, U) \
	X(215, SWITCH_I,       G, J, U) \
	X(216, GLOAD_V,        G, U, G) \
	X(217, IF_F,           G, J, U) \
	X(218, IFNOT_F,        G, J, U) \
	X(219, STOREF_V,       G, G, G) \
	X(220, STOREF_F,       G, G, G) \
	X(221, STOREF_S,       G, G, G) \
	X(222, STOREF_I,       G, G, G) \
	X(223, STOREP_I8,      G, G, G) \
	X(224, LOADP_U8,       G, G, G) \
	X(225, LE_U,           G, G, G) \
	X(226, LT_U,           G, G, G) \
	X(227, DIV_U,          G, G, G) \
	X(228, RSHIFT_U,       G, G, G) \
	X(229, ADD_I64,        G, G, G) \
	X(230, SUB_I64,        G, G, G) \
	X(231, MUL_I64,        G, G, G) \
	X(232, DIV_I64,        G, G, G) \
	X(233, BITAND_I64,     G, G, G) \
	X(234, BITOR_I64,      G, G, G) \
	X(235, BITXOR_I64,     G, G, G) \
	X(236, LSHIFT_I64I,    G, G, G) \
	X(237, RSHIFT_I64I,    G, G, G) \
	X(238, LE_I64,         G, G, G) \
	X(239, LT_I64,         G, G, G) \
	X(240, EQ_I64,         G, G, G) \
	X(241, NE_I64,         G, G, G) \
	X(242, LE_U64,         G, G, G) \
	X(243, LT_U64,         G, G, G) \
	X(244, DIV_U64,        G, G, G) \
	X(245, RSHIFT_U64I,    G, G, G) \
	X(246, STORE_I64,      G, G, U) \
	X(247, STOREP_I64,     G, G, G) \
	X(248, STOREF_I64,     G, G, G) \
	X(249, LOAD_I64,       G, G, G) \
	X(250, LOADA_I64,      A, G, G) \
	X(251, LOADP_I64,      G, G, G) \
	X(252, CONV_UI64,      G, U, G) \
	X(253, CONV_II64,      G, U, G) \
	X(254, CONV_I64I,      G, U, G) \
	X(255, CONV_FD,        G, U, G) \
	X(256, CONV_DF,        G, U, G) \
	X(257, CONV_I64F,      G, U, G) \
	X(258, CONV_FI64,      G, U, G) \
	X(259, CONV_I64D,      G, U, G) \
	X(260, CONV_DI64,      G, U, G) \
	X(261, ADD_D,          G, G, G) \
	X(262, SUB_D,          G, G, G) \
	X(263, MUL_D,          G, G, G) \
	X(264, DIV_D,          G, G, G) \
	X(265, LE_D,           G, G, G) \
	X(266, LT_D,           G, G, G) \
	X(267, EQ_D,           G, G, G) \
	X(268, NE_D,           G, G, G) \
	X(269, STOREP_I16,     G, G, G) \
	X(270, LOADP_I16,      G, G, G) \
	X(271, LOADP_U16,      G, G, G) \
	X(272, LOADP_I8,       G, G, G) \
	X(273, BITEXTEND_I,    G, G, G) \
	X(274, BITEXTEND_U,    G, G, G) \
	X(275, BITCOPY_I,      G, G, G) \
	X(276, CONV_UF,        G, U, G) \
	X(277, CONV_FU,        G, U, G) \
	X(278, CONV_U64D,      G, U, G) \
	X(279, CONV_DU64,      G, U, G) \
	X(280, CONV_U64F,      G, U, G) \
	X(281, CONV_FU64,      G, U, G)

typedef enum
{
#define QC_OPCODE_ENUM(num, name, a, b, c)	QOP_##name = num,
	QC_OPCODES (QC_OPCODE_ENUM)
#undef QC_OPCODE_ENUM
	QOP_BAD = 282,				// an invalid statement or one with an operand out of range
	QOP_JUMP_OUT_OF_RANGE = 283,	// where jumps that leave the statement table land
	QOP_COUNT
} qc_op_t;

#define QOP_NUMREAL		282		// opcodes that can appear in a progs file (OP_NUMREALOPS)

typedef struct
{
	const char		*name;		// FTE's, without OP_
	uint8_t			operands[3];	// qc_operand_t of a, b and c
} qc_opinfo_t;

extern const qc_opinfo_t	qc_opinfo[QOP_COUNT];

// the arguments a CALLn or CALLnH passes, or -1 for any other opcode
static inline int QC_CallArgc (unsigned op)
{
	if (op >= QOP_CALL0 && op <= QOP_CALL8)
		return (int)(op - QOP_CALL0);
	if (op >= QOP_CALL1H && op <= QOP_CALL8H)
		return (int)(op - QOP_CALL1H) + 1;
	return -1;
}
