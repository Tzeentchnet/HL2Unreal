#include "HL2VmtParser.h"
#include "HL2BSPImporter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

namespace HL2VMT
{
    namespace
    {
        struct FCursor
        {
            const TCHAR* Ptr;
            const TCHAR* End;
            int32        Line = 1;

            bool AtEnd() const { return Ptr >= End; }
            TCHAR Peek() const { return AtEnd() ? TEXT('\0') : *Ptr; }
            void Advance() { if (Ptr < End) { if (*Ptr == TEXT('\n')) { ++Line; } ++Ptr; } }
        };

        // Skip whitespace and `// ...` line comments. Source also allows
        // `/* ... */` in some tools; not observed in HL2 VMTs but handled.
        void SkipTrivia(FCursor& C)
        {
            while (!C.AtEnd())
            {
                const TCHAR Ch = C.Peek();
                if (FChar::IsWhitespace(Ch)) { C.Advance(); continue; }
                if (Ch == TEXT('/') && (C.Ptr + 1) < C.End)
                {
                    const TCHAR Next = C.Ptr[1];
                    if (Next == TEXT('/'))
                    {
                        while (!C.AtEnd() && C.Peek() != TEXT('\n')) { C.Advance(); }
                        continue;
                    }
                    if (Next == TEXT('*'))
                    {
                        C.Advance(); C.Advance();
                        while (!C.AtEnd())
                        {
                            if (C.Peek() == TEXT('*') && (C.Ptr + 1) < C.End && C.Ptr[1] == TEXT('/'))
                            {
                                C.Advance(); C.Advance();
                                break;
                            }
                            C.Advance();
                        }
                        continue;
                    }
                }
                break;
            }
        }

        // Read one token: a quoted string ("..." with no escapes — Source VMTs
        // don't use them) or a bare run of non-whitespace, non-brace, non-quote
        // characters. Returns false at EOF or if the next non-trivia char is `}`.
        bool ReadToken(FCursor& C, FString& Out, bool& bWasQuoted)
        {
            SkipTrivia(C);
            if (C.AtEnd()) { return false; }
            const TCHAR First = C.Peek();
            if (First == TEXT('}')) { return false; }

            bWasQuoted = false;
            if (First == TEXT('"'))
            {
                bWasQuoted = true;
                C.Advance();
                const TCHAR* Start = C.Ptr;
                while (!C.AtEnd() && C.Peek() != TEXT('"'))
                {
                    // Forbid embedded newlines from running away to EOF on
                    // unterminated strings — clamp at next line break.
                    if (C.Peek() == TEXT('\n')) { break; }
                    C.Advance();
                }
                Out = FString(C.Ptr - Start, Start);
                if (!C.AtEnd() && C.Peek() == TEXT('"')) { C.Advance(); }
                return true;
            }

            const TCHAR* Start = C.Ptr;
            while (!C.AtEnd())
            {
                const TCHAR Ch = C.Peek();
                if (FChar::IsWhitespace(Ch) || Ch == TEXT('{') || Ch == TEXT('}') || Ch == TEXT('"')) { break; }
                C.Advance();
            }
            Out = FString(C.Ptr - Start, Start);
            return Out.Len() > 0;
        }

        bool ParseBlockBody(FCursor& C, FBlock& Block, FString& OutError, int32 Depth)
        {
            if (Depth > 32) { OutError = TEXT("VMT nesting too deep"); return false; }
            for (;;)
            {
                SkipTrivia(C);
                if (C.AtEnd()) { return true; }
                if (C.Peek() == TEXT('}')) { C.Advance(); return true; }

                FString Key;
                bool bKeyQuoted = false;
                if (!ReadToken(C, Key, bKeyQuoted))
                {
                    return true; // EOF or stray '}' handled above
                }
                const FString KeyLower = Key.ToLower();

                SkipTrivia(C);
                if (C.AtEnd())
                {
                    OutError = FString::Printf(TEXT("Expected value or '{' after key '%s' at line %d"), *Key, C.Line);
                    return false;
                }

                if (C.Peek() == TEXT('{'))
                {
                    C.Advance();
                    TSharedPtr<FBlock> Child = MakeShared<FBlock>();
                    if (!ParseBlockBody(C, *Child, OutError, Depth + 1)) { return false; }
                    Block.Children.Add(KeyLower, Child);
                    continue;
                }

                FString Value;
                bool bValQuoted = false;
                if (!ReadToken(C, Value, bValQuoted))
                {
                    OutError = FString::Printf(TEXT("Expected value for key '%s' at line %d"), *Key, C.Line);
                    return false;
                }
                Block.Pairs.Add(KeyLower, Value);
            }
        }
    } // namespace

    bool Parse(const FString& Source, FDocument& OutDoc, FString& OutError)
    {
        OutDoc = FDocument{};
        OutError.Reset();

        FCursor C{ *Source, *Source + Source.Len() };

        FString Shader;
        bool bQuoted = false;
        if (!ReadToken(C, Shader, bQuoted))
        {
            OutError = TEXT("VMT is empty");
            return false;
        }
        OutDoc.Shader      = Shader;
        OutDoc.ShaderLower = Shader.ToLower();

        SkipTrivia(C);
        if (C.AtEnd() || C.Peek() != TEXT('{'))
        {
            OutError = FString::Printf(TEXT("Expected '{' after shader '%s'"), *Shader);
            return false;
        }
        C.Advance();

        OutDoc.Root = MakeShared<FBlock>();
        if (!ParseBlockBody(C, *OutDoc.Root, OutError, 0)) { return false; }
        return true;
    }

    bool ParseFile(const FString& AbsPath, FDocument& OutDoc, FString& OutError)
    {
        FString Source;
        if (!FFileHelper::LoadFileToString(Source, *AbsPath))
        {
            OutError = FString::Printf(TEXT("Failed to read VMT '%s'"), *AbsPath);
            return false;
        }
        return Parse(Source, OutDoc, OutError);
    }

    namespace
    {
        // Source `materials/<rel>` lookup: try every root, return first hit.
        FString ResolveMaterialPath(const FString& Rel, const TArray<FString>& Roots)
        {
            FString Norm = Rel.Replace(TEXT("\\"), TEXT("/"));
            if (!Norm.EndsWith(TEXT(".vmt"), ESearchCase::IgnoreCase)) { Norm += TEXT(".vmt"); }
            // `include` paths are usually rooted at game dir, e.g. "materials/foo/bar.vmt".
            // Strip a leading "materials/" if present so we can try both forms.
            FString WithoutPrefix = Norm;
            if (WithoutPrefix.StartsWith(TEXT("materials/"), ESearchCase::IgnoreCase))
            {
                WithoutPrefix.RightChopInline(10, EAllowShrinking::No);
            }

            IFileManager& FM = IFileManager::Get();
            for (const FString& Root : Roots)
            {
                const FString Candidates[] =
                {
                    Root / TEXT("materials") / WithoutPrefix,
                    Root / Norm,
                    Root / WithoutPrefix,
                };
                for (const FString& Cand : Candidates)
                {
                    if (FM.FileExists(*Cand)) { return Cand; }
                }
            }
            return {};
        }

        void MergeReplace(FBlock& Dst, const FBlock& Src)
        {
            for (const auto& KV : Src.Pairs)    { Dst.Pairs.Add(KV.Key, KV.Value); }
            for (const auto& KV : Src.Children) { Dst.Children.Add(KV.Key, KV.Value); }
        }
        void MergeInsert(FBlock& Dst, const FBlock& Src)
        {
            for (const auto& KV : Src.Pairs)    { if (!Dst.Pairs.Contains(KV.Key))    { Dst.Pairs.Add(KV.Key, KV.Value); } }
            for (const auto& KV : Src.Children) { if (!Dst.Children.Contains(KV.Key)) { Dst.Children.Add(KV.Key, KV.Value); } }
        }
    } // namespace

    bool ResolvePatches(FDocument& Doc, const TArray<FString>& MaterialsRoots, FString& OutError, int32 MaxDepth)
    {
        for (int32 Depth = 0; Depth < MaxDepth; ++Depth)
        {
            if (Doc.ShaderLower != TEXT("patch")) { return true; }
            if (!Doc.Root.IsValid())
            {
                OutError = TEXT("Patch document has empty body");
                return false;
            }

            FString Include;
            if (!Doc.Root->GetString(TEXT("include"), Include))
            {
                OutError = TEXT("Patch shader missing 'include'");
                return false;
            }

            const FString IncludePath = ResolveMaterialPath(Include, MaterialsRoots);
            if (IncludePath.IsEmpty())
            {
                OutError = FString::Printf(TEXT("Patch include not found: %s"), *Include);
                return false;
            }

            FDocument Base;
            if (!ParseFile(IncludePath, Base, OutError)) { return false; }

            // Apply `replace` then `insert` sub-blocks of the original Patch onto Base.
            if (const TSharedPtr<FBlock>* Replace = Doc.Root->Children.Find(TEXT("replace")))
            {
                if (Replace->IsValid() && Base.Root.IsValid()) { MergeReplace(*Base.Root, **Replace); }
            }
            if (const TSharedPtr<FBlock>* Insert = Doc.Root->Children.Find(TEXT("insert")))
            {
                if (Insert->IsValid() && Base.Root.IsValid()) { MergeInsert(*Base.Root, **Insert); }
            }

            Doc = MoveTemp(Base);
            // Loop in case Base is itself a Patch.
        }
        OutError = TEXT("Patch chain exceeded max depth");
        return false;
    }
} // namespace HL2VMT
