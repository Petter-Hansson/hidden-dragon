#pragma once

typedef void(*split_fn)(const char *, size_t, void *);

inline void split(const char *str, char sep, split_fn fun, void *data)
{
	//copied from http://www.martinbroadhurst.com/split-a-string-in-c.html
	//C++ should have a standard split anwyay for when you're not overly concerned about perf...
	unsigned int start = 0, stop;
	for (stop = 0; str[stop]; stop++) {
		if (str[stop] == sep) {
			fun(str + start, stop - start, data);
			start = stop + 1;
		}
	}
	fun(str + start, stop - start, data);
}
