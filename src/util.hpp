#pragma once

template <typename IteratorT, typename T>
bool Contains(IteratorT begin, IteratorT end, const T& val)
{
	return std::find(begin, end, val) != end;
}

template <typename CollectionT, typename T>
bool Contains(const CollectionT& collection, const T& val)
{
	return Contains(collection.cbegin(), collection.cend(), val);
}

template <typename IteratorT, typename FuncT>
bool ContainsIf(IteratorT begin, IteratorT end, FuncT func)
{
	return std::find_if(begin, end, func) != end;
}

template <typename CollectionT, typename FuncT>
bool ContainsIf(const CollectionT& collection, FuncT func)
{
	return ContainsIf(collection.cbegin(), collection.cend(), func);
}

template <typename FuncT>
void Split(const char *str, char separator, FuncT func)
{
	int start = 0;
	int stop = 0;
	bool hadContent = false;
	for (; str[stop]; ++stop)
	{
		const char c = str[stop];
		if (c == separator)
		{
			if (hadContent)
				func(str + start, stop - start - 1);
			start = stop + 1;
			hadContent = false;
		}
		else
		{
			hadContent = true;
		}
	}

	if (hadContent)
		func(str + start, stop - start);
}

class InformalByteWriter
{
public:
	explicit InformalByteWriter(std::vector<uint8_t>& underlying)
		: _Underlying(underlying)
	{
	}

	template <typename T>
	void WriteBytes(const T& element)
	{
		const uint8_t* const bytes = reinterpret_cast<const uint8_t*>(&element);
		for (int i = 0; i < sizeof(element); ++i)
		{
			_Underlying.push_back(bytes[i]);
		}
	}

	void WriteBytes(const uint8_t* str, std::size_t len)
	{
		for (std::size_t i = 0; i < len; ++i)
		{
			_Underlying.push_back(str[i]);
		}
	}

	void WriteString(const char* str, std::size_t len)
	{
		WriteBytes(reinterpret_cast<const uint8_t*>(str), len);
	}

	void WriteString(const char* str)
	{
		WriteString(str, std::strlen(str));
	}

	void WriteString(const std::string& str)
	{
		WriteString(str.c_str(), str.size());
	}

	void WriteDescription(const char* sequence)
	{
		Split(sequence, ' ', [&](const char* data, std::size_t len)
		{
			_Underlying.push_back(atoi(data));
		});
	}

	void WriteDescription(const std::string& str)
	{
		WriteDescription(str.c_str());
	}
private:
	std::vector<uint8_t>& _Underlying;
};

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
