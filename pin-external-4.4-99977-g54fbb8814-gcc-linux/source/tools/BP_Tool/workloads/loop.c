#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
	int n = argc > 1 ? atoi(argv[1]) : 100000;
	long hits = 0;
	long sum = 0;

	for(int i = 0; i < n; i++) {
		if((i & 7) == 0) {
			hits++;
		} else {
			sum += i;
		}
	}
	printf("loop: n=%d hits=%ld sum=%ld\n", n, hits, sum);
	return 0;
}
