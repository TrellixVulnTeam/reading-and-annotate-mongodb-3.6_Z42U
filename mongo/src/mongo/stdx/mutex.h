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

#pragma once 

#include <mutex>

namespace mongo {
namespace stdx {

using ::std::mutex;            // NOLINT
/*
std::recursive_mutex ����:
std::recursive_mutex �� std::mutex һ����Ҳ��һ�ֿ��Ա������Ķ��󣬵��Ǻ� std::mutex ��ͬ���ǣ�std::recursive_mutex 
����ͬһ���̶߳Ի�����������������ݹ�������������öԻ���������Ķ������Ȩ��std::recursive_mutex �ͷŻ�����ʱ��Ҫ
�����������������ͬ������ unlock()�������Ϊ lock() ������ unlock() ������ͬ

std::time_mutex ����:
try_lock_for ��������һ��ʱ�䷶Χ����ʾ����һ��ʱ�䷶Χ֮���߳����û�л����������ס���� std::mutex �� try_lock() ��ͬ��
try_lock ���������ʱû�л������ֱ�ӷ��� false��������ڴ��ڼ������߳��ͷ�����������߳̿��Ի�öԻ����������������ʱ��
����ָ��ʱ���ڻ���û�л���������򷵻� false��
*/
using ::std::timed_mutex;      // NOLINT
using ::std::recursive_mutex;  // NOLINT

/*
������������ģ��ļ�������
ǰ���ᵽstd::lock_guard��std::unique_lock��std::shared_lock��ģ���ڹ���ʱ�Ƿ�����ǿ�ѡ�ģ�C++11�ṩ��3�ּ������ԡ�

����	tag type	����
(Ĭ��)	��	��������������ǰ�߳�ֱ���ɹ��������
std::defer_lock	std::defer_lock_t	����������
std::try_to_lock	std::try_to_lock_t	���������������������̣߳���������ʱҲ���������ء�
std::adopt_lock	std::adopt_lock_t	�ٶ���ǰ�߳��Ѿ���û�����������Ȩ�����Բ�����������
*/
using ::std::adopt_lock_t;   // NOLINT
using ::std::defer_lock_t;   // NOLINT
using ::std::try_to_lock_t;  // NOLINT

using ::std::lock_guard;   // NOLINT
using ::std::unique_lock;  // NOLINT

/*
constexpr:
�������ʽ��Ҫ������һЩ���㷢���ڱ���ʱ���������ڴ��������������е�ʱ�����Ǻܴ���Ż���������Щ��������ڱ���ʱ����
����ֻ��һ�Σ�������ÿ�γ�������ʱ����Ҫ����һ������ʱ��֪�ĳ����������ض�ֵ��sine��cosin��ȷʵ�������ʹ�ÿ⺯��sin��cos��
����������뻨������ʱ�Ŀ�����ʹ��constexpr������Դ���һ������ʱ�ĺ���������Ϊ����������Ҫ����ֵ��

һ��constexpr��һЩ������ѭ���ϸ�Ҫ��
    ������ֻ����һ��return��䣨�м���������
    ֻ�ܵ�������constexpr����
    ֻ��ʹ��ȫ��constexpr����
*/
constexpr adopt_lock_t adopt_lock{};
constexpr defer_lock_t defer_lock{};
constexpr try_to_lock_t try_to_lock{};

}  // namespace stdx
}  // namespace mongo

