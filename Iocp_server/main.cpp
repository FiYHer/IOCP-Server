#include "iocp.h"
#include <iostream>
#include <string>

class Service : public iocp
{
public:
	Service() {}
	~Service() {}

	/* 连接错误事件 */
	virtual void on_connect_error_event(piocp_client client, piocp_io io, int error)
	{
		std::cout << "[+] 连接错误事件 " << inet_ntoa(client->remote_addr.sin_addr);
		std::cout << " - " << client->sock << std::endl;
	}

	/* 成功连接事件 */
	virtual void on_connect_finish_evnet(piocp_client client, piocp_io io)
	{
		std::cout << "[+] 成功连接事件 " << inet_ntoa(client->remote_addr.sin_addr);
		std::cout << " - " << client->sock << std::endl;
		send_text_to_client(client, "hello\n");
	}

	/* 断开连接事件 */
	virtual void on_connect_close_event(piocp_client client, piocp_io io)
	{
		std::cout << "[+] 断开连接事件 " << inet_ntoa(client->remote_addr.sin_addr);
		std::cout << " - " << client->sock << std::endl;
	}

	/* 接收完毕事件 */
	virtual void on_connect_recv_event(piocp_client client, piocp_io io)
	{
		std::cout << "[+] 接收完毕事件 " << inet_ntoa(client->remote_addr.sin_addr);
		std::cout << " - " << client->sock;
		std::cout << "  [" << io->io_index << "] -> " << io->buffer << std::endl;
		send_text_to_client(client, "nice,recv the data finish\n");
	}

	/* 发送完毕事件 */
	virtual void on_connect_send_event(piocp_client client, piocp_io io)
	{
		std::cout << "[+] 发送完毕事件 " << inet_ntoa(client->remote_addr.sin_addr) << std::endl;
	}
};

void test()
{
	Service g;

	if (g.initialize() == false)
	{
		std::cout << "[-] initialize失败" << std::endl;
		return;
	}

	if (g.start() == false)
	{
		std::cout << "[-] start失败" << std::endl;
		return;
	}

	std::string lines;
	while (true)
	{
		std::cout << "[+] input command : ";
		getline(std::cin, lines);

		if (lines == "exit") break;
		if (lines == "all") g.send_text_to_all_client("Nice To Meet You");
		if (lines == "close") g.close_all_connected();
		if (lines == "show")
		{
			std::cout << "[+] m_free_io_list is -> " << g.m_free_io_list.size() << std::endl;
			std::cout << "[+] m_free_client_list is -> " << g.m_free_client_list.size() << std::endl;
			std::cout << "[+] m_accept_io_list is -> " << g.m_accept_io_list.size() << std::endl;
			std::cout << "[+] m_connect_client_list is -> " << g.m_connect_client_list.size() << std::endl;
		}
	}

	g.release();
}

int main(int argc, char* argv[])
{
	test();

	system("pause");
	return 0;
}