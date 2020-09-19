#include "iocp.h"
#include <iostream>

class Service : public iocp
{
public:
	Service() {}
	~Service() {}

	/* 连接错误事件 */
	virtual void on_connect_error_event(piocp_client client, piocp_io io, int error)
	{
		std::cout << "[+] 连接错误事件 " << inet_ntoa(client->remote_addr.sin_addr) << std::endl;
	}

	/* 成功连接事件 */
	virtual void on_connect_finish_evnet(piocp_client client, piocp_io io)
	{
		std::cout << "[+] 成功连接事件 " << inet_ntoa(client->remote_addr.sin_addr) << std::endl;
		send_text_to_client(client, "hello\n");
	}

	/* 断开连接事件 */
	virtual void on_connect_close_event(piocp_client client, piocp_io io)
	{
		std::cout << "[+] 断开连接事件 " << inet_ntoa(client->remote_addr.sin_addr) << std::endl;
	}

	/* 接收完毕事件 */
	virtual void on_connect_recv_event(piocp_client client, piocp_io io)
	{
		std::cout << "[+] 接收完毕事件 " << inet_ntoa(client->remote_addr.sin_addr);
		std::cout << "  [" << io->io_index << "] -> " << io->buffer << std::endl;
		send_text_to_client(client, "nice,recv the data finish\n");
	}

	/* 发送完毕事件 */
	virtual void on_connect_send_event(piocp_client client, piocp_io io)
	{
		std::cout << "[+] 发送完毕事件 " << inet_ntoa(client->remote_addr.sin_addr) << std::endl;
	}
};

int main(int argc, char* argv[])
{
	Service g;
	assert(g.initialize() && "initialize");
	assert(g.start() && "start");
	std::cout << "[+] 服务器成功启动" << std::endl;

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