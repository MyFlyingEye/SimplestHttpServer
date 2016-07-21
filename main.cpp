#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <csignal>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>


constexpr size_t kBufferSize = 1048576;


std::string GetAddressFromHttp(const std::string& request) {
  size_t pos = request.find("GET");
  if (pos == std::string::npos) {
    return std::string();
  }

  size_t begin = request.find("/", pos);
  size_t end = request.find_first_of(" !#$%&=:?", begin);

  return request.substr(begin, end - begin);
}


ssize_t GetFileSize(std::string name) {
//  stat stat_buf;
//  return stat(name.c_str(), &stat_buf) == 0 ? stat_buf.st_size : -1;
  std::ifstream file(name, std::ifstream::ate | std::ifstream::binary);
  if (file.good()) {
    return file.tellg();
  }
  return -1;
}


std::string GetAnswer202(size_t content_length) {
  std::stringstream string;
  string << "HTTP/1.0 200 OK\r\n"
            "Content-length: " << content_length << "\r\n"
         << "Content-Type: text/html\r\n"
             "\r\n";
  return string.str();
}

std::string GetAnswer404() {
  static const std::string kAns404 = "HTTP/1.0 404 NOT FOUND\r\n"
      "Content-length: 0\r\n"
      "Content-Type: text/html\r\n"
      "\r\n";
  return kAns404;
}


class SocketTcpIp {
 public:
  SocketTcpIp()
      : descriptor_(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) {
    if (descriptor_ < 0) {
      perror("socket");
      throw std::runtime_error("TCP-IP Socket was not created");
    }
  }

  SocketTcpIp(int descriptor)
      : descriptor_(descriptor) {}

  SocketTcpIp(const SocketTcpIp&) = delete;

  SocketTcpIp(SocketTcpIp&& other)
      : descriptor_(other.descriptor_) {
    other.descriptor_ = -1;
  }

  ~SocketTcpIp() {
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
  }

  SocketTcpIp& operator=(const SocketTcpIp&) = delete;
  SocketTcpIp& operator=(SocketTcpIp&&) = delete;

  operator int() const noexcept {
    return Descriptor();
  }

  int Descriptor() const noexcept {
    return descriptor_;
  }

 private:
  int descriptor_;
};


class Server {
 public:
  static constexpr size_t kRecvBufferSize = kBufferSize;

  Server(uint16_t port, uint32_t address)
      : socket_(), port_(port), address_(address) {
    {
      int yes = 1;
      setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    }

    sockaddr_in sock_addr;
    std::memset(&sock_addr, 0, sizeof(sock_addr));
    sock_addr.sin_family = AF_INET;
    sock_addr.sin_port = htons(port_);
//    sock_addr.sin_addr.s_addr = htonl(address_);
    sock_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(socket_, (sockaddr*) &sock_addr, sizeof(sock_addr)) < 0) {
      perror("bind");
      throw std::runtime_error("Failed to bind socket");
    }

    if (listen(socket_, SOMAXCONN) < 0) {
      perror("listen");
      throw std::runtime_error("Failed to listen socket");
    }
  }

  SocketTcpIp Accept() const {
    SocketTcpIp client(accept(socket_, nullptr, nullptr));

    if (client.Descriptor() < 0) {
      perror("accept");
      throw std::runtime_error("Failed to accept connection");
    }

    return std::move(client);
  }

  void Shutdown(const SocketTcpIp& client) const {
    if (shutdown(client, SHUT_RDWR) < 0) {
      perror("shutdown");
      throw std::runtime_error("Failed to shutdown client connection");
    }
  }

  std::string Receive(const SocketTcpIp& client) const {
    char buffer[kRecvBufferSize] = {};

    ssize_t msg_size = recv(client, buffer, sizeof(buffer), MSG_NOSIGNAL);

    if (msg_size < 0) {
      perror("recv");
      throw std::runtime_error("Failed to receive from socket");
    }

    return std::string(buffer, static_cast<size_t>(msg_size));
  }

  void Send(const SocketTcpIp& client, const std::string& message) const {
    ssize_t sent_size = send(client, message.data(), message.size(),
                             MSG_NOSIGNAL);
    while (sent_size > 0) {
      sent_size = send(client, message.data() + sent_size,
                       message.size() - sent_size, MSG_NOSIGNAL);
    }

    if (sent_size < 0) {
      perror("send");
      throw std::runtime_error("Failed to send message to client");
    }
  }

 private:
  SocketTcpIp socket_;
  uint16_t port_;
  uint32_t address_;
};


struct ServerConfuguration {
  uint16_t port;
  uint32_t address;
  std::string directory;
};


ServerConfuguration ParseArgs(int argc, char* const argv[]) {
  static constexpr char kOptions[] = "h:p:d:";

  ServerConfuguration config = {80, INADDR_ANY, "/tmp"};

  int opt;
  while ((opt = getopt(argc, argv, kOptions)) != -1) {
    switch (opt) {
      case 'h': {
        in_addr addr;
        int res = inet_pton(AF_INET, optarg, &addr);
        if (res <= 0) {
          if (res == 0) {
            fprintf(stderr, "IP address not recognized\n");
          } else {
            perror("inet_pton");
          }
          throw std::runtime_error("Could not read IP address from arg");
        }
        config.address = addr.s_addr;
        break;
      }
      case 'p': {
        int port = std::atoi(optarg);
        if (port < 0 || port > static_cast<uint16_t>(-1)) {
          fprintf(stderr, "Port is out of range");
          throw std::runtime_error("Wrong port value");
        }
        config.port = static_cast<uint16_t>(port);
        break;
      }
      case 'd': {
        config.directory = optarg;
        break;
      }
      default: {
        fprintf(stderr, "Usage: %s [-h ip] [-p port] [-d directory]\n",
                argv[0]);
        break;
      }
    }
  }

  return config;
}


void GetAndProcessRequest(const Server& server, SocketTcpIp&& client) {
  std::string request = server.Receive(client);
  std::string address = "." + GetAddressFromHttp(request);
  ssize_t filesize = GetFileSize(address);
  if (filesize >= 0) {
    server.Send(client, GetAnswer202(static_cast<size_t>(filesize)));

    std::ifstream file(address, std::ifstream::binary);
    std::string buf(kBufferSize, '\0');
    file.read(&buf[0], kBufferSize);
    while (file) {
      server.Send(client, buf);
      file.read(&buf[0], kBufferSize);
    }
    buf.resize(file.gcount());
    server.Send(client, buf);
  } else {
    server.Send(client, GetAnswer404());
  }
  server.Shutdown(client);
}


int main(int argc, char* argv[]) {
  std::signal(SIGHUP, SIG_IGN);
  daemon(0, 0);

  ServerConfuguration config = ParseArgs(argc, argv);
  Server server(config.port, config.address);
  if (chdir(config.directory.c_str()) < 0) {
    perror("chdir");
    throw std::runtime_error("Wrong directory name");
  }

  while (true) {
    SocketTcpIp client(std::move(server.Accept()));
    std::thread thread(GetAndProcessRequest, std::cref(server),
                       std::move(client));
    thread.detach();
  }

  return 0;
}
