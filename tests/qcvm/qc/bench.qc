// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Timing workloads for scripts/bench-fte.ps1, compiled after runner.qh with `-O3` and one of
// `-DFIB`, `-DLOOP`, `-DFIELDS`, `-DFIELDS_ADD`, plus `-DN=<size>`.

.float health, armor;

// Calls: LT_F, IFNOT_I, RETURN / SUB_F, CALL1H, SUB_F, STORE_F, CALL1H, ADD_F, RETURN.
float(float n) fib = { if (n < 2) return n; return fib(n - 1) + fib(n - 2); };

// Float arithmetic: LT_F, IFNOT_I, MUL_F, ADD_F, ADD_F, GOTO.
float(float n) loop = { float i, t = 0; for (i = 0; i < n; i++) t += i * 0.5; return t; };

// Entity fields: LT_F, IFNOT_I, LOAD_F, LOAD_F, ADD_F, STOREF_F, ADD_F, GOTO.
float(entity e, float n) fields =
{
	float i;
	for (i = 0; i < n; i++) e.health = e.health + e.armor;
	return e.health;
};

// Compound field update through a pointer: LT_F, IFNOT_I, LOAD_F, ADDRESS, ADDSTOREP_F, ADD_F, GOTO.
float(entity e, float n) fields_add =
{
	float i;
	for (i = 0; i < n; i++) e.health += e.armor;
	return e.health;
};

void() main =
{
	entity e = spawn();
	e.armor = 1;
#ifdef FIB
	pf(fib(N));
#endif
#ifdef LOOP
	pf(loop(N));
#endif
#ifdef FIELDS
	pf(fields(e, N));
#endif
#ifdef FIELDS_ADD
	pf(fields_add(e, N));
#endif
};
