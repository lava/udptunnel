#include <iostream>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <bev/linear_ringbuffer.hpp>

#include <boost/program_options.hpp>

#include <fmt/core.h>
#include <fmt/format.h>
// Raspberry Pi has an ancient version of libfmt.
#if FMT_VERSION < 60102
#  include <fmt/ostream.h>
#else
#  include <fmt/printf.h>
#endif

#define debug(x) std::cerr << #x << ": " << x << std::endl

// Wire Format:
//
// <4-byte PKTLEN> <MSG>
//
// TODO: If we want to support multiple clients at some point,

namespace po = boost::program_options;

std::string print_addr(const struct sockaddr_in *addr) {
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

std::string print_addr(const struct sockaddr *addr) {
  if (addr->sa_family == AF_INET)
    return print_addr(reinterpret_cast<const struct sockaddr_in *>(addr));
  else
    return "printing this addr not supported yet";
}

bool package_complete(const bev::linear_ringbuffer &buf) {
  if (buf.size() < 4)
    return false;
  uint32_t pktsize = *reinterpret_cast<const uint32_t *>(buf.begin());
  return buf.size() >= 4 + pktsize;
}


//                                       udp packets                                                        tunnel
//   (internet)                         -------------> [listen_port, udp] (remote) [connect_port, tcp] ----------------> (local)
//
//
//   If forwarding_port > 0:
//                                       udp packets                                                         tunnel
//   (server) [forwarding_port, udp]  <----------------                   (remote)                     <----------------- (local)
//
int remote_main(uint16_t forwarding_port, uint16_t udp_listenport, uint16_t tcp_listenport, bool accept_remote_udp) {
  int tcp_listenfd = ::socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(tcp_listenport);
  addr.sin_family = AF_INET;

  int yes = 1;
  ::setsockopt(tcp_listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

  ::bind(tcp_listenfd, reinterpret_cast<struct sockaddr *>(&addr),
         sizeof(addr));
  ::getsockname(tcp_listenfd, reinterpret_cast<struct sockaddr *>(&addr),
                &addrlen);
  ::listen(tcp_listenfd, 2);

  fmt::print("Listening on {}\n", print_addr(&addr));

  bool nochdir = true, noclose = true;
  daemon(nochdir, noclose);

  int udp_sockfd = ::socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in udp_addr;
  udp_addr.sin_family = AF_INET;
  udp_addr.sin_addr.s_addr = accept_remote_udp
    ? htonl(INADDR_ANY)
    : htonl(INADDR_LOOPBACK);
  udp_addr.sin_port = htons(udp_listenport);

  ::bind(udp_sockfd, reinterpret_cast<struct sockaddr *>(&udp_addr),
         sizeof(udp_addr));

  struct sockaddr_in tcp_client;

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
      // TODO: Drain incoming data from the client and print a warning if we ignore it.
      if (forwarding_port) {
        FD_SET(client, &rfds);
      }
      FD_SET(udp_sockfd, &rfds);
      if (package_complete(tcp_to_udp)) {
        FD_SET(udp_sockfd, &wfds);
      }
      if (udp_to_tcp.size() > 0) {
        FD_SET(client, &wfds);
      }

      if (::select(nfds, &rfds, &wfds, nullptr, nullptr) == -1) {
        if (errno == EINTR)
          continue;
        else
          break;
      }

      if (FD_ISSET(client, &wfds)) {
        ssize_t n =
            ::send(client, udp_to_tcp.read_head(), udp_to_tcp.size(), 0);
        if (n == 0) { // orderly shutdown
          break;
        }
        if (n < 0 && errno != EINTR) { // error while sending
          break;
        }
        if (n > 0) {
          udp_to_tcp.consume(n);
        }
      }

      if (FD_ISSET(udp_sockfd, &wfds)) {
        assert(tcp_to_udp.size() > 4);
        uint32_t pktsize =
            *reinterpret_cast<uint32_t *>(tcp_to_udp.read_head());
        assert(tcp_to_udp.size() >= 4 + pktsize);
        ssize_t n = ::sendto(udp_sockfd, tcp_to_udp.read_head() + 4, pktsize, 0,
                             reinterpret_cast<struct sockaddr *>(&udp_addr),
                             sizeof(udp_addr));
        if (n > 0) {
          // always clear the full packet, even if n < pktsize for some reason
          tcp_to_udp.consume(4 + pktsize);
        }
        // TODO: What happens if no one is listening on the receiving end?
        // Probably the packet'll just be swallowed, but maybe the kernel
        // also generates an error for localhost connections?
      }

      if (FD_ISSET(client, &rfds)) {
        ssize_t n =
            ::recv(client, tcp_to_udp.write_head(), tcp_to_udp.free_size(), 0);
        fmt::print("Received incoming tcp packet with {} bytes\n", n);
        std::cout << "\n" << std::flush;

        if (n > 0) { // The "normal" path
          tcp_to_udp.commit(n);
        }
        if (n == 0) {
          break; // Orderly shutdown
        }
        if (n < 0 && errno != EINTR) {
          break; // Error
        }
      }

      if (FD_ISSET(udp_sockfd, &rfds)) {
        char payload[65528]; // max udp packet size
        ssize_t n = ::recvfrom(udp_sockfd, &payload, sizeof(payload), 0,
                               nullptr, nullptr);

        fmt::print("Received incoming udp packet with {} bytes\n", n);
        std::cout << "\n" << std::flush;
        if (n > 0 && udp_to_tcp.free_size() >=
                         n + 4) { // packet drop if not enough free size
          int32_t n32 = n;
          ::memcpy(udp_to_tcp.write_head(), &n32, sizeof(n32));
          ::memcpy(udp_to_tcp.write_head() + 4, payload, n);
          udp_to_tcp.commit(n + 4);
        }
      }
    }

    fmt::printf("lost tcp connection, terminating;");
    return 0;
}

//
// If udp_listenport_local, then data will also be transferred from local
// to the remote.
//
int local_main(const std::string &server, uint16_t tcp_listenport_remote,
               uint16_t udp_forwardingport_local, uint16_t udp_listenport_local) {
  int udp_sockfd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udp_listenport_local) {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(udp_listenport_local);
    addr.sin_family = AF_INET;
    ::bind(udp_sockfd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
    ::getsockname(udp_sockfd, reinterpret_cast<struct sockaddr *>(&addr),
                  &addrlen);
    fmt::print("Local socket bound to {}\n", print_addr(&addr));
  }

  // Using `getaddrinfo()` with static linking generates a warning,
  // but as long as we use it only in `local_main()` we should be fine.
  auto ports = std::to_string(tcp_listenport_remote);
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

  std::fflush(stdout);

  std::vector<char> out_pktbuffer(8 * 1024 * 1024);
  bev::linear_ringbuffer tcp_to_udp;

  fd_set rfds;
  fd_set wfds;
  int nfds = std::max(udp_sockfd, tcp_sockfd) + 1;
  struct sockaddr_in client;
  client.sin_family = AF_INET;
  client.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  client.sin_port = htons(udp_forwardingport_local);
  while (true) {
    // todo: switch to poll (although for this use case we're pretty much
    // guaranteed small fds)
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_SET(tcp_sockfd, &rfds);
    if (udp_listenport_local) {
      FD_SET(udp_sockfd, &rfds);
    }
    if (::select(nfds, &rfds, nullptr, nullptr, nullptr) < 0) {
      perror("select: ");
      return -1;
    }

    int flags = 0;
    socklen_t client_len = sizeof(client);

    if (FD_ISSET(udp_sockfd, &rfds)) {
      fmt::printf("received udp msg from localhost\n");
      ssize_t n =
          ::recvfrom(udp_sockfd, &out_pktbuffer[0], out_pktbuffer.size(), flags,
                     reinterpret_cast<struct sockaddr *>(&client), &client_len);
      if (n > 0) {
        int32_t n32 = n;
        ::send(tcp_sockfd, &n32, sizeof(n32), MSG_MORE);
        ::send(tcp_sockfd, &out_pktbuffer[0], n, 0);
      } else {
        break;
      }
    }

    if (FD_ISSET(tcp_sockfd, &rfds)) {
      fmt::printf("received tcp msg from remote\n");
      int tcp_flags = 0;
      ssize_t n =
          ::recv(tcp_sockfd, tcp_to_udp.read_head(), tcp_to_udp.free_size(), tcp_flags);
      if (tcp_to_udp.size() >= 4) {
        uint32_t pktsize =
            *reinterpret_cast<uint32_t *>(tcp_to_udp.read_head());
        assert(tcp_to_udp.size() >= 4 + pktsize);
        ssize_t n = ::sendto(udp_sockfd, tcp_to_udp.read_head() + 4, pktsize, 0,
                             reinterpret_cast<struct sockaddr *>(&client),
                             sizeof(client));
        if (n > 0) {
          // always clear the full packet, even if n < pktsize for some reason
          tcp_to_udp.consume(4 + pktsize);
        }
      } else {
        break;
      }
    }
  }

  fmt::print("Lost TCP connection to server\n");

  ::close(tcp_sockfd);

  return 0;
}

int main(int argc, char *argv[]) {
  std::string server;
  std::string user;
  std::string logfile;
  uint16_t udp_listenport_remote;
  uint16_t tcp_listenport_remote;
  bool replicate;
  bool accept_external_udp;

  // clang-format off
  po::options_description desc("Options");
  desc.add_options()("help", "print help")
    ("port", po::value<uint16_t>(&udp_listenport_remote)->required(),
     "udp port where the remote listens")
    ("logfile", po::value<std::string>(&logfile),
     "log filename")
    ("server", po::value<std::string>(&server),
     "remote server")
    ("server-user", po::value<std::string>(&user),
     "remote server username")
    ("replicate", po::value<bool>(&replicate)->default_value(false),
     "whether to replicate the server onto the remote machine")
    ("external", po::value<bool>(&accept_external_udp)->default_value(false),
     "whether to accept external packets on the server")
    ("__remote-startup", po::value<uint16_t>(&tcp_listenport_remote),
      "ignore; not meant for users! (listen port of the tcp tunnel)");
    // todo: server-only, basically __remote-startup but exposed for users
  // clang-format on

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
    // TODO: We can't use boost default args here, otherwise `count()` above
    // is always true. But we should probably generate a random number here.
    tcp_listenport_remote = 44554;
  }
  po::notify(vm);

  if (!logfile.empty()) {
    FILE *f = ::fopen(logfile.c_str(), "w");
    ::dup2(fileno(f), STDOUT_FILENO);
    ::dup2(fileno(f), STDERR_FILENO);
  }

  if (remote) {
    fmt::print("Starting server end of udp tunnel\n");
    std::cout << "foo" << std::endl;
    uint16_t forwarding_port = 0;
    int retval = remote_main(forwarding_port, udp_listenport_remote, tcp_listenport_remote, accept_external_udp);
    // ::unlink(argv[0])
    return retval;
  }

  if (!vm.count("server") || !vm.count("server-user")) {
    std::cerr << "missing required arg\n" << std::endl;
    return 1;
  }

  auto local_filename = argv[0];
  auto remote_filename = "__udptunnel_server";

  std::string scp_cmd = fmt::format("scp {} {}@{}:~/{}", local_filename, user,
                                    server, remote_filename);
  std::string ssh_cmd = fmt::format("ssh {}@{} -- ./{} --__remote-startup {} "
                                    "--port {} --external {} --logfile udpserver.log",
                                    user, server, remote_filename,
                                    tcp_listenport_remote, udp_listenport_remote, accept_external_udp);

  //debug(ssh_cmd);

  if (replicate) {
    fmt::print("Uploading server binary\n");
    system(scp_cmd.c_str());
  }
  system(ssh_cmd.c_str());

  //::sleep(2); // wait for remote server startup

  local_main(server, tcp_listenport_remote,
      udp_listenport_remote, // forwarding port
      0);                    // listen port

  std::string clean_cmd =
      fmt::format("ssh {}@{} -- rm {}", user, server, remote_filename);
  return 0;
}
