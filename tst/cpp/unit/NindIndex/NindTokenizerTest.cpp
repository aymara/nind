#include "NindIndex/NindTokenizer.h"
#include "doctest.h"
#include <string>
#include <vector>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
vector<string> tokens(const string &text, const NindTokenizer::Options &options = NindTokenizer::Options()) {
    vector<string> result;
    NindTokenizer(options).tokenize(text, result);
    return result;
}
vector<string> list(const char *a = 0, const char *b = 0, const char *c = 0, const char *d = 0) {
    vector<string> result;
    if (a) result.push_back(a);
    if (b) result.push_back(b);
    if (c) result.push_back(c);
    if (d) result.push_back(d);
    return result;
}
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTokenizerTest.DefaultSplitsLikeSourceCode") {
    CHECK(tokens("AuthenticationManager") == list("Authentication", "Manager"));
    CHECK(tokens("snake_case_word") == list("snake", "case", "word"));
    CHECK(tokens("term123") == list("term", "123"));
    CHECK(tokens("XMLHttpRequest") == list("XML", "Http", "Request"));
    CHECK(tokens("FOO_BAR") == list("FOO", "BAR"));
    CHECK(tokens("__init__(self)") == list("init", "self"));
    CHECK(tokens("x86_64") == list("x", "86", "64"));
    CHECK(tokens("A") == list("A"));
    CHECK(tokens("ABCdef") == list("AB", "Cdef"));
    CHECK(tokens("") == list());
    CHECK(tokens("  ,;() ") == list());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTokenizerTest.NonAsciiIsALetterWithoutCase") {
    CHECK(tokens("café crème") == list("café", "crème"));
    CHECK(tokens("caféBar") == list("café", "Bar"));
    CHECK(tokens("été2026") == list("été", "2026"));
    // non-ASCII punctuation and symbols separate words
    CHECK(tokens("it\xE2\x80\x99s \xE2\x80\x98" "class\xE2\x80\x99") == list("it", "s", "class"));
    CHECK(tokens("\xC2\xABx\xC2\xA0y\xC2\xBB a\xC3\x97" "b \xF0\x9F\x98\x80ok") == vector<string>{"x", "y", "a", "b", "ok"});
    CHECK(tokens("\xE5\xAD\x97\xE3\x80\x82\xE5\xAD\x97") == list("\xE5\xAD\x97", "\xE5\xAD\x97"));
    // invalid UTF-8 bytes count as letters, never read beyond the text
    CHECK(tokens("a\xE2\x80") == list("a\xE2\x80"));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTokenizerTest.EachSplitIsOptional") {
    NindTokenizer::Options options;
    options.splitCamelCase = false;
    CHECK(tokens("getHTTPServer2_x", options) == list("getHTTPServer", "2", "x"));
    options = NindTokenizer::Options();
    options.splitUnderscore = false;
    CHECK(tokens("my_varName __x__", options) == list("my_var", "Name", "__x__"));
    options = NindTokenizer::Options();
    options.splitDigits = false;
    CHECK(tokens("utf8Encode x86_64", options) == list("utf8", "Encode", "x86", "64"));
    options.splitCamelCase = false;
    options.splitUnderscore = false;
    CHECK(tokens("utf8Encode x86_64.y", options) == list("utf8Encode", "x86_64", "y"));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTokenizerTest.LowercaseAndLengthLimits") {
    NindTokenizer::Options options;
    options.lowercase = true;
    CHECK(tokens("HTTPServer ÉtéX", options) == list("http", "server", "Été", "x"));
    options = NindTokenizer::Options();
    options.minLength = 2;
    CHECK(tokens("a bc dEf", options) == list("bc", "Ef"));
    options = NindTokenizer::Options();
    CHECK(tokens(string(255, 'a') + " " + string(256, 'b') + " c").size() == 2);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTokenizerTest.SpansPointIntoTheText") {
    const string text = "fooBar = 42";
    vector<pair<size_t, size_t> > spans;
    NindTokenizer().tokenize(text.data(), text.size(), spans);
    REQUIRE_EQ(3u, spans.size());
    CHECK_EQ(0u, spans[0].first);
    CHECK_EQ(3u, spans[0].second);
    CHECK_EQ(3u, spans[1].first);
    CHECK_EQ(6u, spans[1].second);
    CHECK_EQ(9u, spans[2].first);
    CHECK_EQ(11u, spans[2].second);
}
////////////////////////////////////////////////////////////
