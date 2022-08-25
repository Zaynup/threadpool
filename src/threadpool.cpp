#include "threadpool.h"

#include <iostream>

const int TASK_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_THRESHHOLD = 10;
const int THREAD_MAX_IDLE_TIME = 60; // 单位：秒s

/////////////////////// Task类实现 //////////////////////
Task::Task()
    : m_result(nullptr)
{
}

void Task::setResult(Result *result)
{
    m_result = result;
}

void Task::exec()
{
    if (m_result != nullptr)
    {
        m_result->setValue(run()); // 这里发生多态调用
    }
}

////////////////////// Result类实现 /////////////////////
Result::Result(std::shared_ptr<Task> task, bool isValid)
    : m_isValid(isValid),
      m_task(task)
{
    m_task->setResult(this);
}

// 问题一：setValue方法，获取任务执行完的返回值
void Result::setValue(Any value)
{
    // 存储task的返回值
    this->m_value = std::move(value);

    // 已经获取了任务的返回值，增加信号量资源
    m_sem.post();
}

// 问题二：get方法，用户调用这个方法获取task的返回值
Any Result::get()
{
    if (!m_isValid)
    {
        return "";
    }
    m_sem.wait();
    return std::move(m_value);
}

////////////////////// 线程方法实现 //////////////////////
int Thread::s_generateId = 0;

// 线程构造函数
Thread::Thread(ThreadFunc func)
    : m_func(func),
      m_threadId(s_generateId++)
{
}

// 线程析构函数
Thread::~Thread()
{
}

void Thread::start()
{
    // 创建一个线程来执行一个线程函数
    std::thread t(m_func, m_threadId); // 对C++11来说 线程对象t 和 线程函数func
    t.detach();                        // 设置分离线程（出作用域后，线程对象析构，但线程函数仍要继续执行，类似pthread_detach）
}

// 获取线程Id
int Thread::getThreadId() const
{
    return m_threadId;
}

///////////////////// 线程池方法实现 /////////////////////
// 线程池构造函数
ThreadPool::ThreadPool()
    : m_initThreadSize(0),
      m_taskQueueSize(0),
      m_idleThreadSize(0),
      m_curThreadSize(0),
      m_threadSizeMaxThreshHold(THREAD_MAX_THRESHHOLD),
      m_taskQueMaxThreshHold(TASK_MAX_THRESHHOLD),
      m_poolMode(PoolMode::MODE_FIXED),
      m_isPoolRunning(false)
{
}

// 线程池析构函数
ThreadPool::~ThreadPool()
{
    m_isPoolRunning = false;

    // 等待线程池里所有的线程返回  线程有两种状态： 阻塞 & 正在执行任务
    std::unique_lock<std::mutex> lock(m_taskQueMtx);
    m_notEmpty.notify_all();

    m_exitCond.wait(lock, [&]() -> bool
                    { return m_threads.size() == 0; });
}

// 设置线程池的工作模式
void ThreadPool::setMode(PoolMode mode)
{
    if (checkRunningState())
    {
        return;
    }
    m_poolMode = mode;
}

// 设置线程数量上限阈值
void ThreadPool::setThreadSizeMaxThreshHold(int threshhold)
{
    if (checkRunningState())
    {
        return;
    }
    if (m_poolMode == PoolMode::MODE_CACHED)
    {
        m_threadSizeMaxThreshHold = threshhold;
    }
}

// 设置Task任务队列上限阈值
void ThreadPool::setTaskQueMaskThreshHold(int threshhold)
{
    if (checkRunningState())
    {
        return;
    }
    m_taskQueMaxThreshHold = threshhold;
}

// 开启线程池
void ThreadPool::start(int initThreadSize)
{
    // 设置线程池的运行状态
    m_isPoolRunning = true;

    // 记录初始线程个数
    m_initThreadSize = initThreadSize;
    m_curThreadSize = initThreadSize;

    // 创建线程对象
    for (int i = 0; i < m_initThreadSize; i++)
    {
        // 创建Thread线程对象时，需要把线程函数给到Thread线程对象
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        // m_threads.emplace_back(std::move(ptr));// vector旧版
        int threadId = ptr->getThreadId();
        m_threads.emplace(threadId, std::move(ptr));
    }

    // 启动所有线程 std::vector<Thread *> m_threads;
    for (int i = 0; i < m_initThreadSize; i++)
    {
        m_threads[i]->start(); // 需要执行一个线程函数
        m_idleThreadSize++;    // 记录初始空闲线程的数量
    }
}

// 给线程池提交任务
// 用户调用该接口，传入任务对象，生产任务
Result ThreadPool::submitTask(std::shared_ptr<Task> sp)
{
    // 获取锁
    std::unique_lock<std::mutex> lock(m_taskQueMtx);

    // 线程通信 等待任务队列有空余 wait wait_for wait_until
    // 用户提交任务，最长不能阻塞超过1s，否则判断提交任务失败
    // m_notFull.wait(lock, [&]() -> bool
    //                { return m_taskQueue.size() < setTaskQueMaskThreshHold; });
    if (!m_notFull.wait_for(lock, std::chrono::seconds(1), [&]() -> bool
                            { return m_taskQueue.size() < (size_t)m_taskQueMaxThreshHold; }))
    {
        // 表示notFull等待1s钟，条件依然没有满足
        std::cerr << "task queue, is full ,submit task fail." << std::endl;

        // Task  Result
        // return task->getResult(); 不能采用这种方法，随着task被执行完，task对象析构，依赖于task对象的result对象也被析构
        return Result(sp, false);
    }

    // 如果有空余，把任务放入任务队列
    m_taskQueue.emplace(sp);
    m_taskQueueSize++;

    // 因为新放了任务，任务队列肯定不空，在m_notEmpty上通知，赶快分配线程执行任务
    m_notEmpty.notify_all();

    // cached模式 任务处理比较紧急 场景：小而快的任务 需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程出来。
    if (m_poolMode == PoolMode::MODE_CACHED &&
        m_taskQueueSize > m_idleThreadSize &&
        m_curThreadSize < m_threadSizeMaxThreshHold)
    {
        // 创建新线程对象
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        int threadId = ptr->getThreadId();
        m_threads.emplace(threadId, std::move(ptr));

        std::cout << "create new thread" << std::endl;

        // 启动线程
        m_threads[threadId]->start();

        // 修改线程相关的变量
        m_idleThreadSize++;
        m_curThreadSize++;
    }

    // 返回任务的Result对象
    return Result(sp);
}

// 定义线程函数
// 线程池的所有线程从任务队列里面消费任务
void ThreadPool::threadFunc(int threadId) // 线程函数返回，相应的线程也就结束了
{
    auto lastTime = std::chrono::high_resolution_clock().now();

    for (;;)
    {

        std::shared_ptr<Task> task;

        { // 先获取锁
            std::unique_lock<std::mutex> lock(m_taskQueMtx);

            std::cout << "tid:" << std::this_thread::get_id() << " 尝试获取任务。。。" << std::endl;

            // cached模式下，又可能已经创建了很多的线程，但是空闲时间超过60s，
            // 应该把多余的线程结束回收掉(超过m_initThreadSize数量的线程要进行回收)。
            // 当前时间 - 上一次线程执行时间 > 60s

            // 每一秒钟返回一次  怎么区分超时返回？还是有任务待执行返回
            while (m_taskQueue.size() == 0)
            {
                if (!m_isPoolRunning)
                {
                    m_threads.erase(threadId);
                    std::cout << "threadid:" << std::this_thread::get_id() << " exit!" << std::endl;
                    m_exitCond.notify_all();
                    return;
                }

                if (m_poolMode == PoolMode::MODE_CACHED)
                {
                    if (std::cv_status::timeout == m_notEmpty.wait_for(lock, std::chrono::seconds(1)))
                    {
                        auto now = std::chrono::high_resolution_clock().now();
                        auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                        if (dur.count() >= THREAD_MAX_IDLE_TIME && m_curThreadSize > m_initThreadSize)
                        {
                            // 开始回收当前线程
                            // 记录线程数量的相关变量的值修改
                            // 把线程对象从线程列表中删除
                            // threadId->thread对象==》删除
                            m_threads.erase(threadId);
                            m_curThreadSize--;
                            m_idleThreadSize--;

                            std::cout << "threadid:" << std::this_thread::get_id() << "exit !" << std::endl;
                            return;
                        }
                    }
                }
                else
                {
                    // 等待notEmpty条件
                    m_notEmpty.wait(lock);
                }

                // // 线程池要结束，回收线程资源
                // if (!m_isPoolRunning)
                // {
                //     m_threads.erase(threadId);
                //     m_exitCond.notify_all();
                //     std::cout << "threadid:" << std::this_thread::get_id() << "exit !" << std::endl;
                //     return;
                // }
            }

            m_idleThreadSize--;

            std::cout << "tid:" << std::this_thread::get_id() << " 获取任务成功" << std::endl;

            // 从任务队列中取一个出来
            task = m_taskQueue.front();
            m_taskQueue.pop();
            m_taskQueueSize--;

            // 如果依然有剩余任务，继续通知其他线程执行任务
            if (m_taskQueue.size() > 0)
            {
                m_notEmpty.notify_all();
            }

            // 取出一个任务，进行通知，通知可以继续提交生产任务
            m_notFull.notify_all();

        } // 这里就应该把所释放掉，保证其他任务可以提交

        // 当前线程负责执行这个任务
        if (task != nullptr)
        {
            // task->run();
            task->exec(); // 执行任务，把任务的返回值setValue方法给到Result
        }

        m_idleThreadSize++;

        lastTime = std::chrono::high_resolution_clock().now(); // 更新线程执行完任务的时间
    }
}

// 检查pool的运行状态
bool ThreadPool::checkRunningState() const
{
    return m_isPoolRunning;
}