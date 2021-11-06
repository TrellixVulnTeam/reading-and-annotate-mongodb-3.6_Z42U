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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/s/operation_sharding_state.h"

#include "mongo/db/operation_context.h"

namespace mongo {

namespace {

const OperationContext::Decoration<OperationShardingState> shardingMetadataDecoration =
    OperationContext::declareDecoration<OperationShardingState>();

// Max time to wait for the migration critical section to complete
const Milliseconds kMaxWaitForMigrationCriticalSection = Minutes(5);

}  // namespace

OperationShardingState::OperationShardingState() = default;
//���Բο�OperationShardingState::get
OperationShardingState& OperationShardingState::get(OperationContext* opCtx) {
    return shardingMetadataDecoration(opCtx);
}

//execCommandDatabase����
void OperationShardingState::setAllowImplicitCollectionCreation(
    const BSONElement& allowImplicitCollectionCreationElem) {
    if (!allowImplicitCollectionCreationElem.eoo()) {
        _allowImplicitCollectionCreation = allowImplicitCollectionCreationElem.Bool();
    } else {
        _allowImplicitCollectionCreation = true;
    }
}

bool OperationShardingState::allowImplicitCollectionCreation() const {
    return _allowImplicitCollectionCreation;
}

//execCommandDatabase���ã�������mongos���͹�����shardVersion��ϢshardVersion: [ Timestamp(21301, 0), ObjectId('605d4d753af7fbacfbf5544c') ]
void OperationShardingState::initializeShardVersion(NamespaceString nss,
                                                    const BSONElement& shardVersionElt) {
    invariant(!hasShardVersion());

    if (shardVersionElt.eoo() || shardVersionElt.type() != BSONType::Array) {
        return;
    }

    const BSONArray versionArr(shardVersionElt.Obj());
    bool hasVersion = false;
	//��ȡshardversion��Ϣ
    ChunkVersion newVersion = ChunkVersion::fromBSON(versionArr, &hasVersion);

    if (!hasVersion) {
        return;
    }

    if (nss.isSystemDotIndexes()) {
        setShardVersion(std::move(nss), ChunkVersion::IGNORED());
        return;
    }

	//�����OperationShardingState::setShardVersion
    setShardVersion(std::move(nss), std::move(newVersion));
}

//execCommandDatabase����
bool OperationShardingState::hasShardVersion() const {
    return _hasVersion;
}

//CollectionShardingState::_checkShardVersionOk�е��ã���ȡ��mongos��ȡ��shardversion��Ϣ��Ȼ��
//��CollectionShardingState::_checkShardVersionOk�뱾��Ԫ����version���Ƚ�
ChunkVersion OperationShardingState::getShardVersion(const NamespaceString& nss) const {
    if (_ns != nss) {
        return ChunkVersion::UNSHARDED();
    }

    return _shardVersion;
}

//OperationShardingState::initializeShardVersion���ã���������mongos��ȡ����shardVersion��Ϣ�洢��_shardVersion
void OperationShardingState::setShardVersion(NamespaceString nss, ChunkVersion newVersion) {
    // This currently supports only setting the shard version for one namespace.
    invariant(!_hasVersion || _ns == nss);
    invariant(!nss.isSystemDotIndexes() || ChunkVersion::isIgnoredVersion(newVersion));

    _ns = std::move(nss);
    _shardVersion = std::move(newVersion);
    _hasVersion = true;
}

void OperationShardingState::unsetShardVersion(NamespaceString nss) {
    invariant(!_hasVersion || _ns == nss);
    _clear();
}

bool OperationShardingState::waitForMigrationCriticalSectionSignal(OperationContext* opCtx) {
    // Must not block while holding a lock
    invariant(!opCtx->lockState()->isLocked());

    if (_migrationCriticalSectionSignal) {
        _migrationCriticalSectionSignal->waitFor(
            opCtx,
            opCtx->hasDeadline()
                ? std::min(opCtx->getRemainingMaxTimeMillis(), kMaxWaitForMigrationCriticalSection)
                : kMaxWaitForMigrationCriticalSection);
        _migrationCriticalSectionSignal = nullptr;
        return true;
    }

    return false;
}

void OperationShardingState::setMigrationCriticalSectionSignal(
    std::shared_ptr<Notification<void>> critSecSignal) {
    invariant(critSecSignal);
    _migrationCriticalSectionSignal = std::move(critSecSignal);
}

void OperationShardingState::_clear() {
    _hasVersion = false;
    _shardVersion = ChunkVersion();
    _ns = NamespaceString();
}

}  // namespace mongo
