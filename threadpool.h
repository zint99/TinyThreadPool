#pragma once
#ifndef TINY_THREAD_POOL_H
#define TINY_THREAD_POOL_H

#include <vector>   // for thread pool
#include <queue>    // for task queue
#include <atomic>
#include <future>
//#include <condition_variable>
//#include <thread>
#include <functional>   // for std::function
#include <stdexcept>    // for runtime_error

namespace std
{
//线程池最大容量
#define  THREADPOOL_MAX_NUM 16
//线程池是否可以自动增长(如果需要,且不超过 THREADPOOL_MAX_NUM)
//#define  THREADPOOL_AUTO_GROW

/*
    thread pool features
        1.可以提交变参函数或拉姆达表达式的匿名函数执行,可以获取执行返回值
        2.支持类静态成员函数或全局函数,Opteron()函数等
        3.不直接支持类成员函数
*/
class threadpool
{
	unsigned short _initSize;      //初始化线程数量
	using Task = function<void()>; //定义类型
	vector<thread> _pool;          //线程池
	queue<Task> _tasks;            //任务队列
	mutex _lock;                   //任务队列同步锁
#ifdef THREADPOOL_AUTO_GROW
	mutex _lockGrow;               //线程池增长同步锁
#endif // !THREADPOOL_AUTO_GROW
	condition_variable _task_cv;   //条件阻塞
	atomic<bool> _run{ true };     //线程池是否执行
	atomic<int>  _idleThreadNum{ 0 };  //空闲线程数量

public:
	inline threadpool(unsigned short size = 4) { _initSize = size; addThread(size); }   // 默认初始化4个线程并通过addThread添加
	inline ~threadpool()
	{
		_run=false; // 通知线程停止执行任务
		_task_cv.notify_all(); // 唤醒所有线程执行
        // 循环迭代每个线程并调用join
		for (thread& thread : _pool) {
			//thread.detach(); // 另种方法:让线程“自生自灭”
			if (thread.joinable())
				thread.join(); // 等待任务结束， 前提：线程一定会执行完
		}
	}

public:
	// commit系列函数用于提交一个任务到任务队列中
	// 调用future.get()获取返回值时会等待任务执行完,获取返回值
	// 有两种方法可以实现调用类成员，
	// 一种是使用   bind： .commit(std::bind(&Dog::sayHello, &dog));
	// 一种是用   mem_fn： .commit(std::mem_fn(&Dog::sayHello), this)

    // 参数Args...使用了可变参数模板表示函数参数的类型,参数F表示函数对象类型
	template<class F, class... Args>
    // 使用了C++11的decltype或std::result_of来获取函数f的返回值类型RetType
	auto commit(F&& f, Args&&... args) -> future<decltype(f(args...))>
	{
		if (!_run)    // 如果threadpool已经停止，则抛出runtime_error异常
			throw runtime_error("commit error! ThreadPool is stopped.");

        // 在C++11之前，可以使用std::result_of模板类来获取函数的返回值类型typename std::result_of<F(Args...)>::type, 函数 f 的返回值类型
        // 但std::result_of只能用于获取函数对象的返回值类型，而不能用于获取函数指针的返回值类型

        // 获取任务f的返回值类型
		using RetType = decltype(f(args...));   // decltype可以自动推导出所有表达式的类型，包括函数指针和函数对象

        // 把函数入口及参数,打包(绑定):make_shared,packaged_task,bind,forward<Args>(args)...
        // 使用make_shared函数创建一个packaged_task对象，该对象可以将异步操作转换为类似于函数调用的形式，并支持将任务提交到线程池中执行。
        // packaged_task类模板的模板参数是函数的返回值类型，这里使用了之前推导出来的返回值类型RetType。
		auto task = make_shared<packaged_task<RetType()>>(
			bind(forward<F>(f), forward<Args>(args)...) // bind将函数对象和参数打包成一个可调用对象并返回一个std::function对象或函数指针，可以通过调用这个可调用对象来执行原函数
            // 将绑定好的可调用对象作为参数传递给packaged_task对象，生成一个新的任务
        ); 

        // 这个任务task可以在调用task->get_future()函数后返回一个future对象，用于获取异步操作执行的结果
		future<RetType> future = task->get_future();

        // 将打包好的任务对象添加到任务队列中,等待工作线程执行
		{    
			lock_guard<mutex> lock{ _lock }; //对当前块的语句加锁  lock_guard 是 mutex 的 stack 封装类，构造的时候 lock()，析构的时候 unlock()
            /*
                lambda表达式 [task]() {(*task)();} 来定义一个可调用对象，并将该可调用对象添加到任务队列中
                    [task]: 表示从外部捕获了一个名为task的变量，即上一步创建的packaged_task对象
                    {(*task)();}: 使用(*task)()来调用之前绑定好的函数对象，即执行任务
            */
			_tasks.emplace([task]() { // push(Task{...}) 放到队列后面
				(*task)();  // packaged_task对象只能被调用一次，因此在每次调用之后都需要重新创建一个新的packaged_task对象
			});
		}

#ifdef THREADPOOL_AUTO_GROW
		if (_idleThreadNum < 1 && _pool.size() < THREADPOOL_MAX_NUM)
			addThread(1);
#endif // !THREADPOOL_AUTO_GROW

		_task_cv.notify_one(); // 唤醒一个线程执行

        // 返回一个future对象，用于获取异步操作执行的结果
		return future;
	}   // END: auto commit(F&& f, Args&&... args) -> future<decltype(f(args...))>

	// 提交一个无参任务, 且无返回值
	template <class F>  // F表示一个可调用对象的类型
    /*
        F&& task
            万能引用:右值引用＋模板参数推导,可以接受任意类型的可调用对象
            传递参数时需要小心操作，以避免出现引用折叠、类型推导错误和对象生命周期问题等
            使用std::bind或Lambda表达式等方式将参数包装成一个可调用对象，并将该对象作为参数传递给commit2函数
    */
	void commit2(F&& task)
	{
		if (!_run) return;  // 线程池停止运行则直接return
		{
            // 使用lock_guard来保护线程池的任务队列
			lock_guard<mutex> lock{ _lock };
            // 将待执行的任务插入到任务队列中
            // 使用了std::forward将参数task完美转发到任务队列中。1、避免不必要的拷贝和移动操作 2、保留了参数task的完整类型信息和左右值属性
			_tasks.emplace(std::forward<F>(task));  
		}
        // 若定义了THREADPOOL_AUTO_GROW则执行以下代码段
#ifdef THREADPOOL_AUTO_GROW
        // 如果空闲线程数小于1且线程池中的总线程数还未达到最大值，则增加一个线程
		if (_idleThreadNum < 1 && _pool.size() < THREADPOOL_MAX_NUM)
			addThread(1);
#endif // !THREADPOOL_AUTO_GROW
        // 通知任意一个等待中的线程有新任务可执行
		_task_cv.notify_one();
	}
	//空闲线程数量
	int idleCount() { return _idleThreadNum; }
	//线程数量
	int threadCount() { return _pool.size(); }

// 只在线程池定义为可以增长时才开放addThread为public接口
#ifndef THREADPOOL_AUTO_GROW
private:
#endif // !THREADPOOL_AUTO_GROW
	//添加指定数量的线程
	void addThread(unsigned short size)
	{

#ifdef THREADPOOL_AUTO_GROW
        // THREADPOOL_AUTO_GROW开放接口的情况下需要验证线程池是否被停止
		if (!_run)    
			throw runtime_error("Grow error! ThreadPool is stopped.");
        // 在定义了THREADPOOL_AUTO_GROW的情况下,如果线程池未被停止，则通过一个unique_lock<mutex>类型的锁lockGrow来保证在自动增长时的线程安全性
		unique_lock<mutex> lockGrow{ _lockGrow }; //自动增长锁
#endif // !THREADPOOL_AUTO_GROW

        // 使用一个循环来增加线程数量
		for (; _pool.size() < THREADPOOL_MAX_NUM && size > 0; --size)
		{   
            //增加线程数量,但不超过 预定义数量 THREADPOOL_MAX_NUM
            //emplace_back向线程池添加一个工作线程,使用lambda表达式来定义工作线程函数
			_pool.emplace_back(
                // LAMBDA
                    [this]{ 
                    // 使用死循环不断地从任务队列中获取待执行的任务，并执行该任务
                    while (true) //防止 _run==false 时立即结束,此时任务队列可能不为空
                    {
                        Task task; // 获取一个待执行的 task
                        {
                            // 锁lock来保证线程获取任务时安全性
                            // unique_lock 相比 lock_guard 的好处是: 可以随时 unlock() 和 lock()
                            unique_lock<mutex> lock{ _lock };
                            _task_cv.wait(lock, [this] { // wait 直到有 task, 或需要停止
                                return !_run || !_tasks.empty();
                            });
                            // 如果线程池已经停止且任务队列已经为空，则退出循环，结束该工作线程
                            if (!_run && _tasks.empty())
                                return;
                            _idleThreadNum--;   // 该线程在工作,即空闲线程数-1
                            task = move(_tasks.front()); // 按先进先出从任务队列取一个 task
                            _tasks.pop();
                        }
                        task();//执行任务
                        // 任务执行完毕...
        #ifdef THREADPOOL_AUTO_GROW
                        if (_idleThreadNum>0 && _pool.size() > _initSize) //支持自动释放空闲线程,避免峰值过后大量空闲线程
                            return;
        #endif // !THREADPOOL_AUTO_GROW
                        {
                            // 空闲线程的数量加1
                            unique_lock<mutex> lock{ _lock };
                            _idleThreadNum++;
                        }
                    }
                }
            ); // emplace_back
			{
				unique_lock<mutex> lock{ _lock };
				_idleThreadNum++;
			}
		}   // for (; _pool.size() < THREADPOOL_MAX_NUM && size > 0; --size)
	}   // void addThread(unsigned short size)
};

}

#endif 