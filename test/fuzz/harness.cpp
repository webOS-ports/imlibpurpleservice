// Host-side ASan/UBSan harness for the transport's leaf text helpers.
//
// WHY: every function exercised here is fed *remote, attacker-influenced* text -- the body of
// an incoming IM -- and each one walks a heap buffer with raw pointer arithmetic, memcpy and
// hand-rolled size accounting. That is the one bug class static analysis reliably misses and
// the one that has actually taken the transport down in the field. None of these functions
// touch libpurple, db8 or LS2, so they can run natively under ASan on the build host without
// any of the device stack. See test/fuzz/run.sh.
//
// Two modes, same corpus:
//   default      -- run the fixed corpus below, assert nothing crashes, print a summary
//   -DFUZZ_ENTRY -- expose LLVMFuzzerTestOneInput so clang's libFuzzer can drive it

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "sanitize.h"
#include "entities.h"

// Mirrors IMMessage::trustedTags -- the exact allow-list production passes to sanitizeHtml.
static const char* kTrustedTags[] = {
    "b", "/b", "i", "/i", "br", "/br", "u", "/u",
    "B", "/B", "I", "/I", "BR", "/BR", "U", "/U", NULL };

// Run one input through every leaf helper. Return values are freed; the point is the walk,
// not the result. Anything that reads or writes out of bounds trips ASan here.
static void exercise(const char* in)
{
    if (in == NULL) return;

    // sanitizeHtml -- both modes. remove=true is what IMMessage uses; remove=false takes the
    // escaping path that rewrites '<' to "&lt;", i.e. the one that grows the buffer.
    if (char* s = sanitizeHtml(in, (char**)kTrustedTags, true))  free(s);
    if (char* s = sanitizeHtml(in, (char**)kTrustedTags, false)) free(s);

    // unsanitizeHtml takes char* and may work in place -- always hand it a private copy.
    {
        char* copy = strdup(in);
        if (copy) { if (char* u = unsanitizeHtml(copy)) { if (u != copy) free(u); } free(copy); }
    }

    if (char* e = encodeAstralEntities(in)) free(e);

    // decode_html_entities_utf8 writes into a caller buffer documented as needing
    // strlen(src)+1 bytes. Allocate exactly that, so any overrun is a heap overflow ASan sees
    // rather than slack absorbed by a generous buffer.
    {
        size_t n = strlen(in);
        char* dest = (char*)malloc(n + 1);
        if (dest) { decode_html_entities_utf8(dest, in); free(dest); }
    }
}

#ifdef FUZZ_ENTRY
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    std::string s((const char*)data, size);
    s.push_back('\0');          // these are all C-string APIs
    exercise(s.c_str());
    return 0;
}
#else

// Adversarial corpus. Grouped by the property of the walk each entry is aimed at.
static const char* kCorpus[] = {
    // --- ordinary traffic, the baseline
    "hello world",
    "<b>bold</b> and <i>italic</i>",
    "plain text with an & ampersand",

    // --- unterminated tags: '<' with no '>' anywhere. The tag-name scan advances past
    //     tag_start+1 testing only for space, '/' and '>' -- never for the NUL.
    "<",
    "a<",
    "<a",
    "<abc",
    "text then <unterminated",
    "<<<<",
    "a<b<c<d",

    // --- '>' with no '<', and the reverse ordering
    ">",
    "a>b",
    "></b>",

    // --- growth path: every angle bracket becomes 4 bytes when escaping (remove=false).
    //     result_size = message_size + count*3 is the accounting under test.
    "<x><x><x><x><x><x><x><x>",
    "<><><><><><><><><><><><><><><><>",
    "<notallowed attr='v'>text</notallowed>",

    // --- tag names that run to the very end of the buffer
    "<b",
    "</b",
    "<br/",
    "<b ",

    // --- entities, including malformed and oversized numeric forms
    "&lt;&gt;&amp;&quot;&apos;",
    "&#65;&#x41;&#0;&#;&#x;",
    "&#99999999999999999999;",
    "&#x110000;",                       // beyond the Unicode range
    "&notanentity;",
    "&",
    "&#",
    "&#x",
    "&amp",                             // no terminating semicolon

    // --- UTF-8 edge cases: astral pairs, truncated sequences, lone continuation bytes
    "emoji \xF0\x9F\x98\x80 here",      // U+1F600, the astral path
    "\xF0\x9F",                         // truncated 4-byte lead
    "\xE2\x9C",                         // truncated 3-byte lead
    "\x80\x80\x80",                     // lone continuation bytes
    "\xFF\xFE",                         // invalid lead bytes
    "mixed \xF0\x9F\x98\x80 <b>tag</b> &amp; entity",

    // --- tidy-bypass candidates. sanitizeHtml does NOT walk the raw input: it walks
    //     libtidy's output, and tidy balances tags and escapes stray '<' to "&lt;". That
    //     invariant, not any local bound check, is what keeps the tag-name scan in bounds
    //     (it advances testing only for space, '/' and '>' -- never for the NUL). These are
    //     the constructs where a parser may pass raw text through verbatim, so they are the
    //     way an unbalanced '<' could still reach the walk.
    "<script>a<b</script>",
    "<style>a<b</style>",
    "<textarea><b</textarea>",
    "<xmp><a</xmp>",
    "<plaintext><a",
    "<!-- <foo -->",
    "<![CDATA[ <foo ]]>",
    "<pre><a</pre>",
    "<title><a</title>",

    // --- empty and whitespace-only
    "",
    " ",
    "\t\n\r",
};

int main()
{
    const size_t n = sizeof(kCorpus) / sizeof(kCorpus[0]);
    for (size_t i = 0; i < n; ++i) {
        printf("[%2zu/%2zu] %-42.42s", i + 1, n, kCorpus[i][0] ? kCorpus[i] : "(empty)");
        fflush(stdout);                 // flush before the call: if ASan aborts, the last
                                        // line printed names the input that did it
        exercise(kCorpus[i]);
        printf("  ok\n");
    }

    // Long runs, sized to cross allocator bucket boundaries where a small overrun would
    // otherwise land in slack rather than tripping a redzone.
    for (size_t len = 1; len <= 4096; len *= 2) {
        std::string s;
        s.reserve(len * 2);
        for (size_t i = 0; i < len; ++i) s += "<b>";
        printf("[long ] %zu x \"<b>\"", len); fflush(stdout);
        exercise(s.c_str());
        std::string u(len, '<');        // unterminated run of the same length
        exercise(u.c_str());
        printf("  ok\n");
    }

    printf("\nall inputs survived\n");
    return 0;
}
#endif
