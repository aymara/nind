//
// TestFileBytes: raw byte access to files, for the unit tests that corrupt nind
// files on purpose (patch a field, flip a byte, truncate) to check how the
// readers behave on damaged or hostile input.
////////////////////////////////////////////////////////////
#ifndef NindTestFileBytes_H
#define NindTestFileBytes_H
////////////////////////////////////////////////////////////
#include <cstdio>
#include <string>
#include <vector>
////////////////////////////////////////////////////////////
inline std::vector<unsigned char> readFileBytes(const std::string &path) {
    std::vector<unsigned char> bytes;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return bytes;
    unsigned char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) bytes.insert(bytes.end(), buffer, buffer + n);
    fclose(f);
    return bytes;
}
////////////////////////////////////////////////////////////
inline void writeFileBytes(const std::string &path, const std::vector<unsigned char> &bytes) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    if (!bytes.empty()) fwrite(&bytes[0], 1, bytes.size(), f);
    fclose(f);
}
////////////////////////////////////////////////////////////
// little-endian 4-bytes integer at offset (the <Entier4> of the nind grammar)
inline void patchInt4(std::vector<unsigned char> &bytes, const size_t offset, const unsigned int value) {
    for (int i = 0; i < 4; i++) bytes[offset + i] = (value >> (8 * i)) & 0xFF;
}
// big-endian 5-bytes integer at offset (the <Entier5> of the nind grammar)
inline void patchInt5(std::vector<unsigned char> &bytes, const size_t offset, const unsigned long long value) {
    for (int i = 0; i < 5; i++) bytes[offset + i] = (value >> (8 * (4 - i))) & 0xFF;
}
////////////////////////////////////////////////////////////
#endif
