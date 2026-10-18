/* mcimport - copy the saves of a PCSX2 memory card (.ps2) into the PC port's save folder.
 *   mcimport <card.ps2> <save folder, e.g. ...\Saves\card1> [--overwrite]
 * Saves already in the folder are left alone unless --overwrite. */
#include <stdio.h>
#include <string.h>
#include "ps2card.h"

int main(int argc, char **argv)
{
    static char report[16384];
    int n, overwrite = argc > 3 && !strcmp(argv[3], "--overwrite");
    if (argc < 3) {
        fprintf(stderr, "usage: mcimport <card.ps2> <save folder, e.g. Saves\\card1> [--overwrite]\n");
        return 1;
    }
    n = ps2card_import(argv[1], argv[2], overwrite, report, (int)sizeof report);
    fputs(report, stdout);
    if (n < 0) return 1;
    printf("%d save%s copied\n", n, n == 1 ? "" : "s");
    return 0;
}
