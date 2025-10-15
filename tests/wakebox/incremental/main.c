#include <stdio.h>
#include "impl.h"

int main() {
	printf("Simple C Program with Multiple Files\n");
	printf("====================================\n\n");

	// Test the greeting function from impl.c
	greet_user("Alice");

	// Test the math functions from impl.c
	int num1 = 15, num2 = 7;
	printf("\nMath operations with %d and %d:\n", num1, num2);
	printf("Addition: %d + %d = %d\n", num1, num2, add_numbers(num1, num2));
	printf("Multiplication: %d * %d = %d\n", num1, num2, multiply_numbers(num1, num2));

	// Test the factorial function
	int n = 5;
	printf("\nFactorial of %d = %d\n", n, factorial(n));

	printf("\nProgram completed successfully!\n");
	return 0;
}
