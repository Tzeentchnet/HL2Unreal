#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

TAutoConsoleVariable<float> CVarHL2Scale(
    TEXT("hl2.scale"), 2.54f, TEXT("World scale multiplier (inches->cm)"));

TAutoConsoleVariable<int32> CVarHL2ImportProps(
    TEXT("hl2.import_props"), 1, TEXT("0 = skip, 1 = data-table only, 2 = full instances"));
