#include <stdio.h>
#include "impl.h"

// Function to greet a user
void greet_user(const char* name) {
	printf("Hello, %s! Welcome to our C program.\n", name);
}

// Function to add two numbers
int add_numbers(int a, int b) {
	return a + b;
}

// Function to multiply two numbers
int multiply_numbers(int a, int b) {
	return a * b;
}

// Function to calculate factorial
int factorial(int n) {
	if (n <= 1) {
		return 1;
	}
	return n * factorial(n - 1);
}
