import sys

def fibonacci(n: int) -> int:
    if n <= 1:
        return n
    return fibonacci(n - 1) + fibonacci(n - 2)

if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 40
    print(fibonacci(n))
