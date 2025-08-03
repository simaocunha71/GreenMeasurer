// Factorial implementation in C
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

// Function to calculate factorial iteratively
unsigned long long factorial_iterative(int n) {
    if (n < 0) {
        return 0; // Factorial is not defined for negative numbers
    }
    
    unsigned long long result = 1;
    for (int i = 2; i <= n; i++) {
        result *= i;
    }
    return result;
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
    
    if (n > 20) {
        printf("Warning: Results for n > 20 may overflow.\n");
    }
    
    // Calculate factorial using both methods
    unsigned long long result_recursive = factorial_recursive(n);
    unsigned long long result_iterative = factorial_iterative(n);
    
    // Display results
    printf("Factorial of %d:\n", n);
    printf("Recursive method: %llu\n", result_recursive);
    printf("Iterative method: %llu\n", result_iterative);
    
    return 0;
}
    