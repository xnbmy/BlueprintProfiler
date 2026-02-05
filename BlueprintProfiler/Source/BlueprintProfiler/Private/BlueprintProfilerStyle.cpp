#include "BlueprintProfilerStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Framework/Application/SlateApplication.h"
#include "Slate/SlateGameResources.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyleMacros.h"

#define RootToContentDir Style->RootToContentDir

TSharedPtr<FSlateStyleSet> FBlueprintProfilerStyle::StyleInstance = nullptr;

void FBlueprintProfilerStyle::Initialize()
{
	if (!StyleInstance.IsValid())
	{
		StyleInstance = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
	}
}

void FBlueprintProfilerStyle::Shutdown()
{
	FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
	ensure(StyleInstance.IsUnique());
	StyleInstance.Reset();
}

FName FBlueprintProfilerStyle::GetStyleSetName()
{
	static FName StyleSetName(TEXT("BlueprintProfilerStyle"));
	return StyleSetName;
}

const ISlateStyle& FBlueprintProfilerStyle::Get()
{
	return *StyleInstance;
}

void FBlueprintProfilerStyle::ReloadTextures()
{
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
	}
}

TSharedRef<FSlateStyleSet> FBlueprintProfilerStyle::Create()
{
	TSharedRef<FSlateStyleSet> Style = MakeShareable(new FSlateStyleSet(GetStyleSetName()));
	Style->SetContentRoot(IPluginManager::Get().FindPlugin("BlueprintProfiler")->GetBaseDir() / TEXT("Resources"));

	// Define icon sizes
	const FVector2D Icon16x16(16.0f, 16.0f);
	const FVector2D Icon20x20(20.0f, 20.0f);
	const FVector2D Icon40x40(40.0f, 40.0f);

	// Set plugin icon (using default for now)
	Style->Set("BlueprintProfiler.OpenPluginWindow", new IMAGE_BRUSH_SVG(TEXT("PlaceholderButtonIcon"), Icon40x40));

	// Define colors for different severity levels
	Style->Set("BlueprintProfiler.Severity.Low", FLinearColor(0.0f, 1.0f, 0.0f, 1.0f));      // Green
	Style->Set("BlueprintProfiler.Severity.Medium", FLinearColor(1.0f, 1.0f, 0.0f, 1.0f));   // Yellow
	Style->Set("BlueprintProfiler.Severity.High", FLinearColor(1.0f, 0.5f, 0.0f, 1.0f));     // Orange
	Style->Set("BlueprintProfiler.Severity.Critical", FLinearColor(1.0f, 0.0f, 0.0f, 1.0f)); // Red

	// Define text styles
	Style->Set("BlueprintProfiler.Text.Normal", FTextBlockStyle()
		.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 9))
		.SetColorAndOpacity(FLinearColor::White)
	);

	Style->Set("BlueprintProfiler.Text.Bold", FTextBlockStyle()
		.SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 9))
		.SetColorAndOpacity(FLinearColor::White)
	);

	return Style;
}

#undef RootToContentDir