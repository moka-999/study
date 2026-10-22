#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>

/* 字符串格式化所需的最小libc端口；不将文件读写隐式转发到调试串口。 */
void _init(void) {}
int _close(int file) { (void)file; errno = EBADF; return -1; }
int _lseek(int file, int offset, int origin)
{ (void)file; (void)offset; (void)origin; errno = ESPIPE; return -1; }
int _read(int file, char *data, int length)
{ (void)file; (void)data; (void)length; errno = EBADF; return -1; }
int _write(int file, char *data, int length)
{ (void)file; (void)data; (void)length; errno = EBADF; return -1; }
int _fstat(int file, struct stat *state)
{ (void)file; (void)state; errno = EBADF; return -1; }
int _isatty(int file) { (void)file; return 0; }
void *_sbrk(ptrdiff_t increment)
{
    /* 链接脚本已单独预留8KiB，不借用主程序栈空间。 */
    extern uint8_t _end;
    static ptrdiff_t used;
    if (increment < -used || increment > 8192 - used) {
        errno = ENOMEM; return (void *)-1;
    }
    void *previous = &_end + used;
    used += increment;
    return previous;
}
