// sqlite3pp.cpp
//
// The MIT License
//
// Copyright (c) 2012 Wongoo Lee (iwongu at gmail dot com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "sqlite3pp.hpp"
#include <memory>
#include <cstdlib>
#include <cstdio>
#include <boost/shared_ptr.hpp>


#define THROW_ERR(ret) do { if ((ret) != SQLITE_OK) throw database_error(db_); } while(0);

#if SQLITE_VERSION_NUMBER >= 3007015
#define SQLITE3_HAS_ERRMSG
#endif


namespace sqlite3pp
{
    null_type ignore;

    namespace
    {
        int busy_handler_impl(void* p, int cnt)
        {
            database::busy_handler* h = static_cast<database::busy_handler*>(p);
            return (*h)(cnt);
        }

        int commit_hook_impl(void* p)
        {
            database::commit_handler* h = static_cast<database::commit_handler*>(p);
            return (*h)();
        }

        void rollback_hook_impl(void* p)
        {
            database::rollback_handler* h = static_cast<database::rollback_handler*>(p);
            (*h)();
        }

        void update_hook_impl(void* p, int opcode, char const* dbname, char const* tablename, long long int rowid)
        {
            database::update_handler* h = static_cast<database::update_handler*>(p);
            (*h)(opcode, dbname, tablename, rowid);
        }

        int authorizer_impl(void* p, int evcode, char const* p1, char const* p2, char const* dbname, char const* tvname)
        {
            database::authorize_handler* h = static_cast<database::authorize_handler*>(p);
            return (*h)(evcode, p1, p2, dbname, tvname);
        }

    } // namespace

    int enable_shared_cache(bool fenable)
    {
        return sqlite3_enable_shared_cache(fenable);
    }

    database::database(char const* dbname):
        db_(0)
    {
        if (dbname) 
        {
            const int rc = connect(dbname);
            if (rc != SQLITE_OK)
                throw database_error("can't connect database");
        }
    }

    database::~database()
    {
        disconnect();
    }

    int database::connect(char const* dbname)
    {
        disconnect();

        return sqlite3_open(dbname, &db_);
    }

#if SQLITE_VERSION_NUMBER >= 3005000
    int database::connect_v2(char const* dbname, int flags, char const* vfs)
    {
        disconnect();

        return sqlite3_open_v2(dbname, &db_, flags, vfs);
    }
#endif

    int database::disconnect()
    {
        int rc = SQLITE_OK;
        if (db_) {
            rc = sqlite3_close(db_);
            db_ = 0;
        }

        return rc;
    }

    int database::attach(char const* dbname, char const* name)
    {
        return executef("ATTACH '%s' AS '%s'", dbname, name);
    }

    int database::detach(char const* name)
    {
        return executef("DETACH '%s'", name);
    }

    void database::set_busy_handler(busy_handler h)
    {
        bh_ = h;
        sqlite3_busy_handler(db_, bh_ ? busy_handler_impl : 0, &bh_);
    }

    void database::set_commit_handler(commit_handler h)
    {
        ch_ = h;
        sqlite3_commit_hook(db_, ch_ ? commit_hook_impl : 0, &ch_);
    }

    void database::set_rollback_handler(rollback_handler h)
    {
        rh_ = h;
        sqlite3_rollback_hook(db_, rh_ ? rollback_hook_impl : 0, &rh_);
    }

    void database::set_update_handler(update_handler h)
    {
        uh_ = h;
        sqlite3_update_hook(db_, uh_ ? update_hook_impl : 0, &uh_);
    }

    void database::set_authorize_handler(authorize_handler h)
    {
        ah_ = h;
        sqlite3_set_authorizer(db_, ah_ ? authorizer_impl : 0, &ah_);
    }

    int64_t database::last_insert_rowid() const
    {
        return sqlite3_last_insert_rowid(db_);
    }

    int database::error_code() const
    {
        return sqlite3_errcode(db_);
    }

    char const* database::error_msg() const
    {
        return sqlite3_errmsg(db_);
    }

    void database::execute(char const* sql)
    {
        const int rc = eexecute(sql);
        if (rc != SQLITE_OK)
            throw database_error(*this);
    }


    int database::eexecute(char const* sql)
    {
        return sqlite3_exec(db_, sql, 0, 0, 0);
    }

    int database::executef(char const* sql, ...)
    {
        va_list ap;
        va_start(ap, sql);
        boost::shared_ptr<char> msql(sqlite3_vmprintf(sql, ap), sqlite3_free);
        va_end(ap);

        return eexecute(msql.get());
    }

    int database::set_busy_timeout(int ms)
    {
        return sqlite3_busy_timeout(db_, ms);
    }


    statement::statement(database& db, char const* stmt):
        db_(db),
        stmt_(0),
        statement_(stmt ? stmt : ""),
        tail_(0)
    {
        if (stmt)
            prepare(stmt);
    }

    statement::~statement()
    {
        int rc = efinish();
        if (rc != SQLITE_OK)
        {
            // an exception is being handled which called this dtor, we can't throw now
            std::fputs("statement::~statement: sqlite3_finalize returned with error while executing: ", stderr);
            std::fputs(statement_.c_str(), stderr);
            std::fputs("\n", stderr);
            std::fputs("sqlite error: ", stderr);
#ifdef SQLITE3_HAS_ERRMSG
            std::fputs(sqlite3_errstr(rc), stderr);
#else
            std::fputs(sqlite3_errmsg(db_.db_), stderr);
#endif
            std::fputs("\n", stderr);
        }
    }

    void statement::prepare(char const* stmt)
    {
        THROW_ERR(eprepare(stmt));
    }

    int statement::eprepare(char const* stmt)
    {
        int rc = efinish();
        if (rc != SQLITE_OK)
            return rc;

        statement_ = stmt;
        return prepare_impl(stmt);
    }

    int statement::prepare_impl(char const* stmt)
    {
#if SQLITE_VERSION_NUMBER >= 3003009
        return sqlite3_prepare_v2(db_.db_, stmt, strlen(stmt), &stmt_, &tail_);
#else
        return sqlite3_prepare(db_.db_, stmt, strlen(stmt), &stmt_, &tail_);
#endif
    }

    void statement::finish()
    {
        THROW_ERR(efinish());
    }

    int statement::efinish()
    {
        int rc = SQLITE_OK;
        if (stmt_) {
            rc = finish_impl(stmt_);
            stmt_ = 0;
        }
        tail_ = 0;

        return rc;
    }

    int statement::finish_impl(sqlite3_stmt* stmt)
    {
        return sqlite3_finalize(stmt);
    }

    int statement::step()
    {
        return sqlite3_step(stmt_);
    }

    statement& statement::reset()
    {
        THROW_ERR(sqlite3_reset(stmt_));
        return *this;
    }

    statement& statement::bind(int idx, int32_t value)
    {
        return bind(idx, static_cast<int64_t>(value));
    }

    statement& statement::bind(int idx, uint32_t value)
    {
        return bind(idx, static_cast<uint64_t>(value));
    }


    statement& statement::bind(int idx, double value)
    {
        THROW_ERR(sqlite3_bind_double(stmt_, idx, value));
        return *this;
    }

    statement& statement::bind(int idx, int64_t value)
    {
        THROW_ERR(sqlite3_bind_int64(stmt_, idx, value));
        return *this;
    }

    statement& statement::bind(int idx, uint64_t value)
    {
        THROW_ERR(sqlite3_bind_int64(stmt_, idx, static_cast<int64_t>(value)));
        return *this;
    }

    statement& statement::bind(int idx, const std::string& value, bool blob, bool fstatic)
    {
        if (blob)
        {
                THROW_ERR(sqlite3_bind_blob(stmt_, idx, value.c_str(), static_cast<int>(value.size()), fstatic ? SQLITE_STATIC : SQLITE_TRANSIENT));
        }
        else
        {
                THROW_ERR(sqlite3_bind_text(stmt_, idx, value.c_str(), static_cast<int>(value.size()), fstatic ? SQLITE_STATIC : SQLITE_TRANSIENT));
        }
        return *this;
    }

    statement& statement::bind(int idx, char const* value, bool fstatic)
    {
        THROW_ERR(sqlite3_bind_text(stmt_, idx, value, strlen(value), fstatic ? SQLITE_STATIC : SQLITE_TRANSIENT));
        return *this;
    }

    statement& statement::bind(int idx, void const* value, int n, bool fstatic)
    {
        THROW_ERR(sqlite3_bind_blob(stmt_, idx, value, n, fstatic ? SQLITE_STATIC : SQLITE_TRANSIENT));
        return *this;
    }

    statement& statement::bind(int idx)
    {
        THROW_ERR(sqlite3_bind_null(stmt_, idx));
        return *this;
    }

    statement& statement::bind(int idx, null_type)
    {
        bind(idx);
        return *this;
    }

    statement& statement::bind(char const* name, int value)
    {
        const int idx = sqlite3_bind_parameter_index(stmt_, name);
        assert(idx);
        bind(idx, value);
        return *this;
    }

    statement& statement::bind(char const* name, double value)
    {
        const int idx = sqlite3_bind_parameter_index(stmt_, name);
        assert(idx);
        bind(idx, value);
        return *this;
    }

    statement& statement::bind(char const* name, int64_t value)
    {
        const int idx = sqlite3_bind_parameter_index(stmt_, name);
        assert(idx);
        bind(idx, value);
        return *this;
    }

    statement& statement::bind(char const* name, uint64_t value)
    {
        const int idx = sqlite3_bind_parameter_index(stmt_, name);
        assert(idx);
        bind(idx, value);
        return *this;
    }

    statement& statement::bind(char const* name, const std::string& value, bool blob, bool fstatic)
    {
        const int idx = sqlite3_bind_parameter_index(stmt_, name);
        assert(idx);
        bind(idx, value, blob, fstatic);
        return *this;
    }

    statement& statement::bind(char const* name, char const* value, bool fstatic)
    {
        const int idx = sqlite3_bind_parameter_index(stmt_, name);
        assert(idx);
        bind(idx, value, fstatic);
        return *this;
    }

    statement& statement::bind(char const* name, void const* value, int n, bool fstatic)
    {
        const int idx = sqlite3_bind_parameter_index(stmt_, name);
        assert(idx);
        bind(idx, value, n, fstatic);
        return *this;
    }

    statement& statement::bind(char const* name)
    {
        const int idx = sqlite3_bind_parameter_index(stmt_, name);
        assert(idx);
        bind(idx);
        return *this;
    }

    statement& statement::bind(char const* name, null_type)
    {
        bind(name);
        return *this;
    }


    command::bindstream::bindstream(command& cmd, int idx) : cmd_(cmd), idx_(idx)
    {
    }

    command::command(database& db, char const* stmt) : statement(db, stmt)
    {
    }

    command::bindstream command::binder(int idx)
    {
        return bindstream(*this, idx);
    }

    void command::execute()
    {
        THROW_ERR(eexecute());
    }

    int command::eexecute()
    {
        int rc = step();
        if (rc == SQLITE_DONE)
            rc = SQLITE_OK;
        return rc;
    }

    int command::execute_all()
    {
        int rc = eexecute();
        if (rc != SQLITE_OK) return rc;

        char const* sql = tail_;

        while (strlen(sql) > 0) { // sqlite3_complete() is broken.
            sqlite3_stmt* old_stmt = stmt_;

            if ((rc = prepare_impl(sql)) != SQLITE_OK) return rc;

            if ((rc = sqlite3_transfer_bindings(old_stmt, stmt_)) != SQLITE_OK) return rc;

            finish_impl(old_stmt);

            if ((rc = eexecute()) != SQLITE_OK) return rc;

            sql = tail_;
        }

        return rc;
    }


    query::rows::getstream::getstream(rows* rws, int idx):
        rws_(rws)
        , idx_(idx)
    {
    }

    query::rows::rows(sqlite3_stmt* stmt):
        stmt_(stmt)
    {
    }

    int query::rows::data_count() const
    {
        return sqlite3_data_count(stmt_);
    }

    int query::rows::column_type(int idx) const
    {
        return sqlite3_column_type(stmt_, idx);
    }

    int query::rows::column_count() const
    {
        return sqlite3_column_count(stmt_);
    }

    int query::rows::column_bytes(int idx) const
    {
        return sqlite3_column_bytes(stmt_, idx);
    }

    query::rows::getstream query::rows::getter(int idx)
    {
        return getstream(this, idx);
    }


    query::query_iterator::query_iterator():
        cmd_(0)
    {
        rc_ = SQLITE_DONE;
    }

    query::query_iterator::query_iterator(query* cmd):
        cmd_(cmd)
    {
        set_query(cmd);
    }

    void query::query_iterator::set_query(query* cmd)
    {
        assert(cmd);
        cmd_ = cmd;
        rc_ = cmd_->step();
        if (rc_ != SQLITE_ROW && rc_ != SQLITE_DONE)
            throw database_error(cmd_->db_);
    }

    void query::query_iterator::increment()
    {
        assert(cmd_);
        rc_ = cmd_->step();
        if (rc_ != SQLITE_ROW && rc_ != SQLITE_DONE)
            throw database_error(cmd_->db_);
    }

    bool query::query_iterator::equal(query_iterator const& other) const
    {
        return rc_ == other.rc_;
    }

    query::rows query::query_iterator::dereference() const
    {
        assert(cmd_);
        return rows(cmd_->stmt_);
    }

    query::query(database& db, char const* stmt):
        statement(db, stmt)
    {
    }

    int query::column_count() const
    {
        return sqlite3_column_count(stmt_);
    }

    char const* query::column_name(int idx) const
    {
        return sqlite3_column_name(stmt_, idx);
    }

    char const* query::column_decltype(int idx) const
    {
        return sqlite3_column_decltype(stmt_, idx);
    }

    query::rows query::fetchone()
    {
        int rc = step(); 
        if (rc != SQLITE_ROW)
            throw database_error(db_);
        return query::rows(stmt_);
    }

    query::iterator query::begin()
    {
        return query_iterator(this);
    }

    query::iterator query::end()
    {
        return query_iterator();
    }


    transaction::transaction(database& db, bool fcommit, bool freserve):
        db_(&db)
        , fcommit_(fcommit)
    {
        db_->eexecute(freserve ? "BEGIN IMMEDIATE" : "BEGIN");
    }

    transaction::~transaction()
    {
        if (db_)
        {
            const int rc = db_->eexecute(fcommit_ ? "COMMIT" : "ROLLBACK");
            if (rc != SQLITE_OK)
            {
                if (fcommit_)
                    std::fputs("transaction::~transaction: COMMIT returned with error and an exception was being handled\n", stderr);
                else
                    std::fputs("transaction::~transaction: ROLLBACK returned with error and an exception was being handled\n", stderr);
                abort();
            }
        }
    }

    int transaction::commit()
    {
        database* db = db_;
        db_ = 0;
        int rc = db->eexecute("COMMIT");
        return rc;
    }

    int transaction::rollback()
    {
        database* db = db_;
        db_ = 0;
        int rc = db->eexecute("ROLLBACK");
        return rc;
    }


    database_error::database_error(char const* msg) : std::runtime_error(msg)
    {
    }

    database_error::database_error(database& db) : std::runtime_error(sqlite3_errmsg(db.db_))
    {
    }


}
