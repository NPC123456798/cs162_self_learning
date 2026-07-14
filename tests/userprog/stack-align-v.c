#include "tests/lib.h"

int main(void) {
    unsigned int esp;
    asm volatile("movl %%esp, %0" : "=r"(esp));
    return esp % 16;
}