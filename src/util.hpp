#pragma once

template <typename FuncT>
void Split(const char *str, char sep, FuncT func)
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

enum class PathType
{
	Full,
	JustFilename,
};
std::vector<std::string> GetAllFilesInDirectory(const std::string& path, PathType pathType);

class Timer
{
public:
	typedef std::chrono::high_resolution_clock _Clock;

	Timer()
	{
		Restart();
	}

	explicit Timer(const decltype(_Clock::now())& start)
	{
		_Start = start;
	}

	void Restart()
	{
		_Start = _Clock::now();
	}

	double GetElapsed() const
	{
		auto end = _Clock::now();
		std::chrono::duration<double> duration = end - _Start;
		return std::chrono::duration_cast<std::chrono::duration<double>>(duration).count();
	}

	const decltype(_Clock::now())& GetStart() const
	{
		return _Start;
	}
private:
	decltype(_Clock::now()) _Start;
};
