#!/usr/bin/env python3
# debug_test.py -- Python debug test for xwpe
# Set breakpoint at line 10, press Ctrl-G R to start pdb.

def factorial(n):
    if n <= 1:
        return 1 
    return n * factorial(n - 1)

x = 5
result = factorial(x)
print(f"factorial({x}) = {result}") 

