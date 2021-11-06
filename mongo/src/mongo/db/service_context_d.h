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

#pragma once

#include <map>

#include "mongo/db/service_context.h"

namespace mongo {

class Client;
class StorageEngineLockFile;

//ServiceContextMongoD->ServiceContext(����ServiceContext��Ա)
//ServiceEntryPointMongod->ServiceEntryPointImpl->ServiceEntryPoint

//_initAndListen�лṹ��ʹ�ø���   
class ServiceContextMongoD final : public ServiceContext {
public:
    //�����FactoryMap //ServiceContextMongoD._storageFactories��Ա;ʹ��
    using FactoryMap = std::map<std::string, const StorageEngine::Factory*>;

    ServiceContextMongoD();

    ~ServiceContextMongoD();

    StorageEngine* getGlobalStorageEngine() override;

    void createLockFile();
    //��ʼ���洢����
    void initializeGlobalStorageEngine() override;

    void shutdownGlobalStorageEngineCleanly() override;

    //ע��name factory��_storageFactories
    void registerStorageEngine(const std::string& name,
                               const StorageEngine::Factory* factory) override;

    //name�Ƿ���_storageFactories��ע��
    bool isRegisteredStorageEngine(const std::string& name) override;

    //���������_storageFactories��Ա map���õ�
    StorageFactoriesIterator* makeStorageFactoriesIterator() override;

private:
    ////����һ��OperationContext��
    std::unique_ptr<OperationContext> _newOpCtx(Client* client, unsigned opId) override;

    //createLockFile�д���lockfile ServiceContextMongoD::initializeGlobalStorageEngine()��close
    std::unique_ptr<StorageEngineLockFile> _lockFile;

    // logically owned here, but never deleted by anyone.
    //�߼��洢����  MongoDBֻ��һ���洢���棬����MMAP��MongoDB3.0���Ƴ�ʹ��MongoDB�����������棺MMAPv1��WiredTiger��
    //ServiceContextMongoD::initializeGlobalStorageEngine�и�ֵ
    //Ҳ����KVStorageEngine
    StorageEngine* _storageEngine = nullptr; //��ǰ�õĴ洢���棬WiredTiger��ӦKVStorageEngine��

    // All possible storage engines are registered here through MONGO_INIT.
    //���еĴ洢����ע�ᵽ���� registerStorageEngine
    FactoryMap _storageFactories; 
};

//�����ServiceContextMongoD:makeStorageFactoriesIterator ʹ�õ�����������
class StorageFactoriesIteratorMongoD final : public StorageFactoriesIterator {
public:
    typedef ServiceContextMongoD::FactoryMap::const_iterator FactoryMapIterator;

    StorageFactoriesIteratorMongoD(const FactoryMapIterator& begin, const FactoryMapIterator& end);

    bool more() const override;
    const StorageEngine::Factory* next() override;

private:
    FactoryMapIterator _curr;
    FactoryMapIterator _end;
};

}  // namespace mongo

