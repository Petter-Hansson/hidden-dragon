#pragma once

typedef void(*split_fn)(const char *, size_t, void *);

void split(const char *str, char sep, split_fn fun, void *data);
