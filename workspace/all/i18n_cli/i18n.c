#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "defines.h"
#include "config.h"
#include "i18n.h"

void printUsage(void)
{
    printf("usage: i18n get <key> [-f <path>]...\n"
           "       i18n format <key> [-f <path>]... [--] <arg>...\n"
           "       i18n lang\n");
}

// %s-only, positional, never handed to printf(3)/vprintf: a .lang value
// (possibly a third-party or community-contributed translation, see
// I18N.md) can never control how many bytes get written or trigger
// undefined behavior. Any other specifier (%d, %u, %n, ...) is left as
// literal text, never interpreted.
static void format_s(const char *tmpl, char **args, int nargs)
{
    int ai = 0;
    for (const char *p = tmpl; *p; ) {
        if (p[0] == '%' && p[1] == 's') {
            if (ai < nargs) fputs(args[ai++], stdout);
            p += 2;
        } else {
            fputc(*p++, stdout);
        }
    }
    fputc('\n', stdout);
}

int main(int argc, char *argv[])
{
    CFG_init(NULL, NULL);
    I18N_init(CFG_getLanguage());

    if (argc >= 2 && strcmp(argv[1], "-h") == 0) {
        printUsage();
        return EXIT_SUCCESS;
    }

    if (argc >= 2 && strcmp(argv[1], "lang") == 0) {
        printf("%s\n", I18N_active_code());
        return EXIT_SUCCESS;
    }

    int is_get    = (argc >= 2 && strcmp(argv[1], "get") == 0);
    int is_format = (argc >= 2 && strcmp(argv[1], "format") == 0);

    if ((is_get || is_format) && argc >= 3) {
        const char *key = argv[2];

        int i = 3;
        for (; i + 1 < argc && strcmp(argv[i], "-f") == 0; i += 2)
            I18N_load_extra(argv[i + 1]);

        if (is_get) {
            printf("%s\n", T(key));
            return EXIT_SUCCESS;
        }

        // format: everything left, after an optional "--", is %s args
        if (i < argc && strcmp(argv[i], "--") == 0) i++;
        format_s(T(key), &argv[i], argc - i);
        return EXIT_SUCCESS;
    }

    printf("Error: invalid arguments\n");
    printUsage();
    return EXIT_FAILURE;
}
