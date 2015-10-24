#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <string>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <iostream>

struct Options
{
	uint16_t port;
	int length;
	int number;
	bool transmit, receive, nodelay;
	std::string host;
	Options():port(0),length(0),number(0),transmit(false),receive(false),nodelay(false){};
};

struct SessionMessage
{
	int32_t number;
	int32_t length;
}__attribute__((packed));  //取消编译器的优化对齐，采用最小对齐方式

struct PayloadMessage
{
	int32_t length;
	char data[0];
};


//静态函数，作用域仅限于当前文件，不用担心不同模块重名
static int acceptOrDie(uint16_t port)
{
	int listenfd = ::socket(AF_INET, SOCK_STREAM, 0);
	assert(listenfd > 0);

	int yes = 1;
	if(::setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)))
	{
		perror("setsockopt");
		exit(1);
	}

	struct sockaddr_in addr;
	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;
	if(::bind(listenfd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)))
	{
		perror("bind");
		exit(1);
	}

	if(::listen(listenfd,5))
	{
		perror("listen");
		exit(1);
	}

	struct sockaddr_in peer_addr;
	bzero(&peer_addr, sizeof(peer_addr));
	socklen_t addrlen = 0;
	int sockfd = ::accept(listenfd, reinterpret_cast<struct sockaddr *>(&peer_addr), &addrlen);
	if (sockfd < 0)
	{
		perror("accept");
		exit(1);
	}
	::close(listenfd);
	return sockfd;
}

static int write_n(int sockfd, const void* buf, int length)
{
	int written = 0;
	ssize_t nw = 0;
	while(written < length)
	{
		nw = ::write(sockfd, static_cast<const char*>(buf) + written, length-written);
		if(nw > 0)
		{
			written += static_cast<int>(nw);
		}
		else if(nw == 0)
		{
			break;
		}
	}

	return written;
}

static int read_n(int sockfd, void* buf, int length)
{
	int nread = 0;
	while(nread < length)
	{
		ssize_t nr = ::read(sockfd, static_cast<char *>(buf), length);
		if (nr > 0)
		{
			nread += static_cast<int>(nr);
		}
		else if(nr == 0)
		{
			break;
		}
	}
	return nread;
}

struct sockaddr_in resolveOrDie(const char * host, uint16_t port)
{
	struct hostent* he = ::gethostbyname(host);
	if(!he)
	{
		perror("gethostbyname");
		exit(1);
	}
	assert(he->h_addrtype == AF_INET && he->h_length == sizeof(uint32_t));
	struct sockaddr_in addr;
	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr = *reinterpret_cast<struct in_addr *>(he->h_addr);
	return addr;
}

void transmit(const Options& opt)
{
	struct sockaddr_in addr = resolveOrDie(opt.host.c_str(),opt.port);
	printf("connecting to %s:%d\n", inet_ntoa(addr.sin_addr),opt.port);

	int sockfd = ::socket(AF_INET, SOCK_STREAM, 0);
	assert(sockfd >= 0);
	int ret = ::connect(sockfd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));

	if(ret)
	{
		perror("connect");
		printf("Unable to connect %s\n",opt.host.c_str());
		::close(sockfd);
		return;
	}

	printf("connected\n");
	struct SessionMessage sessionMessage;
	memset(&sessionMessage, 0, sizeof(sessionMessage));

	std::cout << "constructing sessionMessage\n";
	std::cout << "number is " << opt.number << std::endl;
	std::cout << "length is " << opt.length << std::endl;

	sessionMessage.number = htonl(opt.number);
	sessionMessage.length = htonl(opt.length);



	if(write_n(sockfd, &sessionMessage, sizeof(sessionMessage)) != sizeof(sessionMessage))
	{
		perror("write SessionMessage");
		exit(1);
	}

	const int total_len = static_cast<int>(sizeof(int32_t) + opt.length);
	PayloadMessage * payload = static_cast<PayloadMessage *>(::malloc(total_len));
	assert(payload);
	payload->length = htonl(opt.length);
	for(int i = 0; i < opt.length; ++i)
	{
		payload->data[i] = "0123456789ABCDEF"[i % 16];
	}

	std::cout << "length is "<< opt.length << " number is " << opt.number << std::endl;
	double total_mb = 1.0 * opt.length * opt.number / 1024 /1024;
	printf("%.3f MiB in total\n", total_mb);

	for (int i = 0; i < opt.number; ++i)
	{
		int nw = write_n(sockfd, payload, total_len);
		assert(nw == total_len);
		std::cout << "sending "<< (i+1) << " payload, waiting for ack...\n";
		int32_t ack = 0;
		int nr = read_n(sockfd, &ack, sizeof(ack));
		std::cout << "received ack...\n";
		assert(nr == sizeof(ack));
		ack = ntohl(ack);
		assert(ack = opt.length);
	}

	::free(payload);
	::close(sockfd);
}

void receive(const Options& opt)
{
	int sockfd = acceptOrDie(opt.port);

	struct SessionMessage sessionMessage = {0,0};
	if(read_n(sockfd, &sessionMessage, sizeof(sessionMessage)) != sizeof(sessionMessage))
	{
		perror("read sessonMessage");
		exit(1);
	}

	sessionMessage.number = ntohl(sessionMessage.number);
	sessionMessage.length = ntohl(sessionMessage.length);
	printf("receive number = %d\nreceive length = %d\n",
		sessionMessage.number,sessionMessage.length);
	const int total_len = static_cast<int>(sizeof(int32_t) + sessionMessage.length);
	PayloadMessage * payload = static_cast<PayloadMessage *>(::malloc(total_len));
	assert(payload);

	for (int i = 0;i < sessionMessage.number; ++i)
	{
		payload->length = 0;
		if(read_n(sockfd, &(payload->length), sizeof(payload->length)) != sizeof(payload->length))
		{
			perror("read length");
			exit(1);
		}
		std::cout << "received " << (i+1) << " payload\n";
		payload->length = ntohl(payload->length);
		assert(payload->length == sessionMessage.length);
		if (read_n(sockfd, payload->data, payload->length) != payload->length)
		{
			perror("read payload");
				exit(1);
		}
		int32_t ack = htonl(payload->length);
		std::cout << "send ack\n";
		if(write_n(sockfd, &ack, sizeof(ack)) != sizeof(ack))
		{
			perror("write ack");
			exit(1);
		}
	}
	
	::free(payload);
	::close(sockfd);

}

int main()
{
	Options options;
	printf("Please select your function: 1.server 2.client\n");
	int function = 0;
	std::cin >> function;

	options.port = 12345;
	options.length = 1000;
	options.number = 10;
	options.host = "localhost";

	if (function == 2)
	{
		transmit(options);
	}
	else if(function == 1)
	{
		receive(options);
	}
	else
	{
		assert(0);
	}
	return 0;
}