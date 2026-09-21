/* Lorentz: A black hole for Internet advertisements
*  (c) 2020 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Syscall prototypes
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */
#ifndef SYSCALLS_H
#define SYSCALLS_H

// Interrupt-safe memory routines
char *Lorentzstrdup(const char *src, const char *file, const char *func, const int line) __attribute__((malloc));
void *Lorentzcalloc(size_t n, size_t size, const char *file, const char *func, const int line) __attribute__((malloc)) __attribute__((alloc_size(1,2)));
void *Lorentzrealloc(void *ptr_in, size_t size, const char *file, const char *func, const int line) __attribute__((alloc_size(2)));
bool Lorentzfree(void *ptr, const char*file, const char *func, const int line);
int Lorentzfallocate(const int fd, const off_t offset, const off_t len, const char *file, const char *func, const int line);


// Interrupt-safe printing routines
// printf() is derived from fprintf(stdout, ...)
// vprintf() is derived from vfprintf(stdout, ...)
int Lorentzfprintf(FILE *stream, const char*file, const char *func, const int line, const char *format, ...) __attribute__ ((format (printf, 5, 6)));
int Lorentzvfprintf(FILE *stream, const char*file, const char *func, const int line, const char *format, va_list args) __attribute__ ((format (printf, 5, 0)));

int Lorentzsprintf(const char *file, const char *func, const int line, char *__restrict__ buffer, const char *format, ...) __attribute__ ((format (printf, 5, 6)));
int Lorentzvsprintf(const char *file, const char *func, const int line, char *__restrict__ buffer, const char *format, va_list args) __attribute__ ((format (printf, 5, 0)));

int Lorentzasprintf(const char *file, const char *func, const int line, char **buffer, const char *format, ...) __attribute__ ((format (printf, 5, 6)));
int Lorentzvasprintf(const char *file, const char *func, const int line, char **buffer, const char *format, va_list args) __attribute__ ((format (printf, 5, 0)));

int Lorentzsnprintf(const char *file, const char *func, const int line, char *__restrict__ buffer, const size_t maxlen, const char *format, ...) __attribute__ ((format (printf, 6, 7)));
int Lorentzvsnprintf(const char *file, const char *func, const int line, char *__restrict__ buffer, const size_t maxlen, const char *format, va_list args) __attribute__ ((format (printf, 6, 0)));

// Interrupt-safe socket routines
ssize_t Lorentzwrite(int fd, const void *buf, size_t total, const char *file, const char *func, const int line);
int Lorentzaccept(int sockfd, struct sockaddr *addr, socklen_t *addrlen, const char *file, const char *func, const int line);
ssize_t Lorentzrecv(int sockfd, void *buf, size_t len, int flags, const bool warn, const char *file, const char *func, const int line);
ssize_t Lorentzrecvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen, const char *file, const char *func, const int line);
int Lorentzselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout, const char *file, const char *func, const int line);
ssize_t Lorentzsendto(int sockfd, void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen, const bool warn, const char *file, const char *func, const int line);

// Interrupt-safe thread routines
int Lorentzpthread_mutex_lock(pthread_mutex_t *__mutex, const char *file, const char *func, const int line);

// Interrupt-safe file routines
FILE *Lorentzfopen(const char *pathname, const char *mode, const char *file, const char *func, const int line) __attribute__ ((__malloc__));

// Syscall helpers
void syscalls_report_error(const char *error, FILE *stream, const int _errno, const char *format, const char *func, const char *file, const int line);

// String-related functions
size_t Lorentzstrlen(const char *s, const char *file, const char *func, const int line);
size_t Lorentzstrnlen(const char *s, const size_t maxlen, const char *file, const char *func, const int line);
char *Lorentzstrcpy(char *dest, const char *src, const char *file, const char *func, const int line);
char *Lorentzstrncpy(char *dest, const char *src, const size_t n, const char *file, const char *func, const int line);
void *Lorentzmemset(void *s, const int c, const size_t n, const char *file, const char *func, const int line);
void *Lorentzmemcpy(void *dest, const void *src, const size_t n, const char *file, const char *func, const int line);
void *Lorentzmemmove(void *dest, const void *src, const size_t n, const char *file, const char *func, const int line);
char *Lorentzstrstr(const char *haystack, const char *needle, const char *file, const char *func, const int line);
int Lorentzstrcmp(const char *s1, const char *s2, const char *file, const char *func, const int line);
int Lorentzstrncmp(const char *s1, const char *s2, const size_t n, const char *file, const char *func, const int line);
int Lorentzstrcasecmp(const char *s1, const char *s2, const char *file, const char *func, const int line);
int Lorentzstrncasecmp(const char *s1, const char *s2, const size_t n, const char *file, const char *func, const int line);
char *Lorentzstrcat(char *dest, const char *src, const char *file, const char *func, const int line);
char *Lorentzstrncat(char *dest, const char *src, const size_t n, const char *file, const char *func, const int line);
int Lorentzmemcmp(const void *s1, const void *s2, const size_t n, const char *file, const char *func, const int line);
void *Lorentzmemmem(const void *haystack, const size_t haystacklen, const void *needle, const size_t needlelen, const char *file, const char *func, const int line);

#endif //SYSCALLS_H
