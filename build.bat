@echo off
gcc -std=gnu11 -O2 -s -static -municode -Wall -Wextra -o fix-rugby18.exe fix-rugby18.c -lshell32
