
#include "pch.h"

#include "Util.hpp"

static std::vector<uint8_t> _workBuffer;

static void LoadBinaryFile(const std::string& filename)
{
	std::ifstream is(filename, std::ios::binary);
	if (is.good())
	{
		is.seekg(0, std::ios_base::end);
		const auto length = is.tellg();
		is.seekg(0, std::ios_base::beg);
		_workBuffer.resize((std::size_t)length);
		is.read(reinterpret_cast<char*>(&*_workBuffer.begin()), length);

		std::cout << "Loaded " << length << " bytes\n";
	}
	else
	{
		std::cerr << "No such file\n";
	}
	is.close();
}

namespace
{
	struct Match
	{
		int Index = 0;
		int Score = 0;

		bool operator<(const Match& rhs) const
		{
			return Score < rhs.Score;
		}
	};
}

static std::vector<uint8_t> GetByteSequenceFromDescription(const std::string& description)
{
	std::vector<uint8_t> sequence;
	constexpr char separator = ' ';
	split(description.c_str(), separator, [&](const char* data, std::size_t len)
	{
		uint8_t nextValue = 0;

		const bool isString = data[0] == '"';
		if (isString)
		{
			data += 1;
			len -= 1;
		}

		bool allDigits = true;
		for (int i = 0; i < (int)len; ++i)
		{
			const char c = data[i];
			if (c == separator)
			{
				assert(i == len - 1);
				//this can only be last character
				len -= 1;
				break;
			}
			if (!std::isdigit(c))
			{
				allDigits = false;
			}
		}

		if (isString && data[len - 1] == '"')
		{
			len -= 1;
		}

		if (isString || !allDigits)
		{
			for (int i = 0; i < (int)len; ++i)
			{
				sequence.push_back(static_cast<uint8_t>(data[i]));
			}
		}
		else if (allDigits)
			sequence.push_back(atoi(data));
	});

	return sequence;
}

static void FindSequence(const std::string& description)
{
	std::vector<uint8_t> sequence = GetByteSequenceFromDescription(description);

	if (sequence.size() > _workBuffer.size())
	{
		std::cout << "Sought sequence can't be larger than buffer to search\n";
		return;
	}

	//for now, do naive O(_workBuffer.size() * sequence.size()) version. optimize if needed

	std::priority_queue<Match> matches;

	int worstMatchScore = 0;
	int bestMatchScore = 0;
	int numWorst = std::numeric_limits<int>::max();
	for (int i = 0, n = _workBuffer.size() - sequence.size() + 1; i < n; ++i)
	{
		int matchScore = 0;
		for (int j = 0; j < (int)sequence.size(); ++j)
		{
			if (_workBuffer[i + j] == sequence[j])
			{
				matchScore += 1;
			}
		}

		if (matchScore > 0)
		{
			//basic complexity limiter to avoid filling memory like hell
			//it would be preferable to get rid of worst matches to keep max size, but no point spending el mucho time
			bestMatchScore = std::max(bestMatchScore, matchScore);
			if (matchScore == worstMatchScore)
			{
				numWorst += 1;
			}
			if (matchScore >= worstMatchScore && (matchScore < bestMatchScore || matchScore == 1) && numWorst > 25)
			{
				worstMatchScore += 1;
				numWorst = 0;
			}

			if (matchScore >= worstMatchScore)
			{
				Match match;
				match.Score = matchScore;
				match.Index = i;
				matches.push(match);
			}
		}
	}

	std::cout << matches.size() << " partial matches stored\n";

	while (!matches.empty())
	{
		std::cout << std::endl;
		const Match match = matches.top();
		matches.pop();

		std::cout << "Match at " << match.Index << " of score " << match.Score << "/" << sequence.size() << std::endl;

		for (int i = 0; i < (int)sequence.size(); ++i)
		{
			const uint8_t value = _workBuffer[i + match.Index];

			std::cout << (int)value << " ";
		}
		std::cout << std::endl;

		for (int i = 0; i < (int)sequence.size(); ++i)
		{
			const uint8_t value = _workBuffer[i + match.Index];
			if (value < 32)
				std::cout << '_';
			else
				std::cout << value;
		}
		std::cout << std::endl;

		std::cout << "Continue (y/n)?> ";
		char shouldContinue;
		std::cin >> shouldContinue;
		if (shouldContinue != 'y' && shouldContinue != 'Y')
			break;
	}
}

int main()
{
	std::cout << "BinExplorer commands:\n";
	std::cout << "loadb" << std::endl;
	std::cout << "finds" << std::endl;
	std::cout << "exit" << std::endl;
	std::cout << std::endl;
	
	char lineBuffer[1024];

	for (;;)
	{
		std::cout << "?> ";
		std::cin.getline(lineBuffer, sizeof(lineBuffer) - 1);
		const std::string line = lineBuffer;
		const auto delimiterPos = line.find(' ');
		const std::string command = line.substr(0, delimiterPos == std::string::npos ? line.size() : delimiterPos);
		std::string arg;
		if (delimiterPos != std::string::npos)
		{
			arg = line.substr(delimiterPos + 1, line.size() - 1 - delimiterPos);
		}

		if (command == "loadb")
		{
			LoadBinaryFile(arg);
		}
		else if (command == "finds")
		{
			FindSequence(arg);
		}
		else if (command == "exit")
		{
			return 0;
		}
		else
		{
			std::cout << "Unknown command\n";
		}
	}
}
