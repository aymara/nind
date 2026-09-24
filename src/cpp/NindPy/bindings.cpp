//
// Python bindings (pybind11) for the core nind C++ index stack.
//
// Exposes the writer/reader classes that have no Python-side equivalent writer
// (NindLexiconIndex, NindTermIndex, NindLocalIndex, NindRetrolexicon) plus the
// in-memory NindLexicon, as module `nind._native`.
//
// Author: Claude <noreply@anthropic.com>
//
// Copyright: 2014-2017 LATEJCON. See LICENCE.md file that comes with this distribution
// This file is part of NIND (as "nouvelle indexation").
// NIND is free software: you can redistribute it and/or modify it under the terms of the
// GNU Less General Public License (LGPL) as published by the Free Software Foundation,
// (see <http://www.gnu.org/licenses/>), either version 3 of the License, or any later version.
////////////////////////////////////////////////////////////
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/operators.h>
#include <sstream>
#include <map>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <limits>
#include "NindBasics/NindPadFile.h"
#include "NindRetrolexicon/NindRetrolexicon.h"
#include "NindLexicon/NindLexicon.h"
#include "NindIndex/NindLexiconIndex.h"
#include "NindIndex/NindTermIndex.h"
#include "NindIndex/NindLocalIndex.h"
#include "NindExceptions.h"
////////////////////////////////////////////////////////////
namespace py = pybind11;
using namespace latecon::nindex;
////////////////////////////////////////////////////////////
namespace {
// 1-D contiguous buffer of 32-bit unsigned ints (array.array('I'), numpy
// uint32, ...), as used by the *_arrays bulk writers below: they build the
// C++ definition straight from flat buffers instead of going through one
// Python Term/Localisation/Document object per entry.
struct UIntView {
    py::buffer_info info;
    const uint32_t *data;
    size_t size;
};
UIntView uintView(const py::buffer &buffer, const char *name)
{
    py::buffer_info info = buffer.request();
    const std::string &format = info.format;
    const bool isUInt32 = info.itemsize == 4 && !format.empty() &&
                          (format.back() == 'I' || format.back() == 'L') &&
                          (format.size() == 1 || format[0] == '<' || format[0] == '=' || format[0] == '@');
    if (!isUInt32)
        throw py::value_error(std::string(name) + ": expected a buffer of 32-bit unsigned ints (e.g. array('I')), got format '" + format + "'");
    if (info.ndim != 1 || (info.shape[0] > 1 && info.strides[0] != 4))
        throw py::value_error(std::string(name) + ": expected a 1-D contiguous buffer");
    const uint32_t *data = static_cast<const uint32_t *>(info.ptr);
    const size_t size = static_cast<size_t>(info.shape[0]);
    return UIntView{std::move(info), data, size};
}
// validates a batch's end offsets: ascending, the last one == itemsNb
void checkEnds(const UIntView &ends, const size_t itemsNb, const char *name)
{
    uint32_t prev = 0;
    for (size_t i = 0; i < ends.size; i++) {
        if (ends.data[i] < prev || ends.data[i] > itemsNb)
            throw py::value_error(std::string(name) + ": end offsets must be ascending and within the data");
        prev = ends.data[i];
    }
    if (prev != itemsNb)
        throw py::value_error(std::string(name) + ": last end offset must equal the data length");
}
// one term definition from its [begin, end) slice of (docIds, freqs)
std::list<NindTermIndex::TermCG> buildTermDef(const uint32_t *docIds, const uint32_t *freqs,
                                               const size_t begin, const size_t end,
                                               const unsigned char cg)
{
    std::vector<std::pair<uint32_t, uint32_t> > postings(end - begin);
    for (size_t i = begin; i < end; i++) postings[i - begin] = std::make_pair(docIds[i], freqs[i]);
    if (!std::is_sorted(postings.begin(), postings.end())) std::sort(postings.begin(), postings.end());
    std::list<NindTermIndex::TermCG> termDef;
    if (postings.empty()) return termDef;
    uint64_t total = 0;
    NindTermIndex::TermCG termCG(cg, 0);
    for (size_t i = 0; i < postings.size(); i++) {
        if (i > 0 && postings[i].first == postings[i - 1].first)
            throw py::value_error("doc_ids: duplicate document id " + std::to_string(postings[i].first));
        total += postings[i].second;
        termCG.documents.push_back(NindTermIndex::Document(postings[i].first, postings[i].second));
    }
    if (total > std::numeric_limits<unsigned int>::max())
        throw py::value_error("frequencies: total term frequency overflows 32 bits");
    termCG.frequency = static_cast<unsigned int>(total);
    termDef.push_back(std::move(termCG));
    return termDef;
}
// one local definition from its [begin, end) slice of occurrences: one Term
// per term id (ascending), localisations kept in occurrence order
std::list<NindLocalIndex::Term> buildLocalDef(const uint32_t *termIds, const uint32_t *positions,
                                               const uint32_t *lengths,
                                               const size_t begin, const size_t end,
                                               const unsigned char cg)
{
    std::map<unsigned int, NindLocalIndex::Term> byTerm;
    for (size_t i = begin; i < end; i++) {
        std::map<unsigned int, NindLocalIndex::Term>::iterator it = byTerm.find(termIds[i]);
        if (it == byTerm.end())
            it = byTerm.insert(std::make_pair(termIds[i], NindLocalIndex::Term(termIds[i], cg))).first;
        it->second.localisation.push_back(NindLocalIndex::Localisation(positions[i], lengths[i]));
    }
    std::list<NindLocalIndex::Term> localDef;
    for (std::map<unsigned int, NindLocalIndex::Term>::iterator it = byTerm.begin(); it != byTerm.end(); it++)
        localDef.push_back(std::move(it->second));
    return localDef;
}
}
////////////////////////////////////////////////////////////
PYBIND11_MODULE(_native, m) {
    m.doc() = "Native (C++) implementation of the nind core index stack";

    py::register_exception<FileException>(m, "NindError", PyExc_RuntimeError);

    py::class_<NindPadFile::Identification>(m, "Identification")
        .def(py::init<>())
        .def(py::init<unsigned int, unsigned int>(),
             py::arg("lexicon_words_nb"), py::arg("lexicon_time"))
        .def_readwrite("lexicon_words_nb", &NindPadFile::Identification::lexiconWordsNb)
        .def_readwrite("lexicon_time", &NindPadFile::Identification::lexiconTime)
        .def(py::self == py::self)
        .def(py::self != py::self)
        .def("__repr__", [](const NindPadFile::Identification &id) {
            return "Identification(lexicon_words_nb=" + std::to_string(id.lexiconWordsNb) +
                   ", lexicon_time=" + std::to_string(id.lexiconTime) + ")";
        });

    // ---- Shared diagnostics structures (NindPadFile / NindIndex) --------
    py::class_<NindPadFile::Repartition>(m, "Repartition")
        .def(py::init<>())
        .def_readwrite("count", &NindPadFile::Repartition::count)
        .def_readwrite("min_value", &NindPadFile::Repartition::minValue)
        .def_readwrite("max_value", &NindPadFile::Repartition::maxValue)
        .def_readwrite("sum", &NindPadFile::Repartition::sum)
        .def_readwrite("mean", &NindPadFile::Repartition::mean)
        .def_readwrite("stddev", &NindPadFile::Repartition::stddev);

    py::class_<NindPadFile::HoleStats>(m, "HoleStats")
        .def(py::init<>())
        .def_readwrite("holes_count", &NindPadFile::HoleStats::holesCount)
        .def_readwrite("holes_size", &NindPadFile::HoleStats::holesSize)
        .def_readwrite("hole_size_histogram", &NindPadFile::HoleStats::holeSizeHistogram)
        .def_readwrite("occupied_size_histogram", &NindPadFile::HoleStats::occupiedSizeHistogram);

    py::class_<NindPadFile::BlockStats>(m, "BlockStats")
        .def(py::init<>())
        .def_readwrite("block_addr", &NindPadFile::BlockStats::blockAddr)
        .def_readwrite("block_num", &NindPadFile::BlockStats::blockNum)
        .def_readwrite("entries_used", &NindPadFile::BlockStats::entriesUsed)
        .def_readwrite("entries_total", &NindPadFile::BlockStats::entriesTotal)
        .def_readwrite("en_vrac_addr", &NindPadFile::BlockStats::enVracAddr)
        .def_readwrite("en_vrac_size", &NindPadFile::BlockStats::enVracSize);

    py::class_<NindPadFile::PadFileStats>(m, "PadFileStats")
        .def(py::init<>())
        .def_readwrite("data_entry_size", &NindPadFile::PadFileStats::dataEntrySize)
        .def_readwrite("specifics_size", &NindPadFile::PadFileStats::specificsSize)
        .def_readwrite("blocks", &NindPadFile::PadFileStats::blocks)
        .def_readwrite("index_total_size", &NindPadFile::PadFileStats::indexTotalSize)
        .def_readwrite("en_vrac_total_size", &NindPadFile::PadFileStats::enVracTotalSize)
        .def_readwrite("identification", &NindPadFile::PadFileStats::identification)
        .def_readwrite("file_size", &NindPadFile::PadFileStats::fileSize);

    py::class_<NindIndex::IndexStats>(m, "IndexStats")
        .def(py::init<>())
        .def_readwrite("max_ident", &NindIndex::IndexStats::maxIdent)
        .def_readwrite("used_count", &NindIndex::IndexStats::usedCount)
        .def_readwrite("holes", &NindIndex::IndexStats::holes)
        .def_readwrite("definition_sizes", &NindIndex::IndexStats::definitionSizes);

    // ---- NindLexiconIndex -------------------------------------------------
    py::class_<NindLexiconIndex>(m, "NindLexiconIndex")
        .def(py::init<const std::string &, bool, bool, unsigned int, unsigned int>(),
             py::arg("file_name_extensionless"), py::arg("is_writer"),
             py::arg("with_retrolexicon") = false,
             py::arg("indirection_bloc_size") = 0,
             py::arg("retro_indirection_bloc_size") = 0)
        .def("add_word", &NindLexiconIndex::addWord, py::arg("components"))
        .def("get_word_id", &NindLexiconIndex::getWordId, py::arg("components"))
        .def("get_identification", &NindLexiconIndex::getIdentification)
        .def("get_components", [](NindLexiconIndex &self, unsigned int ident) -> py::object {
            std::list<std::string> components;
            if (!self.getComponents(ident, components)) return py::none();
            return py::cast(components);
        }, py::arg("ident"))
        .def("get_retrolexicon_file_name", &NindLexiconIndex::getRetrolexiconFileName)
        .def("get_file_name", &NindPadFile::getFileName)
        .def("analyse_pad_file", &NindPadFile::analysePadFile)
        .def("analyse_index", &NindIndex::analyseIndex);

    // ---- NindTermIndex ------------------------------------------------
    py::class_<NindTermIndex::Document>(m, "Document")
        .def(py::init<>())
        .def(py::init<unsigned int, unsigned int>(), py::arg("ident"), py::arg("frequency"))
        .def_readwrite("ident", &NindTermIndex::Document::ident)
        .def_readwrite("frequency", &NindTermIndex::Document::frequency)
        .def("__repr__", [](const NindTermIndex::Document &d) {
            return "Document(ident=" + std::to_string(d.ident) +
                   ", frequency=" + std::to_string(d.frequency) + ")";
        });

    py::class_<NindTermIndex::TermCG>(m, "TermCG")
        .def(py::init<>())
        .def(py::init<unsigned char, unsigned int>(), py::arg("cg"), py::arg("frequency"))
        .def_readwrite("cg", &NindTermIndex::TermCG::cg)
        .def_readwrite("frequency", &NindTermIndex::TermCG::frequency)
        .def_readwrite("documents", &NindTermIndex::TermCG::documents);

    py::class_<NindTermIndex>(m, "NindTermIndex")
        .def(py::init<const std::string &, bool, const NindPadFile::Identification &, unsigned int, unsigned int>(),
             py::arg("file_name_extensionless"), py::arg("is_writer"),
             py::arg("lexicon_identification"), py::arg("specifics_number"),
             py::arg("indirection_bloc_size") = 0)
        .def("get_term_def", [](NindTermIndex &self, unsigned int ident) -> py::object {
            std::list<NindTermIndex::TermCG> termDef;
            if (!self.getTermDef(ident, termDef)) return py::none();
            return py::cast(termDef);
        }, py::arg("ident"))
        .def("get_specific_words", [](NindTermIndex &self) {
            std::list<unsigned int> specifics;
            self.getSpecificWords(specifics);
            return specifics;
        })
        .def("set_term_def", &NindTermIndex::setTermDef,
             py::arg("ident"), py::arg("term_def"), py::arg("file_identification"), py::arg("specifics"))
        // Bulk form of set_term_def for a single-category term: documents
        // given as two parallel uint32 buffers; sorted by document id here
        // if they aren't already (duplicates are rejected), frequency is
        // their sum. Releases the GIL while writing, so a writer must not
        // be shared between Python threads.
        .def("set_term_def_arrays", [](NindTermIndex &self, unsigned int ident,
                                       const py::buffer &docIds, const py::buffer &frequencies,
                                       const NindPadFile::Identification &fileIdentification,
                                       unsigned char cg, const std::list<unsigned int> &specifics) {
            const UIntView ids = uintView(docIds, "doc_ids");
            const UIntView freqs = uintView(frequencies, "frequencies");
            if (ids.size != freqs.size)
                throw py::value_error("doc_ids and frequencies must have the same length");
            const std::list<NindTermIndex::TermCG> termDef = buildTermDef(ids.data, freqs.data, 0, ids.size, cg);
            py::gil_scoped_release release;
            self.setTermDef(ident, termDef, fileIdentification, specifics);
        }, py::arg("ident"), py::arg("doc_ids"), py::arg("frequencies"), py::arg("file_identification"),
           py::arg("cg") = 0, py::arg("specifics") = std::list<unsigned int>())
        // Whole-batch form of set_term_def_arrays: term i is idents[i], its
        // documents are doc_ids/frequencies[ends[i-1]:ends[i]]. Terms are
        // written in the given order; the batch is validated up front, so a
        // ValueError means nothing was written.
        .def("set_term_defs_arrays", [](NindTermIndex &self, const py::buffer &idents,
                                        const py::buffer &ends, const py::buffer &docIds,
                                        const py::buffer &frequencies,
                                        const NindPadFile::Identification &fileIdentification,
                                        unsigned char cg, const std::list<unsigned int> &specifics) {
            const UIntView terms = uintView(idents, "idents");
            const UIntView termEnds = uintView(ends, "ends");
            const UIntView ids = uintView(docIds, "doc_ids");
            const UIntView freqs = uintView(frequencies, "frequencies");
            if (ids.size != freqs.size)
                throw py::value_error("doc_ids and frequencies must have the same length");
            if (terms.size != termEnds.size)
                throw py::value_error("idents and ends must have the same length");
            checkEnds(termEnds, ids.size, "ends");
            std::vector<std::list<NindTermIndex::TermCG> > termDefs(terms.size);
            size_t begin = 0;
            for (size_t i = 0; i < terms.size; i++) {
                termDefs[i] = buildTermDef(ids.data, freqs.data, begin, termEnds.data[i], cg);
                begin = termEnds.data[i];
            }
            py::gil_scoped_release release;
            for (size_t i = 0; i < terms.size; i++)
                self.setTermDef(terms.data[i], termDefs[i], fileIdentification, specifics);
        }, py::arg("idents"), py::arg("ends"), py::arg("doc_ids"), py::arg("frequencies"),
           py::arg("file_identification"), py::arg("cg") = 0,
           py::arg("specifics") = std::list<unsigned int>())
        .def("get_file_name", &NindPadFile::getFileName)
        .def("analyse_pad_file", &NindPadFile::analysePadFile)
        .def("analyse_index", &NindIndex::analyseIndex);

    // ---- NindLocalIndex -----------------------------------------------
    py::class_<NindLocalIndex::Localisation>(m, "Localisation")
        .def(py::init<>())
        .def(py::init<unsigned int, unsigned int>(), py::arg("position"), py::arg("length"))
        .def_readwrite("position", &NindLocalIndex::Localisation::position)
        .def_readwrite("length", &NindLocalIndex::Localisation::length);

    py::class_<NindLocalIndex::Term>(m, "Term")
        .def(py::init<>())
        .def(py::init<unsigned int, unsigned char>(), py::arg("term"), py::arg("cg"))
        .def_readwrite("term", &NindLocalIndex::Term::term)
        .def_readwrite("cg", &NindLocalIndex::Term::cg)
        .def_readwrite("localisation", &NindLocalIndex::Term::localisation);

    py::class_<NindLocalIndex>(m, "NindLocalIndex")
        .def(py::init<const std::string &, bool, const NindPadFile::Identification &, unsigned int>(),
             py::arg("file_name_extensionless"), py::arg("is_writer"),
             py::arg("lexicon_identification"), py::arg("indirection_bloc_size") = 0)
        .def("get_local_def", [](NindLocalIndex &self, unsigned int ident) -> py::object {
            std::list<NindLocalIndex::Term> localDef;
            if (!self.getLocalDef(ident, localDef)) return py::none();
            return py::cast(localDef);
        }, py::arg("ident"))
        .def("get_local_length", [](NindLocalIndex &self, unsigned int ident) -> py::object {
            unsigned int length = 0;
            if (!self.getLocalLength(ident, length)) return py::none();
            return py::cast(length);
        }, py::arg("ident"))
        .def("get_term_idents", [](NindLocalIndex &self, unsigned int ident) -> py::object {
            std::set<unsigned int> termIdents;
            if (!self.getTermIdents(ident, termIdents)) return py::none();
            return py::cast(termIdents);
        }, py::arg("ident"))
        .def("set_local_def", &NindLocalIndex::setLocalDef,
             py::arg("ident"), py::arg("local_def"), py::arg("file_identification"))
        // Bulk form of set_local_def: one entry per occurrence, as three
        // parallel uint32 buffers in document order; grouped here into one
        // Term per term id (ascending, all with category cg), each keeping
        // its localisations in occurrence order. Releases the GIL while
        // writing, so a writer must not be shared between Python threads.
        .def("set_local_def_arrays", [](NindLocalIndex &self, unsigned int ident,
                                        const py::buffer &termIds, const py::buffer &positions,
                                        const py::buffer &lengths,
                                        const NindPadFile::Identification &fileIdentification,
                                        unsigned char cg) {
            const UIntView terms = uintView(termIds, "term_ids");
            const UIntView pos = uintView(positions, "positions");
            const UIntView lens = uintView(lengths, "lengths");
            if (terms.size != pos.size || terms.size != lens.size)
                throw py::value_error("term_ids, positions and lengths must have the same length");
            const std::list<NindLocalIndex::Term> localDef =
                buildLocalDef(terms.data, pos.data, lens.data, 0, terms.size, cg);
            py::gil_scoped_release release;
            self.setLocalDef(ident, localDef, fileIdentification);
        }, py::arg("ident"), py::arg("term_ids"), py::arg("positions"), py::arg("lengths"),
           py::arg("file_identification"), py::arg("cg") = 0)
        // Whole-batch form of set_local_def_arrays: document i is idents[i],
        // its occurrences are term_ids/positions/lengths[ends[i-1]:ends[i]].
        // Documents are written in the given order, built one at a time with
        // the GIL released; the offsets are validated up front, so a
        // ValueError means nothing was written.
        .def("set_local_defs_arrays", [](NindLocalIndex &self, const py::buffer &idents,
                                         const py::buffer &ends, const py::buffer &termIds,
                                         const py::buffer &positions, const py::buffer &lengths,
                                         const NindPadFile::Identification &fileIdentification,
                                         unsigned char cg) {
            const UIntView docs = uintView(idents, "idents");
            const UIntView docEnds = uintView(ends, "ends");
            const UIntView terms = uintView(termIds, "term_ids");
            const UIntView pos = uintView(positions, "positions");
            const UIntView lens = uintView(lengths, "lengths");
            if (terms.size != pos.size || terms.size != lens.size)
                throw py::value_error("term_ids, positions and lengths must have the same length");
            if (docs.size != docEnds.size)
                throw py::value_error("idents and ends must have the same length");
            checkEnds(docEnds, terms.size, "ends");
            py::gil_scoped_release release;
            size_t begin = 0;
            for (size_t i = 0; i < docs.size; i++) {
                const std::list<NindLocalIndex::Term> localDef =
                    buildLocalDef(terms.data, pos.data, lens.data, begin, docEnds.data[i], cg);
                self.setLocalDef(docs.data[i], localDef, fileIdentification);
                begin = docEnds.data[i];
            }
        }, py::arg("idents"), py::arg("ends"), py::arg("term_ids"), py::arg("positions"),
           py::arg("lengths"), py::arg("file_identification"), py::arg("cg") = 0)
        .def("get_doc_count", &NindLocalIndex::getDocCount)
        .def("get_file_name", &NindPadFile::getFileName)
        .def("analyse_pad_file", &NindPadFile::analysePadFile)
        .def("analyse_index", &NindIndex::analyseIndex);

    // ---- NindRetrolexicon -----------------------------------------------
    py::class_<NindRetrolexicon::RetroWord>(m, "RetroWord")
        .def(py::init<>())
        .def(py::init<unsigned int, std::string>(), py::arg("identifiant"), py::arg("mot_simple"))
        .def(py::init<unsigned int, unsigned int, unsigned int>(),
             py::arg("identifiant"), py::arg("identifiant_a"), py::arg("identifiant_s"))
        .def_readwrite("identifiant", &NindRetrolexicon::RetroWord::identifiant)
        .def_readwrite("mot_simple", &NindRetrolexicon::RetroWord::motSimple)
        .def_readwrite("identifiant_a", &NindRetrolexicon::RetroWord::identifiantA)
        .def_readwrite("identifiant_s", &NindRetrolexicon::RetroWord::identifiantS);

    py::class_<NindRetrolexicon>(m, "NindRetrolexicon")
        .def(py::init<const std::string &, bool, const NindPadFile::Identification &, unsigned int>(),
             py::arg("file_name_extensionless"), py::arg("is_writer"),
             py::arg("lexicon_identification"), py::arg("indirection_bloc_size") = 0)
        .def("add_retro_words", &NindRetrolexicon::addRetroWords,
             py::arg("retro_words"), py::arg("lexicon_identification"))
        .def("get_components", [](NindRetrolexicon &self, unsigned int ident) -> py::object {
            std::list<std::string> components;
            if (!self.getComponents(ident, components)) return py::none();
            return py::cast(components);
        }, py::arg("ident"))
        .def("get_file_name", &NindRetrolexicon::getFileName)
        .def("analyse_pad_file", &NindPadFile::analysePadFile);

    // ---- NindLexicon (in-memory) ------------------------------------------
    py::class_<NindLexicon::LexiconChar>(m, "LexiconChar")
        .def(py::init<>())
        .def_readwrite("is_ok", &NindLexicon::LexiconChar::isOk)
        .def_readwrite("sw_nb", &NindLexicon::LexiconChar::swNb)
        .def_readwrite("cw_nb", &NindLexicon::LexiconChar::cwNb)
        .def_readwrite("words_nb", &NindLexicon::LexiconChar::wordsNb)
        .def_readwrite("identification", &NindLexicon::LexiconChar::identification);

    py::class_<NindLexicon>(m, "NindLexicon")
        .def(py::init<const std::string &, bool>(), py::arg("file_name"), py::arg("is_writer"))
        .def("add_word", &NindLexicon::addWord, py::arg("components"))
        .def("get_id", &NindLexicon::getId, py::arg("components"))
        .def("get_identification", [](NindLexicon &self) {
            unsigned int wordsNb = 0, identification = 0;
            self.getIdentification(wordsNb, identification);
            return py::make_tuple(wordsNb, identification);
        })
        .def("integrity_and_counts", [](NindLexicon &self) {
            NindLexicon::LexiconChar lexiconChar;
            self.integrityAndCounts(lexiconChar);
            return lexiconChar;
        })
        .def("dump", [](NindLexicon &self) {
            std::ostringstream oss;
            self.dump(oss);
            return oss.str();
        });
}
////////////////////////////////////////////////////////////
