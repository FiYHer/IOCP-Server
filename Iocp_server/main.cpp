#include "iocp.h"
#include <iostream>

class Service : public iocp
{
public:
	Service() {}
	~Service() {}

	/* ���Ӵ����¼� */
	virtual void on_connect_error_event(piocp_client client, piocp_io io, int error)
	{
		std::cout << "[+] ���Ӵ����¼� " << inet_ntoa(client->remote_addr.sin_addr) << std::endl;
	}

	/* �ɹ������¼� */
	virtual void on_connect_finish_evnet(piocp_client client, piocp_io io)
	{
		std::cout << "[+] �ɹ������¼� " << inet_ntoa(client->remote_addr.sin_addr) << std::endl;
		send_text_to_client(client, "hello\n");
	}

	/* �Ͽ������¼� */
	virtual void on_connect_close_event(piocp_client client, piocp_io io)
	{
		std::cout << "[+] �Ͽ������¼� " << inet_ntoa(client->remote_addr.sin_addr) << std::endl;
	}

	/* ��������¼� */
	virtual void on_connect_recv_event(piocp_client client, piocp_io io)
	{
		std::cout << "[+] ��������¼� " << inet_ntoa(client->remote_addr.sin_addr);
		std::cout << "  [" << io->io_index << "] -> " << io->buffer << std::endl;
		send_text_to_client(client, "nice,recv the data finish\n");
	}

	/* ��������¼� */
	virtual void on_connect_send_event(piocp_client client, piocp_io io)
	{
		std::cout << "[+] ��������¼� " << inet_ntoa(client->remote_addr.sin_addr) << std::endl;
	}
};

int main(int argc, char* argv[])
{
	Service g;
	assert(g.initialize() && "initialize");
	assert(g.start() && "start");
	std::cout << "[+] �������ɹ�����" << std::endl;

	while (true)
	{
		char c = getchar();
		if (c == 'q') break;
		if (c == 'w') g.send_text_to_all_client("nice");
		if (c == 'e') g.close_all_connected();

		std::cout << "m_free_io_list : " << g.m_free_io_list.size() << std::endl;
		std::cout << "m_free_client_list : " << g.m_free_client_list.size() << std::endl;
		std::cout << "m_accept_io_list : " << g.m_accept_io_list.size() << std::endl;
		std::cout << "m_connect_client_list : " << g.m_connect_client_list.size() << std::endl;
	}

	g.release();
	return 0;
}