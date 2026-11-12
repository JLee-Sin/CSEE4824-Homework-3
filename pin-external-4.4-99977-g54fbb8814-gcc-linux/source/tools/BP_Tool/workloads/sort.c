#include <stdio.h>
#include <stdlib.h>

static unsigned seed = 12345;

static int Rnd(void) {
	seed = seed * 1103515245u + 12345u;
	return (seed >> 16) & 0x7fff;
}

static void InsertionSort(int* a, int n) {
	for(int i = 1; i < n; i++) {
		int key = a[i];
		int j = i - 1;
		while(j >= 0 && a[j] > key) {
			a[j + 1] = a[j];
			j--;
		}
		a[j + 1] = key;
	}
}

static int BinarySearch(const int* a, int n, int key) {
	int lo = 0;
	int hi = n - 1;
	while(lo <= hi) {
		int mid = (lo + hi) / 2;
		if(a[mid] == key) {
			return mid;
		}
		if(a[mid] < key) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	return -1;
}

int main(int argc, char** argv) {
	int n = argc > 1 ? atoi(argv[1]) : 1000;
	int* a = malloc(n * sizeof *a);
	int found = 0;

	for(int i = 0; i < n; i++) {
		a[i] = Rnd();
	}
	InsertionSort(a, n);
	for(int i = 0; i < n; i++) {
		if(BinarySearch(a, n, Rnd()) >= 0) {
			found++;
		}
	}

	printf("sort: n=%d min=%d max=%d found=%d\n", n, a[0], a[n - 1], found);
	free(a);
	return 0;
}
