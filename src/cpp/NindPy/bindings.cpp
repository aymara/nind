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
#include <utility>
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
             py::arg("ident"), py::arg("term_def"), py::arg("file_identification"), py::arg("specifics"),
             py::call_guard<py::gil_scoped_release>())
        // Bulk writer taking plain ints instead of Document/TermCG objects (building
        // millions of those in Python costs far more than the write itself): one
        // single-TermCG definition per term, frequency = sum of its postings.
        // The GIL is released for the whole loop, so another thread can keep working.
        .def("set_postings", [](NindTermIndex &self,
                                const std::vector<std::pair<unsigned int, std::vector<std::pair<unsigned int, unsigned int> > > > &termPostings,
                                const NindPadFile::Identification &fileIdentification,
                                const std::list<unsigned int> &specifics,
                                const unsigned char cg) {
            py::gil_scoped_release release;
            for (size_t i = 0; i < termPostings.size(); i++) {
                const std::vector<std::pair<unsigned int, unsigned int> > &postings = termPostings[i].second;
                std::list<NindTermIndex::TermCG> termDef;
                termDef.push_back(NindTermIndex::TermCG(cg, 0));
                NindTermIndex::TermCG &termCG = termDef.back();
                for (size_t j = 0; j < postings.size(); j++) {
                    termCG.documents.push_back(NindTermIndex::Document(postings[j].first, postings[j].second));
                    termCG.frequency += postings[j].second;
                }
                self.setTermDef(termPostings[i].first, termDef, fileIdentification, specifics);
            }
        }, py::arg("term_postings"), py::arg("file_identification"), py::arg("specifics"), py::arg("cg") = 0)
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
             py::arg("ident"), py::arg("local_def"), py::arg("file_identification"),
             py::call_guard<py::gil_scoped_release>())
        // Writer taking a document as its term ids in token order (position = token
        // index, length = 1) instead of Term/Localisation objects, grouped per term in
        // increasing term id order. <nbreLocalisations> is one byte, so a term occurring
        // more than 255 times is written as several consecutive entries for the same id
        // (readers sum them). The GIL is released while building and writing.
        .def("set_local_def_from_term_ids", [](NindLocalIndex &self,
                                               const unsigned int ident,
                                               const std::vector<unsigned int> &termIds,
                                               const NindPadFile::Identification &fileIdentification,
                                               const unsigned char cg) {
            py::gil_scoped_release release;
            std::map<unsigned int, std::vector<unsigned int> > positionsById;
            for (size_t position = 0; position < termIds.size(); position++)
                positionsById[termIds[position]].push_back((unsigned int)position);
            std::list<NindLocalIndex::Term> localDef;
            for (std::map<unsigned int, std::vector<unsigned int> >::const_iterator it = positionsById.begin();
                 it != positionsById.end(); it++) {
                const std::vector<unsigned int> &positions = it->second;
                for (size_t i = 0; i < positions.size(); i++) {
                    if (i % 255 == 0) localDef.push_back(NindLocalIndex::Term(it->first, cg));
                    localDef.back().localisation.push_back(NindLocalIndex::Localisation(positions[i], 1));
                }
            }
            self.setLocalDef(ident, localDef, fileIdentification);
        }, py::arg("ident"), py::arg("term_ids"), py::arg("file_identification"), py::arg("cg") = 0)
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
