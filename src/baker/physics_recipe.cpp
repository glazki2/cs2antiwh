#include "physics_recipe.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace cs2glaz
{

	bool physics_group_accepted(const std::vector<std::string>& tags, const std::string& surface_property)
	{
		const bool untagged = tags.empty();
		const bool explicitly_opaque =
			std::find(tags.begin(), tags.end(), "solid") != tags.end() && std::find(tags.begin(), tags.end(), "blocklight") != tags.end();
		if (!untagged && !explicitly_opaque)
		{
			return false;
		}

		std::string surface = surface_property;
		std::transform(surface.begin(), surface.end(), surface.begin(),
					   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
		constexpr std::string_view porous[] = {"glass", "chainlink", "grate", "foliage", "tree", "water",  "puddle", "window",
											   "cloth", "basket",    "mesh",  "netting", "wire", "screen", "vent"};
		for (const std::string_view value : porous)
		{
			if (surface.find(value) != std::string::npos)
			{
				return false;
			}
		}
		if (explicitly_opaque || surface.empty())
		{
			return true;
		}

		// ponytail: keep this conservative; switch to rendered-material opacity if custom maps outgrow the known surface list.
		constexpr std::string_view opaque[] = {"default", "concrete", "rock",  "boulder",    "brick", "plaster",   "sheetrock", "tile", "dirt",
											   "sand",    "gravel",   "metal", "solidmetal", "wood",  "porcelain", "pottery",   "clay", "carpet"};
		return std::any_of(std::begin(opaque), std::end(opaque), [&surface](std::string_view value) { return surface.starts_with(value); });
	}

} // namespace cs2glaz
