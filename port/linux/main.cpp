// The Linux build's entry point: the game starts at WinMain (src/main.cpp)
// with the command line as one string, as Windows passed it. Run it from
// the game's folder (a retail install), as on Windows.

#include <windows.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include "include/msvcrt.h"

extern "C" int __stdcall WinMain(HINSTANCE prog_inst, HINSTANCE prev_inst, char* cmd_line, int show_wnd_flag);

int main(int argc, char** argv) {
    // _pgmptr: the executable's full path.
    static char program[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", program, sizeof(program) - 1);
    if (length > 0) {
        program[length] = 0;
        _pgmptr = program;
    }

    static char command_line[4096];
    command_line[0] = 0;
    for (int i = 1; i < argc; ++i) {
        if (i > 1)
            strncat(command_line, " ", sizeof(command_line) - strlen(command_line) - 1);
        strncat(command_line, argv[i], sizeof(command_line) - strlen(command_line) - 1);
    }

    static int instance_token;
    return WinMain((HINSTANCE) &instance_token, 0, command_line, SW_SHOWNORMAL);
}
