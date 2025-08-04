// Factorial implementation in C (recursive only)
// Calculates factorial of a number provided as command line argument

#include <stdio.h>
#include <stdlib.h>

// Function to calculate factorial recursively
unsigned long long factorial_recursive(int n) {
    if (n < 0) {
        return 0; // Factorial is not defined for negative numbers
    }
    if (n == 0 || n == 1) {
        return 1;
    }
    return n * factorial_recursive(n - 1);
}

int main(int argc, char *argv[]) {
    // Check if argument is provided
    if (argc != 2) {
        printf("Usage: %s <number>\n", argv[0]);
        printf("Example: %s 5\n", argv[0]);
        return 1;
    }
    
    // Convert argument to integer
    int n = atoi(argv[1]);
    
    // Validate input
    if (n < 0) {
        printf("Error: Factorial is not defined for negative numbers.\n");
        return 1;
    }
    
    // Calculate factorial using recursive method
    unsigned long long result = factorial_recursive(n);
    
    // Display result
    printf("Factorial of %d: %llu\n", n, result);
    
    return 0;
}
    