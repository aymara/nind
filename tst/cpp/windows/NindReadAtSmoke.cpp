// Windows smoke test of the thread-safe read path (NindFile::readAt via ReadFile + OVERLAPPED):
// write a term index, read it from 8 threads with the const API, and interleave the sequential API.
#include "NindIndex/NindTermIndex.h"
#include "NindBasics/NindReadBuffer.h"
#include <atomic>
#include <cstdio>
#include <list>
#include <string>
#include <thread>
#include <vector>
using namespace latecon::nindex;
using namespace std;
typedef NindTermIndex::TermCG TermCG;
typedef NindTermIndex::Document Document;
static list<TermCG> def(unsigned int ident) {
    list<TermCG> d;
    d.push_back(TermCG(0, 0));
    for (unsigned int i = 0; i < 1 + ident % 50; i++) d.back().documents.push_back(Document(1 + i * 7 + ident, 1 + i % 9));
    return d;
}
static bool same(const list<TermCG> &d, const vector<Document> &p) {
    size_t i = 0;
    for (list<Document>::const_iterator it = d.front().documents.begin(); it != d.front().documents.end(); it++, i++)
        if (i >= p.size() || p[i].ident != it->ident || p[i].frequency != it->frequency) return false;
    return i == p.size();
}
int main(int argc, char **argv) {
    const string base = argc > 1 ? argv[1] : "winsmoke";
    const NindIndex::Identification id(0, 0);
    const unsigned int terms = 2000;
    {
        NindTermIndex writer(base, true, id, 0, 64);
        for (unsigned int t = 1; t <= terms; t++) writer.setTermDef(t, def(t), id, list<unsigned int>());
    }
    NindTermIndex reader(base, false, id, 0);
    atomic<unsigned int> bad(0), done(0), exceptions(0);
    vector<thread> threads;
    for (unsigned int k = 0; k < 8; k++)
        threads.push_back(thread([&, k]() {
            NindReadBuffer buffer;
            vector<Document> postings;
            vector<size_t> ends;
            try {
                for (unsigned int n = 0; n < 5000; n++) {
                    const unsigned int t = 1 + (n * 7919 + k * 104729) % terms;
                    if (!reader.getTermPostings(t, postings, ends, buffer) || !same(def(t), postings)) bad++;
                    done++;
                }
            } catch (const std::exception &e) { exceptions++; fprintf(stderr, "exception: %s\n", e.what()); }
        }));
    // the sequential API on the same object meanwhile: its position must not be disturbed
    unsigned int sequentialBad = 0;
    for (unsigned int t = 1; t <= terms; t++) {
        list<TermCG> d;
        if (!reader.getTermDef(t, d) || d.front().documents.size() != def(t).front().documents.size()) sequentialBad++;
    }
    for (size_t i = 0; i < threads.size(); i++) threads[i].join();
    printf("const reads: %u, mismatches: %u, exceptions: %u; sequential mismatches: %u\n",
           done.load(), bad.load(), exceptions.load(), sequentialBad);
    return (bad || exceptions || sequentialBad) ? 1 : 0;
}
