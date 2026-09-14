#include "NindLexicon/NindLexiconFile.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
// Reads every word record left by the writer, in order, until the
// identification record is reached (readNextRecordAsWordDefinition returns
// false right there without consuming it).
struct WordRecord {
    unsigned int ident;
    bool isSimpleWord;
    string simpleWord;
    pair<unsigned int, unsigned int> compoundWord;
};
vector<WordRecord> readAllWords(NindLexiconFile &file) {
    vector<WordRecord> words;
    while (true) {
        WordRecord record;
        record.compoundWord = pair<unsigned int, unsigned int>(0, 0);
        const bool isWord = file.readNextRecordAsWordDefinition(
            record.ident, record.isSimpleWord, record.simpleWord, record.compoundWord);
        if (!isWord) break;
        words.push_back(record);
    }
    return words;
}
}
////////////////////////////////////////////////////////////
TEST(NindLexiconFileTest, FreshWriterStartsWithJustAnIdentification) {
    TestTempDir tmp;
    NindLexiconFile file(tmp.file("fresh.nindlexicon"), true);
    unsigned int maxIdent, identification;
    EXPECT_TRUE(file.readNextRecordAsLexiconIdentification(maxIdent, identification));
    EXPECT_EQ(0u, maxIdent);
}
////////////////////////////////////////////////////////////
TEST(NindLexiconFileTest, WriteThenReadSimpleAndCompoundWords) {
    TestTempDir tmp;
    const string path = tmp.file("words.nindlexicon");
    {
        NindLexiconFile writer(path, true);
        writer.writeSimpleWordDefinition(1, "alpha", 1, 1000);
        writer.writeSimpleWordDefinition(2, "beta", 2, 1001);
        writer.writeCompoundWordDefinition(3, pair<unsigned int, unsigned int>(1, 2), 3, 1002);
    }
    NindLexiconFile reader(path, false);
    const vector<WordRecord> words = readAllWords(reader);
    ASSERT_EQ(3u, words.size());

    EXPECT_EQ(1u, words[0].ident);
    EXPECT_TRUE(words[0].isSimpleWord);
    EXPECT_EQ("alpha", words[0].simpleWord);

    EXPECT_EQ(2u, words[1].ident);
    EXPECT_TRUE(words[1].isSimpleWord);
    EXPECT_EQ("beta", words[1].simpleWord);

    EXPECT_EQ(3u, words[2].ident);
    EXPECT_FALSE(words[2].isSimpleWord);
    EXPECT_EQ(1u, words[2].compoundWord.first);
    EXPECT_EQ(2u, words[2].compoundWord.second);

    unsigned int maxIdent, identification;
    EXPECT_TRUE(reader.readNextRecordAsLexiconIdentification(maxIdent, identification));
    EXPECT_EQ(3u, maxIdent);
    EXPECT_EQ(1002u, identification);
}
////////////////////////////////////////////////////////////
TEST(NindLexiconFileTest, ReopenedWriterAppendsAfterExistingWords) {
    TestTempDir tmp;
    const string path = tmp.file("append.nindlexicon");
    {
        NindLexiconFile writer(path, true);
        writer.writeSimpleWordDefinition(1, "alpha", 1, 1000);
    }
    {
        // A writer reopening an existing file must skip past the words
        // already there before it can append new ones.
        NindLexiconFile writer(path, true);
        unsigned int ident;
        bool isSimpleWord;
        string simpleWord;
        pair<unsigned int, unsigned int> compoundWord;
        while (writer.readNextRecordAsWordDefinition(ident, isSimpleWord, simpleWord, compoundWord)) {}
        writer.writeSimpleWordDefinition(2, "beta", 2, 1001);
    }
    NindLexiconFile reader(path, false);
    const vector<WordRecord> words = readAllWords(reader);
    ASSERT_EQ(2u, words.size());
    EXPECT_EQ("alpha", words[0].simpleWord);
    EXPECT_EQ("beta", words[1].simpleWord);
}
////////////////////////////////////////////////////////////
TEST(NindLexiconFileTest, WritingOnAReaderThrows) {
    TestTempDir tmp;
    const string path = tmp.file("readonly.nindlexicon");
    { NindLexiconFile writer(path, true); }
    NindLexiconFile reader(path, false);
    EXPECT_THROW(reader.writeSimpleWordDefinition(1, "alpha", 1, 1), BadUseException);
}
////////////////////////////////////////////////////////////
TEST(NindLexiconFileTest, OpeningMissingFileAsReaderThrows) {
    TestTempDir tmp;
    EXPECT_THROW(NindLexiconFile(tmp.file("missing.nindlexicon"), false), NindLexiconException);
}
