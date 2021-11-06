//
// detail/scheduler_thread_info.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2017 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_DETAIL_SCHEDULER_THREAD_INFO_HPP
#define ASIO_DETAIL_SCHEDULER_THREAD_INFO_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/op_queue.hpp"
#include "asio/detail/thread_info_base.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace detail {

class scheduler;
class scheduler_operation;

struct scheduler_thread_info : public thread_info_base
{
  ////scheduler::do_wait_one->epoll_reactor::run ��ȡ��Ӧop��
  // ������ͨ��scheduler::task_cleanup��scheduler::work_cleanup����������ӵ�scheduler::op_queue_
  //epoll��ص������¼�����������ӵ�˽�ж���private_op_queue��Ȼ������ӵ�ȫ��op_queue_���У������Ϳ���һ���԰ѻ�ȡ���������¼�������ӵ�ȫ�ֶ��У�ֻ��Ҫ����һ��
  //private_op_queue���г�Ա��op����Ϊdescriptor_state��
  op_queue<scheduler_operation> private_op_queue; 

  //���߳�˽��private_op_queue������op������
  long private_outstanding_work;
};

} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_DETAIL_SCHEDULER_THREAD_INFO_HPP
