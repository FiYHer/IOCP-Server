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
info : ������IOCP������ģ��
time : 2020.9.18
by : fyh
*/

/* �������� */
typedef enum iocp_type
{
	iocp_accept = 666,//���ܿͻ���
	iocp_send,//��������
	iocp_recv,//��������
	iocp_close//�Ͽ��ͻ���
}iocp_type, *piocp_type;

/* I/O�������ݽṹ */
typedef struct iocp_io
{
	WSAOVERLAPPED overlap{ 0 };//�ص��ṹ
	SOCKET sock = INVALID_SOCKET;//�׽���

	char* buffer = nullptr;//������ָ��
	int buffer_size = 0;//��������С

	unsigned __int64 io_index = 0;//I/O���к�
	iocp_type type = iocp_accept;//��������

	void release()
	{
		sock = INVALID_SOCKET;

		if (buffer && buffer_size) memset(buffer, 0, buffer_size);

		io_index = 0;
	}
}iocp_io, *piocp_io;

/* �ͻ������ݽṹ */
typedef struct iocp_client
{
	SOCKET sock = INVALID_SOCKET;//�׽���

	sockaddr_in local_addr{ 0 };//���ص�ַ
	sockaddr_in remote_addr{ 0 };//Զ�̵�ַ

	bool has_close = false;//�Ƿ�Ͽ�����

	unsigned __int64 send_numbers = 0;//�����������ݴ���
	unsigned __int64 recv_numbers = 0;//�����������ݴ���

	unsigned __int64 next_recv_index = 0;//��һ�����յ�I/O���к�
	unsigned __int64 current_recv_index = 0;//��ǰ���յ�I/O���к�

	std::list<piocp_io> lost_io_list;//û�а�˳����ɵ�IO����

	CRITICAL_SECTION criti_lock;//�ؼ���ͬ��

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
	bool m_initialize;//��ʼ��״̬
	bool m_start;//������״̬
	bool m_close;//����ر�״̬

	int m_port;//�����������˿�
	SOCKET m_sock;//�������׽���

	int m_max_accept_num;//���Ͷ�ݵ�Accept I/O����

	int m_max_connections;//���ͻ�������
	int m_max_io_pool;//���io�ṹ�ڴ������
	int m_max_client_pool;//���client�ṹ�ڴ������

	HANDLE m_completion;//��ɶ˿ھ��

	LPFN_ACCEPTEX m_acceptex;//AcceptEx
	LPFN_GETACCEPTEXSOCKADDRS m_getacceptexsockaddrs;//GetAcceptExSockAddrs

	HANDLE m_accept_event;//accept�¼�
	HANDLE m_repost_event;//resport�¼�
	LONGLONG m_report_number;//report����

	HANDLE m_listen_thread;//�����߳̾��
	int m_work_number;//�����߳�����

	CRITICAL_SECTION m_criti_io;//I/O�ṹ�ؼ���
	CRITICAL_SECTION m_criti_client;//client�ṹ�ؼ���
	CRITICAL_SECTION m_criti_accept;//accept���йؼ���
	CRITICAL_SECTION m_criti_connect;//connect���йؼ���

	std::list<piocp_io> m_free_io_list;//io�ṹ�����
	std::list<piocp_client> m_free_client_list;//client�ṹ�����
	std::list<piocp_io> m_accept_io_list;//io�ṹaccept�����
	std::list<piocp_client> m_connect_client_list;//client�ṹ�ͻ����ӻ����

	int m_max_post_send;//��������׳�send��������

private:
	/* �����߳� */
	static void __cdecl  listen_thread(void* data);

	/* �����߳� */
	static DWORD WINAPI work_thread(LPVOID data);

	/* ������ */
	static void error(bool state, const char* text)
	{
		if (state == false)
		{
			std::cout << "[-] " << text << std::endl;
			MessageBoxA(0, text, "����", MB_OK | MB_ICONERROR);
			exit(-1);
		}
	}

public:
	iocp();
	~iocp();

public:
	/* ����һ��I/O�ṹ */
	piocp_io alloc_io(int size);

	/* �ͷ�һ��I/O�ṹ */
	void release_io(piocp_io point);

	/* �ͷ�ȫ��I/O����� */
	void release_all_io();

	/* ����һ��client�ṹ */
	piocp_client alloc_client(SOCKET sock);

	/* �ͷ�һ��client�ṹ */
	void release_client(piocp_client point);

	/* �ͷ�ȫ��client�ṹ */
	void release_all_client();

public:
	/* ����accept���� */
	void push_accept_list(piocp_io point);

	/* ��accept�����Ƴ� */
	void pop_accept_list(piocp_io point);

	/* Ͷ��һ��accept���� */
	bool post_accept(piocp_io point);

	/* Ͷ��һ��recv���� */
	bool post_recv(piocp_client client, piocp_io io);

	/* Ͷ��һ��send���� */
	bool post_send(piocp_client client, piocp_io io);

	/* ����IO */
	void handle_io(DWORD key, piocp_io point, DWORD number, int error);

public:
	/* ���Ӵ����¼� */
	virtual void on_connect_error_event(piocp_client client, piocp_io io, int error) {}

	/* �ɹ������¼� */
	virtual void on_connect_finish_evnet(piocp_client client, piocp_io io) {}

	/* �Ͽ������¼� */
	virtual void on_connect_close_event(piocp_client client, piocp_io io) {}

	/* ��������¼� */
	virtual void on_connect_recv_event(piocp_client client, piocp_io io) {}

	/* ��������¼� */
	virtual void on_connect_send_event(piocp_client client, piocp_io io) {}

public:
	/* ��ʼ�� */
	bool initialize();

	/* �������� */
	bool release();

	/* �������� */
	bool start();

	/* �ر�ȫ���ͻ������� */
	void close_all_connected();

	/* �ر�ָ���ͻ������� */
	void close_connected(piocp_client client);

	/* ���һ���µĿͻ������б� */
	bool add_connect(piocp_client client);

	/* ��ȡ��ȷ��I/O�ṹ */
	piocp_io get_ture_io_info(piocp_client client, piocp_io io);

	/* ��ָ���û�����һ���ַ��� */
	bool send_text_to_client(piocp_client client, const char* text);

	/* ��ÿһ���û�����һ���ַ��� */
	bool send_text_to_all_client(const char* text);
};
