#include <iostream>
#include <vector>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>

#include <bev/linear_ringbuffer.hpp>

#include <boost/program_options.hpp>

#include <fmt/format.h>
#include <fmt/printf.h>



#define debug(x) std::cerr <<  #x  << ": " << x << std::endl

// Wire Format:
//
// <4-byte PKTLEN> <MSG>
//
// TODO: If we want to support multiple clients at some point, 

namespace po = boost::program_options;


std::string print_addr(const struct sockaddr_in* addr) {
	if (!addr) {
		return "null address";
	}
	std::string str(32, 0);
	::inet_ntop(addr->sin_family, &addr->sin_addr, &str[0], str.size());
	str.resize(::strlen(str.c_str()));
	str += ":";
	str += std::to_string(ntohs(addr->sin_port));
	return str;
}

std::string print_addr(const struct sockaddr* addr) {
	if (addr->sa_family == AF_INET)
		return print_addr(reinterpret_cast<const struct sockaddr_in*>(addr));
	else
		return "printing this addr not supported yet";
}


bool package_complete(const bev::linear_ringbuffer& buf) {
	if (buf.size() < 4)
		return false;
	uint32_t pktsize = *reinterpret_cast<const uint32_t*>(buf.begin());
	return buf.size() >= 4 + pktsize;
}


int remote_main(uint16_t listen_port, uint16_t connect_port) {
	int tcp_listenfd = ::socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);
	addr.sin_addr.s_addr = ntohl(INADDR_ANY);
	addr.sin_port = htons(listen_port);
	addr.sin_family = AF_INET;
	::bind(tcp_listenfd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
	::getsockname(tcp_listenfd, reinterpret_cast<struct sockaddr*>(&addr), &addrlen);
	::listen(tcp_listenfd, 2);
	
	fmt::print("Listening on {}\n", print_addr(&addr));

	int udp_sockfd = ::socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in udp_addr;
	udp_addr.sin_family = AF_INET;
	udp_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	udp_addr.sin_port = htons(connect_port);

	struct sockaddr_in tcp_client;
	while (true) {
		// Accept only one client at the same time
		// (if we want to change that, we need to dynamically spawn a new udp
		// socket per connection)
		int client = ::accept(tcp_listenfd, NULL, NULL);
		fmt::print("Received incoming tcp connection\n");

		bev::linear_ringbuffer udp_to_tcp;
		bev::linear_ringbuffer tcp_to_udp;

		fd_set rfds;
		fd_set wfds;
		int nfds = std::max(client, udp_sockfd) + 1;
		while (true) {
			FD_ZERO(&rfds);
			FD_ZERO(&wfds);
			FD_SET(client, &rfds);
			FD_SET(udp_sockfd, &rfds);
			if (package_complete(tcp_to_udp)) {
				FD_SET(udp_sockfd, &wfds);
			}
			if (udp_to_tcp.size() > 0) {
				FD_SET(client, &wfds);
			}

			::select(nfds, &rfds, &wfds, nullptr, nullptr);
	
			if (FD_ISSET(client, &wfds)) {
				ssize_t n = ::send(client, udp_to_tcp.read_head(), udp_to_tcp.size(), 0);
				if (n > 0) {
					udp_to_tcp.consume(n);
				}
			}

			if (FD_ISSET(udp_sockfd, &wfds)) {
				assert(tcp_to_udp.size() > 4);
				uint32_t pktsize = *reinterpret_cast<uint32_t*>(tcp_to_udp.read_head());
				assert(tcp_to_udp.size() >= 4+pktsize);
				ssize_t n = ::sendto(udp_sockfd, tcp_to_udp.read_head()+4, pktsize, 0, reinterpret_cast<struct sockaddr*>(&udp_addr), sizeof(udp_addr));
				if (n > 0) {
					// always clear the full packet, even if n < pktsize for some reason
					tcp_to_udp.consume(4+pktsize);
				} // todo: error handling
			}

			if (FD_ISSET(client, &rfds)) {
				ssize_t n = ::recv(client, tcp_to_udp.write_head(), tcp_to_udp.free_size(), 0);
				if (n > 0) {
					tcp_to_udp.commit(n);
				}
			}

			if (FD_ISSET(udp_sockfd, &rfds)) {
				char payload[65528]; // max udp packet size
				ssize_t n = ::recvfrom(udp_sockfd, &payload, sizeof(payload), 0, nullptr, nullptr);
				if (n > 0 && udp_to_tcp.free_size() >= n+4) { // packet drop if not enough free size
					int32_t n32 = n;
					::memcpy(udp_to_tcp.write_head(), &n32, sizeof(n32));
					::memcpy(udp_to_tcp.write_head()+4, payload, n);
					udp_to_tcp.commit(n+4);
				}
			}
		}

		// todo: break out of the loop above on connection closed
		fmt::printf("dropped connection, awaiting next one;");
	}
}

int local_main(const std::string& server, uint16_t remote_port) {
	int sockfd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);
	addr.sin_addr.s_addr = ntohl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	addr.sin_family = AF_INET;
	::bind(sockfd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
	::getsockname(sockfd, reinterpret_cast<struct sockaddr*>(&addr), &addrlen);

	// Using `getaddrinfo()` with static linking generates a warning,
	// but as long as we use it only in `local_main()` we should be fine.
	auto ports = std::to_string(remote_port);
	debug(ports);
	struct addrinfo *res = nullptr;
	struct addrinfo hints = {0};
	hints.ai_family = AF_INET; // TODO: support ipv6
	hints.ai_socktype = SOCK_STREAM; 
	hints.ai_flags = AI_NUMERICSERV;
	int error = ::getaddrinfo(server.c_str(), ports.c_str(), &hints, &res);
	if (error) {
		fmt::print("Adress lookup error: {}\n", gai_strerror(errno));
		return -1;
	}
	fmt::print("Connecting to {}\n", print_addr(res->ai_addr));
	int tcp_sockfd = ::socket(AF_INET, res->ai_socktype, 0);
	if (::connect(tcp_sockfd, res->ai_addr, res->ai_addrlen)) {
		perror("connect:");
		return -1;
	}

	fmt::print("Local socket bound to \n", print_addr(&addr));

	std::vector<char> pktbuffer(8*1024*1024);

	fd_set rfds;
	fd_set wfds;
	int nfds = std::max(sockfd, tcp_sockfd) + 1;
	struct sockaddr_in client;
	while (true) {
		// todo: switch to poll (although for this use case we're pretty much guaranteed small fds)
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_SET(sockfd, &rfds);
		FD_SET(tcp_sockfd, &rfds);
		if (::select(nfds, &rfds, nullptr, nullptr, nullptr) < 0) {
			perror("select: ");
			return -1;
		}

		int flags = 0;
		socklen_t client_len = sizeof(client);

		if (FD_ISSET(sockfd, &rfds)) {
			fmt::printf("received udp msg from localhost\n");
			ssize_t n = ::recvfrom(sockfd, &pktbuffer[0],  pktbuffer.size(), flags, reinterpret_cast<struct sockaddr*>(&client), &client_len);
			if (n > 0) {
				int32_t n32 = n;
				::send(tcp_sockfd, &n, sizeof(n), MSG_MORE);
				::send(tcp_sockfd, &pktbuffer[0], n, 0);
			}
		}
		
		if (FD_ISSET(tcp_sockfd, &rfds)) {
			fmt::printf("received tcp msg from remote\n");
			int tcp_flags = 0;
			ssize_t n = ::recv(tcp_sockfd, &pktbuffer[0], pktbuffer.size(), tcp_flags);
			// todo: support more than 1 client
			::sendto(sockfd, &pktbuffer[0], n, 0, reinterpret_cast<struct sockaddr*>(&client), sizeof(client));
		}
	}

	::close(sockfd);

	return 0;
}

int main(int argc, char* argv[]) {
    std::string server;
    std::string user;
    uint16_t remote_port;
    uint16_t server_local_port;

    po::options_description desc("Options");
    desc.add_options()
	    ("help", "print help")
	    ("port", po::value<uint16_t>(&remote_port)->required(), "target udp port")
	    ("server", po::value<std::string>(&server), "remote server")
	    ("server-user", po::value<std::string>(&user), "remote server username")
	    ("__remote-startup", po::value<uint16_t>(&server_local_port), "ignore; not meant for users!")
    ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);

    if (vm.count("help")) {
        std::cout << desc << std::endl;
	return 0;
    }

    bool remote = false;
    if (vm.count("__remote-startup")) {
	remote = true;
    } else {
    // todo: figure out default arguments in boost
	server_local_port = 44554;
    }
    po::notify(vm);
 
    if (remote) {
	    return remote_main(server_local_port, remote_port);
    }

    if (!vm.count("server") || !vm.count("server-user")) {
	std::cerr << "missing required arg\n" << std::endl;
	return 1;
    }

    auto local_filename = argv[0];
    auto remote_filename = "__udptunnel_server";

    std::string scp_cmd = fmt::format("scp {} {}@{}:~/{}", local_filename, user, server, remote_filename);
    std::string ssh_cmd = fmt::format("ssh {}@{} -- ./{} --__remote-startup {} --port {} &", user, server, remote_filename, server_local_port, remote_port);

    system(scp_cmd.c_str());
    system(ssh_cmd.c_str());

    ::usleep(10000); // wait for remote server startup

    local_main(server, server_local_port);

    std::string clean_cmd = fmt::format("ssh {}@{} -- rm {}", user, server, remote_filename);
    return 0;
}