/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/replset_dist_lock_manager.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/service_context.h"
#include "mongo/s/catalog/dist_lock_catalog.h"
#include "mongo/s/catalog/type_lockpings.h"
#include "mongo/s/catalog/type_locks.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {

MONGO_FP_DECLARE(setDistLockTimeout);

using std::string;
using std::unique_ptr;

namespace {

// How many times to retry acquiring the lock after the first attempt fails
//��ȡlock���Դ���
const int kMaxNumLockAcquireRetries = 2;

// How frequently to poll the distributed lock when it is found to be locked
//���Ի�ȡlock��Ƶ��
const Milliseconds kLockRetryInterval(500);

}  // namespace

const Seconds ReplSetDistLockManager::kDistLockPingInterval{30};
const Minutes ReplSetDistLockManager::kDistLockExpirationTime{15};

//makeCatalogClient�й���ʹ��
ReplSetDistLockManager::ReplSetDistLockManager(ServiceContext* globalContext,
                                               StringData processID,
                                               unique_ptr<DistLockCatalog> catalog,
                                               Milliseconds pingInterval,
                                               Milliseconds lockExpiration)
    : _serviceContext(globalContext),
    //generateDistLockProcessId����
      _processID(processID.toString()),
      //��ӦDistLockCatalogImpl
      _catalog(std::move(catalog)),
      //ReplSetDistLockManager::kDistLockPingInterval   ping����
      _pingInterval(pingInterval),
      // ReplSetDistLockManager::kDistLockExpirationTime  ��ռ���ʱ��
      _lockExpiration(lockExpiration) {}

ReplSetDistLockManager::~ReplSetDistLockManager() = default;

void ReplSetDistLockManager::startUp() {
    if (!_execThread) {
        _execThread = stdx::make_unique<stdx::thread>(&ReplSetDistLockManager::doTask, this);
    }
}

void ReplSetDistLockManager::shutDown(OperationContext* opCtx) {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _isShutDown = true;
        _shutDownCV.notify_all();
    }

    // Don't grab _mutex, otherwise will deadlock trying to join. Safe to read
    // _execThread since it is modified only at statrUp().
    if (_execThread && _execThread->joinable()) {
        _execThread->join();
        _execThread.reset();
    }

	//DistLockCatalogImpl::stopPing
	//��config.pings���Ƴ�id:processId������¼
    auto status = _catalog->stopPing(opCtx, _processID);
    if (!status.isOK()) {
        warning() << "error encountered while cleaning up distributed ping entry for " << _processID
                  << causedBy(redact(status));
    }
}

std::string ReplSetDistLockManager::getProcessID() {
    return _processID;
}

bool ReplSetDistLockManager::isShutDown() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _isShutDown;
}
/* 
�߳�ջ��ӡ����:
[root@XX ~]# pstack  438787
Thread 1 (process 438787):
#0  0x00007f92b9e2b965 in pthread_cond_wait@@GLIBC_2.3.2 () from /lib64/libpthread.so.0
#1  0x0000555ca0f46bdc in std::condition_variable::wait(std::unique_lock<std::mutex>&) ()
#2  0x0000555ca084936b in mongo::executor::ThreadPoolTaskExecutor::wait(mongo::executor::TaskExecutor::CallbackHandle const&) ()
#3  0x0000555ca057e422 in mongo::ShardRemote::_runCommand(mongo::OperationContext*, mongo::ReadPreferenceSetting const&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, mongo::Duration<std::ratio<1l, 1000l> >, mongo::BSONObj const&) ()
#4  0x0000555ca05bce24 in mongo::Shard::runCommandWithFixedRetryAttempts(mongo::OperationContext*, mongo::ReadPreferenceSetting const&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, mongo::BSONObj const&, mongo::Duration<std::ratio<1l, 1000l> >, mongo::Shard::RetryPolicy) ()
#5  0x0000555ca027b985 in mongo::DistLockCatalogImpl::ping(mongo::OperationContext*, mongo::StringData, mongo::Date_t) ()
#6  0x0000555ca0273aba in mongo::ReplSetDistLockManager::doTask() ()
#7  0x0000555ca0f439f0 in ?? ()
#8  0x00007f92b9e27dd5 in start_thread () from /lib64/libpthread.so.0
#9  0x00007f92b9b50ead in clone () from /lib64/libc.so.6
[root@XX ~]# 
[root@XX ~]# 
[root@XX ~]# 
[root@XX ~]# pstack  438787
Thread 1 (process 438787):
#0  0x00007f92b9e2bd12 in pthread_cond_timedwait@@GLIBC_2.3.2 () from /lib64/libpthread.so.0
#1  0x0000555ca0274184 in mongo::ReplSetDistLockManager::doTask() ()
#2  0x0000555ca0f439f0 in ?? ()
#3  0x00007f92b9e27dd5 in start_thread () from /lib64/libpthread.so.0
#4  0x00007f92b9b50ead in clone () from /lib64/libc.so.6

*/
//mongos mongod cfg�������˸��߳�
void ReplSetDistLockManager::doTask() {
	//mongod��ӡ I SHARDING [thread1] I SHARDING [thread1] creating distributed lock ping thread for process bjcp4287:20001:1581573577:-6950517477465643150 (sleeping for 30000ms)
	//config��ӡ I SHARDING [thread1] creating distributed lock ping thread for process ConfigServer (sleeping for 30000ms)
	LOG(0) << "creating distributed lock ping thread for process " << _processID
           << " (sleeping for " << _pingInterval << ")";

    Timer elapsedSincelastPing(_serviceContext->getTickSource());
    Client::initThread("replSetDistLockPinger");

    while (!isShutDown()) {
        {
            auto opCtx = cc().makeOperationContext();
/*
2020-07-09T11:21:00.163+0800 I COMMAND  [replSetDistLockPinger] command config.lockpings 
command: findAndModify { findAndModify: "lockpings", query: { _id: "ConfigServer" }, update: 
{ $set: { ping: new Date(1594264859964) } }, upsert: true, writeConcern: { w: "majority", wtimeout: 
15000 }, $db: "config" } planSummary: IDHACK keysExamined:1 docsExamined:1 nMatched:1 nModified:1 
keysInserted:1 keysDeleted:1 numYields:0 reslen:322 locks:{ Global: { acquireCount: { r: 2, w: 2 } }, 
Database: { acquireCount: { w: 2 } }, Collection: { acquireCount: { w: 1 } }, oplog: { acquireCount: 
{ w: 1 } } } protocol:op_msg 199ms
*/
			//DistLockCatalogImpl::ping
            auto pingStatus = _catalog->ping(opCtx.get(), _processID, Date_t::now());

            if (!pingStatus.isOK() && pingStatus != ErrorCodes::NotMaster) {
                warning() << "pinging failed for distributed lock pinger" << causedBy(pingStatus);
            }

			//Ҳ����ѭ��ִ��һ�ε�ʱ��
            const Milliseconds elapsed(elapsedSincelastPing.millis());
            if (elapsed > 10 * _pingInterval) {
                warning() << "Lock pinger for proc: " << _processID << " was inactive for "
                          << elapsed << " ms";
            }
			//elapsedSincelastPingʱ������
            elapsedSincelastPing.reset();

            std::deque<std::pair<DistLockHandle, boost::optional<std::string>>> toUnlockBatch;
            {
				//����
                stdx::unique_lock<stdx::mutex> lk(_mutex);
                toUnlockBatch.swap(_unlockList);
            }

			//ReplSetDistLockManager::lockWithSessionID��ȡ����ʱ����ֵļ����쳣�����Ҫ����ͳһ����
            for (const auto& toUnlock : toUnlockBatch) {
                std::string nameMessage = "";
                Status unlockStatus(ErrorCodes::NotYetInitialized,
                                    "status unlock not initialized!");
                if (toUnlock.second) {
                    // A non-empty _id (name) field was provided, unlock by ts (sessionId) and _id.
                    //DistLockCatalogImpl::unlock
                    unlockStatus = _catalog->unlock(opCtx.get(), toUnlock.first, *toUnlock.second);
                    nameMessage = " and " + LocksType::name() + ": " + *toUnlock.second;
                } else {
                	//DistLockCatalogImpl::unlock
                    unlockStatus = _catalog->unlock(opCtx.get(), toUnlock.first);
                }

                if (!unlockStatus.isOK()) {
                    warning() << "Failed to unlock lock with " << LocksType::lockID() << ": "
                              << toUnlock.first << nameMessage << causedBy(unlockStatus);
					//������ӣ��ȴ��´�ѭ��unlock
					queueUnlock(toUnlock.first, toUnlock.second);
                } else {
                    LOG(0) << "distributed lock with " << LocksType::lockID() << ": "
                           << toUnlock.first << nameMessage << " unlocked.";
                }

                if (isShutDown()) {
                    return;
                }
            }
        }

		//����
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        MONGO_IDLE_THREAD_BLOCK;
		//�ȴ����_pingIntervals,Ĭ��30s,Ҳ����30����cfg����config.lockpings������Ҳ����30s���һ��
        _shutDownCV.wait_for(lk, _pingInterval.toSystemDuration(), [this] { return _isShutDown; });
    }
}

//ReplSetDistLockManager::lockWithSessionID����
//���lockDoc��Ӧ�����Ƿ����
StatusWith<bool> ReplSetDistLockManager::isLockExpired(OperationContext* opCtx,
													   //config.locks�е�lockDoc�ĵ�
                                                       LocksType lockDoc,
                                                       const Milliseconds& lockExpiration) {
	//��ȡlocks���е�LocksType._process��Ϣ
	const auto& processID = lockDoc.getProcess(); 
	//��ȡlockping����ͨ��processID���Ҷ�ӦlockDoc���� 
    auto pingStatus = _catalog->getPing(opCtx, processID);

    Date_t pingValue;
    if (pingStatus.isOK()) {
        const auto& pingDoc = pingStatus.getValue();
		//�������ݵ���Ч�Լ��
        Status pingDocValidationStatus = pingDoc.validate();
        if (!pingDocValidationStatus.isOK()) {
            return {ErrorCodes::UnsupportedFormat,
                    str::stream() << "invalid ping document for " << processID << ": "
                                  << pingDocValidationStatus.toString()};
        }

		//��ȡ��Ӧ���ݵ�ping��Ϣ�������������pingֵ{ "_id" : "ConfigServer", "ping" : ISODate("2020-08-09T10:29:20.291Z") }
        pingValue = pingDoc.getPing();
    } else if (pingStatus.getStatus() != ErrorCodes::NoMatchingDocument) {
        return pingStatus.getStatus();
    }  // else use default pingValue if ping document does not exist.

	//��ʼ��һ����ʱ��
    Timer timer(_serviceContext->getTickSource());
	//��serverstatus�н�����localTime��repl.electionId�ֶΣ���䵽DistLockCatalog::ServerInfo�ṹ
	//DistLockCatalogImpl::getServerInfo
	auto serverInfoStatus = _catalog->getServerInfo(opCtx);
    if (!serverInfoStatus.isOK()) { //�쳣
		////û�����ڵ�
        if (serverInfoStatus.getStatus() == ErrorCodes::NotMaster) {
            return false; //û�����ڵ�
        }

		//ֱ�ӷ���DistLockCatalog::ServerInfo��Ϣ
        return serverInfoStatus.getStatus();
    }

    // Be conservative when determining that lock expiration has elapsed by
    // taking into account the roundtrip delay of trying to get the local
    // time from the config server.
    Milliseconds delay(timer.millis() / 2);  // Assuming symmetrical delay.

	//��ȡDistLockCatalog::ServerInfo��Ϣ
    const auto& serverInfo = serverInfoStatus.getValue();

    stdx::lock_guard<stdx::mutex> lk(_mutex);
	//stdx::unordered_map<std::string, DistLockPingInfo> _pingHistory; 
	//����config.locks���е�_id�ֶ���_pingHistory map���в���
    auto pingIter = _pingHistory.find(lockDoc.getName());//LocksType::getName

	//��_pingHistory���в���û�ҵ�, ˵������һ����Ҫ��ȡ�÷ֲ�ʽ�������񣬰ѱ��λ�ȡ����������Ϣ��¼��_pingHistory����
	//����´������������ȡ�������ͬʱ���ָ������ǻ�ȡʧ�ܣ�����Ҫ����жϸ����Ƿ���ڣ�������ڣ����������Ҫǿ�ƹ����ˡ�
    if (pingIter == _pingHistory.end()) {
        // We haven't seen this lock before so we don't have any point of reference
        // to compare and determine the elapsed time. Save the current ping info
        // for this lock.
        //
        _pingHistory.emplace(std::piecewise_construct,
                             std::forward_as_tuple(lockDoc.getName()),
                             		//Ҳ����config.locks�е�process�ֶΣ���config.lockpings�е�_id�ֶ�
                             std::forward_as_tuple(processID, 
								   //config.lockpings�е�ping�ֶ�����
								   pingValue,  //���浽lastPing�������õ�if (pingInfo->lastPing != pingValue 
	                               //Ҳ����db.serverStatus().localTime
	                               serverInfo.serverTime, 
	                               //config.locks���е�ts�ֶ�
	                               lockDoc.getLockID(),
	                               //db.serverStatus().repl.electionId��ȡ��ֵ
	                               serverInfo.electionId));
        return false;
    }

	////db.serverStatus().localTime - delay;
    auto configServerLocalTime = serverInfo.serverTime - delay;

	//��ȡ��Ӧ��DistLockPingInfo
    auto* pingInfo = &pingIter->second;

    LOG(1) << "checking last ping for lock '" << lockDoc.getName() << "' against last seen process "
           << pingInfo->processId << " and ping " << pingInfo->lastPing;

	// ping is active 
	//��Ⱥ�е�����ڵ��cfgͨ�����������Ի�ÿ��30s����һ�Σ��Ͳ������, Ҳ���Ƕ�Ӧ�ڵ�һֱ���ߣ�
	//����Ҫ��Ӧ�ڵ��Լ�������������ReplSetDistLockManager::doTask
    if (pingInfo->lastPing != pingValue ||  

        // Owner of this lock is now different from last time so we can't
        // use the ping data.
        pingInfo->lockSessionId != lockDoc.getLockID() ||

        // Primary changed, we can't trust that clocks are synchronized so
        // treat as if this is a new entry.
        //˵�������������л�
        pingInfo->electionId != serverInfo.electionId) {
        pingInfo->lastPing = pingValue;
        pingInfo->electionId = serverInfo.electionId;
        pingInfo->configLocalTime = configServerLocalTime;
        pingInfo->lockSessionId = lockDoc.getLockID();
        return false;
    }

	//����ʱ������ʷ��¼���е�configLocalTime��С��ֱ�ӷ���false��һ�㲻������������
    if (configServerLocalTime < pingInfo->configLocalTime) {
        warning() << "config server local time went backwards, from last seen: "
                  << pingInfo->configLocalTime << " to " << configServerLocalTime;
        return false;
    }

	//Ҳ����lockDoc��Ӧ��������ʱ�䳬����lockExpiration(һ���ǳ��и�����ʵ����cfgʧ������)��˵��������ʱ�䳬ʱ��
	//��λ�ȡ��ʧ�ܣ����Ҵӵ�һ�λ�ȡ��ʧ�ܵ����λ�ȡ������ʧ�ܵ�ʱ��������lockExpirationʱ�䣬�������ǿ�ƻ�ȡ����(���ʵ��)
    Milliseconds elapsedSinceLastPing(configServerLocalTime - pingInfo->configLocalTime);
    if (elapsedSinceLastPing >= lockExpiration) {
        LOG(0) << "forcing lock '" << lockDoc.getName() << "' because elapsed time "
               << elapsedSinceLastPing << " >= takeover time " << lockExpiration;
        return true;
    }

	//˵������û�г�ʱ��û�й���
    LOG(1) << "could not force lock '" << lockDoc.getName() << "' because elapsed time "
           << durationCount<Milliseconds>(elapsedSinceLastPing) << " < takeover time "
           << durationCount<Milliseconds>(lockExpiration) << " ms";
    return false;
}

//��ȡ�ֲ�ʽ����waitForΪ���ȶ��
//DistLockManager::lock  MigrationManager::_schedule����
StatusWith<DistLockHandle> ReplSetDistLockManager::lockWithSessionID(OperationContext* opCtx,
                                                                     StringData name,
                                                                     StringData whyMessage,
                                                                     const OID& lockSessionID,
    	                                                                Milliseconds waitFor) {
	//��ʱ��  
	Timer timer(_serviceContext->getTickSource());
    Timer msgTimer(_serviceContext->getTickSource());

    // Counts how many attempts have been made to grab the lock, which have failed with network
    // error. This value is reset for each lock acquisition attempt because these are
    // independent write operations.
    //�����쳣���Դ���
    int networkErrorRetries = 0;

	//��ȡconfigShard��Ϣ
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    // Distributed lock acquisition works by tring to update the state of the lock to 'taken'. If
    // the lock is currently taken, we will back off and try the acquisition again, repeating this
    // until the lockTryInterval has been reached. If a network error occurs at each lock
    // acquisition attempt, the lock acquisition will be retried immediately.
    //��ȡ�ֲ�ʽ������ȡʧ��������
    while (waitFor <= Milliseconds::zero() || Milliseconds(timer.millis()) < waitFor) {
        const string who = str::stream() << _processID << ":" << getThreadName();

		//�����ȡ����ʵ���Ѿ��ܾ�û�к�cfgͨ��lockpings�����ˣ����ʵ������ʧ����������Ҫ���ڼ�飬����һֱ��������lock����ʱ��
        auto lockExpiration = _lockExpiration;
        MONGO_FAIL_POINT_BLOCK(setDistLockTimeout, customTimeout) {
            const BSONObj& data = customTimeout.getData();
            lockExpiration = Milliseconds(data["timeoutMs"].numberInt());
        }
		/*  sh.enableSharding("test") ��Ӧ��ӡ����:
		 D SHARDING [conn13] trying to acquire new distributed lock for test-movePrimary ( lock timeout : 900000 ms, ping interval : 30000 ms, 
		     process : ConfigServer ) with lockSessionID: 5f29454be701d489a5999c54, why: enableSharding
		 D SHARDING [conn13] trying to acquire new distributed lock for test ( lock timeout : 900000 ms, ping interval : 30000 ms, 
		     process : ConfigServer ) with lockSessionID: 5f29454be701d489a5999c58, why: enableShardin
		*/

        LOG(1) << "trying to acquire new distributed lock for " << name
               << " ( lock timeout : " << durationCount<Milliseconds>(lockExpiration)
               << " ms, ping interval : " << durationCount<Milliseconds>(_pingInterval)
               << " ms, process : " << _processID << " )"
               << " with lockSessionID: " << lockSessionID << ", why: " << whyMessage.toString();

		//DistLockCatalogImpl::grabLock ���Ի�ȡ��
        auto lockResult = _catalog->grabLock(
            opCtx, name, lockSessionID, who, _processID, Date_t::now(), whyMessage.toString());

        auto status = lockResult.getStatus();

		//��ȡ���ɹ��������µ�lockSessionID
        if (status.isOK()) {
            // Lock is acquired since findAndModify was able to successfully modify
            // the lock document.

			/*
		     ����enableShard (test)��ӡ��Ϣ����:
		     distributed lock 'test-movePrimary' acquired for 'enableSharding', ts : 5f29344b032e473f1999e552
		     distributed lock 'test' acquired for 'enableSharding', ts : 5f29344b032e473f1999e556
			*/
            log() << "distributed lock '" << name << "' acquired for '" << whyMessage.toString()
                  << "', ts : " << lockSessionID;
            return lockSessionID;
        }

		//��ȡ�ֲ�ʽ��û�гɹ������������Ĵ���
		

        // If a network error occurred, unlock the lock synchronously and try again
		//ShardRemote::isRetriableError ͨ��RemoteCommandRetryScheduler::kAllRetriableErrors��ȡ������
		//˵������������û�����ڵ㣬�����������cfgִ��˵��û�����ڵ㣬cfg������Զ�̵���
		//����һ����������ڵ��ˣ����統ǰ���������л������У�����Ҫ����
		if (configShard->isRetriableError(status.code(), Shard::RetryPolicy::kIdempotent) &&
            networkErrorRetries < kMaxNumLockAcquireRetries) {
            LOG(1) << "Failed to acquire distributed lock because of retriable error. Retrying "
                      "acquisition by first unlocking the stale entry, which possibly exists now"
                   << causedBy(redact(status));

			//�쳣����
            networkErrorRetries++;

			//DistLockCatalogImpl::unlock
			//config.locks���е�{ts:lockSessionID, _id:name}�������ݶ�Ӧ��stat�ֶ�����Ϊ0��Ҳ���ǽ���
            status = _catalog->unlock(opCtx, lockSessionID, name);
            if (status.isOK()) {
                // We certainly do not own the lock, so we can retry
                //�������Ի�ȡlock, ��������Ϊcfgû�����ڵ� 
                continue;
            }

            // Fall-through to the error checking logic below
            invariant(status != ErrorCodes::LockStateChangeFailed);

			//������Զ�û�гɹ���ȡ�������쳣��ӡ
            LOG(1)
                << "Failed to retry acquisition of distributed lock. No more attempts will be made"
                << causedBy(redact(status));
        }

        if (status != ErrorCodes::LockStateChangeFailed) {
			//�ô������extractFindAndModifyNewObj��˵��û����config.locks�����ҵ�name��Ӧ���ĵ�
            // An error occurred but the write might have actually been applied on the
            // other side. Schedule an unlock to clean it up just in case.

			//�ѱ���{ts:lockSessionID, _id:name}��¼�����У���ReplSetDistLockManager::doTask()�м��д����д���
            queueUnlock(lockSessionID, name.toString());
            return status;
        }

        // Get info from current lock and check if we can overtake it.
        //��config.locks�в���id:name�����ݣ����ض�Ӧ��һ��LocksType
        auto getLockStatusResult = _catalog->getLockByName(opCtx, name);
        const auto& getLockStatus = getLockStatusResult.getStatus();

		//û�ҵ������쳣
        if (!getLockStatusResult.isOK() && getLockStatus != ErrorCodes::LockNotFound) {
            return getLockStatus;
        }

        // Note: Only attempt to overtake locks that actually exists. If lock was not
        // found, use the normal grab lock path to acquire it.
        if (getLockStatusResult.isOK()) {
			//��ȡ���ҵ������ݣ�Ҳ����locks�е�һ��LocksType
            auto currentLock = getLockStatusResult.getValue();
			//���λ�ȡ��ʧ�ܣ�˵������ǰ������������ռ�ã����ǿ��Լ����������Ƿ������
			//�����ȡ������ʵ���Ѿ��ܾ�û�к�cfgͨ���ˣ�����ܸ����Ѿ�
            auto isLockExpiredResult = isLockExpired(opCtx, currentLock, lockExpiration);

			//˵��������û�й��ڣ��������쳣(��cfgû��master��)
            if (!isLockExpiredResult.isOK()) {
                return isLockExpiredResult.getStatus();
            }

			//
            if (isLockExpiredResult.getValue() || (lockSessionID == currentLock.getLockID())) {
				//DistLockCatalogImpl::overtakeLock ǿ�ƻ�ȡ��
				//��{id:lockID,state:0} or {id:lockID,ts:currentHolderTS}�������ݸ���Ϊ�µ�{ts:lockSessionID, state:2,who:who,...}
				auto overtakeResult = _catalog->overtakeLock(opCtx,
                                                             name,
                                                             lockSessionID,
                                                             currentLock.getLockID(),
                                                             who,
                                                             _processID,
                                                             Date_t::now(),
                                                             whyMessage);

                const auto& overtakeStatus = overtakeResult.getStatus();

                if (overtakeResult.isOK()) { //��ȡ���ɹ���Ҳ����overtakeLock��update�ɹ�
                    // Lock is acquired since findAndModify was able to successfully modify
                    // the lock document.

                    LOG(0) << "lock '" << name << "' successfully forced";
                    LOG(0) << "distributed lock '" << name << "' acquired, ts : " << lockSessionID;
                    return lockSessionID;
                }

                if (overtakeStatus != ErrorCodes::LockStateChangeFailed) {
                    // An error occurred but the write might have actually been applied on the
                    // other side. Schedule an unlock to clean it up just in case.
                    queueUnlock(lockSessionID, boost::none);
                    return overtakeStatus;
                }
            }
        }

        LOG(1) << "distributed lock '" << name << "' was not acquired.";

		//���ȴ�����ֱ�ӷ���
        if (waitFor == Milliseconds::zero()) {
            break;
        }

        // Periodically message for debugging reasons
        if (msgTimer.seconds() > 10) {
            LOG(0) << "waited " << timer.seconds() << "s for distributed lock " << name << " for "
                   << whyMessage.toString();

            msgTimer.reset();
        }

        // A new lock acquisition attempt will begin now (because the previous found the lock to be
        // busy, so reset the retries counter)
        networkErrorRetries = 0;

        const Milliseconds timeRemaining =
            std::max(Milliseconds::zero(), waitFor - Milliseconds(timer.millis()));
        sleepFor(std::min(kLockRetryInterval, timeRemaining));
    }

    return {ErrorCodes::LockBusy, str::stream() << "timed out waiting for " << name};
}

StatusWith<DistLockHandle> ReplSetDistLockManager::tryLockWithLocalWriteConcern(
    OperationContext* opCtx, StringData name, StringData whyMessage, const OID& lockSessionID) {
    const string who = str::stream() << _processID << ":" << getThreadName();

    LOG(1) << "trying to acquire new distributed lock for " << name
           << " ( lock timeout : " << durationCount<Milliseconds>(_lockExpiration)
           << " ms, ping interval : " << durationCount<Milliseconds>(_pingInterval)
           << " ms, process : " << _processID << " )"
           << " with lockSessionID: " << lockSessionID << ", why: " << whyMessage.toString();

    auto lockStatus = _catalog->grabLock(opCtx,
                                         name,
                                         lockSessionID,
                                         who,
                                         _processID,
                                         Date_t::now(),
                                         whyMessage.toString(),
                                         DistLockCatalog::kLocalWriteConcern);

    if (lockStatus.isOK()) {
        log() << "distributed lock '" << name << "' acquired for '" << whyMessage.toString()
              << "', ts : " << lockSessionID;
        return lockSessionID;
    }

    LOG(1) << "distributed lock '" << name << "' was not acquired.";

    if (lockStatus == ErrorCodes::LockStateChangeFailed) {
        return {ErrorCodes::LockBusy, str::stream() << "Unable to acquire " << name};
    }

    return lockStatus.getStatus();
}

void ReplSetDistLockManager::unlock(OperationContext* opCtx, const DistLockHandle& lockSessionID) {
    auto unlockStatus = _catalog->unlock(opCtx, lockSessionID);

    if (!unlockStatus.isOK()) {
        queueUnlock(lockSessionID, boost::none);
    } else {
        LOG(0) << "distributed lock with " << LocksType::lockID() << ": " << lockSessionID
               << "' unlocked.";
    }
}

void ReplSetDistLockManager::unlock(OperationContext* opCtx,
                                    const DistLockHandle& lockSessionID,
                                    StringData name) {
    auto unlockStatus = _catalog->unlock(opCtx, lockSessionID, name);

    if (!unlockStatus.isOK()) {
        queueUnlock(lockSessionID, name.toString());
    } else {
        LOG(0) << "distributed lock with " << LocksType::lockID() << ": '" << lockSessionID
               << "' and " << LocksType::name() << ": '" << name.toString() << "' unlocked.";
    }
}

void ReplSetDistLockManager::unlockAll(OperationContext* opCtx, const std::string& processID) {
    Status status = _catalog->unlockAll(opCtx, processID);
    if (!status.isOK()) {
        warning() << "Error while trying to unlock existing distributed locks"
                  << causedBy(redact(status));
    }
}

Status ReplSetDistLockManager::checkStatus(OperationContext* opCtx,
                                           const DistLockHandle& lockHandle) {
    return _catalog->getLockByTS(opCtx, lockHandle).getStatus();
}

//ReplSetDistLockManager::lockWithSessionID���ã����쳣{ts:lockSessionID, _id:name}��¼��_unlockList
//��ReplSetDistLockManager::doTask()�м��д���
void ReplSetDistLockManager::queueUnlock(const DistLockHandle& lockSessionID,
                                         const boost::optional<std::string>& name) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _unlockList.push_back(std::make_pair(lockSessionID, name));
}

}  // namespace mongo
