#ifndef SOFT_ASSERT_H
#define SOFT_ASSERT_H

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

/* В вызывающей функции должен быть локальный счётчик int fl_err = 0 */
#define SOFT_ASSERT_ERR(cond)                                             \
    do {                                                                  \
        if (!(cond)) {                                                    \
            int saved_errno = errno;                                      \
            dprintf(STDERR_FILENO, "FAIL %s:%d: %s\n",                      \
                    __FILE__, __LINE__, #cond);                            \
            errno = saved_errno;                                          \
            fl_err++;                                                     \
        }                                                                 \
    } while (0)

#endif
