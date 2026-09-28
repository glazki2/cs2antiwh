#include <cstddef>

#include "masked_occlusion_culling/MaskedOcclusionCulling.h"

// CS2GLAZ requests the baseline SSE4.1 implementation. These unreachable
// factories keep the unmodified upstream dispatch source linkable without
// raising the plugin's existing AVX CPU requirement to AVX2 or AVX-512.

namespace MaskedOcclusionCullingAVX2
{
	MaskedOcclusionCulling* CreateMaskedOcclusionCulling(MaskedOcclusionCulling::pfnAlignedAlloc, MaskedOcclusionCulling::pfnAlignedFree)
	{
		return nullptr;
	}
} // namespace MaskedOcclusionCullingAVX2

namespace MaskedOcclusionCullingAVX512
{
	MaskedOcclusionCulling* CreateMaskedOcclusionCulling(MaskedOcclusionCulling::pfnAlignedAlloc, MaskedOcclusionCulling::pfnAlignedFree)
	{
		return nullptr;
	}
} // namespace MaskedOcclusionCullingAVX512
