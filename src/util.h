#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

#define container_of(ptr, type, member) ({ \
                const typeof( ((type *)0)->member ) *__mptr = (ptr); \
                (type *)( (char *)__mptr - offsetof(type,member) );})

#define MIN(x,y) ( (x) < (y) ? (x) : (y) )
#define MAX(x,y) ( (x) > (y) ? (x) : (y) )

#define ARRAY_SIZE(a)  ( sizeof(a) / sizeof(a[0]) )

#define buf_add_char(_buf_, _len_, _ch_) \
	do { \
		if (_len_ < 1) return -1; \
		_buf_[0] = _ch_; \
		_buf_++; \
		_len_--; \
	} while (0)


#define buf_add_str(_buf_, _len_, _fmt_...) \
	do { \
		int written = snprintf(_buf_, _len_, _fmt_); \
		if (written < 0 || written >= _len_) \
			return -1; \
		_buf_ += written; \
		_len_ -= written; \
	} while (0)

#endif
