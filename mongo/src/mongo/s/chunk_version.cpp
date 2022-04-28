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

#include "mongo/platform/basic.h"

#include "mongo/s/chunk_version.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {
/*
3.2�汾�config server ����ǰ�Ķ������ڵ㻻���˸��Ƽ������ɸ��Ƽ������ڸ�����������ԣ�
Sharded Cluster ��ʵ����Ҳ����һЩ��ս��

��ս1�����Ƽ�ԭPrimary �ϵ����ݿ��ܻᷢ���ع����� mongos ���ԣ����ǡ�������·�ɱ�����ֱ��ع��ˡ���
��ս2�����Ƽ����ڵ�����ݱ����ڵ��������������ڵ��϶���������������չ������ӱ��ڵ��϶������ܶ�
�������ݲ������µģ��� mongos ��Ӱ���ǡ����ܶ������ڵ�·�ɱ������������У�mongos �����Լ���·�ɱ�
�汾���ˣ�����ȥ config server ��ȡ���µ�·�ɱ��������ʱ����δ���µı��ڵ��ϣ����ܲ����ܳɹ���
����·�ɱ���


Ӧ�Ե�һ�����⣬MongoDB ��3.2�汾�������� ReadConcern ���Ե�֧�֣�ReadConcern֧�֡�local���͡�majority��2������
local ����ͨ�� read��majority ����֤Ӧ�ö����������Ѿ��ɹ�д�뵽�˸��Ƽ��Ĵ������Ա��
��һ�����ݳɹ�д�뵽�������Ա�����������ݾͿ϶����ᷢ�� rollback��mongos �ڴ� config server ��ȡ����ʱ
����ָ�� readConcern Ϊ majority ����ȷ����ȡ����·����Ϣ�϶����ᱻ�ع���

Mongos ����·�ɱ�汾��Ϣ���� ĳ�� shard��shard�����Լ��İ汾�� mongos �£������� chunk Ǩ�ƣ���
��ʱshard ���˸��� mongos �Լ�Ӧ��ȥ����·�ɱ�������Լ�Ǩ�� chunk ����� config server ʱ�� 
optime����mongos��mongos ���� config server ʱ��ָ�� readConcern ����Ϊ majority����ָ�� 
afterOpTime ��������ȷ������ӱ��ڵ�������ڵ�·�ɱ�



Ӧ�Եڶ������⣬MongoDB ��majority ����Ļ����ϣ������� afterOpTime �Ĳ������������Ŀǰֻ�� Sharded Cluster �ڲ�ʹ�á������������˼�ǡ�������ڵ������oplogʱ���������� afterOpTime ָ����ʱ�������

*/


const char kVersion[] = "version";
/*
mongos> db.chunks.find({ns:"xx.xx"}).limit(1).pretty()
{
        "_id" : "sporthealth.stepsDetail-ssoid_811088201705515807",
        //Lastmod:��һ������major version��һ��movechunk���������Chunk��һ��ShardǨ�Ƶ���һ��Shard�����1��
        //�ڶ���������Minor Version��һ��split����������1��
        "lastmod" : Timestamp(143, 61),
        //lastmodEpoch: epoch : objectID����ʶ���ϵ�Ψһʵ�������ڱ�ʶ�����Ƿ����˱仯��ֻ�е� collection �� drop ���� collection��shardKey����refinedʱ ����������
        "lastmodEpoch" : ObjectId("5f9aa6ec3af7fbacfbc99a27"),
        "ns" : "sporthealth.stepsDetail",
        "min" : {
                "ssoid" : NumberLong("811088201705515807")
        },
        "max" : {
                "ssoid" : NumberLong("811127732696226936")
        },
        "shard" : "sport-health_xyKKIMeg_shard_1"
}
mongos> 
*/
//һ��(majorVersion, minorVersion)�Ķ�Ԫ��)
const char kLastmod[] = "lastmod";

}  // namespace

//2021-05-31T19:29:33.775+0800 I COMMAND  [conn3479863] command sporthealth.stepsDetail command: insert { insert: "stepsDetail", bypassDocumentValidation: false, ordered: true, documents: [ { _id: ObjectId('60b4c89dbba026408eaa55ec'), id: 559306090572558338, clientDataId: "0f015c36e44b4559a2a0e5baa4998d58", ssoid: "212848537", deviceUniqueId: "xxx", deviceType: "Phone", startTimestamp: xxx, endTimestamp: 1622460360000, sportMode: 6, steps: 62, distance: 41, calories: 1499, altitudeOffset: 0, display: 1, syncStatus: 0, workout: 0, modifiedTime: 1622460573621, createTime: new Date(1622460573625), updateTime: new Date(1622460573625), _class: "com.oppo.sporthealthdataprocess.po.StepsDetail" } ], shardVersion: [ Timestamp(33477, 353588), ObjectId('5f9aa6ec3af7fbacfbc99a27') ], lsid: { id: UUID("e8a1985f-c3ff-4b2e-8d6c-5136818c3ba7"), uid: BinData(0, 64A61BF5764A1A00129F0CBAC3D8D4C51E4EAA3B877BF0F06A946E40E9EA172E) }, $clusterTime: { clusterTime: Timestamp(1622460573, 5482), signature: { hash: BinData(0, 4CF2584792D377D057BADF6FE58DE436A017BD13), keyId: 6920984255816273035 } }, $client: { driver: { name: "mongo-java-driver", version: "3.8.2" }, os: { type: "Linux", name: "Linux", architecture: "amd64", version: "3.10.0-957.27.2.el7.x86_64" }, platform: "Java/heytap/1.8.0_252-b09", mongos: { host: "xx.xx:xx", client: "10.xxx.231:xx", version: "3.6.10" } }, $configServerState: { opTime: { ts: Timestamp(1622460570, 3548), t: 5 } }, $db: "sporthealth" } ninserted:1 keysInserted:6 numYields:0 reslen:355 locks:{ Global: { acquireCount: { r: 2, w: 2 } }, Database: { acquireCount: { w: 2 } }, Collection: { acquireCount: { w: 1 } }, oplog: { acquireCount: { w: 1 } } } protocol:op_msg 108ms
//mongos���͵�mongod�������л�Я��shardVersion: shardVersion: [ Timestamp(33477, 353588), ObjectId('5f9aa6ec3af7fbacfbc99a27') ]
const char ChunkVersion::kShardVersionField[] = "shardVersion";

//constructBatchedCommandRequest�е��ã�����shardVersion����
StatusWith<ChunkVersion> ChunkVersion::parseFromBSONForCommands(const BSONObj& obj) {
    return parseFromBSONWithFieldForCommands(obj, kShardVersionField);
}

StatusWith<ChunkVersion> ChunkVersion::parseFromBSONWithFieldForCommands(const BSONObj& obj,
                                                                         StringData field) {
    BSONElement versionElem;
    Status status = bsonExtractField(obj, field, &versionElem);
    if (!status.isOK())
        return status;

    if (versionElem.type() != Array) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "Invalid type " << versionElem.type()
                              << " for shardVersion element. Expected an array"};
    }

    BSONObjIterator it(versionElem.Obj());
    if (!it.more())
        return {ErrorCodes::BadValue, "Unexpected empty version"};

    ChunkVersion version;

    // Expect the timestamp
    {
        BSONElement tsPart = it.next();
        if (tsPart.type() != bsonTimestamp)
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "Invalid type " << tsPart.type()
                                  << " for version timestamp part."};

        version._combined = tsPart.timestamp().asULL();
    }

    // Expect the epoch OID
    {
        BSONElement epochPart = it.next();
        if (epochPart.type() != jstOID)
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "Invalid type " << epochPart.type()
                                  << " for version epoch part."};

        version._epoch = epochPart.OID();
    }

    return version;
}

StatusWith<ChunkVersion> ChunkVersion::parseFromBSONForSetShardVersion(const BSONObj& obj) {
    bool canParse;
    const ChunkVersion chunkVersion = ChunkVersion::fromBSON(obj, kVersion, &canParse);
    if (!canParse)
        return {ErrorCodes::BadValue, "Unable to parse shard version"};

    return chunkVersion;
}

StatusWith<ChunkVersion> ChunkVersion::parseFromBSONForChunk(const BSONObj& obj) {
    bool canParse;
    const ChunkVersion chunkVersion = ChunkVersion::fromBSON(obj, kLastmod, &canParse);
    if (!canParse)
        return {ErrorCodes::BadValue, "Unable to parse shard version"};

    return chunkVersion;
}

//��obj�н�����lastmod�ֶΣ�Ҳ����chunkversion
StatusWith<ChunkVersion> ChunkVersion::parseFromBSONWithFieldAndSetEpoch(const BSONObj& obj,
                                                                         StringData field,
                                                                         const OID& epoch) {
    bool canParse;
    ChunkVersion chunkVersion = ChunkVersion::fromBSON(obj, field.toString(), &canParse);
    if (!canParse)
        return {ErrorCodes::BadValue, "Unable to parse shard version"};
    chunkVersion._epoch = epoch;
    return chunkVersion;
}

void ChunkVersion::appendForSetShardVersion(BSONObjBuilder* builder) const {
    addToBSON(*builder, kVersion);
}

void ChunkVersion::appendForCommands(BSONObjBuilder* builder) const {
    appendWithFieldForCommands(builder, kShardVersionField);
}

void ChunkVersion::appendWithFieldForCommands(BSONObjBuilder* builder, StringData field) const {
    builder->appendArray(field, toBSON());
}

void ChunkVersion::appendForChunk(BSONObjBuilder* builder) const {
    addToBSON(*builder, kLastmod);
}

BSONObj ChunkVersion::toBSON() const {
    BSONArrayBuilder b;
    b.appendTimestamp(_combined);
    b.append(_epoch);
    return b.arr();
}

}  // namespace mongo
