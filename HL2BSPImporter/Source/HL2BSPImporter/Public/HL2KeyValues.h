#pragma once
#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

// General-purpose Valve KeyValues 1 (KV1) parser. Used by surfaceproperties.txt,
// soundscapes / soundscripts, .phy text sections, and any other Source text
// data that follows the KV1 grammar. See HL2VmtParser.h for the VMT-specific
// fast path that this parser does NOT replace.
//
// Grammar (pragmatically):
//   DOCUMENT := (PAIR | BLOCK)*
//   PAIR     := KEY VALUE [CONDITIONAL]
//   BLOCK    := KEY [CONDITIONAL] '{' (PAIR | BLOCK)* '}'
//   KEY/VALUE  := QUOTED_STRING | BARE_TOKEN
//   QUOTED_STRING := '"' (\\" | \\\\ | \\n | \\t | [^"])* '"'
//   BARE_TOKEN    := [^\s{}"]+
//   CONDITIONAL   := '[' (!? '$'? IDENT) ']'    (parsed and discarded; conditional
//                                                 evaluation is left to the caller).
// Comments: '//' to end of line, and '/* ... */' block comments anywhere.
// Keys are case-insensitive (lower-cased on insert); values keep their case.
//
// Output: a tree of FNode. Each FNode is either a leaf (Children.Num() == 0,
// Value populated) or a block (Value empty, Children populated). The root node
// returned by Parse() is a synthetic "" block whose children are the document's
// top-level entries.
//
// `#include "..."` directives are recognised but unresolved in v1 — they emit a
// debug warning and are skipped. Resolution lands when downstream consumers
// (soundscape importers) need it.

namespace HL2KV
{
    struct HL2BSPIMPORTER_API FNode
    {
        // Lower-cased key (KV1 is case-insensitive on lookup).
        FString Key;

        // Populated for leaf nodes; empty for block nodes. Whitespace preserved
        // exactly as authored (callers Trim / Atof / Atoi as needed).
        FString Value;

        // Block children. Order is preserved (KV1 is order-significant for
        // some game data, e.g. soundscape playlist order).
        TArray<TSharedPtr<FNode>> Children;

        bool IsLeaf() const { return Children.Num() == 0; }

        // Find the *first* child with the given key (case-insensitive). Returns
        // null if no match. KV1 allows duplicate keys; callers wanting the
        // last-wins semantics (matching VMT) should iterate Children manually.
        const FNode* FindChild(FStringView Name) const;

        // Find the *first* leaf child's value, returning a pointer to the
        // stored FString. Null if missing or if the matching child is a block.
        const FString* FindString(FStringView Name) const;

        // Convenience getters with defaults. All do case-insensitive key lookup.
        bool   GetBool (FStringView Name, bool   DefaultVal = false) const;
        float  GetFloat(FStringView Name, float  DefaultVal = 0.0f)  const;
        int32  GetInt  (FStringView Name, int32  DefaultVal = 0)     const;
        FString GetString(FStringView Name, const FString& DefaultVal = FString()) const;
    };

    // Parse a UTF-16 / TCHAR source string. Returns false on syntax error and
    // fills OutError with a short diagnostic (line number prefixed).
    HL2BSPIMPORTER_API bool Parse(FStringView Source, FNode& OutRoot, FString& OutError);

    // Parse a file from disk (UTF-8 / ANSI auto-detected).
    HL2BSPIMPORTER_API bool ParseFile(const FString& AbsPath, FNode& OutRoot, FString& OutError);

    // Parse raw bytes (UTF-8 / ANSI). Convenience for in-memory data
    // (e.g. .phy text sections, manifest blobs).
    HL2BSPIMPORTER_API bool ParseBytes(TConstArrayView<uint8> Bytes, FNode& OutRoot, FString& OutError);
}
