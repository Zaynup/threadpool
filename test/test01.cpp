#include <iostream>
#include <chrono>
#include <thread>
using namespace std;

#include "../src/threadpool.h"

/*
有些场景，是希望能够获取线程执行任务得返回值得
举例：
1 + 。。。 + 30000的和
thread1  1 + ... + 10000
thread2  10001 + ... + 20000
.....

main thread：给每一个线程分配计算的区间，并等待他们算完返回结果，合并最终的结果即可
*/

using uLong = unsigned long long;

class MyTask : public Task
{
public:
    MyTask(int begin, int end)
        : m_begin(begin), m_end(end)
    {
    }

    // 问题：怎么设计run()函数的返回值，可以表示任意的类型
    Any run()
    {
        cout << "tid:" << this_thread::get_id() << " begin!" << endl;

        this_thread::sleep_for(chrono::seconds(2));

        uLong sum = 0;
        for (int i = m_begin; i <= m_end; i++)
        {
            sum += i;
        }
        cout << "sum[" << this_thread::get_id() << "]:" << sum << endl;

        cout << "tid:" << this_thread::get_id() << " end!" << endl;

        return sum;
    }

private:
    int m_begin;
    int m_end;
};

int main()
{
    {
        ThreadPool pool;
        pool.setMode(PoolMode::MODE_CACHED);
        // 开始启动线程池
        pool.start(2);

        // linux上，这些Result对象也是局部对象，要析构的！！！
        Result result1 = pool.submitTask(make_shared<MyTask>(1, 100000000));
        Result result2 = pool.submitTask(make_shared<MyTask>(100000001, 200000000));
        Result result3 = pool.submitTask(make_shared<MyTask>(200000001, 300000000));

        uLong sum1 = result1.get().cast_<uLong>();
        uLong sum2 = result2.get().cast_<uLong>();
        uLong sum3 = result3.get().cast_<uLong>();

        cout << "sum = " << (sum1 + sum2 + sum3) << endl;

    } // 这里Result对象也要析构!!! 在vs下，条件变量析构会释放相应资源的

    cout << "main over!" << endl;
}