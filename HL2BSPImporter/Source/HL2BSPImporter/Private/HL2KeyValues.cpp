#include "HL2KeyValues.h"
#include "HL2BSPImporter.h"
#include "Misc/FileHelper.h"

namespace HL2KV
{
    // ---------------- FNode lookups ----------------

    const FNode* FNode::FindChild(FStringView Name) const
    {
        for (const TSharedPtr<FNode>& Child : Children)
        {
            if (Child.IsValid() && Child->Key.Equals(FString(Name), ESearchCase::IgnoreCase))
            {
                return Child.Get();
            }
        }
        return nullptr;
    }

    const FString* FNode::FindString(FStringView Name) const
    {
        for (const TSharedPtr<FNode>& Child : Children)
        {
            if (Child.IsValid() && Child->IsLeaf() && Child->Key.Equals(FString(Name), ESearchCase::IgnoreCase))
            {
                return &Child->Value;
            }
        }
        return nullptr;
    }

    bool FNode::GetBool(FStringView Name, bool DefaultVal) const
    {
        const FString* S = FindString(Name);
        if (!S) { return DefaultVal; }
        FString T = *S; T.TrimStartAndEndInline();
        return !(T == TEXT("0") || T.Equals(TEXT("false"), ESearchCase::IgnoreCase));
    }

    float FNode::GetFloat(FStringView Name, float DefaultVal) const
    {
        const FString* S = FindString(Name);
        return S ? FCString::Atof(**S) : DefaultVal;
    }

    int32 FNode::GetInt(FStringView Name, int32 DefaultVal) const
    {
        const FString* S = FindString(Name);
        return S ? FCString::Atoi(**S) : DefaultVal;
    }

    FString FNode::GetString(FStringView Name, const FString& DefaultVal) const
    {
        const FString* S = FindString(Name);
        return S ? *S : DefaultVal;
    }

    // ---------------- Parser ----------------

    namespace
    {
        struct FCursor
        {
            const TCHAR* Ptr = nullptr;
            const TCHAR* End = nullptr;
            int32        Line = 1;

            bool AtEnd() const { return Ptr >= End; }
            TCHAR Peek() const { return AtEnd() ? TEXT('\0') : *Ptr; }
            TCHAR PeekAt(int32 N) const { return (Ptr + N < End) ? Ptr[N] : TEXT('\0'); }
            void Advance() { if (Ptr < End) { if (*Ptr == TEXT('\n')) { ++Line; } ++Ptr; } }
        };

        FString MakeError(const FCursor& C, const TCHAR* Msg)
        {
            return FString::Printf(TEXT("HL2KV: line %d: %s"), C.Line, Msg);
        }

        // Skip whitespace, '// line comments', '/* block comments */', and
        // optional '[$X360]' / '[!$WIN32]' conditional suffixes that may follow
        // a token. Conditionals are parsed-and-discarded (not evaluated).
        void SkipTrivia(FCursor& C)
        {
            while (!C.AtEnd())
            {
                const TCHAR Ch = C.Peek();
                if (FChar::IsWhitespace(Ch)) { C.Advance(); continue; }
                if (Ch == TEXT('/') && C.PeekAt(1) == TEXT('/'))
                {
                    while (!C.AtEnd() && C.Peek() != TEXT('\n')) { C.Advance(); }
                    continue;
                }
                if (Ch == TEXT('/') && C.PeekAt(1) == TEXT('*'))
                {
                    C.Advance(); C.Advance();
                    while (!C.AtEnd())
                    {
                        if (C.Peek() == TEXT('*') && C.PeekAt(1) == TEXT('/'))
                        {
                            C.Advance(); C.Advance();
                            break;
                        }
                        C.Advance();
                    }
                    continue;
                }
                break;
            }
        }

        // Skip a trailing '[...]' conditional suffix if one immediately follows
        // (no whitespace allowed between previous token and '['). Returns true
        // if a conditional was consumed.
        void SkipInlineConditional(FCursor& C)
        {
            if (C.Peek() != TEXT('[')) { return; }
            C.Advance();
            while (!C.AtEnd() && C.Peek() != TEXT(']'))
            {
                C.Advance();
            }
            if (!C.AtEnd()) { C.Advance(); } // past ']'
        }

        // Read a single token. Returns false at EOF or if the next non-trivia
        // char is '}' (caller treats '}' as end-of-block, not an error).
        // Sets bWasQuoted true for quoted tokens.
        bool ReadToken(FCursor& C, FString& Out, bool& bWasQuoted)
        {
            SkipTrivia(C);
            if (C.AtEnd()) { return false; }
            const TCHAR First = C.Peek();
            if (First == TEXT('}')) { return false; }

            if (First == TEXT('"'))
            {
                C.Advance();
                Out.Reset();
                while (!C.AtEnd())
                {
                    const TCHAR Ch = C.Peek();
                    if (Ch == TEXT('"')) { C.Advance(); bWasQuoted = true; return true; }
                    if (Ch == TEXT('\\') && (C.Ptr + 1) < C.End)
                    {
                        const TCHAR Esc = C.Ptr[1];
                        switch (Esc)
                        {
                            case TEXT('n'):  Out.AppendChar(TEXT('\n')); C.Advance(); C.Advance(); continue;
                            case TEXT('t'):  Out.AppendChar(TEXT('\t')); C.Advance(); C.Advance(); continue;
                            case TEXT('r'):  Out.AppendChar(TEXT('\r')); C.Advance(); C.Advance(); continue;
                            case TEXT('"'):  Out.AppendChar(TEXT('"'));  C.Advance(); C.Advance(); continue;
                            case TEXT('\\'): Out.AppendChar(TEXT('\\')); C.Advance(); C.Advance(); continue;
                            default: break; // fall-through: emit '\' literal
                        }
                    }
                    Out.AppendChar(Ch);
                    C.Advance();
                }
                bWasQuoted = true;
                return true; // unterminated string at EOF — accept
            }

            // Bare token: run of non-whitespace, non-brace, non-quote, non-bracket chars.
            Out.Reset();
            while (!C.AtEnd())
            {
                const TCHAR Ch = C.Peek();
                if (FChar::IsWhitespace(Ch)) { break; }
                if (Ch == TEXT('{') || Ch == TEXT('}') || Ch == TEXT('"') || Ch == TEXT('[')) { break; }
                if (Ch == TEXT('/') && (C.PeekAt(1) == TEXT('/') || C.PeekAt(1) == TEXT('*'))) { break; }
                Out.AppendChar(Ch);
                C.Advance();
            }
            bWasQuoted = false;
            return Out.Len() > 0;
        }

        bool ParseBlockBody(FCursor& C, FNode& OutBlock, FString& OutError, int32 Depth)
        {
            if (Depth > 64)
            {
                OutError = MakeError(C, TEXT("nesting too deep (>64)"));
                return false;
            }

            for (;;)
            {
                SkipTrivia(C);
                if (C.AtEnd()) { return true; }
                if (C.Peek() == TEXT('}'))
                {
                    return true; // caller consumes the '}'
                }

                FString Key;
                bool    bKeyQuoted = false;
                if (!ReadToken(C, Key, bKeyQuoted))
                {
                    if (C.AtEnd()) { return true; }
                    OutError = MakeError(C, TEXT("expected key or '}'"));
                    return false;
                }

                // Recognise but skip '#include "..."' / '#base "..."' directives.
                // Resolution is a follow-up; for now log a verbose warning and
                // discard the value token.
                if (Key.StartsWith(TEXT("#")))
                {
                    FString IncValue;
                    bool bIncQuoted = false;
                    if (ReadToken(C, IncValue, bIncQuoted))
                    {
                        UE_LOG(LogHL2BSPImporter, Verbose,
                            TEXT("HL2KV: ignoring '%s \"%s\"' directive (resolution not implemented)."),
                            *Key, *IncValue);
                    }
                    SkipInlineConditional(C);
                    continue;
                }

                // Allow inline conditional after the key (e.g. `key [!$X360]`).
                SkipTrivia(C);
                SkipInlineConditional(C);
                SkipTrivia(C);

                // Block?
                if (C.Peek() == TEXT('{'))
                {
                    C.Advance();
                    TSharedPtr<FNode> Child = MakeShared<FNode>();
                    Child->Key = Key.ToLower();
                    if (!ParseBlockBody(C, *Child, OutError, Depth + 1)) { return false; }
                    SkipTrivia(C);
                    if (C.AtEnd() || C.Peek() != TEXT('}'))
                    {
                        OutError = MakeError(C, TEXT("expected '}' to close block"));
                        return false;
                    }
                    C.Advance(); // past '}'
                    SkipInlineConditional(C);
                    OutBlock.Children.Add(MoveTemp(Child));
                    continue;
                }

                // Leaf (key value). Read the value token.
                FString Value;
                bool    bValQuoted = false;
                if (!ReadToken(C, Value, bValQuoted))
                {
                    OutError = MakeError(C, TEXT("expected value or '{' after key"));
                    return false;
                }
                SkipInlineConditional(C);

                TSharedPtr<FNode> Child = MakeShared<FNode>();
                Child->Key   = Key.ToLower();
                Child->Value = MoveTemp(Value);
                OutBlock.Children.Add(MoveTemp(Child));
            }
        }
    } // namespace

    bool Parse(FStringView Source, FNode& OutRoot, FString& OutError)
    {
        OutError.Reset();
        OutRoot = FNode{};

        FCursor C;
        C.Ptr  = Source.GetData();
        C.End  = Source.GetData() + Source.Len();
        C.Line = 1;

        if (!ParseBlockBody(C, OutRoot, OutError, /*Depth=*/0))
        {
            return false;
        }
        // Allow trailing whitespace, but reject stray '}'.
        SkipTrivia(C);
        if (!C.AtEnd() && C.Peek() == TEXT('}'))
        {
            OutError = MakeError(C, TEXT("unexpected '}' at top level"));
            return false;
        }
        return true;
    }

    bool ParseBytes(TConstArrayView<uint8> Bytes, FNode& OutRoot, FString& OutError)
    {
        if (Bytes.Num() == 0)
        {
            OutRoot = FNode{};
            return true;
        }

        // Decode UTF-8 with ANSI fallback. UE's FString constructor from
        // UTF8CHAR* handles BOM-less UTF-8 transparently; if the data is plain
        // ANSI it round-trips through FString unchanged (HL2 KV1 files are
        // ASCII-clean in practice).
        FString Source;
        // Skip a leading UTF-8 BOM (EF BB BF) if present.
        int32 Start = 0;
        if (Bytes.Num() >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF)
        {
            Start = 3;
        }
        const ANSICHAR* RawAnsi = reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Start);
        const int32     Len     = Bytes.Num() - Start;
        // FString(int32 Len, const ANSICHAR*) treats input as ANSI; for our
        // ASCII-clean inputs this is correct. For real UTF-8 files (rare for
        // HL2 KV1) extended chars would round-trip as Latin-1; that's a known
        // limitation parallel to HL2VmtParser which uses the same idiom.
        Source = FString(Len, RawAnsi);
        return Parse(Source, OutRoot, OutError);
    }

    bool ParseFile(const FString& AbsPath, FNode& OutRoot, FString& OutError)
    {
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *AbsPath))
        {
            OutError = FString::Printf(TEXT("HL2KV: failed to read file '%s'"), *AbsPath);
            return false;
        }
        return ParseBytes(MakeArrayView(Bytes), OutRoot, OutError);
    }
} // namespace HL2KV
