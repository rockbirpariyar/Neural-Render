#include <iostream>

int main() {
    std::cout << "Pointer size: " << sizeof(void*) << " bytes\n";

    if (sizeof(void*) == 4)
        std::cout << "Architecture: 32-bit x86\n";
    else if (sizeof(void*) == 8)
        std::cout << "Architecture: 64-bit x64\n";

    return 0;
}