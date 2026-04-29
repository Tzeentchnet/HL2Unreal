#pragma once
#include "CoreMinimal.h"

class USurfaceProp;

namespace HL2SP
{
    struct FBuildResult
    {
        int32 NumCreated = 0;
        int32 NumCached  = 0;
        int32 NumFailed  = 0;
    };

    // Parse `<AbsoluteScriptPath>` (KV1 surfaceproperties.txt format) and emit
    // one USurfaceProp asset per surface entry under
    // `<DestPackagePath>/<surface_name>`. Idempotent — re-runs with the same
    // inputs reuse existing assets.
    HL2BSPIMPORTER_API FBuildResult BuildAllFromScript(
        const FString& AbsoluteScriptPath,
        const FString& DestPackagePath,
        FString&       OutError);
}
