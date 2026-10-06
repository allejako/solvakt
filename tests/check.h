#ifndef SOLVAKT_CHECK_H
#define SOLVAKT_CHECK_H

/* Minimalt testramverk: CHECK räknar fel, TEST_RESULT ger processens
 * returkod så att "make test" stannar vid första misslyckade testfil. */

#include <math.h>
#include <stdio.h>

static int check_failures = 0;
static int check_count = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        check_count++;                                                     \
        if (!(cond)) {                                                     \
            check_failures++;                                              \
            printf("  FEL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                  \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(fabs((a) - (b)) <= (eps))

#define TEST_RESULT()                                                      \
    (printf("  %d kontroller, %d fel\n", check_count, check_failures),     \
     check_failures == 0 ? 0 : 1)

#endif
