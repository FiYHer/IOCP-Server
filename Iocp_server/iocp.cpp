#pragma warning(disable : 4244 4018)
#define _CRT_SECURE_NO_WARNINGS

#include "iocp.h"

void __cdecl iocp::listen_thread(void* data)
{
	//ǿ��ת��
	iocp* pthis = (iocp*)data;

	//���ñ�ʶ
	pthis->m_close = false;

	//Ͷ�ݼ���I/O
	for (int i = 0; i < pthis->m_accept_num; i++)
	{
		piocp_io point = pthis->alloc_io(500);
		if (point)
		{
			pthis->push_accept_list(point);
			pthis->post_accept(point);
		}
	}

	//�����¼�����
	int size = 2 + pthis->m_work_number;
	HANDLE* wait_event = new HANDLE[size];
	assert(wait_event && "wait_event listen_thread");
	memset(wait_event, 0, size * sizeof(HANDLE));
	wait_event[0] = pthis->m_accept_event;
	wait_event[1] = pthis->m_repost_event;

	//���������߳�
	for (int i = 0; i < pthis->m_work_number; i++)
	{
		wait_event[2 + i] = (HANDLE)_beginthread(work_thread, 0, pthis);
		assert(wait_event[2 + i] && "_beginthread listen_thread");
	}

	//ѭ������
	while (true)
	{
		//�ȴ��¼�����Ӧ
		int index = WSAWaitForMultipleEvents(size, wait_event, FALSE, 60 * 1000, FALSE);

		//Ҫ�رշ���
		if (pthis->m_close || index == WSA_WAIT_FAILED)
		{
			//�ر�ȫ���ͻ�����
			pthis->close_all_connected();
			Sleep(1000);

			//�رռ����׽���
			closesocket(pthis->m_sock);
			pthis->m_sock = INVALID_SOCKET;
			Sleep(1000);

			//֪ͨȫ�������߳��˳�
			for (int i = 0; i < pthis->m_work_number; i++)
				PostQueuedCompletionStatus(pthis->m_completion, -1, 0, NULL);

			//�ȴ�ȫ�������߳��˳�
			WaitForMultipleObjects(pthis->m_work_number, &wait_event[2], TRUE, 10 * 1000);

			//�ر�ȫ�������߳̾��
			for (int i = 0; i < pthis->m_work_number; i++)
			{
				CloseHandle(wait_event[2 + i]);
				wait_event[2 + i] = NULL;
			}

			//�ر���ɶ˿ھ��
			CloseHandle(pthis->m_completion);
			pthis->m_completion = NULL;

			//�ͷ�ȫ���ռ�
			pthis->release_all_io();
			pthis->release_all_client();
			return;
		}

		//�ȴ���ʱ
		if (index == WSA_WAIT_TIMEOUT)
		{
			EnterCriticalSection(&pthis->m_criti_accept);
			for (auto& it : pthis->m_accept_io_list)
			{
				//��ȡ����ʱ��
				int timer = 0;
				int size = sizeof(int);
				getsockopt(it->sock, SOL_SOCKET, SO_CONNECT_TIME, (char *)&timer, &size);

				//����5���Ӳ����ͳ�ʼ���ݾ���
				if (timer != -1 && timer > 5 * 60)
				{
					if (it->sock != INVALID_SOCKET) closesocket(it->sock);
					it->sock = INVALID_SOCKET;
				}
			}
			LeaveCriticalSection(&pthis->m_criti_accept);

			//��һ��ѭ��
			continue;
		}

		//�õ�,ĳ�¼���������
		index -= WAIT_OBJECT_0;
		WSANETWORKEVENTS ne{ 0 };
		int limit = 0;

		//accept�¼�����,˵��ҪͶ�ݸ����accept����
		if (index == 0)
		{
			WSAEnumNetworkEvents(pthis->m_sock, wait_event[index], &ne);
			if (ne.lNetworkEvents & FD_ACCEPT) limit = 10;
		}

		//report�¼�����,˵�����µĿͻ���������
		if (index == 1)
		{
			limit = InterlockedExchange64(&pthis->m_report_number, 0);
		}

		//�����¼�����,˵������ķ���
		if (index > 1)
		{
			pthis->m_close = true;
			continue;
		}

		//Ͷ��accept����
		for (int i = 0; i < limit && pthis->m_accept_io_list.size() < pthis->m_max_accept_num; i++)
		{
			piocp_io point = pthis->alloc_io(500);
			if (point)
			{
				pthis->push_accept_list(point);
				pthis->post_accept(point);
			}
		}
	}

	return;
}

void __cdecl iocp::work_thread(void* data)
{
	//ǿ��ת��
	iocp* pthis = (iocp*)data;

	//ѭ������
	while (true)
	{
		DWORD numbers = 0;
		DWORD key = 0;
		LPOVERLAPPED overlap = nullptr;

		//�ȴ���Ϣ
		BOOL state = GetQueuedCompletionStatus(pthis->m_completion, &numbers,
			(LPDWORD)&key, (LPOVERLAPPED*)&overlap, WSA_INFINITE);

		//�˳���Ϣ
		if (numbers == -1) return;

		//��ȡio�ṹ
		piocp_io point = CONTAINING_RECORD(overlap, iocp_io, overlap);

		//������
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

		//�����¼�
		pthis->handle_io(key, point, numbers, error);
	}
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
	if (size <= 0) return nullptr;

	piocp_io result = nullptr;
	EnterCriticalSection(&m_criti_io);
	//�����Ϊ��,�����µĿռ�
	if (m_free_io_list.empty())
	{
		result = new iocp_io; assert(result && "result  alloc_io");
		result->release();
		result->buffer = new char[size]; assert(result->buffer && "result->buffer alloc_io");
		memset(result->buffer, 0, size);
		result->buffer_size = size;
	}
	else//����ز�Ϊ��,ֱ���ó���ʹ��
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
	//����������ﵽ���,�ͷź��������
	if (m_free_io_list.size() > m_max_io_pool)
	{
		//���ͷ�buffer�����ͷ�����
		if (point->buffer != nullptr) delete[] point->buffer;
		point->buffer = nullptr;
		delete point;
		point = nullptr;
	}
	else
	{
		//�����������
		point->release();

		//�����б�
		m_free_io_list.push_back(point);
	}
	LeaveCriticalSection(&m_criti_io);
}

void iocp::release_all_io()
{
	EnterCriticalSection(&m_criti_io);

	//�ͷ�ȫ��
	for (auto& it : m_free_io_list)
	{
		if (it->buffer) delete[] it->buffer;
		it->buffer = nullptr;
		delete it;
		it = nullptr;
	}

	//���
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
		//����ռ���ʼ���ؼ���
		result = new iocp_client; assert(result && "result alloc_client");
		result->release();
		InitializeCriticalSection(&result->criti_lock);
	}
	else
	{
		//ֱ���û���ش��ó�
		result = m_free_client_list.back();
		m_free_client_list.pop_back();
	}
	LeaveCriticalSection(&m_criti_client);

	//�����׽���
	result->sock = sock;
	return result;
}

void iocp::release_client(piocp_client point)
{
	if (point == nullptr) return;

	//�ȴ���û�а�˳�����I/O�ṹ
	for (auto& it : point->lost_io_list) release_io(it);
	point->lost_io_list.clear();

	EnterCriticalSection(&m_criti_client);
	if (m_free_client_list.size() > m_max_client_pool)
	{
		//�ͷŹؼ��κ��ͷ�����
		DeleteCriticalSection(&point->criti_lock);
		delete point;
		point = nullptr;
	}
	else
	{
		//���������Ϣ
		point->release();

		//�����б�
		m_free_client_list.push_back(point);
	}
	LeaveCriticalSection(&m_criti_client);
}

void iocp::release_all_client()
{
	EnterCriticalSection(&m_criti_client);

	//�ͷ�ȫ��
	for (auto& it : m_free_client_list)
	{
		DeleteCriticalSection(&it->criti_lock);
		delete it;
		it = nullptr;
	}

	//���
	m_free_client_list.clear();

	LeaveCriticalSection(&m_criti_client);
}

void iocp::push_accept_list(piocp_io point)
{
	if (point == nullptr) return;

	EnterCriticalSection(&m_criti_accept);
	m_accept_io_list.push_back(point);
	LeaveCriticalSection(&m_criti_accept);
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

	//��������
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

	//��������
	io->type = iocp_recv;

	EnterCriticalSection(&client->criti_lock);

	//�������к�
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

	//��ֹ�ͻ�ֻ������Ϣ����������Ϣ
	if (client->send_numbers > m_max_post_send) return false;

	EnterCriticalSection(&client->criti_lock);

	//��������
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
	//ǿ��ת��
	piocp_client client = (piocp_client)key;

	//������ؼ���
	if (client)
	{
		EnterCriticalSection(&client->criti_lock);
		if (point->type == iocp_recv) client->recv_numbers--;
		if (point->type == iocp_send) client->send_numbers--;
		LeaveCriticalSection(&client->criti_lock);

		//���Ҫ�ر�����
		if (client->has_close)
		{
			//û�����������ֱ���ͷ�
			if (client->recv_numbers == 0 && client->send_numbers == 0) release_client(client);
			return;
		}
	}
	else//��Accept�����ó�,Ӧ�ý���Connect������
	{
		pop_accept_list(point);
	}

	//����д���,ֱ�ӹر��׽���
	if (error != NO_ERROR)
	{
		//����accept�׶���
		if (point->type != iocp_accept)
		{
			//��ʾ֪ͨ
			on_connect_error_event(client, point, error);

			//�رտͻ�����
			close_connected(client);

			//û��������ͷ�
			if (client->recv_numbers == 0 && client->send_numbers == 0) release_client(client);
		}
		else//��accept�׶�
		{
			if (point->sock != INVALID_SOCKET)
			{
				closesocket(point->sock);
				point->sock = INVALID_SOCKET;
			}
		}

		return;
	}

	//��ʼ����
	//Accept����
	if (point->type == iocp_accept)
	{
		//�ͻ��˹ر�����
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
			//Ϊ�µĿͻ�����ռ�
			piocp_client new_client = alloc_client(point->sock);
			if (new_client)
			{
				//����ͻ��б�
				add_connect(new_client);

				//��ȡ�ͻ���ַ
				int local_len = 0, remote_len = 0;
				LPSOCKADDR local_addr{ 0 }, remote_addr{ 0 };
				m_getacceptexsockaddrs(point->buffer, point->buffer_size - ((sizeof(sockaddr_in) + 16) * 2),
					sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16,
					(SOCKADDR **)&local_addr, &local_len, (SOCKADDR **)&remote_addr, &remote_len);

				//�����ַ
				memcpy(&new_client->local_addr, local_addr, local_len);
				memcpy(&new_client->remote_addr, remote_addr, remote_len);

				//��������ɶ˿�
				assert(CreateIoCompletionPort((HANDLE)new_client->sock, m_completion, (DWORD)new_client, 0) && "new_client");

				//֪ͨ�û�
				on_connect_finish_evnet(new_client, point);

				//���¿ͻ�Ͷ��3��Read����
				for (int i = 0; i < 3; i++)
				{
					piocp_io temp = alloc_io(500);
					if (temp)
					{
						bool state = post_recv(new_client, temp);
						if (state == false)
						{
							close_connected(new_client);
							break;
						}
					}
				}
			}
		}

		//�ͷ�
		release_io(point);

		//֪ͨ�����߳��ٴ�Ͷ��һ��Accept����
		InterlockedIncrement64(&m_report_number);
		SetEvent(m_repost_event);
	}

	//Recv����
	if (point->type == iocp_recv)
	{
		//�ͻ��Ͽ�����
		if (number == 0)
		{
			//�Ͽ�֪ͨ
			on_connect_close_event(client, point);

			//�ر�����
			close_connected(client);

			//û�����֪ͨ�˾��ͷ�
			if (client->recv_numbers == 0 && client->send_numbers == 0) release_client(client);
			release_io(point);
		}
		else
		{
			//������ȷ��˳���ȡIO��Ϣ
			piocp_io temp = get_ture_io_info(client, point);
			while (temp)
			{
				//��ɶ�ȡ��Ϣ
				on_connect_recv_event(client, temp);

				//�������к�
				::InterlockedIncrement((LONG*)&client->current_recv_index);

				//�ͷ�I/O
				release_io(temp);

				//������ȡ��һ��
				temp = get_ture_io_info(client, NULL);
			}

			//����Ͷ��һ���µ�Recv����
			piocp_io value = alloc_io(500);
			if (value)
			{
				bool state = post_recv(client, value);
				if (state == false) close_connected(client);
			}
		}
	}

	//Send����
	if (point->type == iocp_send)
	{
		//�ͻ��ر��׽���
		if (number == 0)
		{
			//�ر�֪ͨ
			on_connect_close_event(client, point);

			//�ر�����
			close_connected(client);

			//���û������������ֱ���ͷ�
			if (client->recv_numbers == 0 && client->send_numbers == 0) release_client(client);
			release_io(point);
		}
		else
		{
			//д�������֪ͨ
			on_connect_send_event(client, point);

			//�ͷ�
			release_io(point);
		}
	}

	return;
}

bool iocp::initialize()
{
	m_initialize = false;
	m_start = false;
	m_close = true;

	m_port = 7744;
	m_sock = INVALID_SOCKET;

	m_accept_num = 5;
	m_max_accept_num = 30;

	m_max_connections = 2000;
	m_max_io_pool = 200;
	m_max_client_pool = 200;

	m_completion = NULL;

	m_acceptex = nullptr;
	m_getacceptexsockaddrs = nullptr;

	m_accept_event = CreateEventA(NULL, FALSE, FALSE, NULL); assert(m_accept_event && "m_accept_event");
	m_repost_event = CreateEventA(NULL, FALSE, FALSE, NULL); assert(m_repost_event && "m_repost_event");
	m_report_number = 0;

	m_listen_thread = NULL;
	m_work_number = 4;

	InitializeCriticalSection(&m_criti_io);
	InitializeCriticalSection(&m_criti_client);
	InitializeCriticalSection(&m_criti_accept);
	InitializeCriticalSection(&m_criti_connect);

	m_max_post_send = 50;

	//��ʼ�������
	WSADATA wsa{ 0 };
	m_initialize = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
	return m_initialize;
}

bool iocp::release()
{
	//�رձ�ʶ
	m_initialize = false;
	m_start = false;
	m_close = true;

	//���ñ�ʶ,�ü����̴߳����˳�����
	SetEvent(m_accept_event);
	WaitForSingleObject(m_listen_thread, INFINITE);

	WSACleanup();

	if (m_accept_event) CloseHandle(m_accept_event); m_accept_event = NULL;
	if (m_repost_event) CloseHandle(m_repost_event); m_repost_event = NULL;

	if (m_listen_thread) CloseHandle(m_listen_thread); m_listen_thread = NULL;

	DeleteCriticalSection(&m_criti_io);
	DeleteCriticalSection(&m_criti_client);
	DeleteCriticalSection(&m_criti_accept);
	DeleteCriticalSection(&m_criti_connect);

	return true;
}

bool iocp::start()
{
	//����Ҫ�ظ�����������
	if (m_start) return m_start;

	//����״̬
	bool back_state = false;
	__try
	{
		//����TCP�׽���
		m_sock = WSASocketA(AF_INET, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED);
		if (m_sock == INVALID_SOCKET) __leave;

		//�󶨵�ַ
		sockaddr_in addr{ 0 };
		addr.sin_family = AF_INET;
		addr.sin_addr.S_un.S_addr = INADDR_ANY;
		addr.sin_port = ntohs(m_port);
		int result = bind(m_sock, (const sockaddr*)&addr, sizeof(addr));
		if (result == SOCKET_ERROR) __leave;

		//�����˿�
		result = listen(m_sock, 30);
		if (result == SOCKET_ERROR) __leave;

		//�ȴ���һ��IO��ɶ˿�
		m_completion = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
		if (m_completion == NULL) __leave;

		//��ȡAcceptEx������ַ
		GUID GuidAcceptEx = WSAID_ACCEPTEX;
		DWORD dwBytes = 0;
		result = WSAIoctl(m_sock, SIO_GET_EXTENSION_FUNCTION_POINTER,
			&GuidAcceptEx, sizeof(GuidAcceptEx), &m_acceptex, sizeof(m_acceptex),
			&dwBytes, NULL, NULL);
		if (result == SOCKET_ERROR) __leave;

		//��ȡGetAcceptExSockaddrs������ַ
		GUID GuidGetAcceptExSockaddrs = WSAID_GETACCEPTEXSOCKADDRS;
		result = WSAIoctl(m_sock, SIO_GET_EXTENSION_FUNCTION_POINTER,
			&GuidGetAcceptExSockaddrs, sizeof(GuidGetAcceptExSockaddrs), &m_getacceptexsockaddrs, sizeof(m_getacceptexsockaddrs),
			&dwBytes, NULL, NULL);
		if (result == SOCKET_ERROR) __leave;

		//���׽�����IO��ɶ˿ڽ��а�
		CreateIoCompletionPort((HANDLE)m_sock, m_completion, 0, 0);

		//ע��Accept�¼���Ͷ��Accept
		result = WSAEventSelect(m_sock, m_accept_event, FD_ACCEPT);
		if (result == SOCKET_ERROR) __leave;

		//���������¼�
		m_listen_thread = (HANDLE)_beginthread(listen_thread, 0, this);
		if (m_listen_thread == NULL) __leave;

		//���ñ�ʶ
		m_start = true;
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

		//�ر��׽���
		if (it->sock != INVALID_SOCKET) closesocket(it->sock);
		it->sock = INVALID_SOCKET;

		//���ùر�
		it->has_close = true;

		LeaveCriticalSection(&it->criti_lock);
	}
	//���
	m_connect_client_list.clear();

	LeaveCriticalSection(&m_criti_connect);
}

void iocp::close_connected(piocp_client client)
{
	if (client == nullptr) return;

	//�����Ӷ��������ó���
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

	//�ر�����
	if (client->sock != INVALID_SOCKET) closesocket(client->sock);
	client->sock = INVALID_SOCKET;

	//���ùر�״̬
	client->has_close = true;
	LeaveCriticalSection(&client->criti_lock);
}

void iocp::add_connect(piocp_client client)
{
	if (client == nullptr) return;
	if (client->sock == INVALID_SOCKET) return;
	if (client->has_close) return;

	EnterCriticalSection(&m_criti_connect);
	if (m_connect_client_list.size() < m_max_connections)
	{
		m_connect_client_list.push_back(client);
	}
	LeaveCriticalSection(&m_criti_connect);
}

piocp_io iocp::get_ture_io_info(piocp_client client, piocp_io io)
{
	if (client == nullptr) return nullptr;

	if (io)
	{
		//���к���ͬ,ֱ�ӷ���
		if (client->current_recv_index == io->io_index) return io;

		//���кŲ�ͬ,����lost�б�
		client->lost_io_list.push_back(io);
	}

	//�������к�һ����
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