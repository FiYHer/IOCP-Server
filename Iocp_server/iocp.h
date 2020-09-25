#pragma once
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define CHECK_ERROR(state,text) if(state == false) {MessageBoxA(0,__FILE__,"Error",MB_OK | MB_ICONERROR);exit(-1);}

#include <Winsock2.h>
#include <Mswsock.h>
#pragma comment(lib,"Ws2_32.lib")

#include <Windows.h>
#include <process.h>
#include <stdio.h>
#include <assert.h>

#include <iostream>
#include <vector>
#include <list>

/*
info : 高性能IOCP服务器模型
time : 2020.9.18
by : fyh
*/

/* 操作类型 */
typedef enum iocp_type
{
	iocp_accept = 666,//接受客户端
	iocp_send,//发送数据
	iocp_recv,//接收数据
	iocp_close//断开客户端
}iocp_type, *piocp_type;

/* I/O操作数据结构 */
typedef struct iocp_io
{
	WSAOVERLAPPED overlap{ 0 };//重叠结构
	SOCKET sock = INVALID_SOCKET;//套接字

	char* buffer = nullptr;//缓冲区指针
	int buffer_size = 0;//缓冲区大小

	unsigned __int64 io_index = 0;//I/O序列号
	iocp_type type = iocp_accept;//操作类型

	void release()
	{
		sock = INVALID_SOCKET;

		if (buffer && buffer_size) memset(buffer, 0, buffer_size);

		io_index = 0;
	}
}iocp_io, *piocp_io;

/* 客户端数据结构 */
typedef struct iocp_client
{
	SOCKET sock = INVALID_SOCKET;//套接字

	sockaddr_in local_addr{ 0 };//本地地址
	sockaddr_in remote_addr{ 0 };//远程地址

	bool has_close = false;//是否断开连接

	unsigned __int64 send_numbers = 0;//连续发送数据次数
	unsigned __int64 recv_numbers = 0;//连续接收数据次数

	unsigned __int64 next_recv_index = 0;//下一个接收的I/O序列号
	unsigned __int64 current_recv_index = 0;//当前接收的I/O序列号

	std::list<piocp_io> lost_io_list;//没有按顺序完成的IO数据

	CRITICAL_SECTION criti_lock;//关键段同步

	void release()
	{
		sock = INVALID_SOCKET;

		has_close = false;

		send_numbers = 0;
		recv_numbers = 0;

		next_recv_index = 0;
		current_recv_index = 0;
	}
}iocp_client, *piocp_client;

class iocp
{
public:
	bool m_initialize;//初始化状态
	bool m_start;//服务开启状态
	bool m_close;//服务关闭状态

	int m_port;//服务器监听端口
	SOCKET m_sock;//服务器套接字

	int m_max_accept_num;//最多投递的Accept I/O数量

	int m_max_connections;//最大客户连接数
	int m_max_io_pool;//最大io结构内存池数量
	int m_max_client_pool;//最大client结构内存池数量

	HANDLE m_completion;//完成端口句柄

	LPFN_ACCEPTEX m_acceptex;//AcceptEx
	LPFN_GETACCEPTEXSOCKADDRS m_getacceptexsockaddrs;//GetAcceptExSockAddrs

	HANDLE m_accept_event;//accept事件
	HANDLE m_repost_event;//resport事件
	LONGLONG m_report_number;//report数量

	HANDLE m_listen_thread;//监听线程句柄
	int m_work_number;//监听线程数量

	CRITICAL_SECTION m_criti_io;//I/O结构关键段
	CRITICAL_SECTION m_criti_client;//client结构关键段
	CRITICAL_SECTION m_criti_accept;//accept队列关键段
	CRITICAL_SECTION m_criti_connect;//connect队列关键段

	std::list<piocp_io> m_free_io_list;//io结构缓存池
	std::list<piocp_client> m_free_client_list;//client结构缓存池
	std::list<piocp_io> m_accept_io_list;//io结构accept缓存池
	std::list<piocp_client> m_connect_client_list;//client结构客户连接缓存池

	int m_max_post_send;//最大连续抛出send请求数量

private:
	/* 监听线程 */
	static void __cdecl  listen_thread(void* data);

	/* 工作线程 */
	static DWORD WINAPI work_thread(LPVOID data);

	/* 错误检测 */
	static void error(bool state, const char* text)
	{
		if (state == false)
		{
			std::cout << "[-] " << text << std::endl;
			MessageBoxA(0, text, "错误", MB_OK | MB_ICONERROR);
			exit(-1);
		}
	}

public:
	iocp();
	~iocp();

public:
	/* 申请一个I/O结构 */
	piocp_io alloc_io(int size);

	/* 释放一个I/O结构 */
	void release_io(piocp_io point);

	/* 释放全部I/O缓存池 */
	void release_all_io();

	/* 申请一个client结构 */
	piocp_client alloc_client(SOCKET sock);

	/* 释放一个client结构 */
	void release_client(piocp_client point);

	/* 释放全部client结构 */
	void release_all_client();

public:
	/* 加入accept队列 */
	void push_accept_list(piocp_io point);

	/* 从accept队列移除 */
	void pop_accept_list(piocp_io point);

	/* 投递一个accept请求 */
	bool post_accept(piocp_io point);

	/* 投递一个recv请求 */
	bool post_recv(piocp_client client, piocp_io io);

	/* 投递一个send请求 */
	bool post_send(piocp_client client, piocp_io io);

	/* 处理IO */
	void handle_io(DWORD key, piocp_io point, DWORD number, int error);

public:
	/* 连接错误事件 */
	virtual void on_connect_error_event(piocp_client client, piocp_io io, int error) {}

	/* 成功连接事件 */
	virtual void on_connect_finish_evnet(piocp_client client, piocp_io io) {}

	/* 断开连接事件 */
	virtual void on_connect_close_event(piocp_client client, piocp_io io) {}

	/* 接收完毕事件 */
	virtual void on_connect_recv_event(piocp_client client, piocp_io io) {}

	/* 发送完毕事件 */
	virtual void on_connect_send_event(piocp_client client, piocp_io io) {}

public:
	/* 初始化 */
	bool initialize();

	/* 清理数据 */
	bool release();

	/* 开启服务 */
	bool start();

	/* 关闭全部客户的连接 */
	void close_all_connected();

	/* 关闭指定客户的连接 */
	void close_connected(piocp_client client);

	/* 添加一个新的客户进入列表 */
	bool add_connect(piocp_client client);

	/* 获取正确的I/O结构 */
	piocp_io get_ture_io_info(piocp_client client, piocp_io io);

	/* 向指定用户发送一段字符串 */
	bool send_text_to_client(piocp_client client, const char* text);

	/* 向每一个用户发送一段字符串 */
	bool send_text_to_all_client(const char* text);
};
