package lime.system;

import lime.app.Application;
import lime.app.Event;
import lime.system.BackgroundWorker;
import lime.utils.Log;
#if !force_synchronous
#if target.threaded
import sys.thread.Deque;
import sys.thread.Thread;
import sys.thread.Tls;
import sys.thread.IThreadPool;
#elseif cpp
import cpp.vm.Deque;
import cpp.vm.Thread;
import cpp.vm.Tls;
#elseif neko
import neko.vm.Deque;
import neko.vm.Thread;
import neko.vm.Tls;
#end
#end
/**
	A variant of `BackgroundWorker` designed to manage simultaneous jobs. It can
	run up to `maxThreads` jobs at a time and will keep excess jobs in a queue
	until a slot frees up to run them.

	It can also keep a certain number of threads (configurable via `minThreads`)
	running in the background even when no jobs are available. This avoids the
	not-insignificant overhead of stopping and restarting threads.

	Like `BackgroundWorker`, it also offers a single-threaded mode on targets
	that lack threads.

	@see `lime.app.Future.FutureWork` for a working example.
	@see https://player03.com/openfl/threads-guide/ for a tutorial.
**/
#if !lime_debug
@:fileXml('tags="haxe,release"')
@:noDebug
#end
class ThreadPool extends BackgroundWorker
{
	private static inline function clearDeque<T>(deque:Deque<T>):Void
	{
		while (deque.pop(false) != null) {}
	}

	/**
		The number of live threads in this pool, including both active and idle
		threads. Does not count threads that have been instructed to shut down.

		In single-threaded mode, this will equal `activeThreads`.
	**/
	public var currentThreads(get, never):Int;

	#if (!force_synchronous && (target.threaded || cpp || neko))
	/**
		__Set this only from the main thread.__
	**/
	private var __currentThreads:Int = 0;
	#end

	/**
		The number of threads in this pool that are currently working on a job.
		Does not count threads that have been instructed to shut down.

		In single-threaded mode, this instead indicates the number of jobs
		currently being executed.
	**/
	// __Set this only from the main thread.__
	public var activeThreads(default, null):Int = 0;

	/**
		The number of live threads in this pool that aren't currently working on
		anything. In single-threaded mode, this will always be 0.
	**/
	public var idleThreads(get, never):Int;

	/**
		__Set this only from the main thread.__

		The maximum number of live threads this pool can create at once. If this
		value decreases, active jobs will still be allowed to finish.

		Has no effect in single-threaded mode; use `workLoad` instead.
	**/
	public var maxThreads:Int;

	/**
		__Set this only from the main thread.__

		The number of threads that will be kept alive at all times, even if
		there's no work to do. Setting this won't add new threads, it'll just
		keep existing ones running.

		Has no effect in single-threaded mode.
	**/
	public var minThreads:Int;

	/**
		Dispatched on the main thread when a new job begins. Dispatched once per
		job. For best results, add all listeners before scheduling jobs.
	**/
	public var onRun = new Event<Dynamic->Void>();

	/**
		__Add jobs only from the main thread.__
	**/
	private var __pendingJobs = new Deque<ThreadEvent>();
	/**
		__Modify this only from the main thread.__

		The expected length of `__pendingJobs`. Will sometimes be greater than
		the actual length, temporarily.
	**/
	private var __numPendingJobs:Int = 0;

	/**
		@param doWork A single function capable of performing all of this pool's
		jobs. Treat this parameter as though it wasn't optional.
		@param workLoad (Single-threaded mode only) A rough estimate of how much
		of the app's time should be spent on this `ThreadPool`. For instance,
		the default value of 1/2 means this pool will take up about half the
		app's available time every frame. To increase the accuracy of this
		estimate, adjust `doWork` to increase `workIterations`.
	**/
	public function new(?doWork:Dynamic->Void, minThreads:Int = 0, maxThreads:Int = 1, mode:ThreadMode = MULTI_THREADED, ?workLoad:Float = 1/2)
	{
		super(mode, workLoad);

		#if debug
		if (doWork == null)
		{
			Log.warn("doWork argument should not be omitted.");
		}
		#end
		this.doWork = doWork;

		this.minThreads = minThreads;
		this.maxThreads = maxThreads;
	}

	/**
		Permanently shuts down this `ThreadPool`.
	**/
	public override function cancel():Void
	{
		super.cancel();

		clearDeque(__pendingJobs);

		for (i in 0...currentThreads)
		{
			__pendingJobs.push(new ThreadEvent(EXIT, null));
		}
	}

	/**
		Queues a new job, to be run once a thread becomes available.
	**/
	public function queue(state:Dynamic = null):Void
	{
		#if (!force_synchronous && (target.threaded || cpp || neko))
		if (Thread.current() != BackgroundWorker.__mainThread)
		{
			throw "Call queue() only from the main thread.";
		}
		#end

		if (canceled)
		{
			throw "This ThreadPool has been shut down.";
		}
		if (doWork == null)
		{
			throw "ThreadPool constructor requires doWork argument.";
		}

		__pendingJobs.add(new ThreadEvent(WORK, state));
		__numPendingJobs++;
		completed = false;

		if (!Application.current.onUpdate.has(__update))
		{
			Application.current.onUpdate.add(__update);
		}
	}

	/**
		Alias for `queue()`.
		@param doWork Ignored; set `doWork` via the constructor instead.
	**/
	public override function run(?doWork:Dynamic->Void, ?state:Dynamic):Void
	{
		queue(state);
	}

	#if (!force_synchronous && (target.threaded || cpp || neko))
	/**
		__Run this only on a background thread.__

		Retrieves pending jobs, runs them until complete, and repeats.
	**/
	private function __workLoop():Void
	{
		while (!canceled)
		{
			// Get a job.
			var job:ThreadEvent = __pendingJobs.pop(true);

			if (job.event == EXIT)
			{
				return;
			}

			if (job.event != WORK)
			{
				continue;
			}

			// Let the main thread know which job is starting.
			__messageQueue.add(job);

			// Get to work.
			__jobComplete.value = false;
			workIterations.value = 0;

			try
			{
				while (!__jobComplete.value && !canceled)
				{
					workIterations.value++;
					doWork.dispatch(job.state);
				}
			}
			catch (e)
			{
				sendError(e);
			}

			// Do it all again.
		}
	}
	#end

	private override function __update(deltaTime:Int):Void
	{
		#if (!force_synchronous && (target.threaded || cpp || neko))
		if (mode == MULTI_THREADED)
		{
			// Add idle threads until there are enough to perform all pending
			// jobs. This doesn't instantly decrease the number of pending jobs;
			// the new threads will handle that by reading from the deque.
			while (__numPendingJobs > idleThreads && currentThreads < maxThreads)
			{
				Thread.create(__workLoop);
				__currentThreads++; // This implicitly increments `idleThreads`.
			}
		}
		else
		#end
		{
			if (__state == null)
			{
				var job = __pendingJobs.pop(false);
				if (job != null)
				{
					__messageQueue.push(job);
					__state = job.state;
				}
			}

			if (__state != null)
			{
				__jobComplete.value = false;
				workIterations.value = 0;

				try
				{
					var endTime:Float = timestamp() + __workPerFrame;
					do
					{
						workIterations.value++;
						doWork.dispatch(__state);

						if (__jobComplete.value)
						{
							__state = null;
							break;
						}
					}
					while (timestamp() < endTime);
				}
				catch (e)
				{
					__state = null;
					sendError(e);
				}
			}
		}

		var threadEvent:ThreadEvent;
		while ((threadEvent = __messageQueue.pop(false)) != null)
		{
			switch (threadEvent.event)
			{
				case WORK:
					__numPendingJobs--;
					activeThreads++;

					onRun.dispatch(threadEvent.state);

				case PROGRESS:
					onProgress.dispatch(threadEvent.state);

				case COMPLETE, ERROR:
					activeThreads--;

					// Call `onComplete` before closing threads, in case the
					// listener queues a new job.
					if (threadEvent.event == COMPLETE)
					{
						onComplete.dispatch(threadEvent.state);

						if (activeThreads == 0 && __numPendingJobs == 0)
						{
							completed = true;
						}
					}
					else
					{
						onError.dispatch(threadEvent.state);
					}

					#if (!force_synchronous && (target.threaded || cpp || neko))
					// Close idle threads for which there's no pending job.
					if (mode == MULTI_THREADED
						&& ((__numPendingJobs < idleThreads && currentThreads > minThreads)
						|| currentThreads > maxThreads))
					{
						__currentThreads--;
						__pendingJobs.push(new ThreadEvent(EXIT, null));
					}
					#end

				default:
			}
		}

		if (currentThreads == 0)
		{
			Application.current.onUpdate.remove(__update);
		}
	}

	// Getters & Setters

	private inline function get_idleThreads():Int
	{
		return currentThreads - activeThreads;
	}

	private inline function get_currentThreads():Int
	{
		#if (!force_synchronous && (target.threaded || cpp || neko))
		if (mode == MULTI_THREADED)
			return __currentThreads;
		else
		#end
			return activeThreads;
	}
}
