/*
Copyright 2018 The Sphinxd Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <sphinx/reactor.h>

#include <sphinx/reactor-epoll.h>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace sphinx::reactor {

SockAddr::SockAddr(::sockaddr_storage addr, ::socklen_t len)
  : addr{addr}
  , len{len}
{
}

Socket::Socket(int sockfd)
  : _sockfd{sockfd}
{
}

Socket::~Socket()
{
  ::close(_sockfd);
}

int
Socket::sockfd() const
{
  return _sockfd;
}

TcpListener::TcpListener(int sockfd, TcpAcceptFn&& accept_fn)
  : _sockfd{sockfd}
  , _accept_fn{accept_fn}
{
}

TcpListener::~TcpListener()
{
  ::close(_sockfd);
}

void
TcpListener::on_read_event()
{
  accept();
}

void
TcpListener::accept()
{
  int connfd = ::accept4(_sockfd, nullptr, nullptr, SOCK_NONBLOCK);
  if (connfd < 0) {
    throw std::system_error(errno, std::system_category(), "accept4");
  }
  _accept_fn(connfd);
}

int
TcpListener::sockfd() const
{
  return _sockfd;
}

static addrinfo*
lookup_addresses(const std::string& iface, int port, int sock_type)
{
  addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = sock_type;
  hints.ai_protocol = 0;
  hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;
  addrinfo* ret = nullptr;
  int err = getaddrinfo(iface.c_str(), std::to_string(port).c_str(), &hints, &ret);
  if (err != 0) {
    throw std::runtime_error("'" + iface + "': " + gai_strerror(err));
  }
  return ret;
}

std::unique_ptr<TcpListener>
make_tcp_listener(const std::string& iface, int port, int backlog, TcpAcceptFn&& accept_fn)
{
  auto* addresses = lookup_addresses(iface, port, SOCK_STREAM);
  for (addrinfo* rp = addresses; rp != NULL; rp = rp->ai_next) {
    int sockfd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sockfd < 0) {
      continue;
    }
    int one = 1;
    ::setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &one, sizeof(one));
    if (::bind(sockfd, rp->ai_addr, rp->ai_addrlen) < 0) {
      ::close(sockfd);
      continue;
    }
    if (::listen(sockfd, backlog) < 0) {
      ::close(sockfd);
      continue;
    }
    freeaddrinfo(addresses);
    return std::make_unique<TcpListener>(sockfd, std::move(accept_fn));
  }
  freeaddrinfo(addresses);
  throw std::runtime_error("Failed to listen to interface: '" + iface + "'");
}

TcpSocket::TcpSocket(int sockfd, TcpRecvFn&& recv_fn)
  : Socket{sockfd}
  , _recv_fn{recv_fn}
{
}

TcpSocket::~TcpSocket()
{
}

void
TcpSocket::set_tcp_nodelay(bool nodelay)
{
  int value = nodelay;
  if (setsockopt(_sockfd, SOL_TCP, TCP_NODELAY, &value, sizeof(value)) < 0) {
    throw std::system_error(errno, std::system_category(), "setsockopt");
  }
}

void
TcpSocket::send(const char* msg, size_t len, [[gnu::unused]] std::optional<SockAddr> dst)
{
  ssize_t nr = ::send(_sockfd, msg, len, MSG_NOSIGNAL | MSG_DONTWAIT);
  if ((nr < 0) && (errno == ECONNRESET || errno == EPIPE)) {
    return;
  }
  if (nr < 0) {
    throw std::system_error(errno, std::system_category(), "send");
  }
  if (size_t(nr) != len) {
    throw std::runtime_error("partial send");
  }
}

void
TcpSocket::on_read_event()
{
  constexpr size_t rx_buf_size = 256 * 1024;
  std::array<char, rx_buf_size> rx_buf;
  ssize_t nr = ::recv(_sockfd, rx_buf.data(), rx_buf.size(), MSG_DONTWAIT);
  if ((nr < 0 && errno == ECONNRESET)) {
    _recv_fn(this->shared_from_this(), std::string_view{});
    return;
  }
  if (nr < 0) {
    throw std::system_error(errno, std::system_category(), "recv");
  }
  _recv_fn(this->shared_from_this(),
           std::string_view{rx_buf.data(), std::string_view::size_type(nr)});
}

UdpSocket::UdpSocket(int sockfd, UdpRecvFn&& recv_fn)
  : Socket{sockfd}
  , _recv_fn{recv_fn}
{
}

UdpSocket::~UdpSocket()
{
}

void
UdpSocket::send(const char* msg, size_t len, std::optional<SockAddr> dst)
{
  ssize_t nr = ::sendto(_sockfd,
                        msg,
                        len,
                        MSG_NOSIGNAL | MSG_DONTWAIT,
                        reinterpret_cast<::sockaddr*>(&dst->addr),
                        dst->len);
  if ((nr < 0) && (errno == ECONNRESET || errno == EPIPE)) {
    return;
  }
  if (nr < 0) {
    throw std::system_error(errno, std::system_category(), "send");
  }
  if (size_t(nr) != len) {
    throw std::runtime_error("partial send");
  }
}

void
UdpSocket::on_read_event()
{
  constexpr size_t rx_buf_size = 256 * 1024;
  std::array<char, rx_buf_size> rx_buf;
  ::sockaddr_storage src_addr;
  ::socklen_t src_addr_len = sizeof(src_addr);
  ssize_t nr = ::recvfrom(_sockfd,
                          rx_buf.data(),
                          rx_buf.size(),
                          MSG_DONTWAIT,
                          reinterpret_cast<::sockaddr*>(&src_addr),
                          &src_addr_len);
  if ((nr < 0 && errno == ECONNRESET)) {
    _recv_fn(this->shared_from_this(), std::string_view{}, std::nullopt);
    return;
  }
  if (nr < 0) {
    throw std::system_error(errno, std::system_category(), "recvfrom");
  }
  SockAddr src{src_addr, src_addr_len};
  _recv_fn(this->shared_from_this(),
           std::string_view{rx_buf.data(), std::string_view::size_type(nr)},
           src);
}

std::shared_ptr<UdpSocket>
make_udp_socket(const std::string& iface, int port, UdpRecvFn&& recv_fn)
{
  auto* addresses = lookup_addresses(iface, port, SOCK_DGRAM);
  for (addrinfo* rp = addresses; rp != NULL; rp = rp->ai_next) {
    int sockfd = ::socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK, rp->ai_protocol);
    if (sockfd < 0) {
      continue;
    }
    int one = 1;
    ::setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &one, sizeof(one));
    if (::bind(sockfd, rp->ai_addr, rp->ai_addrlen) < 0) {
      ::close(sockfd);
      continue;
    }
    freeaddrinfo(addresses);
    return std::make_shared<UdpSocket>(sockfd, std::move(recv_fn));
  }
  freeaddrinfo(addresses);
  throw std::runtime_error("Failed to listen to interface: '" + iface + "'");
}

pthread_t Reactor::_pthread_ids[max_nr_threads];
std::atomic<bool> Reactor::_thread_is_sleeping[max_nr_threads];
sphinx::spsc::Queue<void*, Reactor::_msg_queue_size> Reactor::_msg_queues[max_nr_threads][max_nr_threads];

void
handler([[gnu::unused]] int sig, [[gnu::unused]] siginfo_t* siginfo, [[gnu::unused]] void* data)
{
}

std::string
Reactor::default_backend()
{
  return "epoll";
}

Reactor::Reactor(size_t thread_id, size_t nr_threads, OnMessageFn&& on_message_fn)
  : _thread_id{thread_id}
  , _nr_threads{nr_threads}
  , _on_message_fn{on_message_fn}
{
  sigset_t mask;
  sigemptyset(&mask);
  struct sigaction sa;
  sa.sa_sigaction = handler;
  sa.sa_mask = mask;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  if (::sigaction(SIGUSR1, &sa, nullptr) < 0) {
    throw std::system_error(errno, std::system_category(), "sigaction");
  }

  sigaddset(&mask, SIGUSR1);
  if (::pthread_sigmask(SIG_BLOCK, &mask, NULL) < 0) {
    throw std::system_error(errno, std::system_category(), "pthread_sigmask");
  }
  _thread_is_sleeping[_thread_id].store(false, std::memory_order_seq_cst);
  _pthread_ids[_thread_id] = pthread_self();
}

Reactor::~Reactor()
{
}

size_t
Reactor::thread_id() const
{
  return _thread_id;
}

size_t
Reactor::nr_threads() const
{
  return _nr_threads;
}

bool
Reactor::send_msg(size_t remote_id, void* msg)
{
  if (remote_id == _thread_id) {
    throw std::invalid_argument("Attempting to send message to self");
  }
  auto& queue = _msg_queues[remote_id][_thread_id];
  if (!queue.try_to_emplace(msg)) {
    return false;
  }
  if (_thread_is_sleeping[remote_id].load(std::memory_order_seq_cst)) {
    _thread_is_sleeping[remote_id].store(false, std::memory_order_seq_cst);
    wake_up(remote_id);
  }
  return true;
}

void
Reactor::wake_up(size_t thread_id)
{
  ::pthread_kill(_pthread_ids[thread_id], SIGUSR1);
}

bool
Reactor::has_messages() const
{
  for (size_t other = 0; other < _nr_threads; other++) {
    if (other == _thread_id) {
      continue;
    }
    auto& queue = _msg_queues[_thread_id][other];
    for (;;) {
      auto* msg = queue.front();
      if (!msg) {
        break;
      }
      return true;
    }
  }
  return false;
}

bool
Reactor::poll_messages()
{
  bool has_messages = false;
  for (size_t other = 0; other < _nr_threads; other++) {
    if (other == _thread_id) {
      continue;
    }
    auto& queue = _msg_queues[_thread_id][other];
    for (;;) {
      auto* msg = queue.front();
      if (!msg) {
        break;
      }
      has_messages |= true;
      _on_message_fn(*msg);
      queue.pop();
    }
  }
  return has_messages;
}

std::unique_ptr<Reactor>
make_reactor(const std::string& backend,
             size_t thread_id,
             size_t nr_threads,
             OnMessageFn&& on_message_fn)
{
  if (backend == "epoll") {
    return std::make_unique<EpollReactor>(thread_id, nr_threads, std::move(on_message_fn));
  } else {
    throw std::invalid_argument("unrecognized '" + backend + "' backend");
  }
}
}