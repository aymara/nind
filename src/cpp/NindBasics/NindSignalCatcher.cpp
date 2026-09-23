//
// C++ Implementation: NindSignalCatcher
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
#include "NindSignalCatcher.h"
#include <stdlib.h>
////////////////////////////////////////////////////////////
using namespace latecon::nindex;
////////////////////////////////////////////////////////////
unsigned int NindSignalCatcher::m_depth = 0;
volatile std::sig_atomic_t NindSignalCatcher::m_ctrlC = 0;
void (*NindSignalCatcher::m_previousHandler)(int) = SIG_DFL;
NindSignalCatcher* NindSignalCatcher::m_instance = 0;
////////////////////////////////////////////////////////////
NindSignalCatcher::NindSignalCatcher() {
}
// Return a pointer to the singleton class
NindSignalCatcher* NindSignalCatcher::Instance() {
    if (m_instance == 0) m_instance = new NindSignalCatcher;
    return m_instance;
}
// Turn on the control-C catcher
void NindSignalCatcher::setCatcher() {
    if (m_depth++ != 0) return;
    m_ctrlC = 0;
    m_previousHandler = signal(SIGINT, attrapeCtrlC);
    if (m_previousHandler == SIG_ERR) m_previousHandler = SIG_DFL;
}
// Turn off the control-C catcher, re-raise control-C if striked meanwhile
void NindSignalCatcher::resetCatcher() {
    if (m_depth == 0) return;
    if (--m_depth != 0) return;
    signal(SIGINT, m_previousHandler);
    if (m_ctrlC) {
        m_ctrlC = 0;
        raise(SIGINT);
    }
}
bool NindSignalCatcher::isUp() {
    return m_depth != 0;
}
// only async-signal-safe operations here
void NindSignalCatcher::attrapeCtrlC (int) {
    m_ctrlC = 1;
}
////////////////////////////////////////////////////////////
