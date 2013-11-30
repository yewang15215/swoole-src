#include "swoole.h"
#include "Server.h"
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

static int swFactoryProcess_manager_loop(swFactory *factory);
static int swFactoryProcess_manager_start(swFactory *factory);

static int swFactoryProcess_worker_loop(swFactory *factory, int worker_pti);
static int swFactoryProcess_worker_spawn(swFactory *factory, int worker_pti);
static int swFactoryProcess_worker_receive(swReactor *reactor, swEvent *event);

static int swFactoryProcess_writer_start(swFactory *factory);
static int swFactoryProcess_writer_loop_unsock(swThreadParam *param);
static int swFactoryProcess_writer_loop_queue(swThreadParam *param);
static int swFactoryProcess_writer_receive(swReactor *, swEvent *);

static int swFactoryProcess_notify(swFactory *factory, swEvent *event);
static int swFactoryProcess_dispatch(swFactory *factory, swEventData *buf);
static int swFactoryProcess_finish(swFactory *factory, swSendData *data);
static int swFactoryProcess_event(swFactory *factory, swEventData *data);

static int swTaskWorker_onTask(swProcessPool *pool, swEventData *task);

int c_worker_pti = 0; //Current Proccess Worker's id
static int worker_task_num = 0;
static int worker_task_always = 0;
static int manager_worker_reloading = 0;
static int manager_reload_flag = 0;
static swProcessPool task_workers;
extern swReactor *swoole_worker_reactor;

int swFactoryProcess_create(swFactory *factory, int writer_num, int worker_num)
{
	swFactoryProcess *object;
	object = sw_memory_pool->alloc(sw_memory_pool, sizeof(swFactoryProcess));
	if (object == NULL)
	{
		swWarn("[swFactoryProcess_create] malloc[0] fail");
		return SW_ERR;
	}
	object->writers = sw_memory_pool->alloc(sw_memory_pool, writer_num*sizeof(swThreadWriter));
	if (object->writers == NULL)
	{
		swWarn("[Main] malloc[object->writers] fail");
		return SW_ERR;
	}
	object->writer_num = writer_num;
	object->writer_pti = 0;

	object->workers = sw_memory_pool->alloc(sw_memory_pool, worker_num* sizeof(swWorker));
	if (object->workers == NULL)
	{
		swWarn("[Main] malloc[object->workers] fail");
		return SW_ERR;
	}
	object->worker_num = worker_num;

	factory->object = object;
	factory->dispatch = swFactoryProcess_dispatch;
	factory->finish = swFactoryProcess_finish;
	factory->start = swFactoryProcess_start;
	factory->notify = swFactoryProcess_notify;
	factory->event = swFactoryProcess_event;
	factory->shutdown = swFactoryProcess_shutdown;
	factory->end = swFactoryProcess_end;
	factory->onTask = NULL;
	factory->onFinish = NULL;
	return SW_OK;
}

int swFactoryProcess_event(swFactory *factory, swEventData *data)
{
	return swProcessPool_dispatch(&task_workers, data);
}

int swFactoryProcess_shutdown(swFactory *factory)
{
	swFactoryProcess *object = factory->object;
	swServer *serv = factory->ptr;
	int i;
	//kill manager process
	kill(object->manager_pid, SIGTERM);
	//kill all child process
	for (i = 0; i < object->worker_num; i++)
	{
		swTrace("[Main]kill worker processor\n");
		kill(object->workers[i].pid, SIGTERM);
	}
#if SW_WORKER_IPC_MODE == 2
	object->rd_queue.free(&object->rd_queue);
	object->wt_queue.free(&object->wt_queue);
#else
	//close pipes
#endif
	return SW_OK;
}

int swFactoryProcess_start(swFactory *factory)
{
	int ret;
	ret = swFactory_check_callback(factory);
	if (ret < 0)
	{
		return SW_ERR;
	}
	//必须先启动manager进程组，否则会带线程fork
	ret = swFactoryProcess_manager_start(factory);
	if (ret < 0)
	{
		return SW_ERR;
	}
	//主进程需要设置为直写模式
	factory->finish = swFactory_finish;
	return SW_OK;
}

static int swFactoryProcess_worker_receive(swReactor *reactor, swEvent *event)
{
	int n;
	swEventData task;
	swFactory *factory = reactor->ptr;
	do
	{
		n = read(event->fd, &task, sizeof(task));
	}
	while(n < 0 && errno == EINTR);
	return swFactoryProcess_worker_excute(factory, &task);
}

int swFactoryProcess_worker_excute(swFactory *factory, swEventData *task)
{
	swServer *serv = factory->ptr;
	factory->last_from_id = task->info.from_id;

	switch(task->info.type)
	{
	case SW_EVENT_DATA:
		factory->onTask(factory, task);
		//只有数据请求任务才计算task_num
		if(!worker_task_always)
		{
			worker_task_num--;
		}
		break;
	case SW_EVENT_CLOSE:
		serv->onClose(serv, task->info.fd, task->info.from_id);
		break;
	case SW_EVENT_CONNECT:
		serv->onConnect(serv, task->info.fd, task->info.from_id);
		break;
	case SW_EVENT_FINISH:
		serv->onFinish(serv, task);
		break;
	default:
		swWarn("[Worker] error event[type=%d]", (int)task->info.type);
		break;
	}
	//stop
	if(worker_task_num < 0)
	{
		swoole_running = 0;
	}
	return SW_OK;
}

//create worker child proccess
static int swFactoryProcess_manager_start(swFactory *factory)
{
	swFactoryProcess *object = factory->object;
	int i, pid, ret;
	int writer_pti;

	swServer *serv = factory->ptr;

#if SW_WORKER_IPC_MODE == 2
#define _SW_PATH_BUF_LEN   128
	//这里使用ftok来获取消息队列的key
	char path_buf[_SW_PATH_BUF_LEN];
	char *path_ptr = getcwd(path_buf, _SW_PATH_BUF_LEN);
	//读数据队列
	if(swQueueMsg_create(&object->rd_queue, 1, ftok(path_ptr, 1), 1) < 0)
	{
		swError("[Master] swPipeMsg_create[In] fail. Error: %s [%d]", strerror(errno), errno);
		return SW_ERR;
	}
	//为TCP创建写队列
	if (serv->have_tcp_sock == 1)
	{
		//写数据队列
		if(swQueueMsg_create(&object->wt_queue, 1, ftok(path_ptr, 2), 1) < 0)
		{
			swError("[Master] swPipeMsg_create[out] fail. Error: %s [%d]", strerror(errno), errno);
			return SW_ERR;
		}
	}
#else
	object->pipes = sw_memory_pool->alloc(sw_memory_pool, object->worker_num * sizeof(swPipe));
	if (object->pipes == NULL)
	{
		swError("malloc[worker_pipes] fail. Error: %s [%d]", strerror(errno), errno);
		return SW_ERR;
	}
	//worker进程的pipes
	for (i = 0; i < object->worker_num; i++)
	{
		if (swPipeUnsock_create(&object->pipes[i], 1, SOCK_DGRAM) < 0)
		{
			swError("create unix socket[1] fail");
			return SW_ERR;
		}
		object->workers[i].pipe_master = object->pipes[i].getFd(&object->pipes[i], 1);
		object->workers[i].pipe_worker = object->pipes[i].getFd(&object->pipes[i], 0);
	}
#endif
	if (serv->task_worker_num > 0)
	{
		if (swProcessPool_create(&task_workers, serv->task_worker_num, serv->max_request)< 0)
		{
			swWarn("[Master] create task_workers fail");
			return SW_ERR;
		}
		//设置指针和回调函数
		task_workers.ptr = serv;
		task_workers.onTask = swTaskWorker_onTask;
	}
	pid = fork();
	switch (pid)
	{
	//创建manager进程
	case 0:
		for (i = 0; i < object->worker_num; i++)
		{
//			close(worker_pipes[i].pipes[0]);
			writer_pti = (i % object->writer_num);
			object->workers[i].writer_id = writer_pti;
			pid = swFactoryProcess_worker_spawn(factory, i);
			if (pid < 0)
			{
				swError("Fork worker process fail");
				return SW_ERR;
			}
			else
			{
				object->workers[i].pid = pid;
			}
		}
		//创建task_worker进程
		if (serv->task_worker_num > 0)
		{
			swProcessPool_start(&task_workers);
		}
		//标识为管理进程
		sw_process_type = SW_PROCESS_MANAGER;
		ret = swFactoryProcess_manager_loop(factory);
		exit(ret);
		break;
		//主进程
	default:
		object->manager_pid = pid;
		//TCP需要writer线程
		if (serv->have_tcp_sock == 1)
		{
			int ret = swFactoryProcess_writer_start(factory);
			if (ret < 0)
			{
				return SW_ERR;
			}
		}
#if SW_WORKER_IPC_MODE != 2
		for (i = 0; i < object->worker_num; i++)
		{
			writer_pti = (i % object->writer_num);
			object->workers[i].writer_id = writer_pti;

			if (serv->have_tcp_sock == 1)
			{
				//将写pipe设置到writer的reactor中
				object->writers[writer_pti].reactor.add(&(object->writers[writer_pti].reactor),
						object->workers[i].pipe_master, SW_FD_PIPE);
			}
		}
#endif
		break;
	case -1:
		swError("[swFactoryProcess_worker_start]fork manager process fail\n");
		return SW_ERR;
	}
	return SW_OK;
}

static int swTaskWorker_onTask(swProcessPool *pool, swEventData *task)
{
	swServer *serv = pool->ptr;
	return serv->onTask(serv, task);
}

static void swManagerSignalHanlde(int sig)
{
	switch (sig)
	{
	case SIGUSR1:
		manager_worker_reloading = 1;
		manager_reload_flag = 0;
		break;
	default:
		break;
	}
}

static int swFactoryProcess_manager_loop(swFactory *factory)
{
	int pid, new_pid;
	int i, writer_pti;
	int reload_worker_i = 0;
	int ret;

	swFactoryProcess *object = factory->object;
	swServer *serv = factory->ptr;
	swWorker *reload_workers;

	reload_workers = sw_memory_pool->alloc(sw_memory_pool, object->worker_num *sizeof(swWorker));
	if (reload_workers == NULL)
	{
		swError("[manager] malloc[reload_workers] fail.\n");
		return SW_ERR;
	}

	//for reload
	swSignalSet(SIGUSR1, swManagerSignalHanlde, 1, 0);

	while (swoole_running > 0)
	{
		pid = wait(NULL);
		swTrace("[manager] worker stop.pid=%d\n", pid);
		if (pid < 0)
		{
			if (manager_worker_reloading == 0)
			{
				swTrace("[Manager] wait fail. Error: %s [%d]", strerror(errno), errno);
			}
			else if (manager_reload_flag == 0)
			{
				memcpy(reload_workers, object->workers, sizeof(swWorker) * object->worker_num);
				manager_reload_flag = 1;
				goto kill_worker;
			}
		}
		if (swoole_running == 1)
		{
			for (i = 0; i < object->worker_num; i++)
			{
				//对比pid
				if (pid != object->workers[i].pid)
				{
					continue;
				}
				else
				{
					pid = 0;
					new_pid = swFactoryProcess_worker_spawn(factory, i);
					if (new_pid < 0)
					{
						swWarn("Fork worker process fail. Error: %s [%d]", strerror(errno), errno);
						return SW_ERR;
					}
					else
					{
						object->workers[i].pid = new_pid;
					}
				}
			}

			//task worker
			if(pid > 0)
			{
				swWorker *exit_worker = swHashMap_find_int(&task_workers.map, pid);
				if (exit_worker != NULL)
				{
					swProcessPool_spawn(&task_workers, exit_worker);
				}
			}
		}
		//reload worker
		kill_worker: if (manager_worker_reloading == 1)
		{
			//reload finish
			if (reload_worker_i >= object->worker_num)
			{
				manager_worker_reloading = 0;
				reload_worker_i = 0;
				continue;
			}
			ret = kill(reload_workers[reload_worker_i].pid, SIGTERM);
			if (ret < 0)
			{
				swWarn("[Manager]kill fail.pid=%d. Error: %s [%d]", reload_workers[reload_worker_i].pid, strerror(errno), errno);
				continue;
			}
			reload_worker_i++;
		}
	}
	return SW_OK;
}

static int swFactoryProcess_worker_spawn(swFactory *factory, int worker_pti)
{
	swFactoryProcess *object = factory->object;
	int i, pid, ret;

	pid = fork();
	if (pid < 0)
	{
		swWarn("[swFactoryProcess_worker_spawn]Fork Worker failError: %s [%d]", strerror(errno), errno);
		return SW_ERR;
	}
	//worker child processor
	else if (pid == 0)
	{
#if SW_WORKER_IPC_MODE != 2
		for (i = 0; i < object->worker_num; i++)
		{
			//非当前的worker_pipe
			if (worker_pti != i)
			{
				close(object->workers[i].pipe_worker);
			}
			//关闭master_pipe
			close(object->workers[i].pipe_master);
		}
#endif
		//标识为worker进程
		sw_process_type = SW_PROCESS_WORKER;
		ret = swFactoryProcess_worker_loop(factory, worker_pti);
		exit(ret);
	}
	//parent,add to writer
	else
	{
		return pid;
	}
}

int swFactoryProcess_end(swFactory *factory, swDataHead *event)
{
	int ret;
	swFactoryProcess *object = factory->object;
	swServer *serv = factory->ptr;
	swEvent ev;

	bzero(&ev, sizeof(swEvent));
	ev.fd = event->fd;
	ev.len = 0; //len=0表示关闭此连接
	ev.from_id = event->from_id;
	return swFactoryProcess_finish(factory, (swSendData *)&ev);
}
/**
 * Worker进程,向writer发送数据
 */
int swFactoryProcess_finish(swFactory *factory, swSendData *resp)
{
	//UDP直接在worker进程内发送
	int ret, sendn, count;;
	swFactoryProcess *object = factory->object;
	swServer *serv = factory->ptr;

	//from_id超过serv->poll_thread_num，这是一个UDP
	//UDP在worker进程中直接发送到客户端
	if(resp->info.from_id >= serv->poll_thread_num)
	{
		ret = swServer_send_udp_packet(serv, resp);
		goto finish;
	}

	//swQueue_data for msg queue
	struct {
		long pti;
		swEventData _send;
	} sdata;

	//写队列mtype
	sdata.pti = (c_worker_pti % serv->writer_num) + 1;

	//copy
	memcpy(sdata._send.data, resp->data, resp->info.len);

	sdata._send.info.fd = resp->info.fd;
	sdata._send.info.len = resp->info.len;
	sdata._send.info.from_id = resp->info.from_id;
	sendn =  resp->info.len + sizeof(resp->info);

	swTrace("[Worker]wt_queue[%ld]->in| fd=%d", sdata.pti, sdata._send.info.fd);

	for (count = 0; count < SW_WORKER_SENDTO_COUNT; count++)
	{
#if SW_WORKER_IPC_MODE == 2
		ret = object->wt_queue.in(&object->wt_queue, (swQueue_data *)&sdata, sendn);
#else
		ret = write(object->workers[c_worker_pti].pipe_worker, &sdata._send, sendn);
#endif
		//printf("wt_queue->in: fd=%d|from_id=%d|data=%s|ret=%d|errno=%d\n", sdata._send.info.fd, sdata._send.info.from_id, sdata._send.data, ret, errno);
		if (ret >= 0)
		{
			break;
		}
		else if (errno == EINTR)
		{
			continue;
		}
		else if (errno == EAGAIN)
		{
			swYield();
		}
		else
		{
			break;
		}
	}
	finish:
	if (ret < 0)
	{
		swWarn("[Worker#%d]sendto writer pipe or queue fail. Error: %s [%d]", getpid(), strerror(errno), errno);
	}
	return ret;
}

static int get_rand(int worker_pti)
{
	srand((int)time(0));
	return rand()%10 * worker_pti;
}

static int swFactoryProcess_worker_loop(swFactory *factory, int worker_pti)
{
	struct {
		long pti;
		swEventData req;
	} rdata;

	swFactoryProcess *object = factory->object;
	swServer *serv = factory->ptr;

	int pipe_rd = object->workers[worker_pti].pipe_worker;
	int n;

	c_worker_pti = worker_pti;
	object->manager_pid = getppid();

#if SW_WORKER_IPC_MODE == 2
	//抢占式,使用相同的队列type
	if (serv->dispatch_mode == SW_DISPATCH_QUEUE)
	{
		//这里必须加1
		rdata.pti = serv->worker_num + 1;
	}
	else
	{
		//必须加1
		rdata.pti = worker_pti + 1;
	}
#else
	swoole_worker_reactor = sw_malloc(sizeof(swReactor));
	if(swoole_worker_reactor == NULL)
	{
		swError("[Worker] malloc for reactor fail");
		return SW_ERR;
	}
	if(swReactorSelect_create(swoole_worker_reactor) < 0)
	{
		swError("[Worker] create worker_reactor fail");
		return SW_ERR;
	}
	swoole_worker_reactor->ptr = factory;
	swoole_worker_reactor->add(swoole_worker_reactor, pipe_rd, SW_FD_PIPE);
	swoole_worker_reactor->setHandle(swoole_worker_reactor, SW_FD_PIPE, swFactoryProcess_worker_receive);
#endif

	if(factory->max_request < 1)
	{
		worker_task_always = 1;
	}
	else
	{
		worker_task_num = factory->max_request;
		worker_task_num += get_rand(worker_pti);
	}

#ifdef HAVE_CPU_AFFINITY
	if (serv->open_cpu_affinity == 1)
	{
		cpu_set_t cpu_set;
		CPU_ZERO(&cpu_set);
		CPU_SET(worker_pti % SW_CPU_NUM, &cpu_set);
		if (0 != sched_setaffinity(getpid(), sizeof(cpu_set), &cpu_set))
		{
			swWarn("pthread_setaffinity_np set fail\n");
		}
	}
#endif
	if (serv->onWorkerStart != NULL)
	{
		//worker进程启动时调用
		serv->onWorkerStart(serv, worker_pti);
	}
	struct timeval timeo;
	timeo.tv_sec = SW_REACTOR_TIMEO_SEC;
	timeo.tv_usec = SW_REACTOR_TIMEO_USEC;

#if SW_WORKER_IPC_MODE == 2
	//主线程
	while (swoole_running > 0)
	{
		n = object->rd_queue.out(&object->rd_queue, (swQueue_data *)&rdata, sizeof(rdata.req));
		if (n < 0)
		{
			swWarn("[Worker]rd_queue[%ld]->out wait. Error: %s [%d]", rdata.pti, strerror(errno), errno);
			continue;
		}
		swFactoryProcess_worker_excute(factory, &rdata.req);
	}
#else
	swoole_worker_reactor->wait(swoole_worker_reactor, &timeo);
#endif
	if (serv->onWorkerStop != NULL)
	{
		//worker进程结束时调用
		serv->onWorkerStop(serv, worker_pti);
	}
	swTrace("[Worker]max request\n");
	return SW_OK;
}

/**
 * for msg queue
 * 头部放一个long让msg queue可以直接插入到消息队列中
 */
static struct {
	long pti;
	swDataHead _send;
} sw_notify_data;

/**
 * 主进程通知worker进程
 */
int swFactoryProcess_notify(swFactory *factory, swDataHead *ev)
{
	swFactoryProcess *object = factory->object;
	memcpy(&sw_notify_data._send, ev, sizeof(swDataHead));
	sw_notify_data._send.len = 0;
	return swFactoryProcess_send2worker(factory, (swEventData *)&sw_notify_data._send, -1);
}

/**
 * 主进程向worker进程发送数据
 * @param worker_id 发到指定的worker进程
 */
int swFactoryProcess_send2worker(swFactory *factory, swEventData *data, int worker_id)
{
	swFactoryProcess *object = factory->object;
	swServer *serv = factory->ptr;
	int pti;
	int ret;
	int send_len = sizeof(data->info) + data->info.len;

	if (worker_id < 0)
	{
		//轮询
		// data->info.from_id > serv->poll_thread_num, UDP必须使用轮询
		if (serv->dispatch_mode == SW_DISPATCH_ROUND || data->info.from_id > serv->poll_thread_num)
		{
			pti = (object->worker_pti++) % object->worker_num;
		}
		//使用fd取摸来散列
		else if (serv->dispatch_mode == SW_DISPATCH_FDMOD)
		{
			pti = data->info.fd % object->worker_num;
		}
		//使用抢占式队列(IPC消息队列)分配
		else
		{
#if SW_WORKER_IPC_MODE != 2
			swError("SW_DISPATCH_QUEUE must use (SW_WORKER_IPC_MODE = 2)");
#endif
			//msgsnd参数必须>0
			//worker进程中正确的mtype应该是pti + 1
			pti = object->worker_num;
		}
	}
	//指定了worker_id
	else
	{
		pti = worker_id;
	}
	swTrace("[ReadThread]sendto: pipe=%d|worker=%d\n", object->workers[pti].pipe_fd, pti);

#if SW_WORKER_IPC_MODE == 2
	//insert to msg queue
	swQueue_data *in_data = (swQueue_data *)((void *)data - sizeof(long));

	//加1防止id为0的worker进程出错
	in_data->mtype = pti + 1;

	swDataHead *info = (swDataHead *)in_data->mdata;
	ret = object->rd_queue.in(&object->rd_queue, in_data, send_len);
	swTrace("[Master]rd_queue[%ld]->in: fd=%d|type=%d|len=%d", in_data->mtype, info->fd, info->type, info->len);
#else
	//send to unix sock
	ret = swWrite(object->workers[pti].pipe_master, (char *) data, send_len);
#endif
	return ret;
}

int swFactoryProcess_dispatch(swFactory *factory, swEventData *data)
{
	swFactoryProcess *object = factory->object;
	data->info.type = SW_EVENT_DATA; //这是一个数据事件
	return swFactoryProcess_send2worker(factory, data, -1);
}

static int swFactoryProcess_writer_start(swFactory *factory)
{
	swFactoryProcess *object = factory->object;
	swThreadParam *param;
	int i;
	pthread_t pidt;
	swThreadStartFunc thread_main;

#if SW_WORKER_IPC_MODE == 2
	thread_main = (swThreadStartFunc) swFactoryProcess_writer_loop_queue;
#else
	thread_main = (swThreadStartFunc) swFactoryProcess_writer_loop_unsock;
#endif

	for (i = 0; i < object->writer_num; i++)
	{
		param = sw_memory_pool->alloc(sw_memory_pool, sizeof(swThreadParam));
		if (param == NULL)
		{
			swError("malloc fail\n");
			return SW_ERR;
		}
		param->object = factory;
		param->pti = i;
		if (pthread_create(&pidt, NULL, thread_main, (void *) param) < 0)
		{
			swTrace("pthread_create fail\n");
			return SW_ERR;
		}
		pthread_detach(pidt);
		object->writers[i].ptid = pidt;
		SW_START_SLEEP;
	}
	return SW_OK;
}

int swFactoryProcess_writer_excute(swFactory *factory, swEventData *resp)
{
	int ret;
	swServer *serv = factory->ptr;

	swSendData send_data;
	swDataHead closeFd;

	//表示关闭
	if (resp->info.len == 0)
	{
		close_fd:
		{
			closeFd.fd = resp->info.fd;
			closeFd.from_id = resp->info.from_id;
			//printf("closeFd.fd=%d|from_id=%d\n", closeFd.fd, closeFd.from_id);
			swServer_close(serv, &closeFd);
		}
		return SW_OK;
	}
	else
	{
		send_data.data = resp->data;
		send_data.info.len = resp->info.len;
		send_data.info.from_id = resp->info.from_id;
		send_data.info.fd = resp->info.fd;
		ret = factory->onFinish(factory, &send_data);
		if (ret < 0)
		{
			//连接已被关闭
			if (errno == EBADF)
			{
				goto close_fd;
			}
			swWarn("factory->onFinish fail.fd=%d|from_id=%d|errno=%d\n", resp->info.fd, resp->info.from_id, errno);
		}
		//printf("[writer]pop.fd=%d|from_id=%d|data=%s\n", resp->info.fd, resp->info.from_id, resp->data);
	}
	return SW_OK;
}

int swFactoryProcess_writer_receive(swReactor *reactor, swDataHead *ev)
{
	int n, ret;
	swFactory *factory = reactor->factory;
	swServer *serv = factory->ptr;
	swEventData resp;

	//Unix Sock UDP
	n = read(ev->fd, &resp, sizeof(resp));
	swTrace("[WriteThread]recv: writer=%d|pipe=%d\n", ev->from_id, ev->fd);
	if (n > 0)
	{
		return swFactoryProcess_writer_excute(factory, &resp);
	}
	else
	{
		swWarn("[WriteThread]sento fail.errno=%d\n", errno);
		return SW_ERR;
	}
}

/**
 * 使用Unix Socket通信
 */
int swFactoryProcess_writer_loop_unsock(swThreadParam *param)
{
	swFactory *factory = param->object;
	swFactoryProcess *object = factory->object;
	int pti = param->pti;
	swReactor *reactor = &(object->writers[pti].reactor);

	struct timeval tmo;
	tmo.tv_sec = 3;
	tmo.tv_usec = 0;

	reactor->factory = factory;
	reactor->id = pti;
	if (swReactorSelect_create(reactor) < 0)
	{
		swWarn("swReactorSelect_create fail\n");
		pthread_exit((void *) param);
	}
	swSingalNone();
	reactor->setHandle(reactor, SW_FD_PIPE, swFactoryProcess_writer_receive);
	reactor->wait(reactor, &tmo);
	reactor->free(reactor);
	pthread_exit((void *) param);
	return SW_OK;
}
/**
 * 使用消息队列通信
 */
int swFactoryProcess_writer_loop_queue(swThreadParam *param)
{
	swFactory *factory = param->object;
	swFactoryProcess *object = factory->object;
	swServer *serv = factory->ptr;

	int ret;
	int pti = param->pti;

	swQueue_data sdata;
	//必须加1,msg_type必须不能为0
	sdata.mtype = pti + 1;

	swSingalNone();
	while (swoole_running > 0)
	{
		swTrace("[Writer]wt_queue[%ld]->out wait", sdata.mtype);
		int ret = object->wt_queue.out(&object->wt_queue, &sdata, sizeof(sdata.mdata));
		if (ret < 0)
		{
			swWarn("[writer]wt_queue->out fail.Error: %s [%d]", strerror(errno), errno);
		}
		else
		{
			swFactoryProcess_writer_excute(factory, (swEventData *)sdata.mdata);
		}
	}
	pthread_exit((void *) param);
	return SW_OK;
}
