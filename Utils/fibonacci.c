#include <stdio.h>

long fib(int n) {
    if (n <= 1)
        return n;
    else
        return fib(n - 1) + fib(n - 2);
}

int main() {
    long r = fib(50);
    return 0;
}
