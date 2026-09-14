//
// TestTempDir: RAII helper for the GTest-based unit tests.
//
// Creates a fresh, empty, uniquely-named directory for a test to write nind
// files into, and recursively removes it (and everything the test wrote into
// it) when the test ends, so tests never leave files behind and never
// collide with each other or with a previous run.
//
// POSIX only (Linux/macOS): fine for these unit tests, which are only built
// when GoogleTest is found (see tst/cpp/CMakeLists.txt).
////////////////////////////////////////////////////////////
#ifndef NindTestTempDir_H
#define NindTestTempDir_H
////////////////////////////////////////////////////////////
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
////////////////////////////////////////////////////////////
class TestTempDir {
public:
    TestTempDir() {
        char buffer[] = "/tmp/nind_test_XXXXXX";
        const char *result = mkdtemp(buffer);
        m_path = result ? result : "";
    }

    ~TestTempDir() {
        if (!m_path.empty()) removeRecursively(m_path);
    }

    // Directory itself, with no trailing slash.
    const std::string &path() const { return m_path; }

    // path()/name : the "fileNameExtensionLess" pattern most nind classes
    // take as constructor argument (they append their own extension).
    std::string file(const std::string &name) const { return m_path + "/" + name; }

private:
    static void removeRecursively(const std::string &dir) {
        DIR *d = opendir(dir.c_str());
        if (!d) return;
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            const std::string full = dir + "/" + name;
            struct stat st;
            if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) removeRecursively(full);
            else unlink(full.c_str());
        }
        closedir(d);
        rmdir(dir.c_str());
    }

    std::string m_path;
};
////////////////////////////////////////////////////////////
#endif
