#pragma once
#include <dxgi.h>
#include <string>
#include <vector>

namespace mcdr
{
    struct RtxSupport
    {
        bool dlssProbed = false;
        bool dlssQueried = false;
        std::string dlssQueryNote;
        bool ngxRuntime = false;
        std::string ngxRuntimePath;
        std::string ngxNote;

        bool dlssSuperResolution = false;
        bool dlssFrameGeneration = false;
        bool dlssRayReconstruction = false;
        std::string dlssSuperResolutionWhy;
        std::string dlssFrameGenerationWhy;
        std::string dlssRayReconstructionWhy;

        bool nrdLinked = false;
        std::string nrdVersion;
        std::string nrdVersionDate;
        std::vector<std::string> nrdDenoisers;

        bool xessProbed = false;
        bool xessRuntime = false;
        std::string xessVersion;
        std::string xessFileVersion;
        std::string xessRuntimePath;
        std::string xessNote;
        bool xessFrameGenPresent = false;
        bool xessLowLatencyPresent = false;

        bool ffxProbed = false;
        bool ffxLoaderLoaded = false;
        std::string ffxLoaderVersion;
        std::string ffxLoaderPath;
        std::string ffxNote;
        bool ffxUpscalerPresent = false;
        bool ffxFrameGenPresent = false;
        bool ffxDenoiserPresent = false;
        bool ffxRadianceCachePresent = false;
        std::string ffxUpscalerVersion;
        std::string ffxFrameGenVersion;
        std::string ffxDenoiserVersion;
        std::string ffxRadianceCacheVersion;

        std::string summary() const;

        std::string jsonFragment() const;
    };

    const RtxSupport& probeRtxSupport(IDXGIAdapter* adapter);

    void resetRtxSupportCache();
}
