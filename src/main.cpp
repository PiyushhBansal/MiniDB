// minidb shell. reads SQL ending in ; runs it, prints a table. \cmds are meta.
#include "database.h"
#include <iostream>
#include <string>
#include <iomanip>
#include <unistd.h>  // _exit for the crash sim

using namespace std;
using namespace minidb;

// print the result set as a text table
static void print_result(const ResultSet& rs) {
    if (!rs.message.empty()) {
        cout << rs.message << "\n";
        if (rs.columns.empty()) return;
    }
    if (rs.columns.empty()) return;

    // figure out column widths
    vector<size_t> w(rs.columns.size());
    for (size_t i = 0; i < rs.columns.size(); ++i) w[i] = rs.columns[i].size();
    for (auto& row : rs.rows)
        for (size_t j = 0; j < row.size(); ++j)
            w[j] = max(w[j], row[j].to_string().size());

    auto sep = [&]() {
        cout << "+";
        for (size_t k = 0; k < w.size(); ++k) { cout << string(w[k] + 2, '-') << "+"; }
        cout << "\n";
    };
    sep();
    cout << "|";
    for (size_t k = 0; k < rs.columns.size(); ++k)
        cout << " " << setw((int)w[k]) << left << rs.columns[k] << " |";
    cout << "\n";
    sep();
    for (auto& row : rs.rows) {
        cout << "|";
        for (size_t k = 0; k < row.size(); ++k)
            cout << " " << setw((int)w[k]) << left << row[k].to_string() << " |";
        cout << "\n";
    }
    sep();
    cout << "(" << rs.rows.size() << " row" << (rs.rows.size() == 1 ? "" : "s") << ")\n";
}

int main(int argc, char** argv) {
    string db_file = argc > 1 ? argv[1] : "minidb.db";
    Database db(db_file);
    cout << "MiniDB shell - database: " << db_file << "\n";
    // print what recovery did on startup (matters after a \crash)
    auto rr = db.recovery_report();
    if (rr.redone || rr.undone || rr.committed || rr.rolled_back)
        cout << "recovery: " << rr.committed << " committed txn(s) redone, "
                  << rr.rolled_back << " loser(s) rolled back ("
                  << rr.redone << " ops redone, " << rr.undone << " undone)\n";
    cout << "Type SQL terminated by ';'. \\help for commands.\n";

    string buffer, line;
    bool interactive = true;
    while (true) {
        if (interactive) cout << (buffer.empty() ? "minidb> " : "    ...> ") << flush;
        if (!getline(cin, line)) break;

        // strip "-- comment" to end of line. track quotes so we dont cut inside a string
        {
            bool in_str = false; char q = 0;
            for (size_t i = 0; i + 1 < line.size(); ++i) {
                char c = line[i];
                if (in_str) { if (c == q) in_str = false; }
                else if (c == '\'' || c == '"') { in_str = true; q = c; }
                else if (c == '-' && line[i + 1] == '-') { line.erase(i); break; }
            }
        }

        // meta commands, only when not mid statement
        if (buffer.empty() && !line.empty() && line[0] == '\\') {
            if (line == "\\q" || line == "\\quit") break;
            if (line == "\\help") {
                cout << "Commands:\n"
                             "  \\tables       list tables\n"
                             "  \\checkpoint   flush dirty pages + write a checkpoint\n"
                             "  \\crash        simulate a power failure (exit WITHOUT flushing pages)\n"
                             "  \\q            clean shutdown (checkpoints first)\n"
                             "SQL: CREATE TABLE, INSERT, SELECT (WHERE/JOIN),\n"
                             "     DELETE, EXPLAIN, BEGIN/COMMIT/ROLLBACK\n";
                continue;
            }
            if (line == "\\tables") {
                for (auto& n : db.catalog()->table_names()) cout << "  " << n << "\n";
                continue;
            }
            if (line == "\\checkpoint") {
                db.checkpoint();
                cout << "checkpoint written\n";
                continue;
            }
            if (line == "\\crash") {
                // fake a crash: _exit so destructors dont run and pages never flush.
                // committed txns come back from the WAL on next startup, uncommitted get rolled back
                cout << "*** simulating crash (no page flush) ***\n";
                cout.flush();
                _exit(1);
            }
            cout << "unknown command: " << line << "\n";
            continue;
        }

        buffer += line + " ";
        // run once we hit a ;
        size_t semi;
        while ((semi = buffer.find(';')) != string::npos) {
            string stmt = buffer.substr(0, semi);
            buffer.erase(0, semi + 1);
            // skip empty ones
            bool blank = stmt.find_first_not_of(" \t\r\n") == string::npos;
            if (blank) continue;
            try {
                ResultSet rs = db.execute(stmt);
                print_result(rs);
            } catch (const exception& e) {
                cout << "ERROR: " << e.what() << "\n";
            }
        }
    }
    db.checkpoint();  // clean exit, flush pages + checkpoint the log
    cout << "bye\n";
    return 0;
}
