// quick smoke test for the heap file / storage layer
#include "heap_file.h"
#include <cstdio>
#include <cassert>
using namespace std;
using namespace minidb;

int main() {
    remove("/tmp/minidb_storage_test.db");
    DiskManager dm("/tmp/minidb_storage_test.db");
    BufferPool bp(&dm, 16);

    Schema schema{{{"id", Type::INT}, {"name", Type::VARCHAR}}};
    PageId root = HeapFile::create(&bp);
    HeapFile hf(&bp, root);

    // insert enough to span a few pages
    vector<RecordId> rids;
    for (int i = 0; i < 500; ++i) {
        Tuple t{Value::Int(i), Value::Str("user_" + to_string(i))};
        rids.push_back(hf.insert(serialize_tuple(t, schema)));
    }

    // scan + count
    int count = 0;
    hf.scan([&](RecordId, const string& rec) {
        Tuple t = deserialize_tuple(rec.data(), rec.size(), schema);
        assert(t[1].s == "user_" + to_string(t[0].i));
        count++;
        return true;
    });
    printf("scanned %d tuples (expected 500)\n", count);
    assert(count == 500);

    // delete every other one, scan again
    for (size_t j = 0; j < rids.size(); j += 2) hf.erase(rids[j]);
    count = 0;
    hf.scan([&](RecordId, const string&) { count++; return true; });
    printf("after deleting half: %d tuples (expected 250)\n", count);
    assert(count == 250);

    bp.flush_all();
    printf("STORAGE TEST PASSED\n");
    return 0;
}
