//
// C++ Implementation: NindTokenizer
//
// Description: Tokenizer for source-code-like text (see NindTokenizer.h).
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
#include "NindTokenizer.h"
#include <cstdint>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
//classes d'octets
enum ByteClass {
    SEPARATOR,      //ponctuation, espaces, controsles ASCII
    UPPER,          //A-Z
    LOWER,          //a-z
    DIGIT,          //0-9
    UNDERSCORE,     //_
    OTHER           //octet d'un caractehre non ASCII (lettre sans casse)
};

struct ByteClasses {
    unsigned char classes[256];
    ByteClasses() {
        for (unsigned int c = 0; c < 256; c++) {
            if (c >= 'A' && c <= 'Z') classes[c] = UPPER;
            else if (c >= 'a' && c <= 'z') classes[c] = LOWER;
            else if (c >= '0' && c <= '9') classes[c] = DIGIT;
            else if (c == '_') classes[c] = UNDERSCORE;
            else if (c >= 0x80) classes[c] = OTHER;
            else classes[c] = SEPARATOR;
        }
    }
};
const ByteClasses BYTE_CLASSES;

inline ByteClass byteClass(const char c)
{
    return (ByteClass)BYTE_CLASSES.classes[(unsigned char)c];
}

//vrai pour les caractehres non ASCII de ponctuation ou de symboles (sejparateurs)
bool isSeparatorCodePoint(const uint32_t c)
{
    return (c >= 0x80 && c <= 0xBF)             //ponctuation et symboles Latin-1 (espace insejcable, guillemets...)
        || c == 0xD7 || c == 0xF7               //multiplication, division
        || (c >= 0x2000 && c <= 0x2BFF)         //ponctuation gejnejrale, flèches, maths, symboles divers...
        || (c >= 0x3000 && c <= 0x303F)         //ponctuation CJC
        || (c >= 0xE000 && c <= 0xF8FF)         //usage privej
        || (c >= 0xFE10 && c <= 0xFE6F)         //formes verticales, petites variantes
        || (c >= 0xFF00 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20)
        || (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65)    //ponctuation pleine chasse
        || c == 0xFEFF                          //BOM
        || (c >= 0x1F000 && c <= 0x1FAFF);      //emoji, symboles
}

//classe et longueur en octets du caractehre en i
inline size_t charClass(const char *text,
                        const size_t i,
                        const size_t length,
                        ByteClass &cls)
{
    const unsigned char c = (unsigned char)text[i];
    cls = byteClass(c);
    if (c < 0x80) return 1;
    //UTF-8 : longueur selon l'octet de teste (un octet invalide compte comme une lettre)
    size_t charLength;
    uint32_t codePoint;
    if ((c & 0xE0) == 0xC0) { charLength = 2; codePoint = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { charLength = 3; codePoint = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { charLength = 4; codePoint = c & 0x07; }
    else return 1;
    if (i + charLength > length) return 1;
    for (size_t k = 1; k < charLength; k++) {
        const unsigned char cont = (unsigned char)text[i + k];
        if ((cont & 0xC0) != 0x80) return 1;
        codePoint = (codePoint << 6) | (cont & 0x3F);
    }
    if (isSeparatorCodePoint(codePoint)) cls = SEPARATOR;
    return charLength;
}
}
////////////////////////////////////////////////////////////
//brief Creates a tokenizer
//param options its options */
NindTokenizer::NindTokenizer(const Options &options):
    m_options(options)
{
}
////////////////////////////////////////////////////////////
//brief Tokenize text into byte spans
void NindTokenizer::tokenize(const char *text,
                             const size_t length,
                             vector<pair<size_t, size_t> > &spans) const
{
    spans.clear();
    size_t i = 0;
    ByteClass cls;
    while (i < length) {
        //saute les sejparateurs (et les soulignejs s'ils coupent)
        size_t charLength = charClass(text, i, length, cls);
        if (cls == SEPARATOR || (cls == UNDERSCORE && m_options.splitUnderscore)) {
            i += charLength;
            continue;
        }
        //le mot
        const size_t begin = i;
        while (i < length) {
            charLength = charClass(text, i, length, cls);
            if (cls == SEPARATOR || (cls == UNDERSCORE && m_options.splitUnderscore)) break;
            i += charLength;
        }
        const size_t end = i;
        //ses morceaux
        size_t pieceBegin = begin;
        for (size_t j = begin + 1; j <= end; j++) {
            if (j < end && !isBoundary(text, j, end)) continue;
            const size_t pieceLength = j - pieceBegin;
            if (pieceLength >= m_options.minLength && pieceLength <= m_options.maxLength)
                spans.push_back(pair<size_t, size_t>(pieceBegin, j));
            pieceBegin = j;
        }
    }
}
////////////////////////////////////////////////////////////
//brief Tokenize text into strings
void NindTokenizer::tokenize(const string &text,
                             vector<string> &tokens) const
{
    vector<pair<size_t, size_t> > spans;
    tokenize(text.data(), text.size(), spans);
    tokens.resize(spans.size());
    for (size_t i = 0; i < spans.size(); i++) getToken(text.data(), spans[i], tokens[i]);
}
////////////////////////////////////////////////////////////
//brief Set a token from a span of text, lowercased if asked
void NindTokenizer::getToken(const char *text,
                             const pair<size_t, size_t> &span,
                             string &token) const
{
    token.assign(text + span.first, span.second - span.first);
    if (!m_options.lowercase) return;
    for (size_t i = 0; i < token.size(); i++) {
        if (token[i] >= 'A' && token[i] <= 'Z') token[i] = token[i] - 'A' + 'a';
    }
}
////////////////////////////////////////////////////////////
//vrai s'il faut couper entre les octets j-1 et j du mot [begin, end)
bool NindTokenizer::isBoundary(const char *text,
                               const size_t j,
                               const size_t end) const
{
    const ByteClass prev = byteClass(text[j - 1]);
    const ByteClass cur = byteClass(text[j]);
    //un souligné qui ne coupe pas reste collé ah ses voisins
    if (prev == UNDERSCORE || cur == UNDERSCORE) return false;
    if (m_options.splitDigits && ((prev == DIGIT) != (cur == DIGIT))) return true;
    if (m_options.splitCamelCase && cur == UPPER) {
        //"fooBar", "caféBar", "utf8Encode"
        if (prev == LOWER || prev == OTHER || prev == DIGIT) return true;
        //"HTTPServer" : la derniehre majuscule commence le mot suivant
        if (prev == UPPER && j + 1 < end && byteClass(text[j + 1]) == LOWER) return true;
    }
    return false;
}
////////////////////////////////////////////////////////////
