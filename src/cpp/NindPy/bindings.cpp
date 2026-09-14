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
        .def("get_file_name", &NindPadFile::getFileName);

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
        .def("get_file_name", &NindPadFile::getFileName);

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
        .def("get_doc_count", &NindLocalIndex::getDocCount)
        .def("get_file_name", &NindPadFile::getFileName);

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
        .def("get_file_name", &NindRetrolexicon::getFileName);

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
