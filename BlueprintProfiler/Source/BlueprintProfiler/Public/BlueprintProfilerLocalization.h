// Copyright xnbmy 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Helper class for localization
 * Provides runtime language switching support
 */
class BLUEPRINTPROFILER_API FBlueprintProfilerLocalization
{
public:
    /**
     * Get localized text based on current editor language
     */
    static FText GetText(const FString& Key, const FString& DefaultChinese, const FString& English);
    
    /**
     * Check if current editor language is Chinese
     */
    static bool IsChinese();
    
    /**
     * Check if current editor language is English
     */
    static bool IsEnglish();
};

// Macro for easy localization
#define BP_LOCTEXT(Key, Chinese, English) FBlueprintProfilerLocalization::GetText(TEXT(Key), TEXT(Chinese), TEXT(English))
