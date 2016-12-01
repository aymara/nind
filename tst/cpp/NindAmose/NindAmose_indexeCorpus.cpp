//
// C++ Implementation: NindAmose_indexeCorpus
//
// Description: un programme pour remplir le lexique, le fichier inverse et le fichier des index locaux
// a partir d'un corpus deja syntaxiquement analyse issu d'un dump Lucene.
//
// Author: jys <jy.sage@orange.fr>, (C) LATEJCON 2016
//
// Copyright: 2014-2016 LATEJCON. See LICENCE.md file that comes with this distribution
// This file is part of NIND (as "nouvelle indexation").
// NIND is free software: you can redistribute it and/or modify it under the terms of the 
// GNU Less General Public License (LGPL) as published by the Free Software Foundation, 
// (see <http://www.gnu.org/licenses/>), either version 3 of the License, or any later version.
// NIND is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without 
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Less General Public License for more details.
////////////////////////////////////////////////////////////
#include "NindAmose/NindTermAmose.h"
#include "NindAmose/NindLocalAmose.h"
#include "NindAmose/NindLexiconAmose.h"
#include "NindExceptions.h"
#include <time.h>
#include <string>
#include <list>
#include <iostream>
#include <iomanip> 
#include <fstream>
#include <sstream>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
static void displayHelp(char* arg0) {
    cout<<"© l'ATEJCON"<<endl;
    cout<<"Programme d'indexation d'un corpus déjà syntaxiquement analysé issu d'un"<<endl;
    cout<<"dump Lucene d'un corpus Amose."<<endl;
    cout<<"Le corpus est un fichier texte avec une ligne par document :"<<endl;
    cout<<"<n° document>  { <terme> <localisation>,<taille> }"<<endl;
    cout<<"Le lexique et les fichiers inverse et d'index locaux sont créés."<<endl;
    cout<<"Les fichiers lexique, inverse et d'index locaux doivent être absents."<<endl;
    cout<<"Les documents sont indexés au fur et à mesure de leur lecture."<<endl;
    cout<<"Le nombre d'entrées des blocs d'indirection est spécifiée pour le lexique,"<<endl;
    cout<<"le fichier inversé et le fichier des index locaux."<<endl;

    cout<<"usage: "<<arg0<<" --help"<< endl;
    cout<<"       "<<arg0<<" <dump documents> <taille lexique> <taille inverse> <taille locaux>"<<endl;
    cout<<"ex :   "<<arg0<<" sample_fre.xml.mult.xml.txt 100003 100000 5000"<<endl;
}
////////////////////////////////////////////////////////////
static void majInverse (const unsigned int id,
                        const unsigned int noDoc,
                        list<NindTermIndex::TermCG> &termIndex);
////////////////////////////////////////////////////////////
#define NO_CG 0
////////////////////////////////////////////////////////////
int main(int argc, char *argv[]) {
    setlocale( LC_ALL, "French" );
    if (argc<4) {displayHelp(argv[0]); return false;}
    const string docsFileName = argv[1];
    if (docsFileName == "--help") {displayHelp(argv[0]); return true;}
    const string lexiconEntryNbStr = argv[2];
    const string termindexEntryNbStr = argv[3];
    const string localindexEntryNbStr = argv[4];
    
    const unsigned int lexiconEntryNb = atoi(lexiconEntryNbStr.c_str());
    const unsigned int termindexEntryNb = atoi(termindexEntryNbStr.c_str());
    const unsigned int localindexEntryNb = atoi(localindexEntryNbStr.c_str());

    try {
        //calcule les noms des fichiers lexique et inverse et index locaux
        const string incompleteFileName = docsFileName.substr(0, docsFileName.find('.'));
        const string lexiconFileName = incompleteFileName + ".lexiconindex";
        const string retrolexiconFileName = incompleteFileName + ".retrolexiconindex";
        const string termindexFileName = incompleteFileName + ".termindex";
        const string localindexFileName = incompleteFileName + ".localindex";
        //pour calculer le temps consomme
        clock_t start, end;
        double cpuTimeUsed;

        /////////////////////////////////////
        bool noOldFiles = true;
        FILE *file =  fopen(lexiconFileName.c_str(), "rb");
        if (file) {
            fclose(file);
            noOldFiles = false;
        }
        file =  fopen(retrolexiconFileName.c_str(), "rb");
        if (file) {
            fclose(file);
            noOldFiles = false;
        }
        file =  fopen(termindexFileName.c_str(), "rb");
        if (file) {
            fclose(file);
            noOldFiles = false;
        }
        file =  fopen(localindexFileName.c_str(), "rb");
        if (file) {
            fclose(file);
            noOldFiles = false;
        }
        if (!noOldFiles) {
            cout<<"Des anciens fichiers lexiques existent !"<<endl;
            cout<<"Veuillez les effacer par la commande : rm "<<incompleteFileName + ".*index"<<endl;
            return false;
        }
        /////////////////////////////////////
        cout<<"Forme le lexique, le fichier inversé et le fichier des index locaux avec "<<docsFileName<<endl;
        start = clock();
        //le lexique ecrivain avec retro lexique (meme taille d'indirection que le fichier inverse)
        NindLexiconAmose nindLexicon(lexiconFileName, true, lexiconEntryNb, termindexEntryNb);
        NindIndex::Identification identification;
        nindLexicon.getIdentification(identification);
        //le fichier inverse ecrivain
        NindTermAmose nindTermAmose(termindexFileName, true, identification, termindexEntryNb);
        //le fichier des index locaux
        NindLocalAmose nindLocalAmose(localindexFileName, true, identification, localindexEntryNb);
        //lit le fichier dump de documents
        unsigned int docsNb = 0;
        unsigned int nbMaj = 0;
        string dumpLine;
        ifstream docsFile(docsFileName.c_str(), ifstream::in);
        if (docsFile.fail()) throw OpenFileException(docsFileName);
        while (getline(docsFile, dumpLine)) {
            //lit 1 ligne = 1 document
            if (docsFile.fail()) throw FormatFileException(docsFileName);
            if (dumpLine.empty()) continue;   //evacue ainsi les lignes vides
            stringstream sdumpLine(dumpLine);
            //10170346  Location.LOCATION:Italie 280,6 création 288,8 création_parti 288,19
            docsNb++;
            unsigned int noDoc;
            string word;
            unsigned int position, taille;
            char comma;
            sdumpLine >> noDoc;
            //noDoc -= 10170000;
            //la structure d'index locaux se fabrique pour un document complet
            list<NindLocalIndex::Term> localIndex;
            //lit tous les termes et leur localisation/taille
            //le python de construction atteste de la validite du format, pas la peine de controler
            while (sdumpLine >> word >> position >> comma >> taille) {
                //cout<<"#"<<word<<"#"<<pos<<"#"<<taille<<endl;
                //le terme
                string lemma;
                unsigned int type;
                string entitejNommeje;
                //si c'est une entitej nommeje, la sejpare en 2
                const size_t pos = word.find(':');
                if (pos != string::npos) {
                    entitejNommeje = word.substr(0, pos);
                    lemma = word.substr(pos);
                    type = NAMED_ENTITY;
                }
                else if (word.find('_') != string::npos) {
                    lemma = word;
                    type = MULTI_TERM;
                }
                else {
                    lemma = word;
                    type = SIMPLE_TERM;
                }
                //recupere l'id du terme dans le lexique, l'ajoute eventuellement
                const unsigned int id = nindLexicon.addTerm(lemma, type, entitejNommeje);
//                 //recupere l'index inverse pour ce terme
//                 list<NindTermIndex::TermCG> termIndex;
//                 //met a jour la definition du terme
//                 nindTermAmose->getTermIndex(id, termIndex);
//                 //si le terme n'existe pas encore, la liste reste vide
//                 majInverse(id, noDoc, termIndex); 
                //recupere l'identification du lexique
                nindLexicon.getIdentification(identification);
                //ecrit sur le fichier inverse
                list<NindTermIndex::Document> newDocuments;
                newDocuments.push_back(NindTermIndex::Document(noDoc, 1));
                nindTermAmose.addDocsToTerm(id, type, newDocuments, identification);
                nbMaj +=1;
                //augmente l'index local 
                localIndex.push_back(NindLocalIndex::Term(id, NO_CG));
                NindLocalIndex::Term &term = localIndex.back();
                term.localisation.push_back(NindLocalIndex::Localisation(position, taille));               
            }
            //ecrit la definition sur le fichier des index locaux
            nindLocalAmose.setLocalIndex(noDoc, localIndex, identification);
        }
        docsFile.close();
        end = clock();
        cout<<nbMaj<<" accès / mises à jour sur "<<lexiconFileName<<endl;
        cout<<nbMaj<<" mises à jour sur "<<termindexFileName<<endl;
        cout<<docsNb<<" mises à jour sur "<<localindexFileName<<endl;
        cpuTimeUsed = ((double) (end - start)) / CLOCKS_PER_SEC;
        cout<<cpuTimeUsed<<" secondes"<<endl;
        cout<<endl;
        cout<<setw(8)<<setfill(' ')<<nindTermAmose.getUniqueTermCount(SIMPLE_TERM)<<" SIMPLE_TERM uniques"<<endl;
        cout<<setw(8)<<setfill(' ')<<nindTermAmose.getUniqueTermCount(MULTI_TERM)<<" MULTI_TERM uniques"<<endl;
        cout<<setw(8)<<setfill(' ')<<nindTermAmose.getUniqueTermCount(NAMED_ENTITY)<<" NAMED_ENTITY uniques"<<endl;
        cout<<setw(8)<<setfill(' ')<<nindTermAmose.getTermOccurrences(SIMPLE_TERM)<<" occurrences de SIMPLE_TERM"<<endl;
        cout<<setw(8)<<setfill(' ')<<nindTermAmose.getTermOccurrences(MULTI_TERM)<<" occurrences de MULTI_TERM"<<endl;
        cout<<setw(8)<<setfill(' ')<<nindTermAmose.getTermOccurrences(NAMED_ENTITY)<<" occurrences de NAMED_ENTITY"<<endl;
        cout<<setw(8)<<setfill(' ')<<nindLocalAmose.getDocCount()<<" documents indexés"<<endl;
    }
    catch (FileException &exc) {cerr<<"EXCEPTION :"<<exc.m_fileName<<" "<<exc.what()<<endl; throw; return false;}
    catch (exception &exc) {cerr<<"EXCEPTION :"<<exc.what()<< endl; throw; return false;}
    catch (...) {cerr<<"EXCEPTION unknown"<< endl; throw; return false; }
}
////////////////////////////////////////////////////////////
