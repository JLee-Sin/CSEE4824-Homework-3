#include <stdio.h>
#include <stdlib.h>

#define MAXN 16
static int n;
static int col[MAXN];
static int diag1[2 * MAXN];
static int diag2[2 * MAXN];

static int Place(int row) {
	if(row == n) {
		return 1;
	}
	int solutions = 0;
	for(int c = 0; c < n; c++) {
		if(col[c] || diag1[row + c] || diag2[row - c + n - 1]) {
			continue;
		}
		col[c] = diag1[row + c] = diag2[row - c + n - 1] = 1;
		solutions += Place(row + 1);
		col[c] = diag1[row + c] = diag2[row - c + n - 1] = 0;
	}
	return solutions;
}

int main(int argc, char** argv) {
	n = argc > 1 ? atoi(argv[1]) : 8;
	if(n < 1 || n > MAXN) {
		fprintf(stderr, "n must be 1..%d\n", MAXN);
		return 1;
	}
	printf("recursion: %d-queens has %d solutions\n", n, Place(0));
	return 0;
}
