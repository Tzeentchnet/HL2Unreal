#pragma once
#include "CoreMinimal.h"

// Minimal Valve KeyValues parser specialised for `.vmt` material files.
//
// VMT grammar (pragmatically):
//   ROOT       := SHADER '{' BODY '}'
//   BODY       := (PAIR | BLOCK)*
//   PAIR       := KEY VALUE
//   BLOCK      := KEY '{' BODY '}'
//   KEY/VALUE  := QUOTED_STRING | BARE_TOKEN
//   QUOTED_STRING := '"' [^"]* '"'
//   BARE_TOKEN    := [^\s{}"]+
// Comments: '//' to end of line. Keys are case-insensitive; values keep their
// original case but trailing/leading whitespace is stripped.
//
// `Patch { include "..." replace { ... } insert { ... } }` indirection is
// resolved by the caller via FParseVmt::ResolvePatches() once content roots
// are known. Recursion is bounded.

namespace HL2VMT
{
    struct FBlock;

    struct FBlock
    {
        // Lower-cased key -> last-wins string value. (VMT does allow duplicate
        // keys but in practice only the last is honoured.)
        TMap<FString, FString>          Pairs;
        // Lower-cased key -> child block. Same last-wins rule.
        TMap<FString, TSharedPtr<FBlock>> Children;

        const FString* FindString(const FString& KeyLower) const { return Pairs.Find(KeyLower); }

        bool GetString(const FString& KeyLower, FString& Out) const
        {
            if (const FString* P = Pairs.Find(KeyLower)) { Out = *P; return true; }
            return false;
        }
        bool GetBool(const FString& KeyLower, bool DefaultVal) const
        {
            FString S; if (!GetString(KeyLower, S)) { return DefaultVal; }
            S.TrimStartAndEndInline();
            return !(S == TEXT("0") || S.Equals(TEXT("false"), ESearchCase::IgnoreCase));
        }
        float GetFloat(const FString& KeyLower, float DefaultVal) const
        {
            FString S; if (!GetString(KeyLower, S)) { return DefaultVal; }
            return FCString::Atof(*S);
        }
    };

    struct FDocument
    {
        // Original shader name as written (e.g. "LightmappedGeneric").
        FString             Shader;
        // Lowercased shader for switching.
        FString             ShaderLower;
        // Top-level block (the contents inside `Shader { ... }`).
        TSharedPtr<FBlock>  Root;
    };

    // Parse a UTF-8 / ASCII VMT source string. Returns false on syntax error and
    // fills OutError with a short diagnostic. Tolerates trailing whitespace and
    // missing trailing brace if the body is otherwise well-formed.
    HL2BSPIMPORTER_API bool Parse(const FString& Source, FDocument& OutDoc, FString& OutError);

    // Convenience: load + parse a .vmt file from disk.
    HL2BSPIMPORTER_API bool ParseFile(const FString& AbsPath, FDocument& OutDoc, FString& OutError);

    // Resolve a single level of `Patch { include ... replace/insert ... }`
    // indirection. If Doc.Shader is "Patch" and the document has an `include`
    // pair, load the referenced VMT (relative to one of MaterialsRoots,
    // searched in order), then apply `replace` (overwrite existing keys) and
    // `insert` (only set keys that do not exist) sub-blocks. Updates Doc in
    // place. Bounded recursion (default depth 4) to defend against cycles.
    HL2BSPIMPORTER_API bool ResolvePatches(
        FDocument& Doc,
        const TArray<FString>& MaterialsRoots,
        FString& OutError,
        int32 MaxDepth = 4);
}
