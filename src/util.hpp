#pragma once

template <typename FuncT>
void split(const char *str, char sep, FuncT func)
{
	//adapted from http://www.martinbroadhurst.com/split-a-string-in-c.html
	//C++ should have a standard split anwyay for when you're not overly concerned about perf...
	unsigned int start = 0, stop;
	for (stop = 0; str[stop]; stop++)
	{
		if (str[stop] == sep)
		{
			func(str + start, stop - start);
			start = stop + 1;
		}
	}
	func(str + start, stop - start);
}
