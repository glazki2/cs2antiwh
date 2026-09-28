#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace cs2glaz
{

	struct semantic_version
	{
		int major {};
		int minor {};
		int patch {};
	};

	inline bool parse_semantic_version(std::string_view text, semantic_version& version)
	{
		if (!text.empty() && text.front() == 'v')
		{
			text.remove_prefix(1);
		}
		std::array<int*, 3> parts {&version.major, &version.minor, &version.patch};
		for (size_t part = 0; part < parts.size(); ++part)
		{
			if (text.empty() || !std::isdigit(static_cast<unsigned char>(text.front())))
			{
				return false;
			}
			int value = 0;
			const char* begin = text.data();
			const char* end = begin;
			while (end != begin + text.size() && std::isdigit(static_cast<unsigned char>(*end)))
			{
				++end;
			}
			const auto parsed = std::from_chars(begin, end, value);
			if (parsed.ec != std::errc {} || parsed.ptr != end || value > 100000)
			{
				return false;
			}
			*parts[part] = value;
			text.remove_prefix(static_cast<size_t>(end - begin));
			if (part + 1 < parts.size())
			{
				if (text.empty() || text.front() != '.')
				{
					return false;
				}
				text.remove_prefix(1);
			}
		}
		return text.empty();
	}

	inline int compare_semantic_versions(const semantic_version& left, const semantic_version& right)
	{
		if (left.major != right.major)
		{
			return left.major < right.major ? -1 : 1;
		}
		if (left.minor != right.minor)
		{
			return left.minor < right.minor ? -1 : 1;
		}
		return left.patch == right.patch ? 0 : (left.patch < right.patch ? -1 : 1);
	}

	inline bool safe_update_version(std::string_view version)
	{
		semantic_version parsed;
		return version.size() <= 32 && parse_semantic_version(version, parsed);
	}

	inline bool valid_sha256_digest(std::string_view digest)
	{
		constexpr std::string_view prefix = "sha256:";
		return digest.size() == prefix.size() + 64 && digest.substr(0, prefix.size()) == prefix
			   && std::all_of(digest.begin() + static_cast<std::ptrdiff_t>(prefix.size()), digest.end(),
							  [](unsigned char character) { return std::isxdigit(character); });
	}

	inline bool parse_crc32(std::string_view text, uint32_t& crc)
	{
		if (text.size() != 10 || text.substr(0, 2) != "0x")
		{
			return false;
		}
		const auto result = std::from_chars(text.data() + 2, text.data() + text.size(), crc, 16);
		return result.ec == std::errc {} && result.ptr == text.data() + text.size() && crc != 0;
	}

	inline std::string_view update_setting_name(std::string_view line)
	{
		const size_t start = line.find_first_not_of(" \t");
		if (start == std::string_view::npos)
		{
			return {};
		}
		size_t end = start;
		while (end < line.size() && (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_'))
		{
			++end;
		}
		const std::string_view name = line.substr(start, end - start);
		return name.rfind("cs2glaz_", 0) == 0 || name == "sv_enable_donttransmit" || name == "mp_playerid" ? name : std::string_view {};
	}

	inline bool safe_update_archive_path(std::string_view path)
	{
		if (path.empty() || path.front() == '/' || path.find_first_of("\\:") != std::string_view::npos)
		{
			return false;
		}
		for (size_t start = 0; start <= path.size();)
		{
			const size_t end = path.find('/', start);
			const std::string_view part = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
			if (part.empty() || part == "." || part == "..")
			{
				return false;
			}
			if (end == std::string_view::npos)
			{
				break;
			}
			start = end + 1;
		}
		return path.rfind("addons/cs2glaz/", 0) == 0 || path.rfind("tools/", 0) == 0 || path.rfind("licenses/", 0) == 0
			   || path == "addons/metamod/cs2glaz.vdf" || path == "cfg/cs2glaz.cfg" || path == "LICENSE" || path == "THIRD_PARTY_NOTICES"
			   || path == "CHANGELOG.md" || path == "README.md";
	}

} // namespace cs2glaz
