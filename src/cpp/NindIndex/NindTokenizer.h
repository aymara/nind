//
// C++ Interface: NindTokenizer
//
// Description: Tokenizer for source-code-like text: words are runs of ASCII
// letters, digits, non-ASCII (UTF-8) characters and, optionally, underscores;
// each word may further be split at camelCase, underscore and letter/digit
// boundaries. Each kind of split is optional.
//
// Copyright: 2026 LATEJCON. See LICENCE.md file that comes with this distribution
// This file is part of NIND (as "nouvelle indexation").
// NIND is free software: you can redistribute it and/or modify it under the terms of the
// GNU Less General Public License (LGPL) as published by the Free Software Foundation,
// (see <http://www.gnu.org/licenses/>), either version 3 of the License, or any later version.
// NIND is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Less General Public License for more details.
////////////////////////////////////////////////////////////
#ifndef NindTokenizer_H
#define NindTokenizer_H
////////////////////////////////////////////////////////////
#include "NindCommonExport.h"
#include <cstddef>
#include <string>
#include <utility>
#include <vector>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
/**\brief Splits source-code-like text into tokens.
* A word is a maximal run of ASCII letters and digits, non-ASCII (UTF-8) characters
* other than punctuation and symbols, and underscores when they don't split. Everything
* else separates words: ASCII punctuation, spaces and controls, and non-ASCII punctuation
* and symbols (Latin-1 punctuation, general punctuation, arrows, maths, CJK and full-width
* punctuation, emoji...). Inside a word:
* - splitCamelCase: "fooBar" -> "foo" "Bar", "HTTPServer" -> "HTTP" "Server";
* - splitUnderscore: "foo_bar" -> "foo" "bar" (underscores are dropped);
* - splitDigits: "utf8" -> "utf" "8".
* Non-ASCII characters count as letters without case (no camelCase split between
* them), and are never split inside. lowercase lowercases ASCII letters only, after
* splitting. Tokens shorter than minLength or longer than maxLength bytes are dropped
* (a lexicon word is at most 254 bytes). Thread-safe (const). */
class DLLExportLexicon NindTokenizer {
public:
    /**\brief Tokenizer options */
    struct Options {
        bool splitCamelCase;
        bool splitUnderscore;
        bool splitDigits;
        bool lowercase;
        unsigned int minLength;     //in bytes
        unsigned int maxLength;     //in bytes
        Options(): splitCamelCase(true), splitUnderscore(true), splitDigits(true), lowercase(false),
            minLength(1), maxLength(254) {}
    };

    /**\brief Creates a tokenizer
    *\param options its options */
    explicit NindTokenizer(const Options &options = Options());

    /**\brief Tokenize text into byte spans
    *\param text text (UTF-8)
    *\param length length of text in bytes
    *\param spans receives (begin, end) byte offsets of every token, in text order
    * (not lowercased: see appendToken) */
    void tokenize(const char *text,
                  const size_t length,
                  std::vector<std::pair<size_t, size_t> > &spans) const;

    /**\brief Tokenize text into strings
    *\param text text (UTF-8)
    *\param tokens receives the tokens, in text order (lowercased if asked) */
    void tokenize(const std::string &text,
                  std::vector<std::string> &tokens) const;

    /**\brief Set a token from a span of text, lowercased if asked
    *\param text text the span is in
    *\param span (begin, end) byte offsets
    *\param token receives the token */
    void getToken(const char *text,
                  const std::pair<size_t, size_t> &span,
                  std::string &token) const;

    /**\brief get the options
    *\return the options */
    const Options &getOptions() const { return m_options; }

private:
    //vrai s'il faut couper entre les octets j-1 et j du mot [begin, end)
    bool isBoundary(const char *text,
                    const size_t j,
                    const size_t end) const;

    Options m_options;
};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
////////////////////////////////////////////////////////////
