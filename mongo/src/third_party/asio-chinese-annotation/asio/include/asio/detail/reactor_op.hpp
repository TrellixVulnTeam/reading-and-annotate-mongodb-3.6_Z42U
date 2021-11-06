//
// detail/reactor_op.hpp
// ~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2017 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_DETAIL_REACTOR_OP_HPP
#define ASIO_DETAIL_REACTOR_OP_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include "asio/detail/operation.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace detail {

//Ҳ����op�ص�  �ο�reactive_socket_service::async_accept->start_accept_op  epoll_reactor::register_internal_descriptor
//reactive_socket_move_accept_op::reactive_socket_accept_op_base::reactor_op�̳й�ϵ
/*
Descriptor_read_op.hpp (include\asio\detail):class descriptor_read_op_base : public reactor_op
Descriptor_write_op.hpp (include\asio\detail):class descriptor_write_op_base : public reactor_op
Reactive_null_buffers_op.hpp (include\asio\detail):class reactive_null_buffers_op : public reactor_op
Reactive_socket_accept_op.hpp (include\asio\detail):class reactive_socket_accept_op_base : public reactor_op
Reactive_socket_connect_op.hpp (include\asio\detail):class reactive_socket_connect_op_base : public reactor_op
Reactive_socket_recvfrom_op.hpp (include\asio\detail):class reactive_socket_recvfrom_op_base : public reactor_op
Reactive_socket_recvmsg_op.hpp (include\asio\detail):class reactive_socket_recvmsg_op_base : public reactor_op
Reactive_socket_recv_op.hpp (include\asio\detail):class reactive_socket_recv_op_base : public reactor_op
Reactive_socket_sendto_op.hpp (include\asio\detail):class reactive_socket_sendto_op_base : public reactor_op
Reactive_socket_send_op.hpp (include\asio\detail):class reactive_socket_send_op_base : public reactor_op
Reactive_wait_op.hpp (include\asio\detail):class reactive_wait_op : public reactor_op
Signal_set_service.ipp (include\asio\detail\impl):class signal_set_service::pipe_read_op : public reactor_op
Signal_set_service.ipp (src\asio-srccode-yyzadd\detail\impl):class signal_set_service::pipe_read_op : public reactor_op
*/ 

// reactor_op(����IO�¼���������)  completion_handler(ȫ������)�̳и��� descriptor_state(reactor_op��Ӧ������IO�¼��������ռ��뵽�ýṹ����epoll��������) 

//mongodbʹ����reactive_socket_accept_op_base   reactive_socket_recv_op_base reactive_socket_send_op_base
//reactive_socket_accept_op_base�̳и���,accept��Ӧ��opΪreactive_socket_accept_op_base   

////����IO�������ִ�м�epoll_reactor::descriptor_state::do_complete��Ȼ�����reactor_op�����Ӧ�ӿ�
class reactor_op  //reactor_op��Ӧ�������¼��ص�ע���epoll_reactor::start_op
//descriptor_state.op_queue_[]�����Ǹ����ͣ�����ִ��ͨ��epoll_wait��ͻ�ȡdescriptor_state : operation�ص���Ϣ    
  : public operation //Ҳ����scheduler_operation
{
public:
  // The error code to be passed to the completion handler.
  asio::error_code ec_;

  // The number of bytes transferred, to be passed to the completion handler.
  //do_perform���еײ�fd���ݶ�д���ֽ���
  //�Ƿ�ӵײ�fd��ȡ���߷���һ��������mongodb���ĵļ����read_op::operator()��write_op::operator()��ʵ��
  std::size_t bytes_transferred_;

  // Status returned by perform function. May be used to decide whether it is
  // worth performing more operations on the descriptor immediately.
  //��ȡ���ݻ���д����
  enum status { not_done, done, done_and_exhausted };

  // Perform the operation. Returns true if it is finished.
  status perform() //ִ��perform_func_
  { //Ҳ����reactive_socket_accept_op_base  reactive_socket_recv_op_base  
  //reactive_socket_send_op_base��Ӧ��do_perform

  //epoll_reactor::descriptor_state::perform_io�е���ִ��
    return perform_func_(this);
  }

protected:
  typedef status (*perform_func_type)(reactor_op*);
  //����accept�������̻ص����̣�һ��ִ��accept������һ��ִ�к�����complete_func����(mongodb�е�task)
  //����recvmsg�������̣�һ��ִ��reactive_socket_recv_op_base::do_perform(����recvmsg)��һ��ִ�к���complete_func����(mongodb�е�task)
  reactor_op(perform_func_type perform_func, func_type complete_func)
  //����������ݵ�complete_funcΪreactive_socket_recv_op::do_complete
    : operation(complete_func),  //complete_func��ֵ��operation����operation��ִ��
      bytes_transferred_(0),
      perform_func_(perform_func)
  {
  }

private:
  //perform_funcҲ����fd�����շ��ײ�ʵ�֣���ֵ��reactor_op.perform_func_, complete_func��ֵ������operation��func,��reactor_op���캯��
  perform_func_type perform_func_;
};

} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_DETAIL_REACTOR_OP_HPP
