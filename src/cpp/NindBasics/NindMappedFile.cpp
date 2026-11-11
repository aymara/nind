//
// C++ Implementation: NindMappedFile (see NindMappedFile.h)
//
// This file is part of NIND (as "nouvelle indexation").
// NIND is free software: you can redistribute it and/or modify it under the terms of the
// GNU Lesser General Public License (see <http://www.gnu.org/licenses/>), either version 3 of
// the License, or any later version.
////////////////////////////////////////////////////////////
#include "NindMappedFile.h"
#include <cstdint>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
#ifndef _WIN32
NindMappedFile::NindMappedFile(const string &fileName):
    m_data(0),
    m_size(0)
{
    const int fd = open(fileName.c_str(), O_RDONLY);
    if (fd < 0) throw OpenFileException(fileName);
    struct stat fileStat;
    if (fstat(fd, &fileStat) != 0 || fileStat.st_size < 0 || (uint64_t)fileStat.st_size > SIZE_MAX) {
        close(fd);
        throw ReadFileException(fileName);
    }
    m_size = (size_t)fileStat.st_size;
    if (m_size != 0) {
        void *mapped = mmap(0, m_size, PROT_READ, MAP_SHARED, fd, 0);
        if (mapped == MAP_FAILED) {
            close(fd);
            throw ReadFileException(fileName);
        }
        m_data = (const unsigned char *)mapped;
    }
    close(fd);          //the mapping keeps the file
}
////////////////////////////////////////////////////////////
NindMappedFile::~NindMappedFile()
{
    if (m_data != 0) munmap((void *)m_data, m_size);
}
#else
////////////////////////////////////////////////////////////
NindMappedFile::NindMappedFile(const string &fileName):
    m_data(0),
    m_size(0),
    m_mapping(0)
{
    HANDLE file = CreateFileA(fileName.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) throw OpenFileException(fileName);
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || (uint64_t)size.QuadPart > SIZE_MAX) {
        CloseHandle(file);
        throw ReadFileException(fileName);
    }
    m_size = (size_t)size.QuadPart;
    if (m_size != 0) {
        HANDLE mapping = CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
        if (mapping == NULL) {
            CloseHandle(file);
            throw ReadFileException(fileName);
        }
        const void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        if (view == NULL) {
            CloseHandle(mapping);
            CloseHandle(file);
            throw ReadFileException(fileName);
        }
        m_mapping = mapping;
        m_data = (const unsigned char *)view;
    }
    CloseHandle(file);  //the mapping keeps the file
}
////////////////////////////////////////////////////////////
NindMappedFile::~NindMappedFile()
{
    if (m_data != 0) UnmapViewOfFile(m_data);
    if (m_mapping != 0) CloseHandle((HANDLE)m_mapping);
}
#endif
////////////////////////////////////////////////////////////
