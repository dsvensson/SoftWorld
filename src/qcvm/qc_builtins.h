// qc_builtins.h -- the builtin registry's internals and FTE's numbering tables
#pragma once

#include "qc_local.h"

typedef struct
{
	const char	*name;
	uint32_t	number;
} qc_builtinnumber_t;

extern const qc_builtinnumber_t	qc_numbers_csqc[], qc_numbers_ssqc[], qc_numbers_menu[];
extern const uint32_t			qc_numbers_csqc_count, qc_numbers_ssqc_count, qc_numbers_menu_count;
extern const char *const		qc_named_csqc[], *const qc_named_ssqc[], *const qc_named_menu[];
extern const uint32_t			qc_named_csqc_count, qc_named_ssqc_count, qc_named_menu_count;

typedef struct
{
	char			*name;
	bool			numbered;
	uint32_t		number;
	qc_builtin_t	func;
} qc_bentry_t;

// a number bound to an entry
typedef struct
{
	uint32_t	number;
	uint32_t	index;
} qc_bnumber_t;

struct qc_builtins_s
{
	qc_numbering_t	numbering;
	qc_bentry_t		**entries;		// a slot each; removed ones stay, unbound
	uint32_t		count, size;
	qc_map_t		by_name;		// keys: the entries' names
	qc_bnumber_t	*numbers;		// by number
	uint32_t		numnumbers, numbersize;
};

bool			QC_BindNumber (const qc_builtins_t *b, uint32_t number, uint32_t *slot);
bool			QC_BindName (const qc_builtins_t *b, const char *name, uint32_t *slot);
qc_builtin_t	QC_BuiltinSlot (const qc_builtins_t *b, uint32_t slot);
