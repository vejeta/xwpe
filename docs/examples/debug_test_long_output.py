#!/usr/bin/env python3
# debug_test_long_output.py -- Test long program output for Ctrl-G P
# Set breakpoint at line 10, Ctrl-G R, F8 until end, Ctrl-G P

def generate_report():
    for i in range(50):
        print(f"Line {i:3d}: Processing item {i*17 % 97:3d} -- status OK -- checksum {i*31337 % 65536:05d}")

x = 1
generate_report()
print("Report complete.")
