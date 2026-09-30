// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Integer switches, bitfields, and enough globals to force 32-bit statements (FTE target).

string(int i) itos = #17;

typedef struct
{
	int lo : 3;
	int mid : 5;
	__uint hi : 7;
} packed_t;

packed_t pk;
float huge[70000];

float(int x) iswitch =
{
	switch (x)
	{
	case 1:
		return 10;
	case 2:
		return 20;
	case 5..9:
		return 50;
	default:
		return -1;
	}
};

void() main =
{
	int i;

	for (i = 0; i < 11; i++)
		puts(ftos(iswitch(i)), " ");
	puts("\n");

	pk.lo = 3;
	pk.mid = -7;
	pk.hi = 100;
	puts("bits ", itos(pk.lo), " ", itos(pk.mid), " ", itos(pk.hi), "\n");
	pk.lo = 9;
	puts("bits_wrap ", itos(pk.lo), " ", itos(pk.mid), "\n");

	huge[69999] = 42;
	huge[12345] = 7;
	show("huge", huge[69999] + huge[12345]);
};
