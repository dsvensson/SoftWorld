// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Arrays, structs and pointers (FTE target).

string(int i) itos = #17;

typedef struct
{
	float a;
	vector v;
	string name;
	int n;
} rec_t;

rec_t recs[4];
float table[10];
int ints[6];
rec_t single;
rec_t copy;

float(float *p, int count) sum_ptr =
{
	float s = 0;
	int k;
	for (k = 0; k < count; k++)
		s += p[k];
	return s;
};

void(rec_t *r, float a) fill =
{
	r->a = a;
	r->v = [a, a * 2, a * 3];
	r->n = (int)a * 10;
	r->name = "filled";
};

float(string s) len =
{
	float n = 0;
	while (s[n])
		n++;
	return n;
};

float(float n) local_array =
{
	float arr[5];
	float i;
	for (i = 0; i < 5; i++)
		arr[i] = n * i;
	return arr[4] + arr[1];
};

void() main =
{
	float i;
	float *p;
	int *ip;
	rec_t *rp;

	for (i = 0; i < 10; i++)
		table[i] = i * i;
	show("table", table[3] + table[9]);
	show("sum", sum_ptr(&table[0], 10));
	show("sum_tail", sum_ptr(&table[5], 5));

	p = &table[2];
	p += 3;
	show("ptr_arith", *p);
	show("ptr_index", p[2]);
	p[1] = 1234;
	show("ptr_store", table[6]);

	for (i = 0; i < 6; i++)
		ints[i] = (int)i * 3;
	ip = &ints[1];
	puts("ints ", itos(ip[0]), " ", itos(ip[4]), " ", itos(*(ip + 2)), "\n");

	for (i = 0; i < 4; i++)
		fill(&recs[i], i + 1);
	rp = &recs[2];
	show("rec_a", rp->a);
	showv("rec_v", rp->v);
	puts("rec_name ", rp->name, " ", itos(rp->n), "\n");
	show("rec_idx", recs[3].a + recs[0].v * '0 0 1');

	single.a = 5; single.v = '1 1 1'; single.name = "orig"; single.n = 9;
	copy = single;
	single.a = 6;
	puts("copy ", ftos(copy.a), " ", vtos(copy.v), " ", copy.name, " ", itos(copy.n), "\n");
	recs[1] = copy;
	show("copied", recs[1].a);

	show("local_array", local_array(3));
	show("sizeof", sizeof(rec_t));
	show("strlen", len("hello world"));
	show("char", "hello"[1]);
};
