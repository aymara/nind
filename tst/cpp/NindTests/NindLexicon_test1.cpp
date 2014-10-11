//
// C++ Implementation: NindLexicon_test1
//
// Description: un test pour remplir le lexique et faire differentes mesures.
//
// Author: Jean-Yves Sage <jean-yves.sage@orange.fr>, (C) LATECON 2014
//
// Copyright: See COPYING file that comes with this distribution
//
////////////////////////////////////////////////////////////
#include "NindLexicon/NindLexiconA.h"
#include "NindLexicon/NindLexiconC.h"
#include "NindExceptions.h"
#include <time.h>
#include <string>
#include <list>
#include <iostream>
#include <fstream>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
static void displayHelp(char* arg0) {
    cout<<"Programme de test de NindLexicon."<<endl;
    cout<<"Teste les versions symétriques avec retro-lexiques."<<endl;
    cout<<"Charge un lexique vide avec le dump de documents spécifié."<<endl;
    cout<<"Un dump de documents est obtenu par AntindexDumpBaseByDocuments sur une base S2."<<endl;
    cout<<"Teste l'écriture puis la lecture et affiche résultats et mesures."<<endl;

    cout<<"usage: "<<arg0<<" --help"<< endl;
    cout<<"       "<<arg0<<" <fichier dump documents>"<<endl;
    cout<<"ex :   "<<arg0<<" fre-theJysBox.fdb-DumpByDocuments.txt"<<endl;
}
////////////////////////////////////////////////////////////
#define LINE_SIZE 65536*64
static void getWords(const string &dumpLine, list<string> &wordsList);
static void split(const string &word, list<string> &simpleWords);
////////////////////////////////////////////////////////////
int main(int argc, char *argv[]) {
    setlocale( LC_ALL, "French" );
    if (argc<2) {displayHelp(argv[0]); return false;}
    const string docsFileName = argv[1];
    if (docsFileName == "--help") {displayHelp(argv[0]); return true;}

    try {
        //pour calculer le temps consomme
        clock_t start, end;
        double cpuTimeUsed;

        /////////////////////////////////////
        cout<<"1) forme le lexique"<<endl;
        start = clock();
        //le lexique
        NindLexiconA nindLexicon;
        //nindLexicon.dump(std::cerr);
        //la correspondance de tous les mots avec leur identifiant
        list<pair<unsigned int, string> > allWords;
        //lit le fichier dump de documents
        unsigned int docsNb = 0;
        char charBuff[LINE_SIZE];
        ifstream docsFile(docsFileName.c_str(), ifstream::in);
        if (docsFile.fail()) throw OpenFileException(docsFileName);
        while (docsFile.good()) {
        //while (!docsFile.eof()) {
            list<string> wordsList;
            docsFile.getline(charBuff, LINE_SIZE);
            if (string(charBuff).empty()) continue;   //evacue ainsi les lignes vides
            docsNb++;
            //if (docsFile.fail()) throw FormatFileException(docsFileName);
            getWords(string(charBuff), wordsList);
            //for(list<string>::const_iterator it = wordsList.begin(); it != wordsList.end(); it++) cerr<<(*it)<<endl;
            //ajoute tous les mots à la suite et dans l'ordre
            for (list<string>::const_iterator wordIt = wordsList.begin(); wordIt != wordsList.end(); wordIt++) {
                list<string> componants;
                split(*wordIt, componants);
                const unsigned int id = nindLexicon.addWord(componants);
                allWords.push_back(pair<unsigned int, string>(id, *wordIt));
            }
        }
        cerr<<"docsFile.good()="<<docsFile.good()<<endl;
        cerr<<"docsFile.fail()="<<docsFile.fail()<<endl;
        cerr<<"docsFile.eof()="<<docsFile.eof()<<endl;
        cerr<<"docsNb="<<docsNb<<endl;
        docsFile.close();
        end = clock();
        cpuTimeUsed = ((double) (end - start)) / CLOCKS_PER_SEC;
        //affiche les données de l'indexation
        cout<<allWords.size()<<" mots de "<<docsNb<<" documents soumis à l'indexation en ";
        cout<<cpuTimeUsed<<" secondes"<<endl;
        //nindLexicon.dump(std::cerr);
        
        /////////////////////////////////////
        cout<<"2) vérifie l'intégrité"<<endl;
        start = clock();
        struct NindLexiconA::LexiconSizes lexiconSizes;
        const bool isOk = nindLexicon.integrityAndCounts(lexiconSizes);
        end = clock();
        cpuTimeUsed = ((double) (end - start)) / CLOCKS_PER_SEC;
        //integrite
        if (isOk) cout<<"lexique OK ";
        else cout<<"lexique NOK ";
        //nombres de mots
        cout<<lexiconSizes.swNb<<" / "<<lexiconSizes.rswNb<<" mots simples, ";
        cout<<lexiconSizes.cwNb<<" / "<<lexiconSizes.rcwNb<<" mots composés"<<endl;
        //nombres d'accès
        cout<<lexiconSizes.successCount<<" accès réussis, "<<lexiconSizes.failCount<<" accès échoués en ";
        cout<<cpuTimeUsed<<" secondes"<<endl;

        /////////////////////////////////////
        cout<<"3) redemande les "<<allWords.size()<<" mots soumis et vérifie leur identifiant"<<endl;
        start = clock();
        for (list<pair<unsigned int, string> >::const_iterator wordIt = allWords.begin(); wordIt != allWords.end(); wordIt++) {
            list<string> componants;
            split(wordIt->second, componants);
            const unsigned int id = nindLexicon.getId(componants);
            if (id != wordIt->first) throw IntegrityException(wordIt->second);
        }
        end = clock();
        cpuTimeUsed = ((double) (end - start)) / CLOCKS_PER_SEC;
        cout<<"OK en "<<cpuTimeUsed<<" secondes"<<endl;

        /////////////////////////////////////
        cout<<"4) redemande les "<<allWords.size()<<" identifiants et vérifie que ça correspond au mot soumis"<<endl;
        start = clock();
        for (list<pair<unsigned int, string> >::const_iterator wordIt = allWords.begin(); wordIt != allWords.end(); wordIt++) {
            list<string> componants;
            nindLexicon.getWord(wordIt->first, componants);
            list<string> origComponants;
            split(wordIt->second, origComponants);
            if (componants != origComponants) throw IntegrityException(wordIt->second);
        }
        end = clock();
        cpuTimeUsed = ((double) (end - start)) / CLOCKS_PER_SEC;
        cout<<"OK en "<<cpuTimeUsed<<" secondes"<<endl;
        return true;
    }
    catch (LexiconException &exc) {cerr<<"EXCEPTION :"<<exc.m_word<<" "<<exc.what()<<endl; return false;}
    catch (FileException &exc) {cerr<<"EXCEPTION :"<<exc.m_fileName<<" "<<exc.what()<<endl; return false;}
    catch (exception &exc) {cerr<<"EXCEPTION :"<<exc.what()<< endl; return false;}
    catch (...) {cerr<<"EXCEPTION unknown"<< endl; return false; }
}
////////////////////////////////////////////////////////////
static void getWords(const string &dumpLine, list<string> &wordsList)
{
    //cerr<<dumpLine<<endl;
    wordsList.clear();
    //1111005 <=> 1 len=12  ::  famille (NC), famille#heureux (NC), heureux (ADJ), se_ressembler (V), ..., façon (NC)
    if (dumpLine.empty()) return;   //evacue ainsi les lignes vides
    size_t posDeb = dumpLine.find("::  ", 0);
    if (posDeb == string::npos) throw FormatFileException();
    posDeb += 4;
    while (true) {
        size_t posSep = dumpLine.find(' ', posDeb);
        wordsList.push_back(dumpLine.substr(posDeb, posSep-posDeb));
        posDeb = posSep + 1;
        posSep = dumpLine.find("), ", posDeb);
        if (posSep == string::npos) break;
        posDeb = posSep + 3;
    }
}
////////////////////////////////////////////////////////////
//decoupe le mot sur les '#' et retourne une liste ordonnee de mots simples
static void split(const string &word, list<string> &simpleWords)
{
    simpleWords.clear();
    size_t posDeb = 0;
    while (true) {
        const size_t posSep = word.find('#', posDeb);
        simpleWords.push_back(word.substr(posDeb, posSep-posDeb));
        if (posSep == string::npos) break;
        posDeb = posSep + 1;
    }
}
////////////////////////////////////////////////////////////


