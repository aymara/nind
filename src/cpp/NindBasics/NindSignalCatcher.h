//
// C++ Interface: NindSignalCatcher
//
// Description: Captation provisoire des control-C pour protejger d'iceux les sections
// critiques
//
// Author: jys <jy.sage@orange.fr>, (C) LATEJCON 2023
//
// Copyright: 2014-2023 LATEJCON. See LICENCE.md file that comes with this distribution
// This file is part of NIND (as "nouvelle indexation").
// NIND is free software: you can redistribute it and/or modify it under the terms of the 
// GNU Less General Public License (LGPL) as published by the Free Software Foundation, 
// (see <http://www.gnu.org/licenses/>), either version 3 of the License, or any later version.
// NIND is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without 
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Less General Public License for more details.
////////////////////////////////////////////////////////////
#ifndef NindSignalCatcher_H
#define NindSignalCatcher_H
////////////////////////////////////////////////////////////
#include "NindCommonExport.h"
#include <csignal>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
class DLLExportLexicon NindSignalCatcher {
public:
    /**\brief Return a pointer to the singleton class */
    static NindSignalCatcher* Instance();
    /**\brief Turn on the control-C catcher (calls may be nested) */
    void setCatcher();
    /**\brief Turn off the control-C catcher. When the outermost critical section ends, the
     * SIGINT handler that was in place before is restored and, if control-C was striked
     * meanwhile, SIGINT is raised again so that this handler (by default: terminate) sees it.
     * An unbalanced call (no critical section open) does nothing. */
    void resetCatcher();
    /**\brief True while a critical section is open (for tests) */
    static bool isUp();
protected:
    NindSignalCatcher();
private:
    //le gestionnaire n'est installej que pendant les sections critiques : hors de celles-ci,
    //le traitement de SIGINT du processus (p. ex. KeyboardInterrupt de Python) n'est pas modifiej
    static unsigned int m_depth;
    static volatile std::sig_atomic_t m_ctrlC;
    static void (*m_previousHandler)(int);
    static void attrapeCtrlC (int signum);
    static NindSignalCatcher* m_instance;
};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
////////////////////////////////////////////////////////////
