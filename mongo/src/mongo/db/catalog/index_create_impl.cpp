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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_create_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/audit.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_parameters.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/quick_exit.h"

namespace mongo {

namespace {
MONGO_INITIALIZER(InitializeMultiIndexBlockFactory)(InitializerContext* const) {
    MultiIndexBlock::registerFactory(
        [](OperationContext* const opCtx, Collection* const collection) {
            return stdx::make_unique<MultiIndexBlockImpl>(opCtx, collection);
        });
    return Status::OK();
}
}  // namespace

using std::unique_ptr;
using std::string;
using std::endl;

MONGO_FP_DECLARE(crashAfterStartingIndexBuild);
MONGO_FP_DECLARE(hangAfterStartingIndexBuild);
MONGO_FP_DECLARE(hangAfterStartingIndexBuildUnlocked);

/*
�ο�https://docs.mongodb.com/manual/reference/parameters/#param.maxIndexBuildMemoryUsageMegabytes
Limits the amount of memory that simultaneous index builds on one collection may consume 
for the duration of the builds. The specified amount of memory is shared between all indexes 
built using a single createIndexes command or its shell helper db.collection.createIndexes().

The memory consumed by an index build is separate from the WiredTiger cache memory (see cacheSizeGB).
//ͬʱ������������������������ֻ��ʹ����ô���ڴ棬
//�����������ڴ�����Ϊ500M��ͬʱ����5����������ÿ���������ʹ��100M
maxIndexBuildMemoryUsageMegabytes  ����


���⣺һ��������Դ��������������Щ�������ʹ��500M�ڴ�
��ô����Ҳ�ͬ�����Ⱥ󴴽�����������������Ⱥ󴴽���5��������5�������ܹ�������5*500M����Ҳֱ��500M��
����500*5,��Ϊ�Ƕ�������д����ģ����Դ���������ʱ����ͬʱ��ӣ������ڴ�OOM

�Ӷ��������ͬʱ�ܣ�����־��
2021-03-11T11:23:25.001+0800 I -        [conn1307219]   Index Build (background): 242400/53092096 0%
2021-03-11T11:23:28.001+0800 I -        [conn1320324]   Index Build (background): 8140200/53092096 15%
2021-03-11T11:23:28.001+0800 I -        [conn1307219]   Index Build (background): 510700/53092096 0%
2021-03-11T11:23:31.001+0800 I -        [conn1307219]   Index Build (background): 801300/53092096 1%
2021-03-11T11:23:31.001+0800 I -        [conn1320324]   Index Build (background): 8394200/53092096 15%
*/
AtomicInt32 maxIndexBuildMemoryUsageMegabytes(500);

class ExportedMaxIndexBuildMemoryUsageParameter
    : public ExportedServerParameter<std::int32_t, ServerParameterType::kStartupAndRuntime> {
public:
    ExportedMaxIndexBuildMemoryUsageParameter()
        : ExportedServerParameter<std::int32_t, ServerParameterType::kStartupAndRuntime>(
              ServerParameterSet::getGlobal(),
              "maxIndexBuildMemoryUsageMegabytes",
              &maxIndexBuildMemoryUsageMegabytes) {}

    virtual Status validate(const std::int32_t& potentialNewValue) {
        if (potentialNewValue < 100) {
            return Status(
                ErrorCodes::BadValue,
                "maxIndexBuildMemoryUsageMegabytes must be greater than or equal to 100 MB");
        }

        return Status::OK();
    }

} exportedMaxIndexBuildMemoryUsageParameter;


/**
 * On rollback sets MultiIndexBlockImpl::_needToCleanup to true.
 */
class MultiIndexBlockImpl::SetNeedToCleanupOnRollback : public RecoveryUnit::Change {
public:
    explicit SetNeedToCleanupOnRollback(MultiIndexBlockImpl* indexer) : _indexer(indexer) {}

    virtual void commit() {}
    virtual void rollback() {
        _indexer->_needToCleanup = true;
    }

private:
    MultiIndexBlockImpl* const _indexer;
};

/**
 * On rollback in init(), cleans up _indexes so that ~MultiIndexBlock doesn't try to clean
 * up _indexes manually (since the changes were already rolled back).
 * Due to this, it is thus legal to call init() again after it fails.
 */
class MultiIndexBlockImpl::CleanupIndexesVectorOnRollback : public RecoveryUnit::Change {
public:
    explicit CleanupIndexesVectorOnRollback(MultiIndexBlockImpl* indexer) : _indexer(indexer) {}

    virtual void commit() {}
    virtual void rollback() {
        _indexer->_indexes.clear();
    }

private:
    MultiIndexBlockImpl* const _indexer;
};

MultiIndexBlockImpl::MultiIndexBlockImpl(OperationContext* opCtx, Collection* collection)
    : _collection(collection),
      _opCtx(opCtx),
      _buildInBackground(false),
      _allowInterruption(false),
      _ignoreUnique(false),
      _needToCleanup(true) {}

//�����������
MultiIndexBlockImpl::~MultiIndexBlockImpl() {
    if (!_needToCleanup || _indexes.empty())
        return;
    while (true) {
        try {
            WriteUnitOfWork wunit(_opCtx);
            // This cleans up all index builds.
            // Because that may need to write, it is done inside
            // of a WUOW. Nothing inside this block can fail, and it is made fatal if it does.
            for (size_t i = 0; i < _indexes.size(); i++) {
				//IndexCatalogImpl::IndexBuildBlock::fail()
                _indexes[i].block->fail();
            }
            wunit.commit();
            return;
        } catch (const WriteConflictException&) {
            continue;
        } catch (const DBException& e) {
            if (e.toStatus() == ErrorCodes::ExceededMemoryLimit)
                continue;
            error() << "Caught exception while cleaning up partially built indexes: " << redact(e);
        } catch (const std::exception& e) {
            error() << "Caught exception while cleaning up partially built indexes: " << e.what();
        } catch (...) {
            error() << "Caught unknown exception while cleaning up partially built indexes.";
        }
        fassertFailed(18644);
    }
}

//���specs�е�������_collection���Ѿ����ڣ����specs��ɾ���������
void MultiIndexBlockImpl::removeExistingIndexes(std::vector<BSONObj>* specs) const {
    for (size_t i = 0; i < specs->size(); i++) {
        Status status =
			//IndexCatalogImpl::prepareSpecForCreate
            _collection->getIndexCatalog()->prepareSpecForCreate(_opCtx, (*specs)[i]).getStatus();
        if (status.code() == ErrorCodes::IndexAlreadyExists) {
            specs->erase(specs->begin() + i);
            i--;
        }
        // intentionally ignoring other error codes
    }
}

/*
 build index on: xx.xxx properties: { v: 2, key: { messageId: 1.0, pushNoShow: 1.0 }, name: "messageId_1_pushNoShow_1", ns: "push_stat.opush_message_report", background: true }
2021-03-10T17:32:21.001+0800 I -        [conn1366036]   Index Build (background): 343800/53092096 0%
2021-03-10T17:32:21.001+0800 I -        [conn1366036]   Index Build (background): 343800/53092096 0%
������������
2021-03-10T17:38:54.001+0800 I -        [conn1366036]   Index Build (background): 50631100/53092096 95%
2021-03-10T17:38:57.001+0800 I -        [conn1366036]   Index Build (background): 51048600/53092096 96%
2021-03-10T17:39:00.001+0800 I -        [conn1366036]   Index Build (background): 51485100/53092096 96%
2021-03-10T17:39:03.000+0800 I -        [conn1366036]   Index Build (background): 51916000/53092096 97%
2021-03-10T17:39:06.001+0800 I -        [conn1366036]   Index Build (background): 52334600/53092096 98%
2021-03-10T17:39:09.002+0800 I -        [conn1366036]   Index Build (background): 52772500/53092096 99%
2021-03-10T17:39:12.002+0800 I -        [conn1366036]   Index Build (background): 53195800/53092096 100%
2021-03-10T17:39:15.000+0800 I -        [conn1366036]   Index Build (background): 53537600/53092096 100%
2021-03-10T17:39:18.001+0800 I -        [conn1366036]   Index Build (background): 53948100/53092096 101%
2021-03-10T17:39:21.000+0800 I -        [conn1366036]   Index Build (background): 54359900/53092096 102%
2021-03-10T17:39:21.177+0800 I INDEX    [conn1366036] build index done.  scanned 54386432 total records. 422 secs
2021-03-10T17:39:21.177+0800 D INDEX    [conn1366036] marking index messageId_1_pushNoShow_1 as ready in snapshot id 207669429993

*/

/*
db.runCommand(
  {
    createIndexes: <collection>,
    indexes: [
        {
            key: {
                <key-value_pair>,
                <key-value_pair>,
                ...
            },
            name: <index_name>,
            <option1>,
            <option2>,
            ...
        },
        { ... },
        { ... }
    ],
    writeConcern: { <write concern> },
    commitQuorum: <int|string>,
    comment: <any>
  }
) 
һ���������ͬʱ�����������
*/


//�������ϵ�ʱ����߳���������ʱ������:DatabaseImpl::createCollection->IndexCatalogImpl::createIndexOnEmptyCollection->IndexCatalogImpl::IndexBuildBlock::init
//MultiIndexBlockImpl::init->IndexCatalogImpl::IndexBuildBlock::init  �������й����У����Ҽ����Ѿ����ڵ�ʱ������

//IndexCatalogImpl::createIndexOnEmptyCollection�е���

//��ĳ��ʵ��������������������ͻȻʵ�����ˣ�mongod���������: 
// restartInProgressIndexesFromLastShutdown->checkNS����


//�������������������ߵ���������  
//CmdCreateIndex::errmsgRun  CmdReIndex::errmsgRun����  
//��������һЩ��ʼ������
StatusWith<std::vector<BSONObj>> 
MultiIndexBlockImpl::init(const BSONObj& spec) {
    const auto indexes = std::vector<BSONObj>(1, spec);
    return init(indexes); //MultiIndexBlockImpl::init
}

//�����MultiIndexBlockImpl::init�е���
//��������һЩ��ʼ������
StatusWith<std::vector<BSONObj>> 
MultiIndexBlockImpl::init(const std::vector<BSONObj>& indexSpecs) {
	//�ŵ�һ��������
    WriteUnitOfWork wunit(_opCtx);

    invariant(_indexes.empty());
    _opCtx->recoveryUnit()->registerChange(new CleanupIndexesVectorOnRollback(this));
	log() << "yang test ...MultiIndexBlockImpl::init";
	
    const string& ns = _collection->ns().ns();
	//��ȡ�ñ��Ӧ��IndexCatalogImpl��Ϣ
    const auto idxCat = _collection->getIndexCatalog();
    invariant(idxCat);
    invariant(idxCat->ok());
	//IndexCatalogImpl::checkUnfinished ����Ƿ���Ϊ�����������
    Status status = idxCat->checkUnfinished();
    if (!status.isOK())
        return status;

    for (size_t i = 0; i < indexSpecs.size(); i++) {
        BSONObj info = indexSpecs[i];

/*db.coll.ensureIndex({"name" : 1}) 
	yang test(MultiIndexBlockImpl::init) ... info:{ ns: "test.coll", v: 2, key: { name: 1.0 }, name: "name_1" }
db.world.ensureIndex({"geometry" : "2dsphere"}) 
	yang test(MultiIndexBlockImpl::init) ... info:{ ns: "test.world", v: 2, key: { geometry: "2dsphere" }, name: "geometry_2dsphere" }
db.things.ensureIndex({name:1}, {background:true}); 
	yang test(MultiIndexBlockImpl::init) ... info:{ ns: "test.things", v: 2, key: { name: 1.0 }, name: "name_1", background: true }
*/
	log() << " yang test(MultiIndexBlockImpl::init) ... info:" << redact(info);
        string pluginName = IndexNames::findPluginName(info["key"].Obj());
        if (pluginName.size()) {
            Status s = _collection->getIndexCatalog()->_upgradeDatabaseMinorVersionIfNeeded(
                _opCtx, pluginName);
            if (!s.isOK())
                return s;
        }

		//�Ƿ��̨����
        // Any foreground indexes make all indexes be built in the foreground.
        _buildInBackground = (_buildInBackground && info["background"].trueValue());
    }

    std::vector<BSONObj> indexInfoObjs;
	//������Ϣ
    indexInfoObjs.reserve(indexSpecs.size());
    std::size_t eachIndexBuildMaxMemoryUsageBytes = 0;
    if (!indexSpecs.empty()) {
		//һ���������ͬʱ������������������������ֻ��ʹ����ô���ڴ棬
		//�����������ڴ�����Ϊ500M��ͬʱ����5����������ÿ���������ʹ��100M
		
        eachIndexBuildMaxMemoryUsageBytes =
            static_cast<std::size_t>(maxIndexBuildMemoryUsageMegabytes.load()) * 1024 * 1024 /
            indexSpecs.size();
    }

    for (size_t i = 0; i < indexSpecs.size(); i++) {
        BSONObj info = indexSpecs[i];
        StatusWith<BSONObj> statusWithInfo =
			//�������� ������ͻ ��������ͻ�ȼ��
            _collection->getIndexCatalog()->prepareSpecForCreate(_opCtx, info);
        Status status = statusWithInfo.getStatus();
		//��������Կ���ǰ��������쳣���������������ִ��
        if (!status.isOK())
            return status;
		//��Ӧ����spec��Ϣ
        info = statusWithInfo.getValue();
        indexInfoObjs.push_back(info);

        IndexToBuild index;

		//IndexToBuild�ำֵ             IndexCatalogImpl::IndexBuildBlock 
        index.block.reset(new IndexCatalogImpl::IndexBuildBlock(_opCtx, _collection, info));
		//��ȡdescriptor��������Ӧ��IndexCatalogEntryImpl��ӵ�_entries����
		//IndexCatalogImpl::IndexBuildBlock::init 
		status = index.block->init();  
        if (!status.isOK())
            return status;

		//��ȡ������Ӧ��accessMethod,Ĭ��Btree_access_method
        index.real = index.block->getEntry()->accessMethod();
		//IndexAccessMethod::initializeAsEmpty
        status = index.real->initializeAsEmpty(_opCtx);
        if (!status.isOK())
            return status;

		//���û�м�backgroud����
        if (!_buildInBackground) {
            // Bulk build process requires foreground building as it assumes nothing is changing
            // under it.
            //IndexAccessMethod::initiateBulk  bulk��ʼ��������һ��BulkBuilder
            index.bulk = index.real->initiateBulk(eachIndexBuildMaxMemoryUsageBytes);
        }

		//��ȡ������ӦIndexDescriptor
        const IndexDescriptor* descriptor = index.block->getEntry()->descriptor();

		//��descriptor��ȡindex.options��Ϣ
        IndexCatalog::prepareInsertDeleteOptions(_opCtx, descriptor, &index.options);
        index.options.dupsAllowed = index.options.dupsAllowed || _ignoreUnique;
        if (_ignoreUnique) {
            index.options.getKeysMode = IndexAccessMethod::GetKeysMode::kRelaxConstraints;
        }

		//build index on: test.world properties: { v: 2, key: { geometry: "2dsphere" }, name: "geometry_2dsphere", ns: "test.world", 2dsphereIndexVersion: 3 }
		
		log() << "build index on: " << ns << " properties: " << descriptor->toString();
        if (index.bulk)
            log() << "\t building index using bulk method; build may temporarily use up to "
                  << eachIndexBuildMaxMemoryUsageBytes / 1024 / 1024 << " megabytes of RAM";

        index.filterExpression = index.block->getEntry()->getFilterExpression();

        // TODO SERVER-14888 Suppress this in cases we don't want to audit.
        audit::logCreateIndex(_opCtx->getClient(), &info, descriptor->indexName(), ns);

		//IndexToBuild��ӵ�_indexes����
        _indexes.push_back(std::move(index));
    }

    if (_buildInBackground)
		//�ñ��̨��������¼��BackgroundOperation
        _backgroundOperation.reset(new BackgroundOperation(ns));

    wunit.commit();

    if (MONGO_FAIL_POINT(crashAfterStartingIndexBuild)) {
        log() << "Index build interrupted due to 'crashAfterStartingIndexBuild' failpoint. Exiting "
                 "after waiting for changes to become durable.";
        Locker::LockSnapshot lockInfo;
        _opCtx->lockState()->saveLockStateAndUnlock(&lockInfo);
        if (_opCtx->recoveryUnit()->waitUntilDurable()) {
            quickExit(EXIT_TEST);
        }
    }

    return indexInfoObjs;
}

/*
 build index on: xx.xx properties: { v: 2, key: { messageId: 1.0, pushNoShow: 1.0 }, name: "messageId_1_pushNoShow_1", ns: "push_stat.opush_message_report", background: true }
2021-03-10T17:32:21.001+0800 I -        [conn1366036]   Index Build (background): 343800/53092096 0%
2021-03-10T17:32:21.001+0800 I -        [conn1366036]   Index Build (background): 343800/53092096 0%
������������
2021-03-10T17:38:54.001+0800 I -        [conn1366036]   Index Build (background): 50631100/53092096 95%
2021-03-10T17:38:57.001+0800 I -        [conn1366036]   Index Build (background): 51048600/53092096 96%
2021-03-10T17:39:00.001+0800 I -        [conn1366036]   Index Build (background): 51485100/53092096 96%
2021-03-10T17:39:03.000+0800 I -        [conn1366036]   Index Build (background): 51916000/53092096 97%
2021-03-10T17:39:06.001+0800 I -        [conn1366036]   Index Build (background): 52334600/53092096 98%
2021-03-10T17:39:09.002+0800 I -        [conn1366036]   Index Build (background): 52772500/53092096 99%
2021-03-10T17:39:12.002+0800 I -        [conn1366036]   Index Build (background): 53195800/53092096 100%
2021-03-10T17:39:15.000+0800 I -        [conn1366036]   Index Build (background): 53537600/53092096 100%
2021-03-10T17:39:18.001+0800 I -        [conn1366036]   Index Build (background): 53948100/53092096 101%
2021-03-10T17:39:21.000+0800 I -        [conn1366036]   Index Build (background): 54359900/53092096 102%
2021-03-10T17:39:21.177+0800 I INDEX    [conn1366036] build index done.  scanned 54386432 total records. 422 secs
2021-03-10T17:39:21.177+0800 D INDEX    [conn1366036] marking index messageId_1_pushNoShow_1 as ready in snapshot id 207669429993

*/

////CmdCreateIndex::errmsgRun����
Status MultiIndexBlockImpl::insertAllDocumentsInCollection(std::set<RecordId>* dupsOut) {
	//���������������´�ӡ:Index Build (background): 13531300/53092096 25%
	const char* curopMessage = _buildInBackground ? "Index Build (background)" : "Index Build";
	//�ñ����ݴ�С CollectionImpl::numRecords
	const auto numRecords = _collection->numRecords(_opCtx);
    stdx::unique_lock<Client> lk(*_opCtx->getClient());
	//���ڴ�ӡ���
    ProgressMeterHolder progress(
        CurOp::get(_opCtx)->setMessage_inlock(curopMessage, curopMessage, numRecords));
    lk.unlock();

    Timer t;

    unsigned long long n = 0;

    PlanExecutor::YieldPolicy yieldPolicy;
	//backgroud��̨����,yield������Ч�ط���PlanYieldPolicy::yield
    if (_buildInBackground) {
        invariant(_allowInterruption);
        yieldPolicy = PlanExecutor::YIELD_AUTO;
    } else {//��PlanYieldPolicy::yield�������ͷ�����Դ��ִ��plan��ʱ��
        yieldPolicy = PlanExecutor::WRITE_CONFLICT_RETRY_ONLY;
    }

	//���ɼ�������Ӧ��PlanExecutor��Ҳ����CollectionScan
    auto exec =
        InternalPlanner::collectionScan(_opCtx, _collection->ns().ns(), _collection, yieldPolicy);

    Snapshotted<BSONObj> objToIndex;
    RecordId loc;
    PlanExecutor::ExecState state;
    int retries = 0;  // non-zero when retrying our last document.
    while (retries ||
			//ͨ��Collection Scan�ķ�ʽ��ȡ����
			//PlanExecutor::getNextSnapshotted  loc��Ӧ���ݵ�key, objToIndex��Ӧ����value
           (PlanExecutor::ADVANCED == (state = exec->getNextSnapshotted(&objToIndex, &loc))) ||
           MONGO_FAIL_POINT(hangAfterStartingIndexBuild)) {
        try {
			//���������������Ƿ���Ա�killop�ɵ�
            if (_allowInterruption)
                _opCtx->checkForInterrupt();

            if (!(retries || (PlanExecutor::ADVANCED == state))) {
                // The only reason we are still in the loop is hangAfterStartingIndexBuild.
                log() << "Hanging index build due to 'hangAfterStartingIndexBuild' failpoint";
                invariant(_allowInterruption);
                sleepmillis(1000);
                continue;
            }

            // Make sure we are working with the latest version of the document.
            //���������Ѿ�ɾ���ˣ���continue����ѭ��
            if (objToIndex.snapshotId() != _opCtx->recoveryUnit()->getSnapshotId() &&
                !_collection->findDoc(_opCtx, loc, &objToIndex)) {
                // doc was deleted so don't index it.
                retries = 0;
                continue;
            }

            // Done before insert so we can retry document if it WCEs.
            //���±��е�����������Ϊ����ļ��������Ȱٷֱȼ�����׼��
            progress->setTotalWhileRunning(_collection->numRecords(_opCtx));

            WriteUnitOfWork wunit(_opCtx);
			//ÿ�����ݶ�Ӧ���������һ������KV������KVд��洢����
            Status ret = insert(objToIndex.value(), loc);
            if (_buildInBackground)
                exec->saveState();
            if (ret.isOK()) {
                wunit.commit();
            } else if (dupsOut && ret.code() == ErrorCodes::DuplicateKey) {
                // If dupsOut is non-null, we should only fail the specific insert that
                // led to a DuplicateKey rather than the whole index build.
                dupsOut->insert(loc);
            } else {
                // Fail the index build hard.
                return ret;
            }
            if (_buildInBackground) {
                auto restoreStatus = exec->restoreState();  // Handles any WCEs internally.
                if (!restoreStatus.isOK()) {
                    return restoreStatus;
                }
            }

            // Go to the next document
            //ProgressMeterHolder::hit
            //���������Ϣ�������������
   			//2021-03-14T14:22:54.000+0800 I -        [conn167]   Index Build: 37460300/54386432 68%
            progress->hit();
            n++;
            retries = 0;
        } catch (const WriteConflictException&) {
            CurOp::get(_opCtx)->debug().writeConflicts++;
            retries++;  // logAndBackoff expects this to be 1 on first call.
            WriteConflictException::logAndBackoff(
                retries, "index creation", _collection->ns().ns());

            // Can't use writeConflictRetry since we need to save/restore exec around call to
            // abandonSnapshot.
            exec->saveState();
            _opCtx->recoveryUnit()->abandonSnapshot();
            auto restoreStatus = exec->restoreState();  // Handles any WCEs internally.
            if (!restoreStatus.isOK()) {
                return restoreStatus;
            }
        }
    }

    uassert(28550,
            "Unable to complete index build due to collection scan failure: " +
                WorkingSetCommon::toStatusString(objToIndex.value()),
            state == PlanExecutor::IS_EOF);

    if (MONGO_FAIL_POINT(hangAfterStartingIndexBuildUnlocked)) {
        // Unlock before hanging so replication recognizes we've completed.
        Locker::LockSnapshot lockInfo;
        _opCtx->lockState()->saveLockStateAndUnlock(&lockInfo);
        while (MONGO_FAIL_POINT(hangAfterStartingIndexBuildUnlocked)) {
            log() << "Hanging index build with no locks due to "
                     "'hangAfterStartingIndexBuildUnlocked' failpoint";
            sleepmillis(1000);
        }
        // If we want to support this, we'd need to regrab the lock and be sure that all callers are
        // ok with us yielding. They should be for BG indexes, but not for foreground.
        invariant(!"the hangAfterStartingIndexBuildUnlocked failpoint can't be turned off");
    }

    progress->finished();

	
    Status ret = doneInserting(dupsOut);
    if (!ret.isOK())
        return ret;

    log() << "build index done.  scanned " << n << " total records. " << t.seconds() << " secs";

    return Status::OK();
}


/*
												  	    \		 (��һ��������server�����������KV����)
												 --------     MultiIndexBlockImpl::insert
											     |        /
	   (��backgroud������ʽ������ע��ִ�в���)	     |
MultiIndexBlockImpl::insertAllDocumentsInCollection---> |
												|	   
												|		 
												|       \       (�ڶ�����ͨ��wt bulkд��洢����)
												---------    MultiIndexBlockImpl::doneInserting->IndexAccessMethod::commitBulk
													    /


*/


//CmdCreateIndex::errmsgRun->MultiIndexBlockImpl::insertAllDocumentsInCollection�е���
//ÿ�����ݶ�Ӧ���������һ������KV������KVд��洢����
Status MultiIndexBlockImpl::insert(const BSONObj& doc, const RecordId& loc) {
	//�������е�����
    for (size_t i = 0; i < _indexes.size(); i++) {
        if (_indexes[i].filterExpression && !_indexes[i].filterExpression->matchesBSON(doc)) {
            continue;
        }

        int64_t unused;
        Status idxStatus(ErrorCodes::InternalError, "");
		//BulkBuilder::insert������ʽ��������  IndexAccessMethod::insert��������ʽ������
        if (_indexes[i].bulk) { //������backgroud��������������֧
        	//BulkBuilder::insert  ����ʽ��ӣ�������KV���ݷ���buf�ڴ��У�����ڴ����ĳ���500M�����ź���д������ļ�
			//�÷�֧������MultiIndexBlockImpl::doneInsertingд��洢����
			idxStatus = _indexes[i].bulk->insert(_opCtx, doc, loc, _indexes[i].options, &unused);
        } else { //backgroud��̨��������������������֧
        	//IndexAccessMethod::insert  
            idxStatus = _indexes[i].real->insert(_opCtx, doc, loc, _indexes[i].options, &unused);
        }

        if (!idxStatus.isOK())
            return idxStatus;
    }
    return Status::OK();
}

/*
												  	    \		 (��һ��������server�����������KV����)
												 --------     MultiIndexBlockImpl::insert
											     |        /
	   (��backgroud������ʽ������ע��ִ�в���)	     |
MultiIndexBlockImpl::insertAllDocumentsInCollection---> |
												|	   
												|		 
												|       \       (�ڶ�����ͨ��wt bulkд��洢����)
												---------    MultiIndexBlockImpl::doneInserting->IndexAccessMethod::commitBulk
													    /


*/


////MultiIndexBlockImpl::insertAllDocumentsInCollection����
Status MultiIndexBlockImpl::doneInserting(std::set<RecordId>* dupsOut) {
    for (size_t i = 0; i < _indexes.size(); i++) {
        if (_indexes[i].bulk == NULL) //��Է�backgroud����
            continue;
        LOG(1) << "\t bulk commit starting for index: "
               << _indexes[i].block->getEntry()->descriptor()->indexName();
		//��������MultiIndexBlockImpl::insert�ӿ��Ķ�
		//IndexAccessMethod::commitBulk��bulk��ʽ����������д������KV���洢����
        Status status = _indexes[i].real->commitBulk(_opCtx,
                                                     std::move(_indexes[i].bulk),
                                                     _allowInterruption,
                                                     _indexes[i].options.dupsAllowed,
                                                     dupsOut);
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

void MultiIndexBlockImpl::abortWithoutCleanup() {
    _indexes.clear();
    _needToCleanup = false;
}

void MultiIndexBlockImpl::commit() {
    for (size_t i = 0; i < _indexes.size(); i++) {
        _indexes[i].block->success();
    }

    _opCtx->recoveryUnit()->registerChange(new SetNeedToCleanupOnRollback(this));
    _needToCleanup = false;
}

}  // namespace mongo
