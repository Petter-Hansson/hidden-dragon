
#include "pch.h"

#include "GameData.hpp"
#include "Util.hpp"

namespace
{
	struct Region
	{
		std::vector<uint8_t> Data;
		int32_t Base = 0; //we use signed base because dump automatically subtracts an offset to account for address randomization
	};

	struct Image
	{
		std::vector<Region> Regions;

		bool IsEmpty() const
		{
			return Regions.empty();
		}
	};

	constexpr uint32_t NoRegionSelected = std::numeric_limits<uint32_t>::max();

	static std::vector<Image> _imageStack;
	static uint32_t _selectedRegionBase = NoRegionSelected;

	struct Match
	{
		int Index = 0;
		int Score = 0;
		const Region* pRegion = nullptr;
		const std::vector<uint8_t>* pWorkBuffer = nullptr;

		bool operator<(const Match& rhs) const
		{
			return Score < rhs.Score;
		}
	};

	class SequenceMatching
	{
	public:
		void TryToMatchSequenceWithRegion(const std::vector<uint8_t>& sequence, const Region& region)
		{
			const std::vector<uint8_t>& workBuffer = region.Data;

			TryToMatchSequence(sequence, workBuffer, &region);
		}

		void TryToMatchSequenceWithDiff(const std::vector<uint8_t>& sequence, const dtl::Diff<uint8_t>& diff, const Region& fromRegion)
		{
			_WorkBuffers.emplace_back(new std::vector<uint8_t>);
			std::vector<uint8_t>& workBuffer = *_WorkBuffers.back();

			for (const auto& hunk : diff.getUniHunks())
			{
				for (const auto& se : hunk.change)
				{
					switch (se.second.type) {
					case dtl::SES_ADD:
					case dtl::SES_DELETE:
					case dtl::SES_COMMON:
						workBuffer.push_back(se.first);
						break;
					}
				}
			}

			TryToMatchSequence(sequence, workBuffer, &fromRegion);
		}

		void DisplayMatches(const std::vector<uint8_t>& sequence)
		{
			std::cout << _Matches.size() << " partial matches stored\n";

			int wrap = 0;
			while (!_Matches.empty())
			{
				std::cout << std::endl;
				const Match match = _Matches.top();
				_Matches.pop();

			RestartMatchEmit:
				std::cout << "Match at " << match.Index << "[";
				if (match.pRegion)
					std::cout << match.pRegion->Base;
				else
					std::cout << "???";
				std::cout << "] of score " << match.Score << "/" << sequence.size() << std::endl;

				EmitMatchSequence(sequence, match, wrap, [](const uint8_t value)
				{
					return (int)value;
				});
				EmitMatchSequence(sequence, match, wrap, [](const uint8_t value)
				{
					if (value < 32)
						return '_';
					return (char)value;
				});

				std::cout << "Continue (y/n/x)?> ";
				char shouldContinue;
				std::cin >> shouldContinue;
				if (shouldContinue == 'x') //extra
				{
					std::cout << "Amount of wrap? ";
					std::cin >> wrap;

					goto RestartMatchEmit;
				}

				if (shouldContinue != 'y' && shouldContinue != 'Y')
				{
					break;
				}
			}
		}
	private:
		std::priority_queue<Match> _Matches;
		int _WorstMatchScore = 0;
		int _BestMatchScore = 0;
		int _NumWorst = std::numeric_limits<int>::max();
		std::vector<std::unique_ptr<std::vector<uint8_t>>> _WorkBuffers;

		void TryToMatchSequence(const std::vector<uint8_t>& sequence, const std::vector<uint8_t>& workBuffer, const Region* region = nullptr)
		{
			if (sequence.size() > workBuffer.size())
			{
				return;
			}

			for (int i = 0, n = workBuffer.size() - sequence.size() + 1; i < n; ++i)
			{
				int matchScore = 0;
				for (int j = 0; j < (int)sequence.size(); ++j)
				{
					if (workBuffer[i + j] == sequence[j])
					{
						matchScore += 1;
					}
				}

				if (matchScore > 0)
				{
					AddMatch(matchScore, i, &workBuffer, region);
				}
			}
		}

		void AddMatch(int matchScore, int index, const std::vector<uint8_t>* workBuffer, const Region* region = nullptr)
		{
			//basic complexity limiter to avoid filling memory like hell
			//it would be preferable to get rid of worst matches to keep max size, but no point spending el mucho time
			_BestMatchScore = std::max(_BestMatchScore, matchScore);
			if (matchScore == _WorstMatchScore)
			{
				_NumWorst += 1;
			}
			if (matchScore >= _WorstMatchScore && (matchScore < _BestMatchScore || matchScore == 1) && _NumWorst > 25)
			{
				_WorstMatchScore += 1;
				_NumWorst = 0;
			}

			if (matchScore >= _WorstMatchScore)
			{
				Match match;
				match.Score = matchScore;
				match.Index = index;
				match.pRegion = region;
				match.pWorkBuffer = workBuffer;
				_Matches.push(match);
			}
		}

		template <typename TransformFuncT>
		static void EmitMatchSequence(const std::vector<uint8_t>& sequence, const Match& match, int wrap, TransformFuncT func)
		{
			const std::vector<uint8_t>& workBuffer = *match.pWorkBuffer;

			for (int i = -wrap; i < 0; ++i)
			{
				if (i + match.Index < 0)
					continue;
				const uint8_t value = workBuffer[i + match.Index];

				std::cout << func(value) << " ";
			}

			std::cout << ' ';

			for (int i = 0; i < (int)sequence.size(); ++i)
			{
				const uint8_t value = workBuffer[i + match.Index];

				std::cout << func(value) << " ";
			}

			std::cout << ' ';

			for (int i = sequence.size(); i < (int)sequence.size() + wrap; ++i)
			{
				if (i + match.Index >= (int)workBuffer.size())
					continue;
				const uint8_t value = workBuffer[i + match.Index];

				std::cout << func(value) << " ";
			}
			std::cout << std::endl;
		}
	};
}

static void PushNewImage()
{
	_imageStack.emplace_back();
}

static void PushImageIfNeeded()
{
	if (_imageStack.empty() || !_imageStack.back().IsEmpty())
		PushNewImage();
}

static void PopImage()
{
	if (_imageStack.empty())
		return;

	_imageStack.pop_back();
}

static void LoadBinaryFile(const std::string& filename, bool segmented)
{
	PushImageIfNeeded();

	std::ifstream is(filename, std::ios::binary);
	if (is.good())
	{
		is.seekg(0, std::ios_base::end);
		const auto length = is.tellg();
		is.seekg(0, std::ios_base::beg);

		if (segmented)
		{
			int64_t remaining = static_cast<int64_t>(length);
			while (remaining > 0)
			{
				if (length < 8)
				{
					std::cerr << "Corrupt region header encountered\n";
					break;
				}

				uint32_t regionBase;
				uint32_t regionLength;
				is.read(reinterpret_cast<char*>(&regionBase), 4);
				is.read(reinterpret_cast<char*>(&regionLength), 4);
				remaining -= 8;

				if (regionLength > remaining)
				{
					std::cerr << "Corrupt region header encountered\n";
					break;
				}

				Region newBuffer;
				newBuffer.Data.resize((std::size_t)regionLength);
				newBuffer.Base = regionBase;
				is.read(reinterpret_cast<char*>(&*newBuffer.Data.begin()), regionLength);

				_imageStack.back().Regions.push_back(std::move(newBuffer));

				remaining -= regionLength;
			}

			std::cout << "Loaded " << _imageStack.back().Regions.size() << " regions\n";
		}
		else
		{
			Region newBuffer;
			newBuffer.Data.resize((std::size_t)length);
			is.read(reinterpret_cast<char*>(&*newBuffer.Data.begin()), length);

			_imageStack.back().Regions.push_back(std::move(newBuffer));
		}

		std::cout << "Loaded " << length << " bytes\n";
	}
	else
	{
		std::cerr << "No such file\n";
	}
	is.close();
}

static std::vector<uint8_t> GetByteSequenceFromDescription(const std::string& description)
{
	std::vector<uint8_t> sequence;
	constexpr char separator = ' ';
	Split(description.c_str(), separator, [&](const char* data, std::size_t len)
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

static void SelectRegion(const std::string& description)
{
	if (description == "")
	{
		_selectedRegionBase = NoRegionSelected;
		std::cout << "Deselected region\n";
	}

	if (std::all_of(description.cbegin(), description.cend(), [](char c)
	{
		return std::isdigit(c);
	}))
	{
		const uint32_t addr = static_cast<uint32_t>(std::atoll(description.c_str()));

		if (ContainsIf(_imageStack.back().Regions, [&](const Region& region)
		{
			return region.Base == addr;
		}))
		{
			_selectedRegionBase = addr;
			std::cout << "Selected region\n";
		}
		else
		{
			std::cout << "No region with such a base address\n";
		}
	}
	else
	{
		std::cout << "Must currently enter region base manually; TODO: select from list and/or select minimum edit distance\n";
	}
}

static void FindSequence(const std::string& description)
{
	if (_imageStack.empty())
		return;

	const std::vector<uint8_t> sequence = GetByteSequenceFromDescription(description);

	//for now, do naive O(_workBuffer.size() * sequence.size()) version. optimize if needed

	SequenceMatching matching;
	for (const Region& region : _imageStack.back().Regions)
	{
		const std::vector<uint8_t>& workBuffer = region.Data;

		if (_selectedRegionBase != NoRegionSelected && _selectedRegionBase != region.Base)
		{
			continue;
		}

		matching.TryToMatchSequenceWithRegion(sequence, region);
	}
	
	matching.DisplayMatches(sequence);
}

template <typename T>
static void EmitCharacter(T c, bool ascii)
{
	if (ascii)
	{
		if (c < 32)
			c = '_';
		std::cout << c << " ";
	}
	else
	{
		std::cout << (int)c << " ";
	}
}

template<typename RegionPointerT>
static dtl::Diff<uint8_t> CalcDiffsOfRegion(RegionPointerT sourceRegion, RegionPointerT targetRegion)
{
	dtl::Diff<uint8_t> diff(sourceRegion->Data, targetRegion->Data);
	diff.enableHuge();
	diff.compose();
	diff.composeUnifiedHunks();

	return diff;
}

static void ShowDataDiffs(bool ascii)
{
	if (_imageStack.size() < 2)
		return;

	if (_selectedRegionBase == NoRegionSelected)
	{
		std::cout << "Must have selected a region to do this\n";
	}

	const auto& regionsOfTarget = _imageStack.back().Regions;
	const auto targetRegion = std::find_if(regionsOfTarget.cbegin(), regionsOfTarget.cend(), [](const Region& r)
	{
		return r.Base == _selectedRegionBase;
	});
	const auto& regionsOfSource = _imageStack[_imageStack.size() - 2].Regions;
	const auto sourceRegion = std::find_if(regionsOfSource.cbegin(), regionsOfSource.cend(), [](const Region& r)
	{
		return r.Base == _selectedRegionBase;
	});

	std::cout << "Size source/target: " << sourceRegion->Data.size() << "/" << targetRegion->Data.size() << std::endl;

	const auto diff = CalcDiffsOfRegion(sourceRegion, targetRegion);

	for (const auto& hunk : diff.getUniHunks())
	{
		std::cout << "@@"
			<< " -" << hunk.a << "," << hunk.b
			<< " +" << hunk.c << "," << hunk.d
			<< " @@" << std::endl;

		for (const auto& se : hunk.common[0])
		{
			EmitCharacter(se.first, ascii);
		}

		auto lastType = dtl::SES_COMMON;

		for (const auto& se : hunk.change)
		{
			switch (se.second.type) {
			case dtl::SES_ADD:
				if (lastType != se.second.type)
					std::cout << std::endl << SES_MARK_ADD;
				EmitCharacter(se.first, ascii);
				break;
			case dtl::SES_DELETE:
				if (lastType != se.second.type)
					std::cout << std::endl << SES_MARK_DELETE;
				EmitCharacter(se.first, ascii);
				break;
			case dtl::SES_COMMON:
				if (lastType != se.second.type)
					std::cout << std::endl;
				EmitCharacter(se.first, ascii);
				break;
			}

			lastType = se.second.type;
		}
		std::cout << std::endl;

		for (const auto& se : hunk.common[1])
		{
			EmitCharacter(se.first, ascii);
		}
		std::cout << std::endl;
	}
}

static void FindSequenceInDiffOfRegion(SequenceMatching& matching, const std::vector<uint8_t>& sequence, const Region& source, const Region& target)
{
	if (source.Data.size() == 0)
		matching.TryToMatchSequenceWithRegion(sequence, target);
	else if (target.Data.size() == 0)
		matching.TryToMatchSequenceWithRegion(sequence, source);
	else
	{
		assert(source.Base == target.Base);

		constexpr int maxSize = 3000000;
		if (source.Data.size() > maxSize)
		{
			std::cout << "Too large region to diff: " << source.Data.size() << std::endl;
			return;
		}
		if (target.Data.size() > maxSize)
		{
			std::cout << "Too large region to diff: " << target.Data.size() << std::endl;
			return;
		}

		const auto diff = CalcDiffsOfRegion(&source, &target);

		matching.TryToMatchSequenceWithDiff(sequence, diff, source);
	}
}

static void FindSequenceInDiffsOfAllRegions(const std::string& description)
{
	if (_imageStack.size() < 2)
		return;

	const std::vector<uint8_t> sequence = GetByteSequenceFromDescription(description);

	const std::vector<Region>& regionsOfTarget = _imageStack.back().Regions;
	const std::vector<Region>& regionsOfSource = _imageStack[_imageStack.size() - 2].Regions;

	const static Region emptyRegion;
	SequenceMatching matching;

	for (const Region& region : regionsOfSource)
	{
		const auto it = std::find_if(regionsOfTarget.cbegin(), regionsOfTarget.cend(), [&](const Region& other)
		{
			return other.Base == region.Base;
		});

		if (it == regionsOfTarget.cend())
		{
			FindSequenceInDiffOfRegion(matching, sequence, region, emptyRegion);
		}
		else
		{
			FindSequenceInDiffOfRegion(matching, sequence, region, *it);
		}
	}
	for (const Region& region : regionsOfTarget)
	{
		const bool found = ContainsIf(regionsOfSource, [&](const Region& other)
		{
			return other.Base == region.Base;
		});
		if (!found)
		{
			FindSequenceInDiffOfRegion(matching, sequence, emptyRegion, region);
		}
	}

	matching.DisplayMatches(sequence);
}

static void ShowRegion(const Region& region)
{
	std::cout << "Region at " << region.Base << " (length " << region.Data.size() << ")\n";

	uint32_t offset = 0;
	while (offset < region.Data.size())
	{
		if (offset != 0)
		{
			std::cout << "Continue (y/n)?> ";
			char shouldContinue;
			std::cin >> shouldContinue;

			if (shouldContinue != 'y' && shouldContinue != 'Y')
			{
				break;
			}
		}

		constexpr uint32_t NEXT = 32;
		const uint32_t end = std::min(offset + NEXT, region.Data.size());
		std::cout << "Offset " << offset << " (addr " << (offset + region.Base) << "): ";
		for (uint32_t i = offset; i < end; ++i)
		{
			EmitCharacter(region.Data[i], false);
		}
		std::cout << std::endl;
		std::cout << "Offset " << offset << " (addr " << (offset + region.Base) << "): ";
		for (uint32_t i = offset; i < end; ++i)
		{
			EmitCharacter(region.Data[i], true);
		}
		std::cout << std::endl;
		offset += NEXT;
	}
}

static void FindRegionDiffs()
{
	if (_imageStack.size() < 2)
		return;

	const std::vector<Region>& regionsOfTarget = _imageStack.back().Regions;
	const std::vector<Region>& regionsOfSource = _imageStack[_imageStack.size() - 2].Regions;

	auto checkForAdditions = [](const std::vector<Region>& a, const std::vector<Region>& b, const char* doesNotExistIn)
	{
		for (const Region& region : a)
		{
			const bool found = ContainsIf(b, [&](const Region& other)
			{
				return other.Base == region.Base;
			});

			if (!found)
			{
				std::cout << "Region at " << region.Base << " does not exist in " << doesNotExistIn << std::endl;
				ShowRegion(region);
			}
		}
	};
	
	checkForAdditions(regionsOfSource, regionsOfTarget, "new");
	checkForAdditions(regionsOfTarget, regionsOfSource, "old");
}

int main()
{
	std::cout << "BinExplorer commands:\n";
	std::cout << "loadb" << std::endl;
	std::cout << "loads" << std::endl;
	std::cout << "selr" << std::endl;
	std::cout << "finds" << std::endl;
	std::cout << "diffb" << std::endl;
	std::cout << "diffs" << std::endl;
	std::cout << "diffr" << std::endl;
	std::cout << "push" << std::endl;
	std::cout << "pop" << std::endl;
	std::cout << "cls" << std::endl;
	std::cout << "exit" << std::endl;
	std::cout << std::endl;
	
	char lineBuffer[1024];
	bool needsPrompt = true;

	for (;;)
	{
		if (needsPrompt)
			std::cout << "?> ";
		needsPrompt = true;

		std::cin.getline(lineBuffer, sizeof(lineBuffer) - 1);
		const std::string line = lineBuffer;
		const auto delimiterPos = line.find(' ');
		const std::string command = line.substr(0, delimiterPos == std::string::npos ? line.size() : delimiterPos);
		std::string arg;
		if (delimiterPos != std::string::npos)
		{
			arg = line.substr(delimiterPos + 1, line.size() - 1 - delimiterPos);
		}

		if (command == "")
		{
			//residual of activity of a command...
			needsPrompt = false;
			continue;
		}
		else if (command == "loadb")
		{
			LoadBinaryFile(arg, false);
		}
		else if (command == "loads")
		{
			LoadBinaryFile(arg, true);
		}
		else if (command == "selr")
		{
			SelectRegion(arg);
		}
		else if (command == "finds")
		{
			FindSequence(arg);
		}
		else if (command == "findd")
		{
			FindSequenceInDiffsOfAllRegions(arg);
		}
		else if (command == "diffb")
		{
			ShowDataDiffs(false);
		}
		else if (command == "diffs")
		{
			ShowDataDiffs(true);
		}
		else if (command == "diffr")
		{
			FindRegionDiffs();
		}
		else if (command == "push")
		{
			PushImageIfNeeded();
		}
		else if (command == "pop")
		{
			PopImage();
			std::cout << _imageStack.size() << " buffers remaining on stack\n";
		}
		else if (command == "cls")
		{
			_imageStack.clear();
		}
		else if (command == "exit")
		{
			return 0;
		}
		else
		{
			std::cout << "Unknown command \"" << command.c_str() << "\"\n";
		}
	}
}
