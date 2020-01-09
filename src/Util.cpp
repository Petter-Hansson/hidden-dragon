#include "pch.h"

#include "Util.hpp"

std::vector<std::string> GetAllFilesInDirectory(const std::string& path, PathType pathType)
{
	auto pathLeafString = [&](const std::experimental::filesystem::directory_entry& entry)
	{
		if (pathType == PathType::JustFilename)
			return entry.path().filename().string();

		return entry.path().string();
	};

	//std::experimental::filesystem::path p(name);
	std::vector<std::string> v;
	std::experimental::filesystem::directory_iterator start(path);
	std::experimental::filesystem::directory_iterator end;
	std::transform(start, end, std::back_inserter(v), pathLeafString);

	return std::move(v);
}
