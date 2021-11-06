//
// detail/timer_queue.hpp
// ~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2017 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_DETAIL_TIMER_QUEUE_HPP
#define ASIO_DETAIL_TIMER_QUEUE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include <cstddef>
#include <vector>
#include "asio/detail/cstdint.hpp"
#include "asio/detail/date_time_fwd.hpp"
#include "asio/detail/limits.hpp"
#include "asio/detail/op_queue.hpp"
#include "asio/detail/timer_queue_base.hpp"
#include "asio/detail/wait_op.hpp"
#include "asio/error.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace detail {

template <typename Time_Traits>
//timer_queue_base�ӿڵ�ʵ����  timer_queue_set.first_��ԱΪ������ 
class timer_queue
  : public timer_queue_base
{
public:
  // The time type.
  //posix_time::ptime
  typedef typename Time_Traits::time_type time_type;

  // The duration type.
  typedef typename Time_Traits::duration_type duration_type;

  // Per-timer data.   //timer_queue.timers_
  class per_timer_data
  {
  public:
    per_timer_data() :
      heap_index_((std::numeric_limits<std::size_t>::max)()),
      next_(0), prev_(0)
    {
    }

  private:
    friend class timer_queue;

    // The operations waiting on the timer.
    //�ö�ʱ����ʱʱ�䵽��ʱ����Ҫִ�е�op����
    op_queue<wait_op> op_queue_;

    // The index of the timer in the heap.
    std::size_t heap_index_;

    // Pointers to adjacent timers in a linked list.
    //��timer������һ�����˫���������뵽timers_ͷ��
    per_timer_data* next_;
    per_timer_data* prev_;
  };

  // Constructor.
  timer_queue()
    : timers_(),
      heap_()
  {
  }

  // Add a new timer to the queue. Returns true if this is the timer that is
  // earliest in the queue, in which case the reactor's event demultiplexing
  // function call may need to be interrupted and restarted.
  //epoll_reactor::schedule_timer�е���
  bool enqueue_timer(const time_type& time, per_timer_data& timer, wait_op* op)
  {
    // Enqueue the timer object.
    if (timer.prev_ == 0 && &timer != timers_)
    {
      if (this->is_positive_infinity(time))
      {
        // No heap entry is required for timers that never expire.
        timer.heap_index_ = (std::numeric_limits<std::size_t>::max)();
      }
      else
      {
        // Put the new timer at the correct position in the heap. This is done
        // first since push_back() can throw due to allocation failure.
        //��ǰheap_ �����С,Ҳ���ǵ�ǰheap_�����Ѿ������˶��ٸ�heap_entry�ڵ�
        timer.heap_index_ = heap_.size();
        heap_entry entry = { time, &timer };

		//�µ�timer����һ��heap_entry���뵽heap_����
        heap_.push_back(entry);
		//�µ�entry�ڵ㰴��time��heap_�����п�������
        up_heap(heap_.size() - 1);
      }

      // Insert the new timer into the linked list of active timers.
      //timer��ӵ�timers_����ͷ�������˫������
      timer.next_ = timers_;
      timer.prev_ = 0;
      if (timers_)
        timers_->prev_ = &timer;
      timers_ = &timer;
    }

    // Enqueue the individual timer operation.
    timer.op_queue_.push(op); //op��ӵ���timer��op����

    // Interrupt reactor only if newly added timer is first to expire.
    return timer.heap_index_ == 0 && timer.op_queue_.front() == op;
  }

  // Whether there are no timers in the queue.
  //timers_����û��timer
  virtual bool empty() const
  {
    return timers_ == 0;
  }

  // Get the time for the timer that is earliest in the queue.
  //epoll_reactor::get_timeout
  virtual long wait_duration_msec(long max_duration) const
  {
    if (heap_.empty())
      return max_duration;

	//��ȡheap_�����е�һ��timer���뵱ǰʱ���ʱ���(ms)��Ҳ���ǻ��ж���ms��һ��timer����
    return this->to_msec(
        Time_Traits::to_posix_duration(
          Time_Traits::subtract(heap_[0].time_, Time_Traits::now())),
        max_duration);
  }

  // Get the time for the timer that is earliest in the queue.
  //epoll_reactor::get_timeout��ִ��
  //��ȡheap_�����е�һ��timer���뵱ǰʱ���ʱ���(us)��Ҳ���ǻ��ж���ms��һ��timer����
  virtual long wait_duration_usec(long max_duration) const
  {
    if (heap_.empty())
      return max_duration;

    return this->to_usec(
        Time_Traits::to_posix_duration(
          Time_Traits::subtract(heap_[0].time_, Time_Traits::now())),
        max_duration);
  }

  // Dequeue all timers not later than the current time.
  //��ȡheap_���Ѿ����ڵ�timer����ȡ����Ӧ��ops��ӵ�ops����
  //timer_queue_set::get_ready_timers�е���
  virtual void get_ready_timers(op_queue<operation>& ops)
  {
    if (!heap_.empty())
    {
      const time_type now = Time_Traits::now();
      while (!heap_.empty() && !Time_Traits::less_than(now, heap_[0].time_))
      {
        per_timer_data* timer = heap_[0].timer_;
        ops.push(timer->op_queue_);
        remove_timer(*timer);
      }
    }
  }

  // Dequeue all timers.
  //��ȡtimers_�����е�����timer��ӵ�ops����
  //timer_queue_set::get_all_timers�е���
  virtual void get_all_timers(op_queue<operation>& ops)
  {
    while (timers_)
    {
      per_timer_data* timer = timers_;
      timers_ = timers_->next_;
      ops.push(timer->op_queue_);
      timer->next_ = 0;
      timer->prev_ = 0;
    }

    heap_.clear();
  }

  // Cancel and dequeue operations for the given timer.
  //epoll_reactor::cancel_timer�е���
  std::size_t cancel_timer(per_timer_data& timer, op_queue<operation>& ops,
      std::size_t max_cancelled = (std::numeric_limits<std::size_t>::max)())
  {
    std::size_t num_cancelled = 0;
    if (timer.prev_ != 0 || &timer == timers_)
    {
      while (wait_op* op = (num_cancelled != max_cancelled)
          ? timer.op_queue_.front() : 0)
      {
        op->ec_ = asio::error::operation_aborted;
        timer.op_queue_.pop();
        ops.push(op);
        ++num_cancelled;
      }
      if (timer.op_queue_.empty())
        remove_timer(timer);
    }
    return num_cancelled;
  }

  // Move operations from one timer to another, empty timer.
  void move_timer(per_timer_data& target, per_timer_data& source)
  {
    target.op_queue_.push(source.op_queue_);

    target.heap_index_ = source.heap_index_;
    source.heap_index_ = (std::numeric_limits<std::size_t>::max)();

    if (target.heap_index_ < heap_.size())
      heap_[target.heap_index_].timer_ = &target;

    if (timers_ == &source)
      timers_ = &target;
    if (source.prev_)
      source.prev_->next_ = &target;
    if (source.next_)
      source.next_->prev_= &target;
    target.next_ = source.next_;
    target.prev_ = source.prev_;
    source.next_ = 0;
    source.prev_ = 0;
  }

private:
  // Move the item at the given index up the heap to its correct position.
  //��heap_�����еĳ�Ա����time_����ʱ���������
  void up_heap(std::size_t index)
  {
    while (index > 0)
    {
      std::size_t parent = (index - 1) / 2;
      if (!Time_Traits::less_than(heap_[index].time_, heap_[parent].time_))
        break;
      swap_heap(index, parent);
      index = parent;
    }
  }

  // Move the item at the given index down the heap to its correct position.
  //��heap_�����еĳ�Ա����time_����ʱ���������
  void down_heap(std::size_t index)
  {
    std::size_t child = index * 2 + 1;
    while (child < heap_.size())
    {
      std::size_t min_child = (child + 1 == heap_.size()
          || Time_Traits::less_than(
            heap_[child].time_, heap_[child + 1].time_))
        ? child : child + 1;
      if (Time_Traits::less_than(heap_[index].time_, heap_[min_child].time_))
        break;
      swap_heap(index, min_child);
      index = min_child;
      child = index * 2 + 1;
    }
  }

  // Swap two entries in the heap.
  void swap_heap(std::size_t index1, std::size_t index2)
  {
    heap_entry tmp = heap_[index1];
    heap_[index1] = heap_[index2];
    heap_[index2] = tmp;
    heap_[index1].timer_->heap_index_ = index1;
    heap_[index2].timer_->heap_index_ = index2;
  }

  // Remove a timer from the heap and list of timers.
  //��heap_���Ƴ�timer   timer::queue::get_ready_timers
  void remove_timer(per_timer_data& timer)
  {
    // Remove the timer from the heap.
    std::size_t index = timer.heap_index_;
    if (!heap_.empty() && index < heap_.size())
    {
      if (index == heap_.size() - 1)
      {
        heap_.pop_back();
      }
      else
      {
        swap_heap(index, heap_.size() - 1);
        heap_.pop_back();
        if (index > 0 && Time_Traits::less_than(
              heap_[index].time_, heap_[(index - 1) / 2].time_))
          up_heap(index);
        else
          down_heap(index);
      }
    }

    // Remove the timer from the linked list of active timers.
    //��timer��timers_�������Ƴ�
    if (timers_ == &timer)
      timers_ = timer.next_;
    if (timer.prev_)
      timer.prev_->next_ = timer.next_;
    if (timer.next_)
      timer.next_->prev_= timer.prev_;
    timer.next_ = 0;
    timer.prev_ = 0;
  }

  // Determine if the specified absolute time is positive infinity.
  template <typename Time_Type>
  static bool is_positive_infinity(const Time_Type&)
  {
    return false;
  }

  // Determine if the specified absolute time is positive infinity.
  template <typename T, typename TimeSystem>
  static bool is_positive_infinity(
      const boost::date_time::base_time<T, TimeSystem>& time)
  {
    return time.is_pos_infinity();
  }

  // Helper function to convert a duration into milliseconds.
  //dת��Ϊms
  template <typename Duration>
  long to_msec(const Duration& d, long max_duration) const
  {
    if (d.ticks() <= 0)
      return 0;
    int64_t msec = d.total_milliseconds();
    if (msec == 0)
      return 1;
    if (msec > max_duration)
      return max_duration;
    return static_cast<long>(msec);
  }

  // Helper function to convert a duration into microseconds.
  //dת��Ϊus
  template <typename Duration>
  long to_usec(const Duration& d, long max_duration) const
  {
    if (d.ticks() <= 0)
      return 0;
    int64_t usec = d.total_microseconds();
    if (usec == 0)
      return 1;
    if (usec > max_duration)
      return max_duration;
    return static_cast<long>(usec);
  }

  // The head of a linked list of all active timers.
  //���е�timer��ʱ����ͨ��������������һ��(˫������)
  per_timer_data* timers_;  

  //ÿ��timer��2����Ա��һ����¼ʱ�䣬һ����¼timer��˽������
  struct heap_entry
  {
    // The time when the timer should fire.
    //time_traits.hpp�ж���typedef boost::posix_time::ptime time_type;  
    //��¼��ʱ��ĳ��ʱ������
    time_type time_; //Ҳ����posix_time::ptimet  

    // The associated timer with enqueued operations.
    //��¼��timer��һЩ˽������
    per_timer_data* timer_;
  };

  // The heap of timers, with the earliest timer at the front.
  //ÿ��timer��Ӧһ��heap_entry�ڵ㣬�������heap_entry.heap_�����������Ŀ���ǿ��Կ��ٶ�λ�Ǹ�timer����
  std::vector<heap_entry> heap_;
};

} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_DETAIL_TIMER_QUEUE_HPP
