#include "CommandLine.h"

int wmain(int count, wchar_t** args) {
    return CheckCommandLine(count, args, false);
}
