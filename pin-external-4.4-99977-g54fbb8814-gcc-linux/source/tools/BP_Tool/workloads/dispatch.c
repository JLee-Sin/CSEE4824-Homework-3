#include <stdio.h>
#include <stdlib.h>

typedef long (*OpFn)(long acc, long x);

static long OpAdd(long acc, long x) {
	return acc + x;
}

static long OpSub(long acc, long x) {
	return acc - x;
}

static long OpMul(long acc, long x) {
	return acc * (x | 1) % 1000003;
}

static long OpXor(long acc, long x) {
	return acc ^ x;
}

static OpFn ops[] = {OpAdd, OpSub, OpMul, OpXor};

int main(int argc, char** argv) {
	int n = argc > 1 ? atoi(argv[1]) : 20000;
	unsigned seed = 1;
	long acc = 1;

	for(int i = 0; i < n; i++) {
		seed = seed * 1103515245u + 12345u;
		acc = ops[(seed >> 16) & 3](acc, i);
	}
	printf("dispatch: n=%d acc=%ld\n", n, acc);
	return 0;
}
