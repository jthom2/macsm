#include "masmrun.h"

static void usage(FILE *stream) {
    fprintf(stream,
            "macsm %s\n"
            "usage: macsm [--check] [--trace] [--debug] [--dump-symbols] [--dump-data] path/to/program.asm\n"
            "\n"
            "Runs a focused MASM/Irvine32-style .asm program through a native C11 interpreter.\n"
            "Debug flags write human-readable output to stderr and never change program stdout.\n"
            "--check writes a static compatibility report to stdout and does not execute.\n",
            MASMRUN_VERSION);
}

int main(int argc, char **argv) {
    RunOptions options;
    memset(&options, 0, sizeof(options));
    const char *path = NULL;
    bool check = false;

    for (int i = 1; i < argc; i++) {
        if (ci_eq(argv[i], "-h") || ci_eq(argv[i], "--help")) {
            usage(stdout);
            return 0;
        }
        if (ci_eq(argv[i], "--check")) {
            check = true;
            continue;
        }
        if (ci_eq(argv[i], "--trace")) {
            options.trace = true;
            continue;
        }
        if (ci_eq(argv[i], "--debug")) {
            options.debug = true;
            continue;
        }
        if (ci_eq(argv[i], "--dump-symbols")) {
            options.dump_symbols = true;
            continue;
        }
        if (ci_eq(argv[i], "--dump-data")) {
            options.dump_data = true;
            continue;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "macsm: unknown option: %s\n", argv[i]);
            usage(stderr);
            return MASMRUN_EXIT_USAGE;
        }
        if (path) {
            fprintf(stderr, "macsm: only one input file is supported\n");
            usage(stderr);
            return MASMRUN_EXIT_USAGE;
        }
        path = argv[i];
    }

    if (!path) {
        usage(stderr);
        return MASMRUN_EXIT_USAGE;
    }
    options.source_path = path;

    if (check && (options.trace || options.debug || options.dump_symbols || options.dump_data)) {
        fprintf(stderr, "macsm: --check cannot be combined with execution debug flags\n");
        return MASMRUN_EXIT_USAGE;
    }
    if (options.debug && options.trace) {
        fprintf(stderr, "macsm: --debug cannot be combined with --trace\n");
        return MASMRUN_EXIT_USAGE;
    }

    if (check) {
        return check_file(path);
    }

    Program program;
    parse_program(&program, path);
    if (options.dump_symbols) {
        dump_symbols(&program);
    }
    if (options.dump_data) {
        dump_data(&program);
    }
    int code = run_program(&program, &options);
    free_program(&program);
    return code;
}
