// hand-rolled tokenizer + recursive descent parser. grammar's small enough
// that a parser generator would be overkill. keywords are case-insensitive.
#pragma once

#include "ast.h"
#include <cctype>
#include <stdexcept>

using namespace std;

namespace minidb {

struct Token {
    enum Type { IDENT, NUMBER, STRING, PUNCT, END } type;
    string text;
};

class Tokenizer {
public:
    explicit Tokenizer(const string& sql) : s_(sql) {}

    vector<Token> run() {
        vector<Token> toks;
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (isspace((unsigned char)c)) { pos_++; continue; }
            if (isalpha((unsigned char)c) || c == '_') {
                size_t start = pos_;
                while (pos_ < s_.size() && (isalnum((unsigned char)s_[pos_]) || s_[pos_] == '_' || s_[pos_] == '.'))
                    pos_++;
                toks.push_back({Token::IDENT, s_.substr(start, pos_ - start)});
            } else if (isdigit((unsigned char)c) || (c == '-' && pos_ + 1 < s_.size() && isdigit((unsigned char)s_[pos_+1]))) {
                size_t start = pos_;
                pos_++;
                while (pos_ < s_.size() && isdigit((unsigned char)s_[pos_])) pos_++;
                toks.push_back({Token::NUMBER, s_.substr(start, pos_ - start)});
            } else if (c == '\'' || c == '"') {
                char q = c; pos_++;
                string lit;
                while (pos_ < s_.size() && s_[pos_] != q) lit += s_[pos_++];
                pos_++;  // skip closing quote
                toks.push_back({Token::STRING, lit});
            } else if (c == '<' || c == '>' || c == '!' || c == '=') {
                // grab two-char ops too: <=, >=, !=, <>
                string op(1, c);
                pos_++;
                if (pos_ < s_.size() && (s_[pos_] == '=' || s_[pos_] == '>')) { op += s_[pos_]; pos_++; }
                toks.push_back({Token::PUNCT, op});
            } else {
                toks.push_back({Token::PUNCT, string(1, c)});
                pos_++;
            }
        }
        toks.push_back({Token::END, ""});
        return toks;
    }
private:
    string s_;
    size_t pos_ = 0;
};

class Parser {
public:
    explicit Parser(const string& sql) : toks_(Tokenizer(sql).run()) {}

    Statement parse() {
        string kw = upper(peek().text);
        if (kw == "CREATE") return parse_create();
        if (kw == "INSERT") return parse_insert();
        if (kw == "SELECT") return parse_select();
        if (kw == "DELETE") return parse_delete();
        if (kw == "BEGIN") { Statement s; s.kind = Statement::BEGIN_TXN; return s; }
        if (kw == "COMMIT") { Statement s; s.kind = Statement::COMMIT_TXN; return s; }
        if (kw == "ROLLBACK") { Statement s; s.kind = Statement::ROLLBACK_TXN; return s; }
        throw DBError("parse error: unexpected '" + peek().text + "'");
    }

private:
    // token helpers
    const Token& peek(int k = 0) const { return toks_[min(pos_ + k, toks_.size() - 1)]; }
    Token next() { return toks_[min(pos_++, toks_.size() - 1)]; }
    bool accept_kw(const string& kw) {
        if (peek().type == Token::IDENT && upper(peek().text) == kw) { pos_++; return true; }
        return false;
    }
    void expect_kw(const string& kw) {
        if (!accept_kw(kw)) throw DBError("parse error: expected '" + kw + "', got '" + peek().text + "'");
    }
    bool accept_punct(const string& p) {
        if (peek().type == Token::PUNCT && peek().text == p) { pos_++; return true; }
        return false;
    }
    void expect_punct(const string& p) {
        if (!accept_punct(p)) throw DBError("parse error: expected '" + p + "', got '" + peek().text + "'");
    }
    static string upper(string s) {
        for (auto& c : s) c = toupper((unsigned char)c);
        return s;
    }

    Statement parse_create() {
        Statement st; st.kind = Statement::CREATE;
        expect_kw("CREATE"); expect_kw("TABLE");
        st.create.table = next().text;
        expect_punct("(");
        while (true) {
            if (peek().type == Token::IDENT && upper(peek().text) == "PRIMARY") {
                next(); expect_kw("KEY"); expect_punct("(");
                st.create.pk_column = next().text;
                expect_punct(")");
            } else {
                Column col;
                col.name = next().text;
                string ty = upper(next().text);
                col.type = (ty == "INT" || ty == "INTEGER" || ty == "BIGINT") ? Type::INT : Type::VARCHAR;
                st.create.schema.columns.push_back(col);
            }
            if (accept_punct(",")) continue;
            break;
        }
        expect_punct(")");
        // optional USING MVCC | USING HEAP
        if (accept_kw("USING")) {
            string eng = upper(next().text);
            st.create.use_mvcc = (eng == "MVCC");
        }
        return st;
    }

    Statement parse_insert() {
        Statement st; st.kind = Statement::INSERT;
        expect_kw("INSERT"); expect_kw("INTO");
        st.insert.table = next().text;
        expect_kw("VALUES");
        while (true) {
            expect_punct("(");
            Tuple row;
            while (true) {
                row.push_back(parse_literal());
                if (accept_punct(",")) continue;
                break;
            }
            expect_punct(")");
            st.insert.rows.push_back(move(row));
            if (accept_punct(",")) continue;
            break;
        }
        return st;
    }

    Statement parse_select() {
        Statement st; st.kind = Statement::SELECT;
        auto& sel = st.select;
        expect_kw("SELECT");
        while (true) {
            sel.columns.push_back(next().text);
            if (accept_punct(",")) continue;
            break;
        }
        expect_kw("FROM");
        sel.table = next().text;
        if (accept_kw("JOIN")) {
            sel.has_join = true;
            sel.join_table = next().text;
            expect_kw("ON");
            string left = next().text;
            expect_punct("=");
            string right = next().text;
            sel.join_left_col = left;
            sel.join_right_col = right;
        }
        if (accept_kw("WHERE")) parse_where(sel.where);
        return st;
    }

    Statement parse_delete() {
        Statement st; st.kind = Statement::DELETE_;
        expect_kw("DELETE"); expect_kw("FROM");
        st.del.table = next().text;
        if (accept_kw("WHERE")) parse_where(st.del.where);
        return st;
    }

    void parse_where(vector<Predicate>& out) {
        while (true) {
            Predicate p;
            p.column = next().text;
            string op = next().text;
            p.op = op == "=" ? CmpOp::EQ : op == "!=" || op == "<>" ? CmpOp::NE
                   : op == "<" ? CmpOp::LT : op == "<=" ? CmpOp::LE
                   : op == ">" ? CmpOp::GT : CmpOp::GE;
            p.literal = parse_literal();
            out.push_back(p);
            if (accept_kw("AND")) continue;
            break;
        }
    }

    Value parse_literal() {
        Token t = next();
        if (t.type == Token::NUMBER) return Value::Int(stoll(t.text));
        if (t.type == Token::STRING) return Value::Str(t.text);
        // bare ident used as a value, just treat as a string
        return Value::Str(t.text);
    }

    vector<Token> toks_;
    size_t pos_ = 0;
};

}  // namespace minidb
