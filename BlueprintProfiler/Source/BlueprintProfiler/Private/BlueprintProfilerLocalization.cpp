#include "BlueprintProfilerLocalization.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"

FText FBlueprintProfilerLocalization::GetText(const FString& Key, const FString& DefaultChinese, const FString& English)
{
    if (IsChinese())
    {
        return FText::FromString(DefaultChinese);
    }
    return FText::FromString(English);
}

bool FBlueprintProfilerLocalization::IsChinese()
{
    FString CurrentLanguage = FInternationalization::Get().GetCurrentLanguage()->GetName();
    return CurrentLanguage.Contains(TEXT("zh")) || CurrentLanguage.Contains(TEXT("Chinese"));
}

bool FBlueprintProfilerLocalization::IsEnglish()
{
    FString CurrentLanguage = FInternationalization::Get().GetCurrentLanguage()->GetName();
    return CurrentLanguage.Contains(TEXT("en")) || CurrentLanguage.Contains(TEXT("English"));
}
