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
#include <queue>
#include <map>
#include <functional>
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
#include "NindIndex/NindIndexMerge.h"
#include "NindIndex/NindIndexVerify.h"
#include "NindIndex/NindIndexUpdate.h"
#include "NindIndex/NindBm25Scorer.h"
#include "NindIndex/NindIndexBuilder.h"
#include "NindIndex/NindTokenizer.h"
#include "NindIndex/NindDocNames.h"
#include "NindIndex/NindSegmentedIndex.h"
#include "NindIndex/NindSegmentedUpdate.h"
#include <unordered_map>
#include <cmath>
#include <mutex>
#include <thread>
#include <memory>
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
// document ids from Python: a buffer of 64-bit (array('Q')) or 32-bit (array('I')) unsigned ints,
// copied as 64-bit ids (NindDocId)
struct DocIdsView {
    std::vector<uint64_t> values;
    const uint64_t *data;
    size_t size;
};
DocIdsView docIdsView(const py::buffer &buffer, const char *name)
{
    py::buffer_info info = buffer.request();
    const std::string &format = info.format;
    const bool native = format.size() == 1 || format[0] == '<' || format[0] == '=' || format[0] == '@';
    const char code = format.empty() ? 0 : format.back();
    const bool isUInt32 = info.itemsize == 4 && native && (code == 'I' || code == 'L');
    const bool isUInt64 = info.itemsize == 8 && native && (code == 'Q' || code == 'L' || code == 'K');
    if (!isUInt32 && !isUInt64)
        throw py::value_error(std::string(name) + ": expected a buffer of 64-bit or 32-bit unsigned ints (e.g. array('Q')), got format '" + format + "'");
    if (info.ndim != 1 || (info.shape[0] > 1 && info.strides[0] != info.itemsize))
        throw py::value_error(std::string(name) + ": expected a 1-D contiguous buffer");
    DocIdsView view;
    const size_t size = static_cast<size_t>(info.shape[0]);
    view.values.resize(size);
    if (isUInt64) {
        const uint64_t *data = static_cast<const uint64_t *>(info.ptr);
        std::copy(data, data + size, view.values.begin());
    }
    else {
        const uint32_t *data = static_cast<const uint32_t *>(info.ptr);
        std::copy(data, data + size, view.values.begin());
    }
    view.data = view.values.data();
    view.size = size;
    return view;
}
// (document, length) pairs from two buffers of the same size (ids 'I' or 'Q', lengths 'I')
std::vector<std::pair<NindDocId, unsigned int> > docLengthPairs(const py::buffer &docIds, const py::buffer &lengths)
{
    const DocIdsView ids = docIdsView(docIds, "doc_ids");
    const UIntView lens = uintView(lengths, "lengths");
    if (ids.size != lens.size) throw py::value_error("doc_ids and lengths must have the same length");
    std::vector<std::pair<NindDocId, unsigned int> > pairs(ids.size);
    for (size_t i = 0; i < ids.size; i++) pairs[i] = std::make_pair((NindDocId)ids.data[i], lens.data[i]);
    return pairs;
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
}
namespace {
py::bytes uintBytes(const std::vector<uint32_t> &values)
{
    return py::bytes(reinterpret_cast<const char *>(values.data()), values.size() * sizeof(uint32_t));
}
py::bytes uint64Bytes(const std::vector<uint64_t> &values)
{
    return py::bytes(reinterpret_cast<const char *>(values.data()), values.size() * sizeof(uint64_t));
}


// The thread-safe (const) reads of readers each use a buffer of their own (see NindFile::readAt):
// a local one for a single lookup, the pooled scratch's for a search. Not a thread_local: in a
// dynamically loaded module, every access to one may cost a __tls_get_addr call, and with
// link-time optimization the compiler recomputed its address in the decoding loops (11% of a
// search's instructions).

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
        // many words at once (same idents as add_word on each in turn, each hash bucket
        // read and written once): words is a list of component lists
        .def("add_words", [](NindLexiconIndex &self, const std::vector<std::list<std::string> > &words) {
            std::vector<unsigned int> idents;
            py::gil_scoped_release release;
            self.addWords(words, idents);
            return idents;
        }, py::arg("words"))
        // the same for simple words: words is a list of strings
        .def("add_simple_words", [](NindLexiconIndex &self, const std::vector<std::string> &words) {
            std::vector<unsigned int> idents;
            py::gil_scoped_release release;
            std::vector<std::list<std::string> > lists(words.size());
            for (size_t i = 0; i < words.size(); i++) lists[i].push_back(words[i]);
            self.addWords(lists, idents);
            return idents;
        }, py::arg("words"))
        // bulk mode (see NindIndex::beginBulk): only for a file no reader opens before
        // end_bulk(), which must then be called (the destructor does it too, ignoring errors)
        .def("begin_bulk", &NindIndex::beginBulk, py::call_guard<py::gil_scoped_release>())
        .def("end_bulk", &NindIndex::endBulk, py::call_guard<py::gil_scoped_release>())
        .def("is_bulk", &NindIndex::isBulk)
        .def("is_format_v2", &NindIndex::isFormatV2)
        .def("get_max_ident", &NindPadFile::getMaxIdentAt)   //the indirections' capacity
        .def("get_word_id", [](NindLexiconIndex &self, const std::list<std::string> &components) {
            // a reader: thread-safe const path, GIL released; a writer: the sequential API
            if (self.isWriter()) return self.getWordId(components);
            py::gil_scoped_release release;
            NindReadBuffer buffer;
            return self.getWordId(components, buffer);
        }, py::arg("components"))
        .def("get_identification", &NindLexiconIndex::getIdentification)
        .def("get_modulo", &NindLexiconIndex::getModulo)
        .def("get_components", [](NindLexiconIndex &self, unsigned int ident) -> py::object {
            std::list<std::string> components;
            if (!self.getComponents(ident, components)) return py::none();
            return py::cast(components);
        }, py::arg("ident"))
        .def("get_retrolexicon_file_name", &NindLexiconIndex::getRetrolexiconFileName)
        // every word as (ident, prefix_ident, last_component), in increasing ident order:
        // prefix_ident is 0 for a simple word, else the ident of the compound word's prefix
        .def("get_entries", [](NindLexiconIndex &self) {
            std::vector<NindLexiconIndex::Entry> entries;
            self.getEntries(entries);
            py::list result;
            for (size_t i = 0; i < entries.size(); i++)
                result.append(py::make_tuple(entries[i].ident, entries[i].prefixIdent, entries[i].lastComponent));
            return result;
        })
        .def("get_file_name", &NindPadFile::getFileName)
        .def("analyse_pad_file", &NindPadFile::analysePadFile)
        .def("analyse_index", &NindIndex::analyseIndex);

    // ---- NindTermIndex ------------------------------------------------
    py::class_<NindTermIndex::Document>(m, "Document")
        .def(py::init<>())
        .def(py::init<NindDocId, unsigned int>(), py::arg("ident"), py::arg("frequency"))
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

    // format v2 blocks, as a term's directory describes them (get_term_directory)
    py::class_<NindTermIndex::Block>(m, "Block")
        .def_readonly("first", &NindTermIndex::Block::first)
        .def_readonly("last", &NindTermIndex::Block::last)
        .def_readonly("docs_nb", &NindTermIndex::Block::docsNb)
        .def_readonly("max_frequency", &NindTermIndex::Block::maxFrequency)
        .def_readonly("min_length", &NindTermIndex::Block::minLength)
        .def_readonly("offset", &NindTermIndex::Block::offset)
        .def_readonly("length", &NindTermIndex::Block::length)
        // the competitive (frequency, length) pairs (empty: unknown lengths)
        .def_property_readonly("impacts", [](const NindTermIndex::Block &block) {
            return std::vector<std::pair<unsigned int, unsigned int> >(block.impacts, block.impacts + block.impactsNb);
        });
    py::class_<NindTermIndex::TermDirectory::Category>(m, "DirectoryCategory")
        .def_readonly("cg", &NindTermIndex::TermDirectory::Category::cg)
        .def_readonly("frequency", &NindTermIndex::TermDirectory::Category::frequency)
        .def_readonly("docs_nb", &NindTermIndex::TermDirectory::Category::docsNb)
        .def_readonly("inline_postings", &NindTermIndex::TermDirectory::Category::inlinePostings)
        .def_readonly("blocks", &NindTermIndex::TermDirectory::Category::blocks);

    py::class_<NindTermIndex>(m, "NindTermIndex")
        .def(py::init<const std::string &, bool, const NindPadFile::Identification &, unsigned int, unsigned int>(),
             py::arg("file_name_extensionless"), py::arg("is_writer"),
             py::arg("lexicon_identification"), py::arg("specifics_number"),
             py::arg("indirection_bloc_size") = 0)
        // a term's directory (format v2: blocks' descriptors, not their postings; v1: every
        // posting inline), per category; None if the term has no definition
        .def("get_term_directory", [](const NindTermIndex &self, unsigned int ident) -> py::object {
            NindTermIndex::TermDirectory directory;
            bool found;
            {
                py::gil_scoped_release release;
                NindReadBuffer buffer;
                found = self.getTermDirectory(ident, directory, buffer);
            }
            if (!found) return py::none();
            return py::cast(directory.categories);
        }, py::arg("ident"))
        .def("get_term_def", [](NindTermIndex &self, unsigned int ident) -> py::object {
            std::list<NindTermIndex::TermCG> termDef;
            bool found;
            if (self.isWriter()) found = self.getTermDef(ident, termDef);
            else {
                py::gil_scoped_release release;
                NindReadBuffer buffer;
                found = self.getTermDef(ident, termDef, buffer);
            }
            if (!found) return py::none();
            return py::cast(termDef);
        }, py::arg("ident"))
        .def("get_specific_words", [](NindTermIndex &self) {
            std::list<unsigned int> specifics;
            self.getSpecificWords(specifics);
            return specifics;
        })
        // bulk mode (see NindIndex::beginBulk): only for a file no reader opens before
        // end_bulk(), which must then be called (the destructor does it too, ignoring errors)
        .def("begin_bulk", &NindIndex::beginBulk, py::call_guard<py::gil_scoped_release>())
        .def("end_bulk", &NindIndex::endBulk, py::call_guard<py::gil_scoped_release>())
        .def("is_bulk", &NindIndex::isBulk)
        .def("is_format_v2", &NindIndex::isFormatV2)
        .def("get_max_ident", &NindPadFile::getMaxIdentAt)   //the indirections' capacity
        .def("set_term_def", &NindTermIndex::setTermDef,
             py::arg("ident"), py::arg("term_def"), py::arg("file_identification"), py::arg("specifics"),
             py::call_guard<py::gil_scoped_release>())
        // Bulk form of set_term_def for a single-category term: documents
        // given as two parallel uint32 buffers; sorted by document id here
        // if they aren't already (duplicates are rejected), frequency is
        // their sum. Releases the GIL while writing, so a writer must not
        // be shared between Python threads.
        .def("set_term_def_arrays", [](NindTermIndex &self, unsigned int ident,
                                       const py::buffer &docIds, const py::buffer &frequencies,
                                       const NindPadFile::Identification &fileIdentification,
                                       unsigned char cg, const std::list<unsigned int> &specifics) {
            const DocIdsView ids = docIdsView(docIds, "doc_ids");
            const UIntView freqs = uintView(frequencies, "frequencies");
            if (ids.size != freqs.size)
                throw py::value_error("doc_ids and frequencies must have the same length");
            const std::list<NindTermIndex::TermCG> termDef = NindIndexBuilder::termDef(ids.data, freqs.data, 0, ids.size, cg);
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
            const DocIdsView ids = docIdsView(docIds, "doc_ids");
            const UIntView freqs = uintView(frequencies, "frequencies");
            if (ids.size != freqs.size)
                throw py::value_error("doc_ids and frequencies must have the same length");
            if (terms.size != termEnds.size)
                throw py::value_error("idents and ends must have the same length");
            checkEnds(termEnds, ids.size, "ends");
            // every definition checked before anything is written, then one at a time: built (from
            // the buffers, held by the caller) and written (memory: one definition, not all)
            {
                std::vector<std::pair<uint64_t, uint32_t> > scratch;
                size_t begin = 0;
                for (size_t i = 0; i < terms.size; i++) {
                    NindIndexBuilder::checkTermDef(ids.data, freqs.data, begin, termEnds.data[i], scratch);
                    begin = termEnds.data[i];
                }
            }
            py::gil_scoped_release release;
            size_t begin = 0;
            for (size_t i = 0; i < terms.size; i++) {
                const std::list<NindTermIndex::TermCG> termDef =
                    NindIndexBuilder::termDef(ids.data, freqs.data, begin, termEnds.data[i], cg);
                begin = termEnds.data[i];
                self.setTermDef(terms.data[i], termDef, fileIdentification, specifics);
            }
        }, py::arg("idents"), py::arg("ends"), py::arg("doc_ids"), py::arg("frequencies"),
           py::arg("file_identification"), py::arg("cg") = 0,
           py::arg("specifics") = std::list<unsigned int>())
        // (writer) the documents' lengths (doc_ids 'I' or 'Q', lengths 'I'), for the blocks'
        // <longueurMin>: the lengths a search scores with
        .def("set_document_lengths", [](NindTermIndex &self, const py::buffer &docIds, const py::buffer &lengths) {
            self.setDocumentLengths(docLengthPairs(docIds, lengths));
        }, py::arg("doc_ids"), py::arg("lengths"))
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
        .def("get_local_def", [](NindLocalIndex &self, NindDocId ident) -> py::object {
            std::list<NindLocalIndex::Term> localDef;
            bool found;
            if (self.isWriter()) found = self.getLocalDef(ident, localDef);
            else {
                py::gil_scoped_release release;
                NindReadBuffer buffer;
                found = self.getLocalDef(ident, localDef, buffer);
            }
            if (!found) return py::none();
            return py::cast(localDef);
        }, py::arg("ident"))
        .def("get_local_length", [](NindLocalIndex &self, NindDocId ident) -> py::object {
            unsigned int length = 0;
            if (!self.getLocalLength(ident, length)) return py::none();
            return py::cast(length);
        }, py::arg("ident"))
        .def("get_term_idents", [](NindLocalIndex &self, NindDocId ident) -> py::object {
            std::set<unsigned int> termIdents;
            if (!self.getTermIdents(ident, termIdents)) return py::none();
            return py::cast(termIdents);
        }, py::arg("ident"))
        // bulk mode (see NindIndex::beginBulk): only for a file no reader opens before
        // end_bulk(), which must then be called (the destructor does it too, ignoring errors)
        .def("begin_bulk", &NindIndex::beginBulk, py::call_guard<py::gil_scoped_release>())
        .def("end_bulk", &NindIndex::endBulk, py::call_guard<py::gil_scoped_release>())
        .def("is_bulk", &NindIndex::isBulk)
        .def("is_format_v2", &NindIndex::isFormatV2)
        .def("get_max_ident", &NindPadFile::getMaxIdentAt)   //the indirections' capacity
        .def("set_local_def", &NindLocalIndex::setLocalDef,
             py::arg("ident"), py::arg("local_def"), py::arg("file_identification"),
             py::call_guard<py::gil_scoped_release>())
        // Bulk form of set_local_def: one entry per occurrence, as three
        // parallel uint32 buffers in document order; grouped here into one
        // Term per term id (ascending, all with category cg), each keeping
        // its localisations in occurrence order. Releases the GIL while
        // writing, so a writer must not be shared between Python threads.
        .def("set_local_def_arrays", [](NindLocalIndex &self, NindDocId ident,
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
                NindIndexBuilder::localDef(terms.data, pos.data, lens.data, 0, terms.size, cg);
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
            const DocIdsView docs = docIdsView(idents, "idents");
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
                    NindIndexBuilder::localDef(terms.data, pos.data, lens.data, begin, docEnds.data[i], cg);
                self.setLocalDef(docs.data[i], localDef, fileIdentification);
                begin = docEnds.data[i];
            }
        }, py::arg("idents"), py::arg("ends"), py::arg("term_ids"), py::arg("positions"),
           py::arg("lengths"), py::arg("file_identification"), py::arg("cg") = 0)
        .def("get_doc_count", &NindLocalIndex::getDocCount)
        .def("get_doc_ident_pairs", [](NindLocalIndex &self) {
            std::vector<std::pair<NindDocId, unsigned int> > docIdents;
            self.getDocIdentPairs(docIdents);
            return docIdents;
        })
        .def("get_doc_idents", [](NindLocalIndex &self) {
            std::vector<NindDocId> docIdents;
            self.getDocIdents(docIdents);
            return docIdents;
        })
        .def("get_file_name", &NindPadFile::getFileName)
        .def("analyse_pad_file", &NindPadFile::analysePadFile)
        .def("analyse_index", &NindIndex::analyseIndex);

    // ---- NindIndexMerge -------------------------------------------------
    py::class_<NindIndexMerge::Stats>(m, "MergeStats")
        .def_readonly("shards_nb", &NindIndexMerge::Stats::shardsNb)
        .def_readonly("words_nb", &NindIndexMerge::Stats::wordsNb)
        .def_readonly("terms_nb", &NindIndexMerge::Stats::termsNb)
        .def_readonly("docs_nb", &NindIndexMerge::Stats::docsNb)
        .def_readonly("identification", &NindIndexMerge::Stats::identification)
        .def("__repr__", [](const NindIndexMerge::Stats &stats) {
            return "MergeStats(shards_nb=" + std::to_string(stats.shardsNb) + ", words_nb=" + std::to_string(stats.wordsNb) +
                   ", terms_nb=" + std::to_string(stats.termsNb) + ", docs_nb=" + std::to_string(stats.docsNb) + ")";
        });

    // Offline merge of index shards (disjoint sets of documents, no term specifics) into
    // a new index. Indirection block sizes of 0 are sized to fit. Releases the GIL.
    m.def("merge_indexes", [](const std::vector<std::string> &shards, const std::string &target,
                              const bool withRetrolexicon, const unsigned int lexiconIndirectionBlocSize,
                              const unsigned int retroIndirectionBlocSize, const unsigned int termIndirectionBlocSize,
                              const unsigned int localIndirectionBlocSize, const py::object &docIds,
                              const py::object &docLengths) {
        NindIndexMerge::Options options;
        if (!docIds.is_none() || !docLengths.is_none())
            options.documentLengths = docLengthPairs(docIds.cast<py::buffer>(), docLengths.cast<py::buffer>());
        options.withRetrolexicon = withRetrolexicon;
        options.lexiconIndirectionBlocSize = lexiconIndirectionBlocSize;
        options.retroIndirectionBlocSize = retroIndirectionBlocSize;
        options.termIndirectionBlocSize = termIndirectionBlocSize;
        options.localIndirectionBlocSize = localIndirectionBlocSize;
        py::gil_scoped_release release;
        return NindIndexMerge(shards, options).merge(target);
    }, py::arg("shards"), py::arg("target"), py::arg("with_retrolexicon") = false,
       py::arg("lexicon_indirection_bloc_size") = 0, py::arg("retro_indirection_bloc_size") = 0,
       py::arg("term_indirection_bloc_size") = 0, py::arg("local_indirection_bloc_size") = 0,
       py::arg("doc_ids") = py::none(), py::arg("doc_lengths") = py::none());

    // ---- NindIndexUpdate ------------------------------------------------
    py::class_<NindIndexUpdate::Options>(m, "IndexUpdateOptions")
        .def(py::init<>())
        .def_readwrite("with_local_index", &NindIndexUpdate::Options::withLocalIndex)
        .def_readwrite("with_retrolexicon", &NindIndexUpdate::Options::withRetrolexicon)
        .def_readwrite("lexicon_min_buckets", &NindIndexUpdate::Options::lexiconMinBuckets)
        .def_readwrite("lexicon_max_load", &NindIndexUpdate::Options::lexiconMaxLoad)
        .def_readwrite("indirection_bloc_min", &NindIndexUpdate::Options::indirectionBlocMin);
    py::class_<NindIndexUpdate::Occurrence>(m, "Occurrence")
        .def(py::init([](const std::list<std::string> &word, unsigned char cg, unsigned int position, unsigned int length) {
            return NindIndexUpdate::Occurrence{word, cg, position, length};
        }), py::arg("word"), py::arg("cg") = 0, py::arg("position") = 0, py::arg("length") = 1)
        .def_readwrite("word", &NindIndexUpdate::Occurrence::word)
        .def_readwrite("cg", &NindIndexUpdate::Occurrence::cg)
        .def_readwrite("position", &NindIndexUpdate::Occurrence::position)
        .def_readwrite("length", &NindIndexUpdate::Occurrence::length);
    py::class_<NindIndexUpdate::Stats>(m, "IndexUpdateStats")
        .def_readonly("added_docs_nb", &NindIndexUpdate::Stats::addedDocsNb)
        .def_readonly("removed_docs_nb", &NindIndexUpdate::Stats::removedDocsNb)
        .def_readonly("new_words_nb", &NindIndexUpdate::Stats::newWordsNb)
        .def_readonly("terms_nb", &NindIndexUpdate::Stats::termsNb)
        .def_readonly("in_place_terms_nb", &NindIndexUpdate::Stats::inPlaceTermsNb)
        .def_readonly("migrated", &NindIndexUpdate::Stats::migrated)
        .def_readonly("rebuilt", &NindIndexUpdate::Stats::rebuilt);
    // An incremental update (see NindIndexUpdate.h): documents added (or replaced) and removed,
    // written by commit() (GIL released)
    py::class_<NindIndexUpdate>(m, "NindIndexUpdate")
        .def(py::init<const std::string &, const NindIndexUpdate::Options &>(),
             py::arg("file_name_extensionless"), py::arg("options") = NindIndexUpdate::Options())
        .def("set_document_lengths", [](NindIndexUpdate &self, const py::buffer &docIds, const py::buffer &lengths) {
            self.setDocumentLengths(docLengthPairs(docIds, lengths));
        }, py::arg("doc_ids"), py::arg("lengths"))
        .def("add_document", [](NindIndexUpdate &self, NindDocId ident, const std::vector<std::string> &words,
                                unsigned char cg, long length) {
            self.addDocument(ident, words, cg, length);
        }, py::arg("ident"), py::arg("words"), py::arg("cg") = 0, py::arg("length") = -1)
        .def("add_document_occurrences", [](NindIndexUpdate &self, NindDocId ident,
                                            const std::vector<NindIndexUpdate::Occurrence> &occurrences, long length) {
            self.addDocument(ident, occurrences, length);
        }, py::arg("ident"), py::arg("occurrences"), py::arg("length") = -1)
        .def("remove_document", &NindIndexUpdate::removeDocument, py::arg("ident"))
        .def("pending_nb", &NindIndexUpdate::pendingNb)
        .def("commit", &NindIndexUpdate::commit, py::call_guard<py::gil_scoped_release>());

    // ---- NindIndexVerify ------------------------------------------------
    py::class_<NindIndexVerify::Report>(m, "VerifyReport")
        .def_readonly("format_v2", &NindIndexVerify::Report::formatV2)
        .def_readonly("words_nb", &NindIndexVerify::Report::wordsNb)
        .def_readonly("terms_nb", &NindIndexVerify::Report::termsNb)
        .def_readonly("postings_nb", &NindIndexVerify::Report::postingsNb)
        .def_readonly("documents_nb", &NindIndexVerify::Report::documentsNb)
        .def_readonly("with_local_index", &NindIndexVerify::Report::withLocalIndex)
        .def_readonly("problems_nb", &NindIndexVerify::Report::problemsNb)
        .def_readonly("problems", &NindIndexVerify::Report::problems)
        .def_property_readonly("ok", &NindIndexVerify::Report::ok)
        .def("__bool__", &NindIndexVerify::Report::ok)
        .def("__repr__", [](const NindIndexVerify::Report &r) {
            return "<VerifyReport " + std::string(r.ok() ? "ok" : std::to_string(r.problemsNb) + " problems")
                + ", format v" + (r.formatV2 ? "2" : "1") + ", " + std::to_string(r.wordsNb) + " words, "
                + std::to_string(r.termsNb) + " terms, " + std::to_string(r.documentsNb) + " documents>";
        });
    // Checks an index (see NindIndexVerify.h): never throws on a damaged index, reports it.
    // Releases the GIL.
    m.def("verify_index", [](const std::string &base, const bool crossCheck, const size_t maxProblems) {
        py::gil_scoped_release release;
        return NindIndexVerify::verify(base, crossCheck, maxProblems);
    }, py::arg("base"), py::arg("cross_check") = false, py::arg("max_problems") = 100);

    // ---- NindRetrolexicon -----------------------------------------------
    // ---- NindTokenizer ----------------------------------------------------
    py::class_<NindTokenizer::Options>(m, "TokenizerOptions")
        .def(py::init([](bool splitCamelCase, bool splitUnderscore, bool splitDigits, bool lowercase,
                         unsigned int minLength, unsigned int maxLength) {
            NindTokenizer::Options options;
            options.splitCamelCase = splitCamelCase;
            options.splitUnderscore = splitUnderscore;
            options.splitDigits = splitDigits;
            options.lowercase = lowercase;
            options.minLength = minLength;
            options.maxLength = maxLength;
            return options;
        }), py::arg("split_camel_case") = true, py::arg("split_underscore") = true,
            py::arg("split_digits") = true, py::arg("lowercase") = false,
            py::arg("min_length") = 1, py::arg("max_length") = 254)
        .def_readwrite("split_camel_case", &NindTokenizer::Options::splitCamelCase)
        .def_readwrite("split_underscore", &NindTokenizer::Options::splitUnderscore)
        .def_readwrite("split_digits", &NindTokenizer::Options::splitDigits)
        .def_readwrite("lowercase", &NindTokenizer::Options::lowercase)
        .def_readwrite("min_length", &NindTokenizer::Options::minLength)
        .def_readwrite("max_length", &NindTokenizer::Options::maxLength);

    py::class_<NindTokenizer>(m, "Tokenizer")
        .def(py::init<const NindTokenizer::Options &>(), py::arg("options") = NindTokenizer::Options())
        .def("get_options", &NindTokenizer::getOptions)
        .def("tokenize", [](const NindTokenizer &self, const std::string &text) {
            std::vector<std::string> tokens;
            self.tokenize(text, tokens);
            return tokens;
        }, py::arg("text"))
        // Tokenize many documents at once, for IndexBuilder: returns (vocabulary,
        // term_ids, ends) where vocabulary lists the distinct tokens (in order of first
        // occurrence), term_ids (uint32 bytes) is every token as its index in vocabulary,
        // and document i is term_ids[ends[i-1]:ends[i]] (ends: uint32 bytes).
        .def("tokenize_batch", [](const NindTokenizer &self, const std::vector<std::string> &texts) {
            std::vector<std::string> vocabulary;
            std::vector<uint32_t> termIds, ends;
            {
                py::gil_scoped_release release;
                std::unordered_map<std::string, uint32_t> localIds;
                std::vector<std::pair<size_t, size_t> > spans;
                std::string token;
                for (size_t d = 0; d < texts.size(); d++) {
                    self.tokenize(texts[d].data(), texts[d].size(), spans);
                    for (size_t i = 0; i < spans.size(); i++) {
                        self.getToken(texts[d].data(), spans[i], token);
                        std::unordered_map<std::string, uint32_t>::const_iterator it = localIds.find(token);
                        if (it == localIds.end()) {
                            it = localIds.insert(std::make_pair(token, (uint32_t)vocabulary.size())).first;
                            vocabulary.push_back(token);
                        }
                        termIds.push_back((*it).second);
                    }
                    if (termIds.size() > std::numeric_limits<uint32_t>::max())
                        throw py::value_error("tokenize_batch: too many tokens in one batch");
                    ends.push_back((uint32_t)termIds.size());
                }
            }
            return py::make_tuple(vocabulary, uintBytes(termIds), uintBytes(ends));
        }, py::arg("texts"));

    // ---- IndexBuilder -----------------------------------------------------
    // Accumulates a corpus as term ids, then writes its term index and local index:
    // add_documents() chunk by chunk in increasing document id order, then
    // write_term_index() and write_local_index() (both release the GIL and only read
    // the builder: they can run concurrently, each on its own writer).
    py::class_<NindIndexBuilder>(m, "IndexBuilder")
        .def(py::init<>())
        .def("add_documents", [](NindIndexBuilder &self, uint64_t firstDocId, const py::buffer &ends,
                                 const py::buffer &termIds, const py::buffer &idMap) {
            const UIntView documentEnds = uintView(ends, "ends");
            const UIntView localIds = uintView(termIds, "term_ids");
            const UIntView map = uintView(idMap, "id_map");
            py::gil_scoped_release release;
            self.addDocuments(firstDocId, documentEnds.data, documentEnds.size, localIds.data, localIds.size,
                              map.data, map.size);
        }, py::arg("first_doc_id"), py::arg("ends"), py::arg("term_ids"), py::arg("id_map"))
        .def("write_term_index", &NindIndexBuilder::writeTermIndex,
             py::arg("term_index"), py::arg("file_identification"), py::arg("cg") = 0,
             py::call_guard<py::gil_scoped_release>())
        .def("write_local_index", &NindIndexBuilder::writeLocalIndex,
             py::arg("local_index"), py::arg("file_identification"), py::arg("cg") = 0,
             py::call_guard<py::gil_scoped_release>())
        // (document ids as uint64 bytes, lengths in tokens as uint32 bytes)
        .def("doc_lengths", [](const NindIndexBuilder &self) {
            std::vector<NindDocId> docIds;
            std::vector<uint32_t> lengths;
            self.docLengths(docIds, lengths);
            return py::make_tuple(uint64Bytes(std::vector<uint64_t>(docIds.begin(), docIds.end())), uintBytes(lengths));
        })
        .def_property_readonly("docs_nb", &NindIndexBuilder::docsNb)
        .def_property_readonly("tokens_nb", &NindIndexBuilder::tokensNb)
        .def_property_readonly("max_term_id", &NindIndexBuilder::maxTermId);

    // ---- NindDocNames -----------------------------------------------------
    // The names of an index's documents (their identifiers in the corpus), both ways.
    py::class_<NindDocNames>(m, "NindDocNames")
        .def(py::init<const std::string &>(), py::arg("file_name"))
        // writes a new file (never overwrites one); ValueError on a repeated ident or name
        .def_static("write", [](const std::string &fileName, const py::buffer &docIds,
                                const std::vector<std::string> &names) {
            const DocIdsView ids = docIdsView(docIds, "doc_ids");
            py::gil_scoped_release release;
            NindDocNames::write(fileName, ids.values, names);
        }, py::arg("file_name"), py::arg("doc_ids"), py::arg("names"))
        // the shards' files into a new file, without the names in Python; ValueError on documents
        // overlapping between shards or a name in several of them
        .def_static("merge", [](const std::vector<std::string> &shardFiles, const std::string &fileName) {
            py::gil_scoped_release release;
            NindDocNames::merge(shardFiles, fileName);
        }, py::arg("shard_files"), py::arg("file_name"))
        .def("name", [](const NindDocNames &self, uint64_t docId) -> py::object {
            std::string name;
            if (!self.getName(docId, name)) return py::none();
            return py::str(name);
        }, py::arg("doc_id"))
        // the names of many documents (None for a document without name)
        .def("names", [](const NindDocNames &self, const std::vector<uint64_t> &docIds) {
            py::list result;
            std::string name;
            for (size_t i = 0; i < docIds.size(); i++) {
                if (self.getName(docIds[i], name)) result.append(py::str(name));
                else result.append(py::none());
            }
            return result;
        }, py::arg("doc_ids"))
        .def("doc_id", [](const NindDocNames &self, const std::string &name) -> py::object {
            NindDocId docId = 0;
            if (!self.getDocIdent(name, docId)) return py::none();
            return py::int_(docId);
        }, py::arg("name"))
        // every (document, name): (doc ids as uint64 bytes, names), in increasing doc id order
        .def("entries", [](const NindDocNames &self) {
            std::vector<NindDocId> docIds;
            std::vector<std::string> names;
            self.getEntries(docIds, names);
            return py::make_tuple(uint64Bytes(docIds), names);
        })
        .def("__len__", &NindDocNames::size);

    // ---- NindSegmentedIndex -----------------------------------------------
    // An index made of immutable segments, searched as one (see NindSegmentedIndex.h).
    py::class_<NindSegmentedIndex::SegmentInfo>(m, "SegmentInfo")
        .def_readonly("name", &NindSegmentedIndex::SegmentInfo::name)
        .def_readonly("base", &NindSegmentedIndex::SegmentInfo::base)
        .def_readonly("docs_nb", &NindSegmentedIndex::SegmentInfo::docsNb)
        .def_readonly("deleted_nb", &NindSegmentedIndex::SegmentInfo::deletedNb);
    py::class_<NindSegmentedIndex>(m, "SegmentedIndex")
        .def(py::init([](const std::string &base, const NindSegmentedIndex *previous) {
            py::gil_scoped_release release;
            return new NindSegmentedIndex(base, previous);
        }), py::arg("base"), py::arg("previous") = nullptr)
        .def("search", [](const NindSegmentedIndex &self, const std::vector<std::string> &words, double k1, double b,
                          long topK, bool prune, unsigned int threads) {
            std::vector<std::list<std::string> > query;
            for (size_t w = 0; w < words.size(); w++) query.push_back(std::list<std::string>(1, words[w]));
            py::gil_scoped_release release;
            return self.search(query, k1, b, topK, prune, threads);
        }, py::arg("words"), py::arg("k1") = 1.5, py::arg("b") = 0.75, py::arg("top_k") = -1,
           py::arg("prune") = false, py::arg("threads") = 1)
        .def("document_frequency", [](const NindSegmentedIndex &self, const std::string &word) {
            const std::list<std::string> components(1, word);
            py::gil_scoped_release release;
            return self.documentFrequency(components);
        }, py::arg("word"))
        .def("term_frequency", [](const NindSegmentedIndex &self, const std::string &word, uint64_t docId) {
            const std::list<std::string> components(1, word);
            py::gil_scoped_release release;
            return self.termFrequency(components, docId);
        }, py::arg("word"), py::arg("doc_id"))
        .def("document_length", &NindSegmentedIndex::documentLength, py::arg("doc_id"))
        .def("contains", &NindSegmentedIndex::contains, py::arg("doc_id"))
        .def("name", [](const NindSegmentedIndex &self, uint64_t docId) -> py::object {
            std::string name;
            if (!self.getName(docId, name)) return py::none();
            return py::str(name);
        }, py::arg("doc_id"))
        .def("doc_id", [](const NindSegmentedIndex &self, const std::string &name) -> py::object {
            NindDocId docId = 0;
            if (!self.getDocIdent(name, docId)) return py::none();
            return py::int_((uint64_t)docId);
        }, py::arg("name"))
        .def("has_names", &NindSegmentedIndex::hasNames)
        .def("max_doc_id", [](const NindSegmentedIndex &self) -> py::object {
            NindDocId docId = 0;
            if (!self.getMaxDocIdent(docId)) return py::none();
            return py::int_((uint64_t)docId);
        })
        // (document ids as uint64 bytes, lengths as uint32 bytes), by increasing document
        .def("documents", [](const NindSegmentedIndex &self) {
            std::vector<NindDocId> docIds;
            std::vector<uint32_t> lengths;
            self.getDocuments(docIds, lengths);
            return py::make_tuple(uint64Bytes(std::vector<uint64_t>(docIds.begin(), docIds.end())), uintBytes(lengths));
        })
        .def_property_readonly("docs_nb", &NindSegmentedIndex::docsNb)
        .def_property_readonly("avg_doc_len", &NindSegmentedIndex::avgDocLength)
        .def("segments", &NindSegmentedIndex::segments)
        .def_static("read_manifest", &NindSegmentedIndex::readManifest, py::arg("base"))
        .def_static("write_manifest", &NindSegmentedIndex::writeManifest, py::arg("base"), py::arg("segments"))
        .def_static("segment_base", &NindSegmentedIndex::segmentBase, py::arg("base"), py::arg("segment"));

    py::class_<NindSegmentedUpdate::Stats>(m, "SegmentedUpdateStats")
        .def_readonly("added_docs_nb", &NindSegmentedUpdate::Stats::addedDocsNb)
        .def_readonly("removed_docs_nb", &NindSegmentedUpdate::Stats::removedDocsNb)
        .def_readonly("segments_nb", &NindSegmentedUpdate::Stats::segmentsNb)
        .def_readonly("merges_nb", &NindSegmentedUpdate::Stats::mergesNb)
        .def_readonly("merged_docs_nb", &NindSegmentedUpdate::Stats::mergedDocsNb);
    py::class_<NindSegmentedUpdate>(m, "SegmentedUpdate")
        .def(py::init([](const std::string &base, bool withLocalIndex, unsigned int mergeFactor) {
            NindSegmentedUpdate::Options options;
            options.withLocalIndex = withLocalIndex;
            options.mergeFactor = mergeFactor;
            return new NindSegmentedUpdate(base, options);
        }), py::arg("base"), py::arg("with_local_index") = true, py::arg("merge_factor") = 10)
        .def("add_document", &NindSegmentedUpdate::addDocument, py::arg("doc_id"), py::arg("words"), py::arg("name") = std::string())
        .def("remove_document", &NindSegmentedUpdate::removeDocument, py::arg("doc_id"))
        .def("commit", &NindSegmentedUpdate::commit, py::call_guard<py::gil_scoped_release>())
        .def_static("force_merge", &NindSegmentedUpdate::forceMerge, py::arg("base"), py::call_guard<py::gil_scoped_release>())
        .def_static("max_doc_id", [](const std::string &base) -> py::object {
            NindDocId docId = 0;
            if (!NindSegmentedUpdate::getMaxDocIdent(base, docId)) return py::none();
            return py::int_((uint64_t)docId);
        }, py::arg("base"));

    // ---- Bm25Scorer -------------------------------------------------------
    // Okapi BM25 search driven by the postings of the query terms (only the documents
    // containing them are scored). Built once per index from its documents' lengths;
    // search() reads the term index on its thread-safe path (NindFile::readAt): several threads
    // can search one reader at once, each with its own scratch arrays (one pool per scorer)
    // with the GIL released.
    py::class_<NindBm25Scorer>(m, "Bm25Scorer")
        .def(py::init([](const py::buffer &docIds, const py::buffer &lengths, double avgDocLength) {
            const DocIdsView ids = docIdsView(docIds, "doc_ids");
            const UIntView lens = uintView(lengths, "lengths");
            return new NindBm25Scorer(std::vector<NindDocId>(ids.data, ids.data + ids.size),
                                      std::vector<uint32_t>(lens.data, lens.data + lens.size), avgDocLength);
        }), py::arg("doc_ids"), py::arg("lengths"), py::arg("avg_doc_len"))
        .def("search", [](const NindBm25Scorer &self, const NindTermIndex &termIndex,
                          const std::vector<unsigned int> &termIds, double k1, double b, long topK, bool prune,
                          unsigned int threads) {
            py::gil_scoped_release release;
            return self.search(termIndex, termIds, k1, b, topK, prune, threads);
        }, py::arg("term_index"), py::arg("term_ids"),
             py::arg("k1") = 1.5, py::arg("b") = 0.75, py::arg("top_k") = -1,
             py::arg("prune") = false, py::arg("threads") = 1)
        .def_property_readonly("docs_nb", &NindBm25Scorer::docsNb);

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
