#ifndef CONSOLE_H
#define CONSOLE_H

int open_console(void);
int open_console_mode(int defer_log);
int attach_console_log(const char *path, int create);
int close_console(void);
void print_log(const char *format, ...);
void pause_display(int milliseconds);
int received_signal(void);
int output_failed(void);

#endif
