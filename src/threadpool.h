#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <functional>
#include <unordered_map>

// 信号量的实现
class semaphore
{
public:
    semaphore(int limit = 0)
        : m_resLimit(limit),
          m_isExit(false)
    {
    }
    ~semaphore()
    {
        m_isExit = true;
    }

    // 获取一个信号量资源
    void wait()
    {
        if (m_isExit)
        {
            return;
        }
        std::unique_lock<std::mutex> lock(m_mtx);
        m_cond.wait(lock, [&]() -> bool
                    { return m_resLimit > 0; });
        m_resLimit--;
    }

    // 增加一个信号量资源
    void post()
    {
        if (m_isExit)
        {
            return;
        }
        std::unique_lock<std::mutex> lock(m_mtx);
        m_resLimit++;
        m_cond.notify_all(); // 通知条件变量wait的地方，可以工作了
    }

private:
    std::atomic_bool m_isExit;
    int m_resLimit;
    std::mutex m_mtx;
    std::condition_variable m_cond;
};

// Any类型：可以接收任意数据的类型
class Any
{
public:
    Any() = default;
    ~Any() = default;
    Any(const Any &) = delete;
    Any &operator=(const Any &) = delete;
    Any(Any &&) = default;
    Any &operator=(Any &&) = default;

    // 这个构造函数可以让Any接收任意其他类型的数据
    template <typename T>
    Any(T data) : m_base(std::make_unique<Derived<T>>(data)) {}

    // 这个方法可以让Any对象里面存储的data数据提取出来
    template <typename T>
    T cast_()
    {
        // 我们怎么从Base找到它所指向的Derived对象，从它里面取出data成员变量
        // 基类指针->派生类指针
        Derived<T> *dp = dynamic_cast<Derived<T> *>(m_base.get());
        if (dp == nullptr)
        {
            throw "type is unmatched!";
        }
        return dp->m_data;
    }

private:
    // 基类类型
    class Base
    {
    public:
        virtual ~Base() = default;
    };

    // 派生类类型
    template <typename T>
    class Derived : public Base
    {
    public:
        Derived(T data) : m_data(data) {}

        T m_data;
    };

private:
    // 定义一个基类的指针
    std::unique_ptr<Base> m_base;
};

class Task;

// 实现接收提交到线程池的task任务执行完成后的返回值类型Result
class Result
{
public:
    Result(std::shared_ptr<Task> task, bool isValid = true);
    ~Result() = default;

    // 问题一：setValue方法，获取任务执行完的返回值
    void setValue(Any value);

    // 问题二：get方法，用户调用这个方法获取task的返回值
    Any get();

private:
    Any m_value;                  // 存储任务的返回值
    semaphore m_sem;              // 线程通信的信号量
    std::shared_ptr<Task> m_task; // 指向对应获取对象返回值的任务
    std::atomic_bool m_isValid;   // 返回值是否有效
};

class Task
{
public:
    Task();
    ~Task() = default;
    // 用户可以自定义任意任务类型，从Task继承，
    // 重写run()方法，实现自定义任务处理
    virtual Any run() = 0;

    void setResult(Result *result);

    void exec();

private:
    Result *m_result; // 为什么不用强智能指针？因为会产生交叉引用
};

// 线程池支持的模式
enum class PoolMode
{
    MODE_FIXED,  // 固定线程的数量
    MODE_CACHED, // 线程数量可动态增长
};

// 线程类型
class Thread
{
public:
    // 线程函数对象类型
    using ThreadFunc = std::function<void(int)>;

    // 线程构造函数
    Thread(ThreadFunc func);

    // 线程析构函数
    ~Thread();

    // 启动线程
    void start();

    // 获取线程Id
    int getThreadId() const;

private:
    ThreadFunc m_func;
    static int s_generateId;
    int m_threadId;
};

// 线程池类型
class ThreadPool
{
public:
    // 线程池构造函数
    ThreadPool();

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    // 线程池析构函数
    ~ThreadPool();

    // 设置线程池的工作模式
    void setMode(PoolMode mode);

    // 设置CACHED模式下的线程数量上限阈值
    void setThreadSizeMaxThreshHold(int threshhold);

    // 设置Task任务队列上限阈值
    void setTaskQueMaskThreshHold(int threshhold);

    // 开启线程池
    void start(int initThreadSize = std::thread::hardware_concurrency());

    // 给线程池提交任务
    Result submitTask(std::shared_ptr<Task> sp);

private:
    // 定义线程函数
    void threadFunc(int threadId);

    // 检查pool的运行状态
    bool checkRunningState() const;

private:
    // 用裸指针需要手动析构容器内的每一个对象，因此改用unique_ptr
    // std::vector<Thread *> m_threads;

    std::unordered_map<int, std::unique_ptr<Thread>> m_threads; // 线程列表

    int m_initThreadSize;             // 初始线程数量
    int m_threadSizeMaxThreshHold;    // 任务队列数量上限阈值
    std::atomic_int m_curThreadSize;  // 记录当前线程池中线程的总数量
    std::atomic_int m_idleThreadSize; // 记录空闲线程的数量

    /*
    问题: 当队列采用Task基类裸指针，若用户没有考虑那么多（对象声明周期），
         创建临时对象提交任务队列，提交后该临时对象析构，则任务队列实际
         拿到的是已经析构的对象，什么也访问不了，没有意义。
    解决方法：传入智能指针，拉长对象生命周期，且生命周期结束能自动释放资源。
    */
    // std::queue<Task *> m_taskQueue;
    std::queue<std::shared_ptr<Task>> m_taskQueue; // 任务队列
    std::atomic_int m_taskQueueSize;               // 任务的数量
    int m_taskQueMaxThreshHold;                    // 任务队列数量上限阈值

    std::mutex m_taskQueMtx;            // 保证任务队列的线程安全
    std::condition_variable m_notFull;  // 表示任务队列不满
    std::condition_variable m_notEmpty; // 表示任务队列不空
    std::condition_variable m_exitCond; // 等待线程资源全部回收

    PoolMode m_poolMode;              // 当前线程的工作模式
    std::atomic_bool m_isPoolRunning; // 当前线程池的启动状态
};

#endif