#pragma once

#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <limits>
#include <condition_variable>
#include <type_traits>
#include "NoExcept.h"
#include "Allocator.h"
#include "Assert.h"
#include "AsyncTask.h"
#include "EnumsAll.h"
#include "SharedObject.h"
#include "Exception.h"
#include "Thread.h"
#include "Process.h"
#include "SyncQueue.h"
#include "SyncSharedQueue.h"

namespace core
{
    static const int QueueSize = 128;
    static const int AllocationAreaSize = 1024 * 1024 * 16;
    
    template<typename Queue, std::size_t poolSize>
    class _AsyncExecutor
    {
    public:
        typedef std::unique_ptr<_AsyncExecutor<Queue, poolSize>> executor_ptr;
        typedef _AsyncExecutor<Queue, poolSize> _self;
        
        void set_queue(std::size_t idx, Queue* queuePtr)
        {
            VERIFY(idx < poolSize, "idx - %d >= poolSize - %d, invalid queue idx", idx, poolSize);
            m_queues[idx] = queuePtr;
        }
        
        Queue& get_queue(std::size_t idx)
        {
            VERIFY(idx < poolSize,"requested queue {%d}, dosen't exists - pool size - {%d}", idx, poolSize);
            return *m_queues[idx];
        }
        
        template<typename Derived, typename... Args>
        static void entry_point(int idx, Args&&... args)
        {
            std::cout<<"Child process"<<std::endl;
            typename Derived::executor_value_type executor = Derived::get_executor(std::forward<Args>(args)...);
            Queue& queue = executor.get_queue(idx);
            while(true)
            {
                typename Queue::value_type task;
                queue.pop(task);
                if(task->is_terminate_task())
                {
                    task->complete();
                    break;
                }
                
                try
                {
                    task->start();
                }
                catch(Exception& exception)
                {
                    task->set_failure_reason(exception.GetMessage());
                    task->notify_on_failure();
                }
            }
        }
        
    private:
        Queue* m_queues[poolSize];
    };
    
    
    template<ExecutionModel, std::size_t> class ConcreteAsyncExecutor;
    
    template<ExecutionModel model, std::size_t poolSize>
    class AsyncExecutor
    {
    public:
        typedef std::unique_ptr<AsyncExecutor> executor_ptr;
        virtual ~AsyncExecutor()=default;
    
        virtual void stop() = 0;
    
        template<typename Callable, typename... Args>
        AsyncTask::task_ptr make_task(Callable callable, Args&&... args)
        {
            auto& executor = static_cast<ConcreteAsyncExecutor<model, poolSize>&>(*this);
            return executor.make_task(callable, std::forward<Args>(args)...);
        }
    
        template<typename Return, typename Callable, typename... Args>
        future<Return> make_task(Callable callable, Args&&... args)
        {
            auto& executor = static_cast<ConcreteAsyncExecutor<model, poolSize>&>(*this);
            return executor.template make_task<Return>(callable, std::forward<Args>(args)...);
        }
        
        template<typename... Args>
        static executor_ptr make_executor(Args&&... args)
        {
            return executor_ptr(new ConcreteAsyncExecutor<model, poolSize>(std::forward<Args>(args)...));
        }
    };
    
    template<ExecutionModel model, std::size_t poolSize>
    class ConcreteAsyncExecutor : public AsyncExecutor<model, poolSize>{};
    
    template<std::size_t poolSize>
    class ConcreteAsyncExecutor<ExecutionModel::Process, poolSize>
            : public AsyncExecutor<ExecutionModel::Process, poolSize>
    {
    public:
        typedef ConcreteAsyncExecutor<ExecutionModel::Process, poolSize> _self;
        typedef ConcreteAsyncTask<ExecutionModel::Process> _concrete_task;
        typedef AsyncTask _task;
        typedef SyncSharedQueue<_task*, QueueSize> _queue;
        typedef _AsyncExecutor<_queue, poolSize> _executor;
        typedef _executor executor_value_type;
        typedef Allocator<_task> _allocator_type;
        typedef std::unique_ptr<Allocator<_task>> _allocator_ptr;
        typedef std::vector<ChildProcess> _child_processes;
        
        ConcreteAsyncExecutor(const std::string& name, bool owner)
            :m_sharedObject(name, SharedObject::AccessMod::READ_WRITE), m_owner(owner), m_executor(new _executor()), m_stopped(false)
        {
            static_assert(poolSize > 0, "pool size must be positive");
            std::size_t chunkSize = _self::chunk_size();
            m_sharedObject.Allocate(chunkSize);
            m_region = m_sharedObject.Map(0, chunkSize, SharedObject::AccessMod::READ_WRITE);
            m_allocator.reset(new Allocator<_task>(HeapType::Shared, m_region.GetPtr() + allocation_offset(), AllocationAreaSize));
            m_childProcesses.reserve(poolSize);
            if(m_owner)
            {
                for(int idx = 0; idx < poolSize; idx++)
                {
                    auto offset = static_cast<std::ptrdiff_t >(_queue::chunk_size()*idx);
                    m_executor->set_queue(idx, new _queue(m_region.GetPtr() + offset, true));
                    m_childProcesses.emplace_back(
                            Process::SpawnChildProcess(&_executor::template entry_point<_self, const std::string&>, idx, name)
                    );
                }
            }
            else
            {
                for(int idx = 0; idx < poolSize; idx++)
                {
                    auto offset = static_cast<std::ptrdiff_t >(_queue::chunk_size()*idx);
                    m_executor->set_queue(idx, new _queue(m_region.GetPtr() + offset, false));
                }
            }
        }
        
        ConcreteAsyncExecutor(ConcreteAsyncExecutor&& object) NOEXCEPT(true)
            :m_sharedObject(object.m_sharedObject), m_region(m_region), m_owner(false),
                m_executor(std::move(object.m_executor)), m_stopped(m_stopped)
        {
            std::swap(m_owner, object.m_owner);
        }
        
        virtual ~ConcreteAsyncExecutor()
        {
            if(m_owner)
            {
                stop();
                m_region.UnMap();
                m_sharedObject.Unlink();
            }
        }
        
        template<typename Callable, typename... Args>
        AsyncTask::task_ptr make_task(Callable callable, Args&&... args)
        {
            AsyncTask::task_ptr task(new _concrete_task(*m_allocator, callable, std::forward<Args>(args)...), [](AsyncTask* ptr){delete ptr;});
            push_task(reinterpret_cast<_concrete_task*>(task.get())->get_task());
            return std::move(task);
        }
    
        template<typename Callable, typename... Args>
        AsyncTask::task_ptr make_task(terminate_task, Callable callable, Args&&... args)
        {
            AsyncTask::task_ptr task(new _concrete_task(terminate_task(), *m_allocator, callable, std::forward<Args>(args)...), [](AsyncTask* ptr){delete ptr;});
            push_task(reinterpret_cast<_concrete_task*>(task.get())->get_task());
            return std::move(task);
        }
    
        template<typename Return, typename Callable, typename... Args>
        future<Return> make_task(Callable callable, Args&&... args)
        {
            typedef ConcreteFutureTask<ExecutionModel::Process, Return> _concrete_future_task;
            future<Return> futureTask(new _concrete_future_task(*m_allocator, callable, std::forward<Args>(args)...), [](Future<Return>* ptr){delete ptr;});
            push_task(reinterpret_cast<_concrete_future_task*>(futureTask.get())->get_task());
            return std::move(futureTask);
        }
    
        void stop() override
        {
            if(m_stopped)
                return;
        
            for(int idx = 0; idx < poolSize; idx++)
            {
                _queue& queue = m_executor->get_queue(idx);
                AsyncTask::task_ptr task = make_task(terminate_task(), std::function<void()>());
                try
                {
                    task->wait();
                }
                catch(Exception& exc){}
            }
            
            for(ChildProcess& process : m_childProcesses)
                process.wait();
            
            m_stopped = true;
        }
        
    private:
        friend _executor;
        
        executor_value_type _get_executor(){ return *m_executor; }
    
        static executor_value_type get_executor(const std::string& name)
        {
            _self executor(name, false);
            return executor._get_executor();
        }
        
        char* allocation_area_ptr()
        {
            return m_region.GetPtr() + allocation_offset();
        }
        
        static constexpr std::size_t chunk_size()
        {
            return _queue::chunk_size()*poolSize + AllocationAreaSize;
        }
        
        static constexpr std::size_t allocation_offset()
        {
            return _queue::chunk_size()*poolSize;
        }
    
        void push_task(_task* task)
        {
            static int idx = 0;
            _queue& queue = m_executor->get_queue(idx);
            idx = (idx + 1) % poolSize;
            queue.push(task);
        }


    private:
        SharedObject m_sharedObject;
        SharedRegion m_region;
        _child_processes m_childProcesses;
        bool m_owner;
        typename _executor::executor_ptr m_executor;
        _allocator_ptr m_allocator;
        bool m_stopped;
    };
    
    template<std::size_t poolSize>
    class ConcreteAsyncExecutor<ExecutionModel::Thread, poolSize>
        : public AsyncExecutor<ExecutionModel::Thread, poolSize>
    {
    public:
        typedef ConcreteAsyncExecutor<ExecutionModel::Thread, poolSize> _self;
        typedef ConcreteAsyncTask<ExecutionModel::Thread> _concrete_task;
        typedef SyncQueue<AsyncTask::shared_task_ptr> _queue;
        typedef _AsyncExecutor<_queue, poolSize> _executor;
        typedef _executor& executor_value_type;
        typedef std::vector<std::unique_ptr<Thread>> _thread_pool;
        
        ConcreteAsyncExecutor()
            :m_stopped(false)
        {
            static_assert(poolSize > 0, "pool size must be positive");
            m_threadPool.reserve(poolSize);
            m_queueGuard.reserve(poolSize);
            for(int idx = 0; idx < poolSize; idx++)
            {
                m_queueGuard.emplace_back();
                m_executor.set_queue(idx, &m_queueGuard.back());
                m_threadPool.emplace_back(new Thread(std::string("AsyncExec_") +
                                                     std::to_string(idx), std::bind(&_executor::template entry_point<_self, _self*&>, idx, this)));
            }
        }
        
        virtual ~ConcreteAsyncExecutor()
        {
            stop();
        }
        
        template<typename Callable, typename... Args>
        AsyncTask::task_ptr make_task(Callable callable, Args&&... args)
        {
            AsyncTask::task_ptr task(new _concrete_task(callable, std::forward<Args>(args)...), [](AsyncTask* ptr){delete ptr;});
            push_task(reinterpret_cast<_concrete_task*>(task.get())->get_task());
            return std::move(task);
        }
    
        template<typename Callable, typename... Args>
        AsyncTask::task_ptr make_task(terminate_task, Callable callable, Args&&... args)
        {
            AsyncTask::task_ptr task(new _concrete_task(terminate_task(), callable, std::forward<Args>(args)...), [](AsyncTask* ptr){delete ptr;});
            push_task(reinterpret_cast<_concrete_task*>(task.get())->get_task());
            return std::move(task);
        }
    
        template<typename Return, typename Callable, typename... Args>
        future<Return> make_task(Callable callable, Args&&... args)
        {
            typedef ConcreteFutureTask<ExecutionModel::Thread, Return> _concrete_future_task;
            future<Return> futureTask(new _concrete_future_task(callable, std::forward<Args>(args)...), [](AsyncTask* ptr){delete ptr;});
            push_task(reinterpret_cast<_concrete_future_task*>(futureTask.get())->get_task());
            return std::move(futureTask);
        }
        
        void stop() override
        {
            if(m_stopped)
                return;
        
            for(int idx = 0; idx < poolSize; idx++)
            {
                _queue& queue = m_executor.get_queue(idx);
                AsyncTask::task_ptr task = make_task(terminate_task(), std::function<void(void)>());
                queue.push(reinterpret_cast<_concrete_task*>(task.get())->get_task());
                try{
                    task->wait();
                }
                catch(Exception& exc){}
            }
            for(const auto& thread : m_threadPool)
            {
                thread->join();
            }
            m_stopped = true;
        }
        
    private:
        friend _executor;
    
        void push_task(const AsyncTask::shared_task_ptr& task)
        {
            static int idx = 0;
            _queue& queue = m_executor.get_queue(idx);
            idx = (idx + 1) % poolSize;
            queue.push(task);
        }
        
        executor_value_type _get_executor() { return m_executor; }
        
        static executor_value_type get_executor(_self* self)
        {
            return self->_get_executor();
        }
    
    private:
        std::vector<_queue> m_queueGuard;
         _thread_pool m_threadPool;
        _AsyncExecutor<_queue, poolSize> m_executor;
        bool m_stopped;
    };
    
    
}
