// qc_opcode.c -- names and operand kinds of the QuakeC opcodes

#include "qc_opcode.h"

#define G	QC_OPND_G
#define A	QC_OPND_A
#define J	QC_OPND_J
#define I	QC_OPND_I
#define U	QC_OPND_U

const qc_opinfo_t qc_opinfo[QOP_COUNT] =
{
#define QC_OPCODE_INFO(num, name, a, b, c)	[num] = {#name, {a, b, c}},
	QC_OPCODES (QC_OPCODE_INFO)
#undef QC_OPCODE_INFO
	[QOP_BAD] = {"BAD", {U, U, U}},
	[QOP_JUMP_OUT_OF_RANGE] = {"JUMP_OUT_OF_RANGE", {U, U, U}},
};
