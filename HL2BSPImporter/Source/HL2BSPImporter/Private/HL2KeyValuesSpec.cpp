// Automation specs for HL2KV (general KV1 parser). Run from Window → Developer
// Tools → Session Frontend → Automation, filter on 'HL2.KeyValues'.
#include "HL2KeyValues.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace HL2KVSpec
{
    // Lookup helper used by every test. Fails the test if the named child is
    // missing or not a leaf.
    static bool ExpectLeaf(FAutomationTestBase& T, const HL2KV::FNode& Block, const TCHAR* Name, const TCHAR* Expected)
    {
        const FString* Got = Block.FindString(Name);
        if (!T.TestNotNull(FString::Printf(TEXT("leaf '%s' present"), Name), (const void*)Got))
        {
            return false;
        }
        return T.TestEqual(FString::Printf(TEXT("leaf '%s' value"), Name), *Got, FString(Expected));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHL2KVEmptyDoc, "HL2.KeyValues.EmptyDocument",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHL2KVEmptyDoc::RunTest(const FString&)
{
    HL2KV::FNode Root; FString Err;
    TestTrue(TEXT("parse"), HL2KV::Parse(TEXT(""), Root, Err));
    TestEqual(TEXT("error empty"), Err, FString());
    TestEqual(TEXT("no children"), Root.Children.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHL2KVSingleKV, "HL2.KeyValues.SingleKV",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHL2KVSingleKV::RunTest(const FString&)
{
    HL2KV::FNode Root; FString Err;
    TestTrue(TEXT("parse"), HL2KV::Parse(TEXT("\"foo\" \"bar\""), Root, Err));
    TestEqual(TEXT("count"), Root.Children.Num(), 1);
    HL2KVSpec::ExpectLeaf(*this, Root, TEXT("foo"), TEXT("bar"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHL2KVNested, "HL2.KeyValues.NestedBlock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHL2KVNested::RunTest(const FString&)
{
    HL2KV::FNode Root; FString Err;
    const TCHAR* Src = TEXT(
        "concrete {\n"
        "  friction 0.8\n"
        "  elasticity 0.25\n"
        "}");
    TestTrue(TEXT("parse"), HL2KV::Parse(Src, Root, Err));
    TestEqual(TEXT("top count"), Root.Children.Num(), 1);
    const HL2KV::FNode* Cn = Root.FindChild(TEXT("concrete"));
    TestNotNull(TEXT("concrete present"), Cn);
    if (Cn)
    {
        TestFalse(TEXT("concrete is block"), Cn->IsLeaf());
        TestEqual(TEXT("concrete float friction"), Cn->GetFloat(TEXT("friction")), 0.8f);
        TestEqual(TEXT("concrete float elasticity"), Cn->GetFloat(TEXT("elasticity")), 0.25f);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHL2KVDuplicateKeys, "HL2.KeyValues.DuplicateKeys",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHL2KVDuplicateKeys::RunTest(const FString&)
{
    HL2KV::FNode Root; FString Err;
    // Document with two leaves of the same key. KV1 preserves both; FindString
    // returns the FIRST match (vs VMT's last-wins map semantics).
    const TCHAR* Src = TEXT("\"k\" \"v1\" \"k\" \"v2\"");
    TestTrue(TEXT("parse"), HL2KV::Parse(Src, Root, Err));
    TestEqual(TEXT("count"), Root.Children.Num(), 2);
    const FString* First = Root.FindString(TEXT("k"));
    TestNotNull(TEXT("first present"), First);
    if (First) { TestEqual(TEXT("first wins"), *First, FString(TEXT("v1"))); }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHL2KVEscapes, "HL2.KeyValues.Escapes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHL2KVEscapes::RunTest(const FString&)
{
    HL2KV::FNode Root; FString Err;
    TestTrue(TEXT("parse"), HL2KV::Parse(TEXT("\"a\" \"line1\\nline2\""), Root, Err));
    const FString* V = Root.FindString(TEXT("a"));
    TestNotNull(TEXT("a present"), V);
    if (V)
    {
        TestEqual(TEXT("escape decoded"), *V, FString(TEXT("line1\nline2")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHL2KVTabsAndSpaces, "HL2.KeyValues.MixedWhitespace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHL2KVTabsAndSpaces::RunTest(const FString&)
{
    HL2KV::FNode Root; FString Err;
    const TCHAR* Src = TEXT("Block\n{\n\tkey1\tvalue1\n\tkey2 \"value 2\"\n}\n");
    TestTrue(TEXT("parse"), HL2KV::Parse(Src, Root, Err));
    const HL2KV::FNode* Bl = Root.FindChild(TEXT("block"));
    TestNotNull(TEXT("block present"), Bl);
    if (Bl)
    {
        HL2KVSpec::ExpectLeaf(*this, *Bl, TEXT("key1"), TEXT("value1"));
        HL2KVSpec::ExpectLeaf(*this, *Bl, TEXT("key2"), TEXT("value 2"));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHL2KVComments, "HL2.KeyValues.Comments",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHL2KVComments::RunTest(const FString&)
{
    HL2KV::FNode Root; FString Err;
    const TCHAR* Src = TEXT(
        "// leading line comment\n"
        "outer { /* mid block */\n"
        "  k v // trailing comment\n"
        "}\n"
        "/* trailing block comment */");
    TestTrue(TEXT("parse"), HL2KV::Parse(Src, Root, Err));
    const HL2KV::FNode* O = Root.FindChild(TEXT("outer"));
    TestNotNull(TEXT("outer present"), O);
    if (O) { HL2KVSpec::ExpectLeaf(*this, *O, TEXT("k"), TEXT("v")); }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHL2KVUnmatchedBrace, "HL2.KeyValues.UnmatchedBraceRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHL2KVUnmatchedBrace::RunTest(const FString&)
{
    HL2KV::FNode Root; FString Err;
    // Stray '}' at top level must fail.
    AddExpectedError(TEXT("HL2KV"), EAutomationExpectedErrorFlags::Contains, 0);
    TestFalse(TEXT("parse rejects stray '}'"), HL2KV::Parse(TEXT("a b }"), Root, Err));
    TestFalse(TEXT("error message present"), Err.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHL2KVConditional, "HL2.KeyValues.ConditionalSuffix",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHL2KVConditional::RunTest(const FString&)
{
    HL2KV::FNode Root; FString Err;
    // Conditional suffix is parsed-and-discarded; the leaf still appears in the tree.
    TestTrue(TEXT("parse"), HL2KV::Parse(TEXT("\"k\" \"v\" [!$X360]"), Root, Err));
    HL2KVSpec::ExpectLeaf(*this, Root, TEXT("k"), TEXT("v"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHL2KVBareTokens, "HL2.KeyValues.BareTokens",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHL2KVBareTokens::RunTest(const FString&)
{
    HL2KV::FNode Root; FString Err;
    // Source files routinely write bare tokens for keys+values.
    TestTrue(TEXT("parse"), HL2KV::Parse(TEXT("base default friction 0.8 elasticity 0.25"), Root, Err));
    TestEqual(TEXT("count"), Root.Children.Num(), 3);
    HL2KVSpec::ExpectLeaf(*this, Root, TEXT("base"),       TEXT("default"));
    HL2KVSpec::ExpectLeaf(*this, Root, TEXT("friction"),   TEXT("0.8"));
    HL2KVSpec::ExpectLeaf(*this, Root, TEXT("elasticity"), TEXT("0.25"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHL2KVIncludeIgnored, "HL2.KeyValues.IncludeDirectiveIgnored",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHL2KVIncludeIgnored::RunTest(const FString&)
{
    HL2KV::FNode Root; FString Err;
    // #base / #include directives are recognised and skipped (verbose-logged).
    const TCHAR* Src = TEXT("#base \"other.txt\"\nfoo bar\n");
    TestTrue(TEXT("parse"), HL2KV::Parse(Src, Root, Err));
    TestEqual(TEXT("only foo present"), Root.Children.Num(), 1);
    HL2KVSpec::ExpectLeaf(*this, Root, TEXT("foo"), TEXT("bar"));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
