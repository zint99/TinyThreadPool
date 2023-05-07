#pragma once
#ifndef TINY_THREAD_POOL_H
#define TINY_THREAD_POOL_H

#include <vector> // for thread pool
#include <queue>  // for task queue
#include <atomic>
#include <future>
#include <condition_variable>
#include <thread>
#include <functional> // for std::function
#include <stdexcept>  // for runtime_error

namespace std
{
// 线程池最大容量
#define THREADPOOL_MAX_NUM 16
	// 线程池是否可以自动增长(如果需要,且不超过 THREADPOOL_MAX_NUM)
	// #define  THREADPOOL_AUTO_GROW

	/*
		thread pool features
			1.可以提交变参函数或拉姆达表达式的匿名函数执行,可以获取执行返回值
			2.支持类静态成员函数或全局函数,Opteron()函数等
			3.不直接支持类成员函数
	*/
	class threadpool
	{
		using Task = function<void()>; // 定义任务类型别名
		unsigned short _initSize;	   // 线程池中初始线程数量
		vector<thread> _pool;		   // 线程池
		queue<Task> _tasks;			   // 任务队列:todo封装为SAFEQUEUE..
		mutex _lock;				   // 任务队列互斥锁:用于实现线程安全的任务队列，保证添加任务和获取任务的互斥性
		condition_variable _task_cv;   // 保证多线程获取task的同步操作
		atomic<bool> _run{true};	   // 线程池是否执行
		atomic<int> _idleThreadNum{0}; // 空闲线程数量

#ifdef THREADPOOL_AUTO_GROW
		mutex _lockGrow; // 线程池增长同步锁
#endif					 // !THREADPOOL_AUTO_GROW

	public:
		inline threadpool(unsigned short size = 4)
		{
			_initSize = size;
			addThread(size);
		} // 默认初始化4个线程并通过addThread添加工作线程
		inline ~threadpool()
		{
			_run = false;		   // 通知线程停止执行任务
			_task_cv.notify_all(); // 唤醒所有线程执行
								   // 循环迭代每个线程并调用join
			for (thread &thread : _pool)
			{
				// thread.detach(); // 另种方法:让线程“自生自灭”
				if (thread.joinable())
					thread.join(); // 等待任务结束， 前提：线程一定会执行完
			}
		}

	public:
		/*
			commit系列函数用于提交一个任务到任务队列中,调用future.get()获取返回值时会等待任务执行完,获取返回值
			有两种方法可以实现调用类成员，
				bind： .commit(std::bind(&Dog::sayHello, &dog));
				mem_fn： .commit(std::mem_fn(&Dog::sayHello), this)
		*/

		// 参数Args...使用了可变参数模板表示函数参数的类型,参数F表示函数对象类型
		// auto尾后类型推导，万能引用，可变模板参数，future，decltype
		// 使用了C++11的decltype或std::result_of来获取函数f的返回值类型RetType
		template <class F, class... Args>
		/*
			F&& task：万能引用:右值引用＋模板参数推导,可以接受任意类型的可调用对象
				绑定到universial reference上的对象可能具有lvaluesness或者rvalueness，正是因为有这种二义性，所以产生了std::forward
				传递参数时需要小心操作，以避免出现引用折叠、类型推导错误和对象生命周期问题等
		*/
		auto commit(F &&f, Args &&...args) -> future<decltype(f(args...))>
		{
			// 如果threadpool已经停止，则抛出runtime_error异常
			if (!_run)
				throw runtime_error("commit error! ThreadPool is stopped.");

			/*
				这段代码用于定义异步任务返回值类型别名
					- C++11之前可以使用std::result_of模板类来获取函数的返回值类型
						- typename std::result_of<F(Args...)>::type, 函数 f 的返回值类型
						- 但不适用于函数指针
					- decltype可以自动推导出所有表达式的类型，包括函数指针和函数对象
			*/
			using RetType = decltype(f(args...));

			/*
				这段代码通过bind使用完美转发将(异步任务f和其参数args...)打包为一个(function包装的通用表示的可调用对象)
				并使用decltype推导异步任务返回值类型作为该可调用对象的返回值类型
					- bind将异步任务和参数打包成一个可调用对象并返回一个std::function对象或函数指针，可以通过调用这个可调用对象来执行原函数
					- forward保留异步任务参数的左右值属性，防止左右值错误
			*/
			std::function<RetType()> func = std::bind(std::forward<F>(f), std::forward<Args>(args)...);

			/*
				这段代码使用make_shared函数创建封装packaged_task类型对象的智能指针该对象可以将异步操作转换为类似于函数调用的形式，并支持将任务提交到线程池中执行。
				- packaged_task<函数类型> 用于封装任何可调用对象为异步任务
					- 异步任务get_future()获得关联的future对象,调用get()获得异步任务返回值
					- RetType()表示可调用对象的类型：RetType是返回值，()是参数列表
				- make_shared托管该packaged_task封装的异步任务对象，packaged_task<RetType()>为类型，func为实例化参数
			*/
			auto task = make_shared<packaged_task<RetType()>>(func);

			// packaged_task封装的异步任务调用get_future返回一个关联的future对象，用于获取异步操作执行的结果
			future<RetType> future = task->get_future();

			// 将打包好的任务对象添加到任务队列中,等待工作线程执行
			{
				// 实现线程安全的任务队列，mutex使得添加任务到任务队列时，各个线程互斥访问任务队列
				lock_guard<mutex> lock{_lock};

				/*
					这段代码将智能指针task封装的packaged_task对象(封装了异步任务)取出，包装为void函数
						lambda表达式 [task]() {(*task)();} 来定义一个可调用对象，并将该可调用对象添加到任务队列中
							[task]: 表示从外部捕获了一个名为task的变量，即上一步创建的packaged_task异步任务
							{(*task)();}: 使用(*task)()来调用之前绑定好的函数对象，即执行任务
				*/
				// Warp packaged task into void function
				_tasks.emplace([task]()
							   { (*task)(); });
			}

#ifdef THREADPOOL_AUTO_GROW
			if (_idleThreadNum < 1 && _pool.size() < THREADPOOL_MAX_NUM)
				addThread(1);
#endif // !THREADPOOL_AUTO_GROW

			_task_cv.notify_one(); // 唤醒一个线程执行

			// 返回一个future对象，用于获取异步操作执行的结果
			return future;
		} // END: auto commit(F&& f, Args&&... args) -> future<decltype(f(args...))>

		// 提交一个无参任务, 且无返回值
		template <class F> // F表示一个可调用对象的类型
		void commit2(F &&task)
		{
			if (!_run)
				return; // 线程池停止运行则直接return
			{
				// 使用lock_guard来保护线程池的任务队列
				lock_guard<mutex> lock{_lock};
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

		// 空闲线程数量
		int idleCount() { return _idleThreadNum; }
		// 线程数量
		int threadCount() { return _pool.size(); }

// 只在线程池定义为可以增长时才开放addThread为public接口
#ifndef THREADPOOL_AUTO_GROW
	private:
#endif // !THREADPOOL_AUTO_GROW
	   // 添加指定数量的线程：此函数为 main thread 中调用，线程池固定数量时为private接口
		void addThread(unsigned short size)
		{

#ifdef THREADPOOL_AUTO_GROW
			// THREADPOOL_AUTO_GROW开放接口的情况下需要验证线程池是否被停止
			if (!_run)
				throw runtime_error("Grow error! ThreadPool is stopped.");
			// 在定义了THREADPOOL_AUTO_GROW的情况下,如果线程池未被停止，则通过一个unique_lock<mutex>类型的锁lockGrow来保证在自动增长时的线程安全性
			unique_lock<mutex> lockGrow{_lockGrow}; // 自动增长锁
#endif												// !THREADPOOL_AUTO_GROW

			// 使用一个循环来增加线程数量，但不超过 预定义数量 THREADPOOL_MAX_NUM
			for (; _pool.size() < THREADPOOL_MAX_NUM && size > 0; --size)
			{
				// emplace_back向线程池_pool添加一个工作线程,使用lambda表达式来定义工作线程的回调函数
				// 注意lambda函数是子线程的逻辑，主线程会直接一次性使用lambda函数创建好size个子线程，并添加到线程池中
				_pool.emplace_back(
					[this] // !lambda函数为子线程中的执行逻辑
					{
						// 循环从任务队列中获取待执行的任务，并执行该任务
						while (true) // 防止条件2中 _run==false 时立即结束,将继续弹出任务来执行直到任务执行完毕再依次释放线程
						{
							Task task; // task为将从任务队列中获取的待执行任务
							{
								// 该代码块为多线程访问任务队列取任务执行，需要上锁保证只有一个子线程可以访问任务队列，防止多线程竞争
								unique_lock<mutex> lock{_lock};
								/*
									获得访问任务队列锁的子线程并非无条件向下执行，通过条件变量检查线程池运行状态以及任务队列是否为空
									以下情况重新获得互斥锁并继续执行
										1. 线程池停止运行 i.e. _run(false) && 任务队列为空_tasks.empty(true)
										2. 线程池停止运行 i.e. _run(false) && 任务队列不为空_tasks.empty(false)
										3. 线程池继续运行 i.e. _run(true) && 任务队列不为空_tasks.empty(false)
									以下情况将释放互斥锁并阻塞等待条件变量
										4. 线程池继续运行 i.e. _run(true) && 任务队列为空_tasks.empty(true)
								*/
								_task_cv.wait(lock, [this]
											  {
												  // 若条件4成立，则释放互斥锁并阻塞等待条件变量
												  return !_run || !_tasks.empty(); });
								// 条件1：如果线程池已经停止且任务队列已经为空，则退出循环，结束该工作线程
								if (!_run && _tasks.empty())
									return;
								_idleThreadNum--;			 // 该线程在工作,即空闲线程数-1
								task = move(_tasks.front()); // 按先进先出从任务队列取一个 task
								_tasks.pop();
							}
							task(); // 执行任务
									// 任务执行完毕...
#ifdef THREADPOOL_AUTO_GROW
							if (_idleThreadNum > 0 && _pool.size() > _initSize) // 支持自动释放空闲线程,避免峰值过后大量空闲线程
								return;
#endif // !THREADPOOL_AUTO_GROW
							{
								// 空闲线程的数量加1
								unique_lock<mutex> lock{_lock};
								_idleThreadNum++;
							}
						} // while
					});	  // emplace_back
				{
					// 有可能子线程也在操作_idleThreadNum，所以主线程也需要上锁
					unique_lock<mutex> lock{_lock};
					_idleThreadNum++;
				}
			} // for (; _pool.size() < THREADPOOL_MAX_NUM && size > 0; --size)
		}	  // void addThread(unsigned short size)
	};

}

#endif