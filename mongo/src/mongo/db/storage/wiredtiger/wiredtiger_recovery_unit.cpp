// wiredtiger_recovery_unit.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {
// SnapshotIds need to be globally unique, as they are used in a WorkingSetMember to
// determine if documents changed, but a different recovery unit may be used across a getMore,
// so there is a chance the snapshot ID will be reused.
AtomicUInt64 nextSnapshotId{1};

logger::LogSeverity kSlowTransactionSeverity = logger::LogSeverity::Debug(1);
}  // namespace

//WiredTigerRecoveryUnit��WiredTigerKVEngine._sessionCache��ͨ��WiredTigerKVEngine::newRecoveryUnit()��������
//WiredTigerKVEngine::newRecoveryUnit�е���
WiredTigerRecoveryUnit::WiredTigerRecoveryUnit(WiredTigerSessionCache* sc)
    : WiredTigerRecoveryUnit(sc, sc->getKVEngine()->getOplogManager()) {}

WiredTigerRecoveryUnit::WiredTigerRecoveryUnit(WiredTigerSessionCache* sc,
                                               WiredTigerOplogManager* oplogManager)
    : _sessionCache(sc),
      _oplogManager(oplogManager),
      _inUnitOfWork(false),
      _active(false),
      _mySnapshotId(nextSnapshotId.fetchAndAdd(1)) {}

/*
(gdb) bt
#0  mongo::WiredTigerSessionCache::releaseSession (this=0x7fa0cb267dc0, session=0x7fa0ce96c0f0) at src/mongo/db/storage/wiredtiger/wiredtiger_session_cache.cpp:441
#1  0x00007fa0c7fb7a41 in mongo::WiredTigerRecoveryUnit::~WiredTigerRecoveryUnit (this=0x7fa0ceb37200, __in_chrg=<optimized out>) at src/mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.cpp:71
#2  0x00007fa0c9611046 in operator() (this=<optimized out>, __ptr=<optimized out>) at /usr/local/include/c++/5.4.0/bits/unique_ptr.h:76
#3  ~unique_ptr (this=0x7fa0ceac3bb0, __in_chrg=<optimized out>) at /usr/local/include/c++/5.4.0/bits/unique_ptr.h:236
#4  ~OperationContext (this=0x7fa0ceac3b40, __in_chrg=<optimized out>) at src/mongo/db/operation_context.h:124
#5  ~OperationContext (this=0x7fa0ceac3b40, __in_chrg=<optimized out>) at src/mongo/db/operation_context.h:124
#6  mongo::ServiceContext::OperationContextDeleter::operator() (this=<optimized out>, opCtx=0x7fa0ceac3b40) at src/mongo/db/service_context.cpp:300
*/
WiredTigerRecoveryUnit::~WiredTigerRecoveryUnit() {
    invariant(!_inUnitOfWork);
    _abort();
}

void WiredTigerRecoveryUnit::prepareForCreateSnapshot(OperationContext* opCtx) {
    invariant(!_active);  // Can't already be in a WT transaction.
    invariant(!_inUnitOfWork);
    invariant(!_readFromMajorityCommittedSnapshot);

    // Starts the WT transaction that will be the basis for creating a named snapshot.
    getSession();
    _areWriteUnitOfWorksBanned = true;
}

/*
RecoveryUnit��װ��wiredTiger�������RecoveryUnit::_txnOpen ��Ӧ��WT���beginTransaction��  WiredTigerRecoveryUnit
RecoveryUnit::_txnClose��װ��WT���commit_transaction��rollback_transaction��
*/
//WiredTigerRecoveryUnit::commitUnitOfWork����
void WiredTigerRecoveryUnit::_commit() {
    try {
        if (_session && _active) {
            _txnClose(true);
        }

		//AddCollectionChange::commit
        for (Changes::const_iterator it = _changes.begin(), end = _changes.end(); it != end; ++it) {
            (*it)->commit();
        }
        _changes.clear();

        invariant(!_active);
    } catch (...) {
        std::terminate();
    }
}

//WiredTigerRecoveryUnit::abortUnitOfWork()����
void WiredTigerRecoveryUnit::_abort() {
    try {
        if (_session && _active) {
            _txnClose(false);
        }
		

        for (Changes::const_reverse_iterator it = _changes.rbegin(), end = _changes.rend();
             it != end;
             ++it) {
            Change* change = it->get();
            LOG(2) << "CUSTOM ROLLBACK " << redact(demangleName(typeid(*change)));
            change->rollback();
        }
        _changes.clear();

        invariant(!_active);
    } catch (...) {
        std::terminate();
    }
}

//WriteUnitOfWork::WriteUnitOfWork
void WiredTigerRecoveryUnit::beginUnitOfWork(OperationContext* opCtx) {
    invariant(!_areWriteUnitOfWorksBanned);
    invariant(!_inUnitOfWork);
    _inUnitOfWork = true;
}

//WriteUnitOfWork::commit 
void WiredTigerRecoveryUnit::commitUnitOfWork() {
    invariant(_inUnitOfWork);
    _inUnitOfWork = false;
    _commit();
}

//WriteUnitOfWork::~WriteUnitOfWork
void WiredTigerRecoveryUnit::abortUnitOfWork() {
    invariant(_inUnitOfWork);
    _inUnitOfWork = false;
    _abort();
}

//����WiredTigerSessionCache::getSession()ѡ����ֱ��new�µ�UniqueWiredTigerSession��
//��ֱ��ʹ��WiredTigerSessionCache._sessions�����е�session
void WiredTigerRecoveryUnit::_ensureSession() {
    if (!_session) {
		//WiredTigerSessionCache::getSession()ѡ����ֱ��new�µ�UniqueWiredTigerSession��
		//��ֱ��ʹ��WiredTigerSessionCache._sessions�����е�session
        _session = _sessionCache->getSession(); //WiredTigerSessionCache::getSession  
    }
}

bool WiredTigerRecoveryUnit::waitUntilDurable() {
    invariant(!_inUnitOfWork);
    // _session may be nullptr. We cannot _ensureSession() here as that needs shutdown protection.
    const bool forceCheckpoint = false;
    const bool stableCheckpoint = false;
    _sessionCache->waitUntilDurable(forceCheckpoint, stableCheckpoint);
    return true;
}

//DatabaseImpl::createCollection  KVStorageEngine::dropDatabase�ȵ���
void WiredTigerRecoveryUnit::registerChange(Change* change) {
    invariant(_inUnitOfWork);
    _changes.push_back(std::unique_ptr<Change>{change});
}

void WiredTigerRecoveryUnit::assertInActiveTxn() const {
    fassert(28575, _active);
}

// ע��WiredTigerRecoveryUnit::getSession��WiredTigerRecoveryUnit::getSessionNoTxn������
//��ȡsession,ͨ������ʽ 
//WiredTigerCursor::WiredTigerCursor����
WiredTigerSession* WiredTigerRecoveryUnit::getSession() {
    if (!_active) { //��һ�ε��õ�ʱ����if���棬�´���get��ʱ��ֱ�ӷ���_session
        _txnOpen(); //���������_ensureSession
    }
    return _session.get(); //��ȡUniqueWiredTigerSession��Ӧ��WiredTigerSession
}

// ע��WiredTigerRecoveryUnit::getSession��WiredTigerRecoveryUnit::getSessionNoTxn������
WiredTigerSession* WiredTigerRecoveryUnit::getSessionNoTxn() {
    _ensureSession();
    return _session.get(); //��ȡUniqueWiredTigerSession��Ӧ��WiredTigerSession
}

/*
#0  mongo::WiredTigerRecoveryUnit::_txnClose (this=this@entry=0x7fb1db1afd80, commit=commit@entry=false) at src/mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.cpp:199
#1  0x00007fb1d44455bf in mongo::WiredTigerRecoveryUnit::abandonSnapshot (this=0x7fb1db1afd80) at src/mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.cpp:177
#2  0x00007fb1d5a71767 in ~GlobalLock (this=0x7fb1d3a5b100, __in_chrg=<optimized out>) at src/mongo/db/concurrency/d_concurrency.h:203
#3  mongo::Lock::DBLock::~DBLock (this=0x7fb1d3a5b0c8, __in_chrg=<optimized out>) at src/mongo/db/concurrency/d_concurrency.cpp:221
#4  0x00007fb1d465e659 in mongo::(anonymous namespace)::FindCmd::run
*/
void WiredTigerRecoveryUnit::abandonSnapshot() {
    invariant(!_inUnitOfWork);
    if (_active) {
        // Can't be in a WriteUnitOfWork, so safe to rollback
        _txnClose(false);
    }
    _areWriteUnitOfWorksBanned = false;
}

void WiredTigerRecoveryUnit::prepareSnapshot() {
    // Begin a new transaction, if one is not already started.
    getSession();
}

void* WiredTigerRecoveryUnit::writingPtr(void* data, size_t len) {
    // This API should not be used for anything other than the MMAP V1 storage engine
    MONGO_UNREACHABLE;
}

/*
#0  mongo::WiredTigerRecoveryUnit::_txnClose (this=this@entry=0x7fb1db1afd80, commit=commit@entry=false) at src/mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.cpp:199
#1  0x00007fb1d44455bf in mongo::WiredTigerRecoveryUnit::abandonSnapshot (this=0x7fb1db1afd80) at src/mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.cpp:177
#2  0x00007fb1d5a71767 in ~GlobalLock (this=0x7fb1d3a5b100, __in_chrg=<optimized out>) at src/mongo/db/concurrency/d_concurrency.h:203
#3  mongo::Lock::DBLock::~DBLock (this=0x7fb1d3a5b0c8, __in_chrg=<optimized out>) at src/mongo/db/concurrency/d_concurrency.cpp:221
#4  0x00007fb1d465e659 in mongo::(anonymous namespace)::FindCmd::run
*/

/*
RecoveryUnit��װ��wiredTiger�������RecoveryUnit::_txnOpen ��Ӧ��WT���beginTransaction�� 
RecoveryUnit::_txnClose��װ��WT���commit_transaction��rollback_transaction��
*/ //�����ύ������ع�
void WiredTigerRecoveryUnit::_txnClose(bool commit) {
    invariant(_active);
    WT_SESSION* s = _session->getSession();
    if (_timer) {
        const int transactionTime = _timer->millis();
        if (transactionTime >= serverGlobalParams.slowMS) {
            LOG(kSlowTransactionSeverity) << "Slow WT transaction. Lifetime of SnapshotId "
                                          << _mySnapshotId << " was " << transactionTime << "ms";
        }
    }

    int wtRet;
    if (commit) {
        wtRet = s->commit_transaction(s, NULL);
        LOG(3) << "WT commit_transaction for snapshot id " << _mySnapshotId;
    } else {
        wtRet = s->rollback_transaction(s, NULL);
        invariant(!wtRet);
        LOG(3) << "WT rollback_transaction for snapshot id " << _mySnapshotId;
    }

    if (_isTimestamped) {
        _oplogManager->triggerJournalFlush();
        _isTimestamped = false;
    }
    invariantWTOK(wtRet);

    _active = false;
    _mySnapshotId = nextSnapshotId.fetchAndAdd(1);
    _isOplogReader = false;
}

SnapshotId WiredTigerRecoveryUnit::getSnapshotId() const {
    // TODO: use actual wiredtiger txn id
    //WiredTigerRecoveryUnit::WiredTigerRecoveryUnit��WiredTigerRecoveryUnit::_txnClose���ID����
    return SnapshotId(_mySnapshotId);
}

Status WiredTigerRecoveryUnit::setReadFromMajorityCommittedSnapshot() {
    auto snapshotName = _sessionCache->snapshotManager().getMinSnapshotForNextCommittedRead();
    if (!snapshotName) {
        return {ErrorCodes::ReadConcernMajorityNotAvailableYet,
                "Read concern majority reads are currently not possible."};
    }

    _majorityCommittedSnapshot = *snapshotName;
    _readFromMajorityCommittedSnapshot = true;
    return Status::OK();
}

boost::optional<Timestamp> WiredTigerRecoveryUnit::getMajorityCommittedSnapshot() const {
    if (!_readFromMajorityCommittedSnapshot)
        return {};
    return _majorityCommittedSnapshot;
}

//WiredTigerRecoveryUnit::getSession��ִ��,��ȡһ��session,��begin_transaction
/*
RecoveryUnit��װ��wiredTiger�������RecoveryUnit::_txnOpen ��Ӧ��WT���beginTransaction��  
RecoveryUnit::_txnClose��װ��WT���commit_transaction��rollback_transaction��
*/
//WiredTigerRecoveryUnit::getSession��һ�λ�ȡsession��ʱ����ã���_ensureSession��ȷ��_session
void WiredTigerRecoveryUnit::_txnOpen() { //begin_transaction�ڸú�����
    invariant(!_active);
    _ensureSession();

    // Only start a timer for transaction's lifetime if we're going to log it.
    if (shouldLog(kSlowTransactionSeverity)) {
        _timer.reset(new Timer());
    }

	//Ҳ���ǻ�ȡWiredTigerSession._session��ͨ��WiredTigerSession���WT_SESSION* getSession()��ȡ
    WT_SESSION* session = _session->getSession(); 
    if (_readAtTimestamp != Timestamp::min()) {
        uassertStatusOK(_sessionCache->snapshotManager().beginTransactionAtTimestamp(
            _readAtTimestamp, session));
    } else if (_readFromMajorityCommittedSnapshot) {
        _majorityCommittedSnapshot =
            _sessionCache->snapshotManager().beginTransactionOnCommittedSnapshot(session);
    } else if (_isOplogReader) {
        _sessionCache->snapshotManager().beginTransactionOnOplog(
            _sessionCache->getKVEngine()->getOplogManager(), session);
    } else {
        invariantWTOK(session->begin_transaction(session, NULL));  //begin_transaction
    }

    LOG(3) << "WiredTigerRecoveryUnit::_txnOpen WT begin_transaction for snapshot id " << _mySnapshotId;
    _active = true;
}

/*
�� Server ���յ�һ�� insert �����󣬻���ǰ���� LocalOplogInfo::getNextOpTimes() �����伴��Ҫд�� 
oplog entry ���� ts ֵ����ȡ��� ts ����Ҫ�����ģ����Ⲣ����д��������ͬ���� ts��Ȼ�� Server ��
����� WiredTigerRecoveryUnit::setTimestamp ���� WiredTiger ���������񣬲��Ұ���������к���д��
���� commit_ts ������Ϊ oplog entry �� ts��insert �����������ִ����ɺ󣬻�����Ӧ�� oplog entry 
Ҳͨ��ͬһ����д�� WiredTiger Table �У�֮��������ύ��

Ҳ����˵ MongoDB ��ͨ����д oplog ��д�����ŵ�ͬһ�������У�����֤������־��ʵ������֮���һ���ԣ�
ͬʱҲȷ���ˣ�oplog entry ts ��д���������������޸ĵİ汾����һ�µġ�
�ο�: https://mongoing.com/archives/77853
*/

//commit_timestamp��ָ���Ǳ�������ڵ�Ӧ�õ�����־��timestamp
//WiredTigerRecordStore::_insertRecords  oplogDiskLocRegister�е���ִ��
Status WiredTigerRecoveryUnit::setTimestamp(Timestamp timestamp) {
    _ensureSession();
    LOG(3) << "WT set timestamp of future write operations to " << timestamp;
    WT_SESSION* session = _session->getSession();
    invariant(_inUnitOfWork);

    // Starts the WT transaction associated with this session.
    getSession();

    const std::string conf = "commit_timestamp=" + integerToHex(timestamp.asULL());
    auto rc = session->timestamp_transaction(session, conf.c_str());
    if (rc == 0) {
        _isTimestamped = true;
    }
    return wtRCToStatus(rc, "timestamp_transaction");
}

//���WiredTigerRecoveryUnit::_txnOpen   WiredTigerRecoveryUnit::setTimestamp�Ķ�
//waitForReadConcern�е���
Status WiredTigerRecoveryUnit::selectSnapshot(Timestamp timestamp) {
    _readAtTimestamp = timestamp;
    return Status::OK();
}

void WiredTigerRecoveryUnit::setIsOplogReader() {
    // Note: it would be nice to assert !active here, but OplogStones currently opens a cursor on
    // the oplog while the recovery unit is already active.
    _isOplogReader = true;
}

void WiredTigerRecoveryUnit::beginIdle() {
    // Close all cursors, we don't want to keep any old cached cursors around.
    if (_session) {
        _session->closeAllCursors("");
    }
}

/*
[root@bogon mongo]# grep "WiredTigerCursor curwrap(" * -r
db/storage/wiredtiger/wiredtiger_index.cpp:    WiredTigerCursor curwrap(_uri, _tableId, false, opCtx);
db/storage/wiredtiger/wiredtiger_index.cpp:    WiredTigerCursor curwrap(_uri, _tableId, false, opCtx);
db/storage/wiredtiger/wiredtiger_index.cpp:    WiredTigerCursor curwrap(_uri, _tableId, false, opCtx);
db/storage/wiredtiger/wiredtiger_index.cpp:    WiredTigerCursor curwrap(_uri, _tableId, false, opCtx);
db/storage/wiredtiger/wiredtiger_record_store.cpp:    WiredTigerCursor curwrap(_uri, _tableId, true, opCtx);
db/storage/wiredtiger/wiredtiger_record_store.cpp:    WiredTigerCursor curwrap(_uri, _tableId, true, opCtx); //WiredTigerCursor::WiredTigerCursor
db/storage/wiredtiger/wiredtiger_record_store.cpp:        WiredTigerCursor curwrap(_uri, _tableId, true, opCtx);
db/storage/wiredtiger/wiredtiger_record_store.cpp:    WiredTigerCursor curwrap(_uri, _tableId, true, opCtx);
db/storage/wiredtiger/wiredtiger_record_store.cpp:    WiredTigerCursor curwrap(_uri, _tableId, true, opCtx);
db/storage/wiredtiger/wiredtiger_record_store.cpp:    WiredTigerCursor curwrap(_uri, _tableId, true, opCtx);
db/storage/wiredtiger/wiredtiger_util.cpp:    WiredTigerCursor curwrap("metadata:create", WiredTigerSession::kMetadataTableId, false, opCtx);
*/
// ---------------------
//����WiredTigerRecordStore::_insertRecords�����
WiredTigerCursor::WiredTigerCursor(const std::string& uri,
                                   uint64_t tableId,
                                   bool forRecordStore,
                                   OperationContext* opCtx) {
    _tableID = tableId;
	//WiredTigerRecoveryUnit* get
    _ru = WiredTigerRecoveryUnit::get(opCtx); //��ȡopCtx��Ӧ��RecoveryUnit
    _session = _ru->getSession(); //WiredTigerRecoveryUnit::getSession
    //����WiredTigerSession::getCursor->(session->open_cursor)��ȡwiredtiger cursor
    _cursor = _session->getCursor(uri, tableId, forRecordStore); //����tableId��uri��ȡ��Ӧ��cursor
    if (!_cursor) {
        error() << "no cursor for uri: " << uri;
    }
}

WiredTigerCursor::~WiredTigerCursor() {
    _session->releaseCursor(_tableID, _cursor); //WiredTigerSession::releaseCursor
    _cursor = NULL;
}

void WiredTigerCursor::reset() {
    invariantWTOK(_cursor->reset(_cursor));
}
}
