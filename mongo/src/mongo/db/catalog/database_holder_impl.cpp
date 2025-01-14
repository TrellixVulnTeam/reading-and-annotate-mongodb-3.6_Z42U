/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/database_holder_impl.h"

#include "mongo/base/init.h"
#include "mongo/db/audit.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/namespace_uuid_cache.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

//定义一个全局的DatabaseHolder  
DatabaseHolder& dbHolderImpl() {
    static DatabaseHolder _dbHolder;
    return _dbHolder;
}

MONGO_INITIALIZER_WITH_PREREQUISITES(InitializeDbHolderimpl, ("InitializeDatabaseHolderFactory"))
(InitializerContext* const) {
    registerDbHolderImpl(dbHolderImpl);
    return Status::OK();
}


MONGO_INITIALIZER(InitializeDatabaseHolderFactory)(InitializerContext* const) {
    DatabaseHolder::registerFactory([] { return stdx::make_unique<DatabaseHolderImpl>(); });
    return Status::OK();
}

}  // namespace

using std::set;
using std::size_t;
using std::string;
using std::stringstream;

namespace {

//ns一般是db.collection结构，解析出db信息
StringData _todb(StringData ns) {
    size_t i = ns.find('.');
    if (i == std::string::npos) {
        uassert(13074, "db name can't be empty", ns.size());
        return ns;
    }

    uassert(13075, "db name can't be empty", i > 0);

    const StringData d = ns.substr(0, i);
    uassert(13280,
            "invalid db name: " + ns.toString(),
            NamespaceString::validDBName(d, NamespaceString::DollarInDbNameBehavior::Allow));

    return d;
}

}  // namespace

//AutoGetDb::AutoGetDb或者AutoGetOrCreateDb::AutoGetOrCreateDb->DatabaseHolderImpl::get从DatabaseHolderImpl._dbs数组查找获取Database
//DatabaseImpl::createCollection创建collection的表全部添加到_collections数组中
//AutoGetCollection::AutoGetCollection通过Database::getCollection或者UUIDCatalog::lookupCollectionByUUID(从UUIDCatalog._catalog数组通过查找uuid可以获取collection表信息)
//注意AutoGetCollection::AutoGetCollection构造函数可以是uuid，也有一个构造函数是nss，也就是可以通过uuid查找，也可以通过nss查找

//AutoGetDb::AutoGetDb  AutoGetOrCreateDb::AutoGetOrCreateDb调用
Database* DatabaseHolderImpl::get(OperationContext* opCtx, StringData ns) const {
    const StringData db = _todb(ns);
    invariant(opCtx->lockState()->isDbLockedForMode(db, MODE_IS));

    stdx::lock_guard<SimpleMutex> lk(_m);
	//所有的DB保存到了  
    DBs::const_iterator it = _dbs.find(db);
    if (it != _dbs.end()) {
        return it->second;
    }

    return NULL;
}

std::set<std::string> DatabaseHolderImpl::_getNamesWithConflictingCasing_inlock(StringData name) {
    std::set<std::string> duplicates;

    for (const auto& nameAndPointer : _dbs) {
        // A name that's equal with case-insensitive match must be identical, or it's a duplicate.
        if (name.equalCaseInsensitive(nameAndPointer.first) && name != nameAndPointer.first)
            duplicates.insert(nameAndPointer.first);
    }
    return duplicates;
}

std::set<std::string> DatabaseHolderImpl::getNamesWithConflictingCasing(StringData name) {
    stdx::lock_guard<SimpleMutex> lk(_m);
    return _getNamesWithConflictingCasing_inlock(name);
}

//AutoGetOrCreateDb::AutoGetOrCreateDb  OldClientContext::OldClientContext->OldClientContext::_finishInit()调用，生成Database
//DatabaseHolderImpl::openDb创建DB，每个DB对应一个DatabaseImpl，
//使用某个库的时候才会openDb，如果不使用么某个DB，即使存在也不会构造对应DatabaseImpl
Database* DatabaseHolderImpl::openDb(OperationContext* opCtx, StringData ns, bool* justCreated) {
    const StringData dbname = _todb(ns);
    invariant(opCtx->lockState()->isDbLockedForMode(dbname, MODE_X));

    if (justCreated)
        *justCreated = false;  // Until proven otherwise.

    stdx::unique_lock<SimpleMutex> lk(_m);

    // The following will insert a nullptr for dbname, which will treated the same as a non-
    // existant database by the get method, yet still counts in getNamesWithConflictingCasing.
    //该库在内存中已经存在，直接返回
    if (auto db = _dbs[dbname])
        return db;

    // We've inserted a nullptr entry for dbname: make sure to remove it on unsuccessful exit.
    auto removeDbGuard = MakeGuard([this, &lk, dbname] {
        if (!lk.owns_lock())
            lk.lock();
        _dbs.erase(dbname);
    });

    // Check casing in lock to avoid transient duplicates.
    auto duplicates = _getNamesWithConflictingCasing_inlock(dbname);
    uassert(ErrorCodes::DatabaseDifferCase,
            str::stream() << "db already exists with different case already have: ["
                          << *duplicates.cbegin()
                          << "] trying to create ["
                          << dbname.toString()
                          << "]",
            duplicates.empty());


    // Do the catalog lookup and database creation outside of the scoped lock, because these may
    // block. Only one thread can be inside this method for the same DB name, because of the
    // requirement for X-lock on the database when we enter. So there is no way we can insert two
    // different databases for the same name.
    lk.unlock();

	//获取KVStorageEngine
    StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
	//KVStorageEngine::getDatabaseCatalogEntry获取对应KVDatabaseCatalogEntryBase信息
	//获取该db对应的DatabaseCatalogEntry信息，
	//一个db对应一个KVDatabaseCatalogEntryBase，包含该db得所有表信息
    DatabaseCatalogEntry* entry = storageEngine->getDatabaseCatalogEntry(opCtx, dbname);

    if (!entry->exists()) {
        audit::logCreateDatabase(&cc(), dbname);
        if (justCreated)
            *justCreated = true;
    }

	//构造DatabaseImpl
    auto newDb = stdx::make_unique<Database>(opCtx, dbname, entry);

    // Finally replace our nullptr entry with the new Database pointer.
    removeDbGuard.Dismiss();
    lk.lock();
	
    auto it = _dbs.find(dbname);
    invariant(it != _dbs.end() && it->second == nullptr);
	//DatabaseImpl添加到_dbs数组
    it->second = newDb.release();
	//冲突检测
    invariant(_getNamesWithConflictingCasing_inlock(dbname.toString()).empty());

    return it->second;
}

//DatabaseImpl::dropDatabase中调用
void DatabaseHolderImpl::close(OperationContext* opCtx, StringData ns, const std::string& reason) {
    invariant(opCtx->lockState()->isW());

    const StringData dbName = _todb(ns);

    stdx::lock_guard<SimpleMutex> lk(_m);

	//没找到该DB说明本身就不存在
    DBs::const_iterator it = _dbs.find(dbName);
    if (it == _dbs.end()) {
        return;
    }

    auto db = it->second;
	//清除该db下面得所有uuid
    UUIDCatalog::get(opCtx).onCloseDatabase(db);
	//从全局NamespaceUUIDCache中清除NamespaceUUIDCache._cache
    for (auto&& coll : *db) {
        NamespaceUUIDCache::get(opCtx).evictNamespace(coll->ns());
    }

	//DatabaseImpl::close
    db->close(opCtx, reason);
    delete db;
    db = nullptr;

	//从_dbs数组中剔除后该db
    _dbs.erase(it);

	//KVStorageEngine::closeDatabase
    getGlobalServiceContext()
        ->getGlobalStorageEngine()
        ->closeDatabase(opCtx, dbName.toString())
        .transitional_ignore();
}

bool DatabaseHolderImpl::closeAll(OperationContext* opCtx,
                                  BSONObjBuilder& result,
                                  bool force,
                                  const std::string& reason) {
    invariant(opCtx->lockState()->isW());

    stdx::lock_guard<SimpleMutex> lk(_m);

    set<string> dbs;
    for (DBs::const_iterator i = _dbs.begin(); i != _dbs.end(); ++i) {
        dbs.insert(i->first);
    }

    BSONArrayBuilder bb(result.subarrayStart("dbs"));
    int nNotClosed = 0;
    for (set<string>::iterator i = dbs.begin(); i != dbs.end(); ++i) {
        string name = *i;

        LOG(2) << "DatabaseHolder::closeAll name:" << name;
		//该库下面有表在后台加索引，则需要删除kill掉该后台操作后才能删除该表相关数据
        if (!force && BackgroundOperation::inProgForDb(name)) {
            log() << "WARNING: can't close database " << name
                  << " because a bg job is in progress - try killOp command";
            nNotClosed++;
            continue;
        }

        Database* db = _dbs[name];
		//DatabaseImpl::close
        db->close(opCtx, reason);
        delete db;

        _dbs.erase(name);

        getGlobalServiceContext()
            ->getGlobalStorageEngine()
            ->closeDatabase(opCtx, name)
            .transitional_ignore();

        bb.append(name);
    }

    bb.done();
    if (nNotClosed) {
        result.append("nNotClosed", nNotClosed);
    }

    return true;
}
}  // namespace mongo
