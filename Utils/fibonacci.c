#include <stdio.h>
#include <stdlib.h>

long fib(int n) {
    if (n <= 1)
        return n;
    else
        return fib(n - 1) + fib(n - 2);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <number>\n", argv[0]);
        return 1;
    }
    
    int n = atoi(argv[1]);
    long r = fib(n);
    return 0;
}
