#ifndef CONSOLE_H
#define CONSOLE_H

int open_console(void);
void print_log(const char *format, ...);
int received_signal(void);
int output_failed(void);

#endif
