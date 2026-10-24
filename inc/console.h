#ifndef CONSOLE_H
#define CONSOLE_H

int open_console(void);
int close_console(void);
void print_log(const char *format, ...);
void pause_display(int milliseconds);
int received_signal(void);
int output_failed(void);

#endif
