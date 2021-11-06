/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/transport/service_entry_point_impl.h"

namespace mongo {

/**
 * The entry point into mongod. Just a wrapper around assembleResponse.
 */

/*
Tips: 
  mongos��mongod���������Ϊ��Ҫ�̳����紫��ģ���������ࣿ
ԭ����һ�������Ӧһ������session����session��Ӧ�������ֺ�SSM״̬��Ψһ��Ӧ�����пͻ�������
��Ӧ��SSM״̬����Ϣȫ��������ServiceEntryPointImpl._sessions��Ա�У���command�����ģ��Ϊ
SSM״̬�������е�dealTask����ͨ���ü̳й�ϵ��ServiceEntryPointMongod��ServiceEntryPointMongos��
��Ҳ�Ϳ��Ժ�״̬�������������������ͬʱҲ���Ի�ȡ��ǰ�����Ӧ��session������Ϣ��
*/
 
//ServiceContextMongoD->ServiceContext(����ServiceEntryPoint��Ա)
//ServiceEntryPointMongod->ServiceEntryPointImpl->ServiceEntryPoint

//_initAndListen->��serviceContext->setServiceEntryPoint���й���ʹ�ø���, ����ServiceContextMongoD::ServiceContext._serviceEntryPoint
//class ServiceEntryPointMongod final : public ServiceEntryPointImpl { //ԭʼ����
//mongod������ڵ�  �̳�ServiceEntryPointImpl��Ҳ��ȷ���˶�Ӧ������session��Ϣ
class ServiceEntryPointMongod : public ServiceEntryPointImpl {//yang change
    MONGO_DISALLOW_COPYING(ServiceEntryPointMongod);

public:
    using ServiceEntryPointImpl::ServiceEntryPointImpl;
    
    //ServiceEntryPointMongod::handleRequest(mongod������ڴ���)  
    //ServiceEntryPointMongos::handleRequest mongos������ڴ���
    DbResponse handleRequest(OperationContext* opCtx, const Message& request) override;
};

}  // namespace mongo
