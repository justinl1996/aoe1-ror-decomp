// The Linux build's entry point: the game starts at WinMain (src/main.cpp)
// with the command line as one string, as Windows passed it. Run it from
// the game's folder (a retail install), as on Windows.

#include <windows.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "include/msvcrt.h"

extern "C" int __stdcall WinMain(HINSTANCE prog_inst, HINSTANCE prev_inst, char* cmd_line, int show_wnd_flag);

// On a crash, the stack as module+offset lines (llvm-symbolizer
// --obj=empiresx turns the game's into source lines).
static void report_crash(int signal_number) {
    void* frames[64];
    int count = backtrace(frames, 64);
    fprintf(stderr, "empiresx: crashed (signal %d). Stack:\n", signal_number);
    for (int i = 0; i < count; ++i) {
        Dl_info info;
        if (dladdr(frames[i], &info) != 0 && info.dli_fname != 0)
            fprintf(stderr, "  %s+0x%lx\n", info.dli_fname,
                    (unsigned long) ((char*) frames[i] - (char*) info.dli_fbase));
        else
            fprintf(stderr, "  %p\n", frames[i]);
    }
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}

int main(int argc, char** argv) {
    // AOE_NO_CRASH_HANDLER leaves crashes to a debugger or sanitizer.
    if (getenv("AOE_NO_CRASH_HANDLER") == 0) {
        signal(SIGSEGV, report_crash);
        signal(SIGABRT, report_crash);
        signal(SIGFPE, report_crash);
        signal(SIGILL, report_crash);
        signal(SIGBUS, report_crash);
    }

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
