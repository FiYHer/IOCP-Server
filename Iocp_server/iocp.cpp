#pragma warning(disable : 4244 4018)
#define _CRT_SECURE_NO_WARNINGS

#include "iocp.h"

void __cdecl iocp::listen_thread(void* data)
{
	//强行转化
	iocp* pthis = (iocp*)data;
	iocp::error(pthis, "线程参数空指针");

	//设置标识
	pthis->m_close = false;

	//投递3个I/O
	for (int i = 0; i < 3; i++)
	{
		piocp_io point = pthis->alloc_io(500);
		if (point)
		{
			if (pthis->post_accept(point))
				pthis->push_accept_list(point);
			else
				pthis->release_io(point);
		}
	}

	iocp::error(pthis->m_work_number > 0, "listen_thread 工作线程数量少于1");
	//构造事件数组
	int size = 2 + pthis->m_work_number;
	HANDLE* wait_event = new HANDLE[size];
	iocp::error(wait_event, "listen_thread 构造时间数组失败");

	memset(wait_event, 0, size * sizeof(HANDLE));

	//前两个设置我们的事件句柄
	wait_event[0] = pthis->m_accept_event;
	wait_event[1] = pthis->m_repost_event;

	//创建工作线程
	for (int i = 0; i < pthis->m_work_number; i++)
	{
		wait_event[2 + i] = CreateThread(NULL, 0, work_thread, pthis, 0, nullptr);
		iocp::error(wait_event[2 + i], "listen_thread 工作线程创建失败");
	}

	//循环处理
	while (true)
	{
		//等待事件组的响应
		int index = WSAWaitForMultipleEvents(size, wait_event, FALSE, 60 * 1000, FALSE);

		//要关闭服务
		if (pthis->m_close || index == WSA_WAIT_FAILED)
		{
			pthis->m_start = false;
			break;
		}

		//等待超时
		if (index == WSA_WAIT_TIMEOUT)
		{
			EnterCriticalSection(&pthis->m_criti_accept);
			for (auto& it : pthis->m_accept_io_list)
			{
				//获取连接时间
				int timer = 0;
				int size = sizeof(int);
				getsockopt(it->sock, SOL_SOCKET, SO_CONNECT_TIME, (char *)&timer, &size);

				//大于5分钟不发送初始数据就踢
				if (timer != -1 && timer > 5 * 60)
				{
					if (it->sock != INVALID_SOCKET) closesocket(it->sock);
					it->sock = INVALID_SOCKET;
				}
			}
			LeaveCriticalSection(&pthis->m_criti_accept);

			//下一次循环
			continue;
		}

		//好的,某事件被触发了
		index -= WAIT_OBJECT_0;
		WSANETWORKEVENTS ne{ 0 };
		int limit = 0;

		//accept事件触发,说明要投递更多的accept请求
		if (index == 0)
		{
			WSAEnumNetworkEvents(pthis->m_sock, wait_event[index], &ne);
			if (ne.lNetworkEvents & FD_ACCEPT) limit = 10;
		}

		//report事件触发,说明有新的客户尝试连接
		else if (index == 1)
		{
			limit = InterlockedExchange64(&pthis->m_report_number, 0);
		}

		//工作事件触发,说明错误的发生
		else if (index > 1)
		{
			pthis->m_close = true;
			continue;
		}

		//投递accept请求
		for (int i = 0; i < limit && pthis->m_accept_io_list.size() < pthis->m_max_accept_num; i++)
		{
			piocp_io point = pthis->alloc_io(500);
			if (point)
			{
				if (pthis->post_accept(point))
					pthis->push_accept_list(point);
				else
					pthis->release_io(point);
			}
		}
	}

	//关闭全部客户连接
	pthis->close_all_connected();
	Sleep(1000);

	//通知全部工作线程退出
	for (int i = 0; i < pthis->m_work_number; i++)
		PostQueuedCompletionStatus(pthis->m_completion, -1, 0, NULL);

	//等待全部工作线程退出
	WaitForMultipleObjects(pthis->m_work_number, &wait_event[2], TRUE, 10 * 1000);

	//关闭全部工作线程句柄
	for (int i = 0; i < pthis->m_work_number; i++)
		CloseHandle(wait_event[2 + i]);

	//关闭监听套接字
	closesocket(pthis->m_sock);
	pthis->m_sock = INVALID_SOCKET;

	//关闭完成端口句柄
	CloseHandle(pthis->m_completion);
	pthis->m_completion = NULL;

	//释放全部空间
	pthis->release_all_io();
	pthis->release_all_client();

	//结束线程
	_endthread();
	return;
}

DWORD WINAPI iocp::work_thread(LPVOID data)
{
	//强行转化
	iocp* pthis = (iocp*)data;
	iocp::error(pthis, "work_thread 线程参数为空");

	//循环处理
	while (true)
	{
		DWORD numbers = 0;
		DWORD key = 0;
		LPOVERLAPPED overlap = nullptr;

		//等待消息
		BOOL state = GetQueuedCompletionStatus(pthis->m_completion, &numbers,
			(LPDWORD)&key, (LPOVERLAPPED*)&overlap, WSA_INFINITE);

		//退出消息
		if (numbers == -1) break;

		//获取io结构
		piocp_io point = CONTAINING_RECORD(overlap, iocp_io, overlap);

		//错误处理
		int error = NO_ERROR;
		if (state == FALSE)
		{
			SOCKET sock = INVALID_SOCKET;
			if (point->type == iocp_accept) sock = pthis->m_sock;
			else
			{
				if (key == 0) break;
				sock = ((piocp_client)key)->sock;
			}
			DWORD flags = 0;
			state = WSAGetOverlappedResult(sock, &point->overlap, &numbers, FALSE, &flags);
			if (state == FALSE) error = WSAGetLastError();
		}

		//处理事件
		pthis->handle_io(key, point, numbers, error);
	}

	return 0;
}

iocp::iocp()
{
	initialize();
}

iocp::~iocp()
{
	release();
}

piocp_io iocp::alloc_io(int size)
{
	iocp::error(size > 0, "alloc_io size小于0");

	piocp_io result = nullptr;
	EnterCriticalSection(&m_criti_io);
	//缓存池为空,申请新的空间
	if (m_free_io_list.empty())
	{
		result = new iocp_io;
		iocp::error(result, "alloc_io new iocp_io失败");

		result->release();
		result->buffer = new char[size];
		iocp::error(result->buffer, "alloc_io new result->buffer失败");

		memset(result->buffer, 0, size);
		result->buffer_size = size;
	}
	else//缓存池不为空,直接拿出来使用
	{
		result = m_free_io_list.back();
		m_free_io_list.pop_back();
	}
	LeaveCriticalSection(&m_criti_io);

	return result;
}

void iocp::release_io(piocp_io point)
{
	if (point == nullptr) return;

	EnterCriticalSection(&m_criti_io);

	//先查找列表里面是不是有该IO指针了,防止列表出现重复的
	bool state = false;
	for (const auto& it : m_free_io_list)
		if (it == point)
		{
			state = true;
			break;
		}

	//没有重复的
	if (state == false)
	{
		//缓存池数量达到最大,释放后面进来的
		if (m_free_io_list.size() > m_max_io_pool)
		{
			//先释放buffer后再释放自身
			if (point->buffer != nullptr) delete[] point->buffer;
			point->buffer = nullptr;
			delete point;
			point = nullptr;
		}
		else
		{
			//清空无用数据
			point->release();

			//加入列表
			m_free_io_list.push_back(point);
		}
	}

	LeaveCriticalSection(&m_criti_io);
}

void iocp::release_all_io()
{
	EnterCriticalSection(&m_criti_io);

	//释放全部
	for (auto& it : m_free_io_list)
	{
		if (it->buffer) delete[] it->buffer;
		it->buffer = nullptr;
		delete it;
		it = nullptr;
	}

	//清空
	m_free_io_list.clear();

	LeaveCriticalSection(&m_criti_io);
}

piocp_client iocp::alloc_client(SOCKET sock)
{
	if (sock == INVALID_SOCKET) return nullptr;

	piocp_client result = nullptr;
	EnterCriticalSection(&m_criti_client);
	if (m_free_client_list.empty())
	{
		//申请空间后初始化关键段
		result = new iocp_client;
		iocp::error(result, "alloc_client new失败");

		result->release();
		InitializeCriticalSection(&result->criti_lock);
	}
	else
	{
		//直接用缓存池从拿出
		result = m_free_client_list.back();

		//清空旧相关数据
		result->release();

		m_free_client_list.pop_back();
	}
	LeaveCriticalSection(&m_criti_client);

	//保存套接字
	result->sock = sock;
	return result;
}

void iocp::release_client(piocp_client point)
{
	if (point == nullptr) return;

	//先处理没有按顺序处理的I/O结构
	for (auto& it : point->lost_io_list) release_io(it);
	point->lost_io_list.clear();

	EnterCriticalSection(&m_criti_client);

	//判断列表里面是否有重复的
	bool state = false;
	for (const auto& it : m_free_client_list)
		if (it == point)
		{
			state = true;
			break;
		}

	//没有重复的
	if (state == false)
	{
		if (m_free_client_list.size() > m_max_client_pool)
		{
			//释放关键段后释放自身
			DeleteCriticalSection(&point->criti_lock);
			delete point;
			point = nullptr;
		}
		else
		{
			//清空无用信息
			point->release();

			//加入列表
			m_free_client_list.push_back(point);
		}
	}

	LeaveCriticalSection(&m_criti_client);
}

void iocp::release_all_client()
{
	EnterCriticalSection(&m_criti_client);

	//释放全部
	for (auto& it : m_free_client_list)
	{
		DeleteCriticalSection(&it->criti_lock);
		delete it;
		it = nullptr;
	}

	//清空
	m_free_client_list.clear();

	LeaveCriticalSection(&m_criti_client);
}

void iocp::push_accept_list(piocp_io point)
{
	if (point)
	{
		EnterCriticalSection(&m_criti_accept);
		m_accept_io_list.push_back(point);
		LeaveCriticalSection(&m_criti_accept);
	}
}

void iocp::pop_accept_list(piocp_io point)
{
	if (point == nullptr) return;

	EnterCriticalSection(&m_criti_accept);
	for (auto it = m_accept_io_list.begin(); it != m_accept_io_list.end(); it++)
	{
		if ((*it) == point)
		{
			m_accept_io_list.erase(it);
			break;
		}
	}
	LeaveCriticalSection(&m_criti_accept);
}

bool iocp::post_accept(piocp_io point)
{
	if (point == nullptr) return false;

	//设置类型
	point->type = iocp_accept;

	DWORD size = 0;
	point->sock = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	BOOL state = m_acceptex(m_sock, point->sock, point->buffer, point->buffer_size - ((sizeof(sockaddr_in) + 16) * 2),
		sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, &size, &point->overlap);
	if (state == FALSE && WSAGetLastError() != WSA_IO_PENDING) return false;
	return true;
}

bool iocp::post_recv(piocp_client client, piocp_io io)
{
	if (client == nullptr || io == nullptr) return false;

	//设置类型
	io->type = iocp_recv;

	EnterCriticalSection(&client->criti_lock);

	//设置序列号
	io->io_index = client->next_recv_index;

	DWORD size = 0, flags = 0;
	WSABUF buf{ 0 };
	buf.buf = io->buffer;
	buf.len = io->buffer_size;
	int result = WSARecv(client->sock, &buf, 1, &size, &flags, &io->overlap, NULL);
	if (result != NO_ERROR)
	{
		int code = WSAGetLastError();
		if (code != WSA_IO_PENDING)
		{
			LeaveCriticalSection(&client->criti_lock);
			return false;
		}
	}

	client->next_recv_index++;
	client->recv_numbers++;
	LeaveCriticalSection(&client->criti_lock);

	return true;
}

bool iocp::post_send(piocp_client client, piocp_io io)
{
	if (client == nullptr || io == nullptr) return false;

	//防止客户只发送消息而不接收消息
	if (client->send_numbers > m_max_post_send) return false;

	EnterCriticalSection(&client->criti_lock);

	//设置类型
	io->type = iocp_send;

	DWORD size = 0, flags = 0;
	WSABUF buf{ 0 };
	buf.buf = io->buffer;
	buf.len = io->buffer_size;
	int result = WSASend(client->sock, &buf, 1, &size, flags, &io->overlap, NULL);
	if (result == NO_ERROR)
	{
		client->send_numbers++;
	}
	LeaveCriticalSection(&client->criti_lock);

	return  result == NO_ERROR;
}

void iocp::handle_io(DWORD key, piocp_io point, DWORD number, int error)
{
	//强制转换
	piocp_client client = (piocp_client)key;

	//减少相关计数
	if (client)
	{
		EnterCriticalSection(&client->criti_lock);
		if (point->type == iocp_recv) client->recv_numbers--;
		if (point->type == iocp_send) client->send_numbers--;
		LeaveCriticalSection(&client->criti_lock);

		//如果要关闭连接
		if (client->has_close)
		{
			//没有了请求处理就直接释放
			if (client->recv_numbers == 0 && client->send_numbers == 0) release_client(client);
			return;
		}
	}
	else//从Accept队列拿出,应该进入Connect队列了
		pop_accept_list(point);

	//如果有错误,直接关闭套接字
	if (error != NO_ERROR)
	{
		//不是accept阶段了
		if (point->type != iocp_accept)
		{
			//显示通知
			on_connect_error_event(client, point, error);

			//关闭客户连接
			close_connected(client);

			//没有请求后释放
			if (client->recv_numbers == 0 && client->send_numbers == 0) release_client(client);
		}
		else//是accept阶段
		{
			if (point->sock != INVALID_SOCKET)
			{
				closesocket(point->sock);
				point->sock = INVALID_SOCKET;
			}
		}

		return;
	}

	//开始处理
	//Accept处理
	if (point->type == iocp_accept)
	{
		//客户端关闭连接
		if (number == 0)
		{
			if (point->sock != INVALID_SOCKET)
			{
				closesocket(point->sock);
				point->sock = INVALID_SOCKET;
			}
		}
		else
		{
			//为新的客户申请空间
			piocp_client new_client = alloc_client(point->sock);
			if (new_client)
			{
				//加入客户列表
				if (add_connect(new_client))
				{
					//获取客户地址
					int local_len = 0, remote_len = 0;
					LPSOCKADDR local_addr{ 0 }, remote_addr{ 0 };
					m_getacceptexsockaddrs(point->buffer, point->buffer_size - ((sizeof(sockaddr_in) + 16) * 2),
						sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16,
						(SOCKADDR **)&local_addr, &local_len, (SOCKADDR **)&remote_addr, &remote_len);

					//保存地址
					memcpy(&new_client->local_addr, local_addr, local_len);
					memcpy(&new_client->remote_addr, remote_addr, remote_len);

					//关联到完成端口
					HANDLE res = CreateIoCompletionPort((HANDLE)new_client->sock, m_completion, (DWORD)new_client, 0);
					iocp::error(res, "handle_io CreateIoCompletionPort失败");

					//通知用户
					on_connect_finish_evnet(new_client, point);

					//向新客户投递3个Read请求
					for (int i = 0; i < 3; i++)
					{
						piocp_io temp = alloc_io(500);
						if (temp)
						{
							bool state = post_recv(new_client, temp);
							if (state == false) close_connected(new_client);
							else std::cout << "[+] 向 " << temp->sock << " 投递Recv请求" << std::endl;
						}
					}
				}
				else
				{
					close_connected(new_client);
					release_client(new_client);
				}
			}
		}

		//释放
		release_io(point);

		//通知监听线程再次投递一个Accept请求
		InterlockedIncrement64(&m_report_number);

		//触发事件
		SetEvent(m_repost_event);
	}

	//Recv处理
	if (point->type == iocp_recv)
	{
		//客户断开连接
		if (number == 0)
		{
			//断开通知
			on_connect_close_event(client, point);

			//关闭连接
			close_connected(client);

			//没有相关通知了就释放
			if (client->recv_numbers == 0 && client->send_numbers == 0) release_client(client);
			release_io(point);
		}
		else
		{
			//按照正确的顺序读取IO消息
			piocp_io temp = get_ture_io_info(client, point);
			while (temp)
			{
				//完成读取消息
				on_connect_recv_event(client, temp);

				//递增序列号
				::InterlockedIncrement((LONG*)&client->current_recv_index);

				//释放I/O
				release_io(temp);

				//继续获取下一个
				temp = get_ture_io_info(client, NULL);
			}

			//继续投递一个新的Recv请求
			piocp_io value = alloc_io(500);
			if (value)
			{
				bool state = post_recv(client, value);
				if (state == false) close_connected(client);
				else std::cout << "向 " << value->sock << " 投递Recv请求" << std::endl;
			}
		}
	}

	//Send处理
	if (point->type == iocp_send)
	{
		//客户关闭套接字
		if (number == 0)
		{
			//关闭通知
			on_connect_close_event(client, point);

			//关闭连接
			close_connected(client);

			//如果没有了相关请求就直接释放
			if (client->recv_numbers == 0 && client->send_numbers == 0) release_client(client);
			release_io(point);
		}
		else
		{
			//写操作完毕通知
			on_connect_send_event(client, point);

			//释放
			release_io(point);
		}
	}

	return;
}

bool iocp::initialize()
{
	m_initialize = false;	// 没有初始化
	m_start = false;		// 没有开始
	m_close = true;		// 已经关闭

	m_port = 7744;		// 端口
	m_sock = INVALID_SOCKET;	// 无效套接字

	m_max_accept_num = 30;	// 最多AcceptI/O请求

	m_max_connections = 20000;		// 最多连接数
	m_max_io_pool = 200;
	m_max_client_pool = 200;

	m_completion = NULL;

	m_acceptex = nullptr;
	m_getacceptexsockaddrs = nullptr;

	m_accept_event = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (m_accept_event == NULL)
		return false;

	m_repost_event = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (m_repost_event == NULL)
		return false;

	m_report_number = 0;

	m_listen_thread = NULL;
	m_work_number = 4;

	InitializeCriticalSection(&m_criti_io);
	InitializeCriticalSection(&m_criti_client);
	InitializeCriticalSection(&m_criti_accept);
	InitializeCriticalSection(&m_criti_connect);

	m_max_post_send = 50;

	//初始化网络库
	WSADATA wsa{ 0 };
	m_initialize = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
	return m_initialize;
}

bool iocp::release()
{
	//关闭标识
	if (m_initialize == false) return false;
	m_initialize = false;
	m_start = false;
	m_close = true;

	//设置标识,让监听线程处理退出操作
	SetEvent(m_accept_event);
	WaitForSingleObject(m_listen_thread, INFINITE);

	//因为监听线程是使用_beginthread创建起来的,所以不能用CloseHandle()来关闭句柄
	m_listen_thread = NULL;

	WSACleanup();

	if (m_accept_event)
		CloseHandle(m_accept_event);
	if (m_repost_event)
		CloseHandle(m_repost_event);

	m_accept_event = NULL;
	m_repost_event = NULL;

	DeleteCriticalSection(&m_criti_io);
	DeleteCriticalSection(&m_criti_client);
	DeleteCriticalSection(&m_criti_accept);
	DeleteCriticalSection(&m_criti_connect);

	return true;
}

bool iocp::start()
{
	//不需要重复开启服务器
	if (m_start) return m_start;

	//返回状态
	bool back_state = false;
	__try
	{
		//创建TCP套接字
		m_sock = WSASocketA(AF_INET, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED);
		if (m_sock == INVALID_SOCKET) __leave;

		//绑定地址
		sockaddr_in addr{ 0 };
		addr.sin_family = AF_INET;
		addr.sin_addr.S_un.S_addr = INADDR_ANY;
		addr.sin_port = ntohs(m_port);
		int result = bind(m_sock, (const sockaddr*)&addr, sizeof(addr));
		if (result == SOCKET_ERROR) __leave;

		//监听端口
		result = listen(m_sock, 30);
		if (result == SOCKET_ERROR) __leave;

		//先创建一个IO完成端口
		m_completion = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
		if (m_completion == NULL) __leave;

		//获取AcceptEx函数地址
		GUID GuidAcceptEx = WSAID_ACCEPTEX;
		DWORD dwBytes = 0;
		result = WSAIoctl(m_sock, SIO_GET_EXTENSION_FUNCTION_POINTER,
			&GuidAcceptEx, sizeof(GuidAcceptEx), &m_acceptex, sizeof(m_acceptex),
			&dwBytes, NULL, NULL);
		if (result == SOCKET_ERROR) __leave;

		//获取GetAcceptExSockaddrs函数地址
		GUID GuidGetAcceptExSockaddrs = WSAID_GETACCEPTEXSOCKADDRS;
		result = WSAIoctl(m_sock, SIO_GET_EXTENSION_FUNCTION_POINTER,
			&GuidGetAcceptExSockaddrs, sizeof(GuidGetAcceptExSockaddrs), &m_getacceptexsockaddrs, sizeof(m_getacceptexsockaddrs),
			&dwBytes, NULL, NULL);
		if (result == SOCKET_ERROR) __leave;

		//将套接字与IO完成端口进行绑定
		if (CreateIoCompletionPort((HANDLE)m_sock, m_completion, 0, 0) == NULL) __leave;

		//注册Accept事件以投递Accept
		result = WSAEventSelect(m_sock, m_accept_event, FD_ACCEPT);
		if (result == SOCKET_ERROR) __leave;

		//创建监听事件
		m_listen_thread = (HANDLE)_beginthread(listen_thread, 0, this);
		if (m_listen_thread == NULL) __leave;

		//设置开始标识
		m_start = true;

		//设置返回状态
		back_state = true;
	}
	__finally
	{
		if (back_state == false)
		{
			if (m_sock != INVALID_SOCKET)
			{
				closesocket(m_sock);
				m_sock = INVALID_SOCKET;
			}

			if (m_completion != NULL)
			{
				CloseHandle(m_completion);
				m_completion = NULL;
			}

			if (m_listen_thread != NULL)
			{
				CloseHandle(m_listen_thread);
				m_listen_thread = NULL;
			}
		}
	}

	return back_state;
}

void iocp::close_all_connected()
{
	EnterCriticalSection(&m_criti_connect);

	for (auto& it : m_connect_client_list)
	{
		EnterCriticalSection(&it->criti_lock);

		//关闭套接字
		if (it->sock != INVALID_SOCKET) closesocket(it->sock);
		it->sock = INVALID_SOCKET;

		//设置关闭
		it->has_close = true;

		LeaveCriticalSection(&it->criti_lock);
	}

	//清空
	m_connect_client_list.clear();

	LeaveCriticalSection(&m_criti_connect);
}

void iocp::close_connected(piocp_client client)
{
	if (client == nullptr) return;

	//从连接队列里面拿出来
	EnterCriticalSection(&m_criti_connect);
	for (auto it = m_connect_client_list.begin(); it != m_connect_client_list.end(); it++)
	{
		if ((*it) == client)
		{
			m_connect_client_list.erase(it);
			break;
		}
	}
	LeaveCriticalSection(&m_criti_connect);

	EnterCriticalSection(&client->criti_lock);

	//关闭连接
	if (client->sock != INVALID_SOCKET) closesocket(client->sock);
	client->sock = INVALID_SOCKET;

	//设置关闭状态
	client->has_close = true;
	LeaveCriticalSection(&client->criti_lock);
}

bool iocp::add_connect(piocp_client client)
{
	if (client == nullptr) return false;
	if (client->sock == INVALID_SOCKET) return false;
	if (client->has_close) return false;

	bool state = true;
	EnterCriticalSection(&m_criti_connect);
	if (m_connect_client_list.size() < m_max_connections)
		m_connect_client_list.push_back(client);
	else
		state = false;
	LeaveCriticalSection(&m_criti_connect);

	return state;
}

piocp_io iocp::get_ture_io_info(piocp_client client, piocp_io io)
{
	if (client == nullptr) return nullptr;

	EnterCriticalSection(&client->criti_lock);

	if (io)
	{
		//序列号相同,直接返回
		if (client->current_recv_index == io->io_index) return io;

		//序列号不同,插入lost列表
		client->lost_io_list.push_back(io);
	}

	//查找序列号一样的
	piocp_io result = nullptr;
	for (auto it = client->lost_io_list.begin(); it != client->lost_io_list.end(); it++)
	{
		if (client->current_recv_index == (*it)->io_index)
		{
			result = (*it);
			client->lost_io_list.erase(it);
			break;
		}
	}

	LeaveCriticalSection(&client->criti_lock);

	return result;
}

bool iocp::send_text_to_client(piocp_client client, const char* text)
{
	piocp_io io = alloc_io(500);
	bool state = false;
	if (io)
	{
		strcpy(io->buffer, text);
		io->buffer_size = strlen(text);
		state = post_send(client, io);
	}
	return state;
}

bool iocp::send_text_to_all_client(const char* text)
{
	EnterCriticalSection(&m_criti_connect);
	for (const auto& it : m_connect_client_list)
	{
		send_text_to_client(it, text);
	}
	LeaveCriticalSection(&m_criti_connect);

	return true;
}