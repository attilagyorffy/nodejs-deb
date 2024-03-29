#include <node_net.h>
#include <v8.h>

#include <node.h>
#include <node_buffer.h>

#include <string.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h> /* inet_pton */

#include <netdb.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <sys/ioctl.h>

#ifdef __linux__
# include <linux/sockios.h> /* For the SIOCINQ / FIONREAD ioctl */
#endif
/* Non-linux platforms like OS X define this ioctl elsewhere */
#ifndef FIONREAD
#include <sys/filio.h>
#endif

#include <errno.h>

#ifdef __OpenBSD__
#include <sys/uio.h>
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))


namespace node {

using namespace v8;

static Persistent<String> errno_symbol;
static Persistent<String> syscall_symbol;

static Persistent<String> fd_symbol;
static Persistent<String> size_symbol;
static Persistent<String> address_symbol;
static Persistent<String> port_symbol;
static Persistent<String> type_symbol;
static Persistent<String> tcp_symbol;
static Persistent<String> unix_symbol;

static Persistent<FunctionTemplate> recv_msg_template;


#define FD_ARG(a)                                        \
  int fd;                                                \
  if (!(a)->IsInt32() || (fd = (a)->Int32Value()) < 0) { \
    return ThrowException(Exception::TypeError(          \
          String::New("Bad file descriptor argument"))); \
  }

static inline bool SetCloseOnExec(int fd) {
  return (fcntl(fd, F_SETFD, FD_CLOEXEC) != -1);
}


static inline bool SetNonBlock(int fd) {
  return (fcntl(fd, F_SETFL, O_NONBLOCK) != -1);
}


static inline bool SetSockFlags(int fd) {
  int flags = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
  return SetNonBlock(fd) && SetCloseOnExec(fd);
}


// Creates nonblocking pipe
static Handle<Value> Pipe(const Arguments& args) {
  HandleScope scope;
  int fds[2];

  if (pipe(fds) < 0) return ThrowException(ErrnoException(errno, "pipe"));

  if (!SetSockFlags(fds[0]) || !SetSockFlags(fds[1])) {
    int fcntl_errno = errno;
    close(fds[0]);
    close(fds[1]);
    return ThrowException(ErrnoException(fcntl_errno, "fcntl"));
  }

  Local<Array> a = Array::New(2);
  a->Set(Integer::New(0), Integer::New(fds[0]));
  a->Set(Integer::New(1), Integer::New(fds[1]));
  return scope.Close(a);
}


// Creates nonblocking socket pair
static Handle<Value> SocketPair(const Arguments& args) {
  HandleScope scope;

  int fds[2];

  // XXX support SOCK_DGRAM?
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
    return ThrowException(ErrnoException(errno, "socketpair"));
  }

  if (!SetSockFlags(fds[0]) || !SetSockFlags(fds[1])) {
    int fcntl_errno = errno;
    close(fds[0]);
    close(fds[1]);
    return ThrowException(ErrnoException(fcntl_errno, "fcntl"));
  }

  Local<Array> a = Array::New(2);
  a->Set(Integer::New(0), Integer::New(fds[0]));
  a->Set(Integer::New(1), Integer::New(fds[1]));
  return scope.Close(a);
}


// Creates a new non-blocking socket fd
// t.socket("TCP");
// t.socket("UNIX");
// t.socket("UDP");
static Handle<Value> Socket(const Arguments& args) {
  HandleScope scope;

  // default to TCP
  int domain = PF_INET;
  int type = SOCK_STREAM;
#ifdef SO_REUSEPORT
  bool set_reuseport = false;
#endif

  if (args[0]->IsString()) {
    String::Utf8Value t(args[0]->ToString());
    // FIXME optimize this cascade.
    if (0 == strcasecmp(*t, "TCP")) {
      domain = PF_INET;
      type = SOCK_STREAM;
    } else if (0 == strcasecmp(*t, "TCP4")) {
      domain = PF_INET;
      type = SOCK_STREAM;
    } else if (0 == strcasecmp(*t, "TCP6")) {
      domain = PF_INET6;
      type = SOCK_STREAM;
    } else if (0 == strcasecmp(*t, "UNIX")) {
      domain = PF_UNIX;
      type = SOCK_STREAM;
    } else if (0 == strcasecmp(*t, "UNIX_DGRAM")) {
      domain = PF_UNIX;
      type = SOCK_DGRAM;
    } else if (0 == strcasecmp(*t, "UDP")) {
      domain = PF_INET;
      type = SOCK_DGRAM;
#ifdef SO_REUSEPORT
      set_reuseport = true;
#endif
    } else if (0 == strcasecmp(*t, "UDP4")) {
      domain = PF_INET;
      type = SOCK_DGRAM;
#ifdef SO_REUSEPORT
      set_reuseport = true;
#endif
    } else if (0 == strcasecmp(*t, "UDP6")) {
      domain = PF_INET6;
      type = SOCK_DGRAM;
#ifdef SO_REUSEPORT
      set_reuseport = true;
#endif
    } else {
      return ThrowException(Exception::Error(
            String::New("Unknown socket type.")));
    }
  }

  int fd = socket(domain, type, 0);

  if (fd < 0) return ThrowException(ErrnoException(errno, "socket"));

  if (!SetSockFlags(fd)) {
    int fcntl_errno = errno;
    close(fd);
    return ThrowException(ErrnoException(fcntl_errno, "fcntl"));
  }

#ifdef SO_REUSEPORT
  // needed for datagrams to be able to have multiple processes listening to
  // e.g. broadcasted datagrams.
  if (set_reuseport) {
    int flags = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char *)&flags,
               sizeof(flags));
  }
#endif

  return scope.Close(Integer::New(fd));
}


// NOT AT ALL THREAD SAFE - but that's okay for node.js
// (yes this is all to avoid one small heap alloc)
static struct sockaddr *addr;
static socklen_t addrlen;
static inline Handle<Value> ParseAddressArgs(Handle<Value> first,
                                             Handle<Value> second,
                                             bool is_bind) {
  static struct sockaddr_un un;
  static struct sockaddr_in in;
  static struct sockaddr_in6 in6;

  if (first->IsString() && !second->IsString()) {
    // UNIX
    String::Utf8Value path(first->ToString());

    if ((size_t) path.length() >= ARRAY_SIZE(un.sun_path)) {
      return Exception::Error(String::New("Socket path too long"));
    }

    memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    memcpy(un.sun_path, *path, path.length());

    addr = (struct sockaddr*)&un;
    addrlen = sizeof(un) - sizeof(un.sun_path) + path.length() + 1;

  } else {
    // TCP or UDP
    memset(&in, 0, sizeof in);
    memset(&in6, 0, sizeof in6);

    int port = first->Int32Value();
    in.sin_port = in6.sin6_port = htons(port);
    in.sin_family = AF_INET;
    in6.sin6_family = AF_INET6;

    bool is_ipv4 = true;

    if (!second->IsString()) {
      in.sin_addr.s_addr = htonl(is_bind ? INADDR_ANY : INADDR_LOOPBACK);
      in6.sin6_addr = is_bind ? in6addr_any : in6addr_loopback;
    } else {
      String::Utf8Value ip(second->ToString());

      if (inet_pton(AF_INET, *ip, &(in.sin_addr)) <= 0) {
        is_ipv4 = false;
        if (inet_pton(AF_INET6, *ip, &(in6.sin6_addr)) <= 0) {
          return ErrnoException(errno, "inet_pton", "Invalid IP Address");
        }
      }
    }

    addr = is_ipv4 ? (struct sockaddr*)&in : (struct sockaddr*)&in6;
    addrlen = is_ipv4 ? sizeof in : sizeof in6;
  }
  return Handle<Value>();
}


// Bind with UNIX
//   t.bind(fd, "/tmp/socket")
// Bind with TCP
//   t.bind(fd, 80, "192.168.11.2")
//   t.bind(fd, 80)
static Handle<Value> Bind(const Arguments& args) {
  HandleScope scope;

  if (args.Length() < 2) {
    return ThrowException(Exception::TypeError(
          String::New("Must have at least two args")));
  }

  FD_ARG(args[0])

  Handle<Value> error = ParseAddressArgs(args[1], args[2], true);
  if (!error.IsEmpty()) return ThrowException(error);

  int flags = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));

  int r = bind(fd, addr, addrlen);

  if (r < 0) {
    return ThrowException(ErrnoException(errno, "bind"));
  }

  return Undefined();
}


static Handle<Value> Close(const Arguments& args) {
  HandleScope scope;

  FD_ARG(args[0])

  if (0 > close(fd)) {
    return ThrowException(ErrnoException(errno, "close"));
  }

  return Undefined();
}


// t.shutdown(fd, "read");      -- SHUT_RD
// t.shutdown(fd, "write");     -- SHUT_WR
// t.shutdown(fd, "readwrite"); -- SHUT_RDWR
// second arg defaults to "write".
static Handle<Value> Shutdown(const Arguments& args) {
  HandleScope scope;

  FD_ARG(args[0])

  int how = SHUT_WR;

  if (args[1]->IsString()) {
    String::Utf8Value t(args[1]->ToString());
    if (0 == strcasecmp(*t, "write")) {
      how = SHUT_WR;
    } else if (0 == strcasecmp(*t, "read")) {
      how = SHUT_RD;
    } else if (0 == strcasecmp(*t, "readwrite")) {
      how = SHUT_RDWR;
    } else {
      return ThrowException(Exception::Error(String::New(
            "Unknown shutdown method. (Use 'read', 'write', or 'readwrite'.)")));
    }
  }

  if (0 > shutdown(fd, how)) {
    return ThrowException(ErrnoException(errno, "shutdown"));
  }

  return Undefined();
}


// Connect with unix
//   t.connect(fd, "/tmp/socket")
//
// Connect with TCP or UDP
//   t.connect(fd, 80, "192.168.11.2")
//   t.connect(fd, 80, "::1")
//   t.connect(fd, 80)
//  the third argument defaults to "::1"
static Handle<Value> Connect(const Arguments& args) {
  HandleScope scope;

  if (args.Length() < 2) {
    return ThrowException(Exception::TypeError(
          String::New("Must have at least two args")));
  }

  FD_ARG(args[0])

  Handle<Value> error = ParseAddressArgs(args[1], args[2], false);
  if (!error.IsEmpty()) return ThrowException(error);

  int r = connect(fd, addr, addrlen);

  if (r < 0 && errno != EINPROGRESS) {
    return ThrowException(ErrnoException(errno, "connect"));
  }

  return Undefined();
}

#define ADDRESS_TO_JS(info, address_storage, addrlen) \
do { \
  char ip[INET6_ADDRSTRLEN]; \
  int port; \
  struct sockaddr_in *a4; \
  struct sockaddr_in6 *a6; \
  struct sockaddr_un *au; \
  if (addrlen == 0) { \
    (info)->Set(address_symbol, String::Empty()); \
  } else { \
    switch ((address_storage).ss_family) { \
      case AF_INET6: \
        a6 = (struct sockaddr_in6*)&(address_storage); \
        inet_ntop(AF_INET6, &(a6->sin6_addr), ip, INET6_ADDRSTRLEN); \
        port = ntohs(a6->sin6_port); \
        (info)->Set(address_symbol, String::New(ip)); \
        (info)->Set(port_symbol, Integer::New(port)); \
        break; \
      case AF_INET: \
        a4 = (struct sockaddr_in*)&(address_storage); \
        inet_ntop(AF_INET, &(a4->sin_addr), ip, INET6_ADDRSTRLEN); \
        port = ntohs(a4->sin_port); \
        (info)->Set(address_symbol, String::New(ip)); \
        (info)->Set(port_symbol, Integer::New(port)); \
        break; \
      case AF_UNIX: \
        /*
         * Three types of addresses (see man 7 unix):
         *   * unnamed:  sizeof(sa_family_t) (sun_path should not be used)
         *   * abstract (Linux extension): sizeof(struct sockaddr_un)
         *   * pathname: sizeof(sa_family_t) + strlen(sun_path) + 1
         */ \
        au = (struct sockaddr_un*)&(address_storage); \
        if (addrlen == sizeof(sa_family_t)) { \
          (info)->Set(address_symbol, String::Empty()); \
        } else if (addrlen == sizeof(struct sockaddr_un)) { \
          /* first byte is '\0' and all remaining bytes are name;
           * it is not NUL-terminated and may contain embedded NULs */ \
          (info)->Set(address_symbol, String::New(au->sun_path + 1, sizeof(au->sun_path - 1))); \
        } else { \
          (info)->Set(address_symbol, String::New(au->sun_path)); \
        } \
        break; \
      default: \
        (info)->Set(address_symbol, String::Empty()); \
    } \
  } \
} while (0)


static Handle<Value> GetSockName(const Arguments& args) {
  HandleScope scope;

  FD_ARG(args[0])

  struct sockaddr_storage address_storage;
  socklen_t len = sizeof(struct sockaddr_storage);

  int r = getsockname(fd, (struct sockaddr *) &address_storage, &len);

  if (r < 0) {
    return ThrowException(ErrnoException(errno, "getsockname"));
  }

  Local<Object> info = Object::New();

  ADDRESS_TO_JS(info, address_storage, len);

  return scope.Close(info);
}


static Handle<Value> GetPeerName(const Arguments& args) {
  HandleScope scope;

  FD_ARG(args[0])

  struct sockaddr_storage address_storage;
  socklen_t len = sizeof(struct sockaddr_storage);

  int r = getpeername(fd, (struct sockaddr *) &address_storage, &len);

  if (r < 0) {
    return ThrowException(ErrnoException(errno, "getsockname"));
  }

  Local<Object> info = Object::New();

  ADDRESS_TO_JS(info, address_storage, len);

  return scope.Close(info);
}


static Handle<Value> Listen(const Arguments& args) {
  HandleScope scope;

  FD_ARG(args[0])
  int backlog = args[1]->IsInt32() ? args[1]->Int32Value() : 128;

  if (0 > listen(fd, backlog)) {
    return ThrowException(ErrnoException(errno, "listen"));
  }


  return Undefined();
}


// var peerInfo = t.accept(server_fd);
//
//   peerInfo.fd
//   peerInfo.address
//   peerInfo.port
//
// Returns a new nonblocking socket fd. If the listen queue is empty the
// function returns null (wait for server_fd to become readable and try
// again)
static Handle<Value> Accept(const Arguments& args) {
  HandleScope scope;

  FD_ARG(args[0])

  struct sockaddr_storage address_storage;
  socklen_t len = sizeof(struct sockaddr_storage);

  int peer_fd = accept(fd, (struct sockaddr*) &address_storage, &len);

  if (peer_fd < 0) {
    if (errno == EAGAIN) return scope.Close(Null());
    if (errno == ECONNABORTED) return scope.Close(Null());
    return ThrowException(ErrnoException(errno, "accept"));
  }

  if (!SetSockFlags(peer_fd)) {
    int fcntl_errno = errno;
    close(peer_fd);
    return ThrowException(ErrnoException(fcntl_errno, "fcntl"));
  }

  Local<Object> peer_info = Object::New();

  peer_info->Set(fd_symbol, Integer::New(peer_fd));

  ADDRESS_TO_JS(peer_info, address_storage, len);

  return scope.Close(peer_info);
}


static Handle<Value> SocketError(const Arguments& args) {
  HandleScope scope;

  FD_ARG(args[0])

  int error;
  socklen_t len = sizeof(int);
  int r = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);

  if (r < 0) {
    return ThrowException(ErrnoException(errno, "getsockopt"));
  }

  return scope.Close(Integer::New(error));
}


//  var bytesRead = t.read(fd, buffer, offset, length);
//  returns null on EAGAIN or EINTR, raises an exception on all other errors
//  returns 0 on EOF.
static Handle<Value> Read(const Arguments& args) {
  HandleScope scope;

  if (args.Length() < 4) {
    return ThrowException(Exception::TypeError(
          String::New("Takes 4 parameters")));
  }

  FD_ARG(args[0])

  if (!Buffer::HasInstance(args[1])) {
    return ThrowException(Exception::TypeError(
          String::New("Second argument should be a buffer")));
  }

  Buffer * buffer = ObjectWrap::Unwrap<Buffer>(args[1]->ToObject());

  size_t off = args[2]->Int32Value();
  if (off >= buffer->length()) {
    return ThrowException(Exception::Error(
          String::New("Offset is out of bounds")));
  }

  size_t len = args[3]->Int32Value();
  if (off + len > buffer->length()) {
    return ThrowException(Exception::Error(
          String::New("Length is extends beyond buffer")));
  }

  ssize_t bytes_read = read(fd, (char*)buffer->data() + off, len);

  if (bytes_read < 0) {
    if (errno == EAGAIN || errno == EINTR) return Null();
    return ThrowException(ErrnoException(errno, "read"));
  }

  return scope.Close(Integer::New(bytes_read));
}

//  var info = t.recvfrom(fd, buffer, offset, length, flags);
//    info.size // bytes read
//    info.port // from port
//    info.address // from address
//  returns null on EAGAIN or EINTR, raises an exception on all other errors
//  returns object otherwise
static Handle<Value> RecvFrom(const Arguments& args) {
  HandleScope scope;

  if (args.Length() < 5) {
    return ThrowException(Exception::TypeError(
          String::New("Takes 5 parameters")));
  }

  FD_ARG(args[0])

  if (!Buffer::HasInstance(args[1])) {
    return ThrowException(Exception::TypeError(
          String::New("Second argument should be a buffer")));
  }

  Buffer * buffer = ObjectWrap::Unwrap<Buffer>(args[1]->ToObject());

  size_t off = args[2]->Int32Value();
  if (off >= buffer->length()) {
    return ThrowException(Exception::Error(
          String::New("Offset is out of bounds")));
  }

  size_t len = args[3]->Int32Value();
  if (off + len > buffer->length()) {
    return ThrowException(Exception::Error(
          String::New("Length is extends beyond buffer")));
  }

  int flags = args[4]->Int32Value();

  struct sockaddr_storage address_storage;
  socklen_t addrlen = sizeof(struct sockaddr_storage);

  ssize_t bytes_read = recvfrom(fd, (char*)buffer->data() + off, len, flags,
                                (struct sockaddr*) &address_storage, &addrlen);

  if (bytes_read < 0) {
    if (errno == EAGAIN || errno == EINTR) return Null();
    return ThrowException(ErrnoException(errno, "read"));
  }

  Local<Object> info = Object::New();

  info->Set(size_symbol, Integer::New(bytes_read));

  ADDRESS_TO_JS(info, address_storage, addrlen);

  return scope.Close(info);
}


// bytesRead = t.recvMsg(fd, buffer, offset, length)
// if (recvMsg.fd) {
//   receivedFd = recvMsg.fd;
// }
static Handle<Value> RecvMsg(const Arguments& args) {
  HandleScope scope;

  if (args.Length() < 4) {
    return ThrowException(Exception::TypeError(
          String::New("Takes 4 parameters")));
  }

  FD_ARG(args[0])

  if (!Buffer::HasInstance(args[1])) {
    return ThrowException(Exception::TypeError(
          String::New("Second argument should be a buffer")));
  }

  Buffer * buffer = ObjectWrap::Unwrap<Buffer>(args[1]->ToObject());

  size_t off = args[2]->Int32Value();
  if (off >= buffer->length()) {
    return ThrowException(Exception::Error(
          String::New("Offset is out of bounds")));
  }

  size_t len = args[3]->Int32Value();
  if (off + len > buffer->length()) {
    return ThrowException(Exception::Error(
          String::New("Length is extends beyond buffer")));
  }

  struct iovec iov[1];
  iov[0].iov_base = (char*)buffer->data() + off;
  iov[0].iov_len = len;

  struct msghdr msg;
  msg.msg_flags = 0;
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;
  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  /* Set up to receive a descriptor even if one isn't in the message */
  char cmsg_space[64]; // should be big enough
  msg.msg_controllen = 64;
  msg.msg_control = (void *) cmsg_space;

  ssize_t bytes_read = recvmsg(fd, &msg, 0);

  if (bytes_read < 0) {
    if (errno == EAGAIN || errno == EINTR) return Null();
    return ThrowException(ErrnoException(errno, "recvMsg"));
  }

  // Why not return a two element array here [bytesRead, fd]? Because
  // creating an object for each recvmsg() action is heavy. Instead we just
  // assign the recved fd to a globalally accessable variable (recvMsg.fd)
  // that the wrapper can pick up. Since we're single threaded, this is not
  // a problem - just make sure to copy out that variable before the next
  // call to recvmsg().
  //
  // XXX: Some implementations can send multiple file descriptors in a
  //      single message. We should be using CMSG_NXTHDR() to walk the
  //      chain to get at them all. This would require changing the
  //      API to hand these back up the caller, is a pain.

  int received_fd = -1;
  for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
       msg.msg_controllen > 0 && cmsg != NULL;
       cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_type == SCM_RIGHTS) {
      if (received_fd != -1) {
        fprintf(stderr, "ignoring extra FD received: %d\n", received_fd);
      }

      received_fd = *(int *) CMSG_DATA(cmsg);
    } else {
      fprintf(stderr, "ignoring non-SCM_RIGHTS ancillary data: %d\n",
        cmsg->cmsg_type
      );
    }
  }

  recv_msg_template->GetFunction()->Set(
    fd_symbol,
    (received_fd != -1) ?
      Integer::New(received_fd) :
      Null()
  );

  return scope.Close(Integer::New(bytes_read));
}


//  var bytesWritten = t.write(fd, buffer, offset, length);
//  returns null on EAGAIN or EINTR, raises an exception on all other errors
static Handle<Value> Write(const Arguments& args) {
  HandleScope scope;

  if (args.Length() < 4) {
    return ThrowException(Exception::TypeError(
          String::New("Takes 4 parameters")));
  }

  FD_ARG(args[0])

  if (!Buffer::HasInstance(args[1])) {
    return ThrowException(Exception::TypeError(
          String::New("Second argument should be a buffer")));
  }

  Buffer * buffer = ObjectWrap::Unwrap<Buffer>(args[1]->ToObject());

  size_t off = args[2]->Int32Value();
  if (off >= buffer->length()) {
    return ThrowException(Exception::Error(
          String::New("Offset is out of bounds")));
  }

  size_t len = args[3]->Int32Value();
  if (off + len > buffer->length()) {
    return ThrowException(Exception::Error(
          String::New("Length is extends beyond buffer")));
  }

  ssize_t written = write(fd, (char*)buffer->data() + off, len);

  if (written < 0) {
    if (errno == EAGAIN || errno == EINTR) {
      return scope.Close(Integer::New(0));
    }
    return ThrowException(ErrnoException(errno, "write"));
  }

  return scope.Close(Integer::New(written));
}


// var bytes = sendmsg(fd, buf, off, len, fd, flags);
//
// Write a buffer with optional offset and length to the given file
// descriptor. Note that we refuse to send 0 bytes.
//
// The 'fd' parameter is a numerical file descriptor, or the undefined value
// to send none.
//
// The 'flags' parameter is a number representing a bitmask of MSG_* values.
// This is passed directly to sendmsg().
//
// Returns null on EAGAIN or EINTR, raises an exception on all other errors
static Handle<Value> SendMsg(const Arguments& args) {
  HandleScope scope;

  struct iovec iov;

  if (args.Length() < 2) {
    return ThrowException(Exception::TypeError(
          String::New("Takes 2 parameters")));
  }

  // The first argument should be a file descriptor
  FD_ARG(args[0])

  // Grab the actul data to be written, stuffing it into iov
  if (!Buffer::HasInstance(args[1])) {
    return ThrowException(Exception::TypeError(
      String::New("Expected either a string or a buffer")));
  }

  Buffer *buf = ObjectWrap::Unwrap<Buffer>(args[1]->ToObject());

  size_t offset = 0;
  if (args.Length() >= 3 && !args[2]->IsUndefined()) {
    if (!args[2]->IsUint32()) {
      return ThrowException(Exception::TypeError(
        String::New("Expected unsigned integer for offset")));
    }

    offset = args[2]->Uint32Value();
    if (offset >= buf->length()) {
      return ThrowException(Exception::Error(
        String::New("Offset into buffer too large")));
    }
  }

  size_t length = buf->length() - offset;
  if (args.Length() >= 4 && !args[3]->IsUndefined()) {
    if (!args[3]->IsUint32()) {
      return ThrowException(Exception::TypeError(
        String::New("Expected unsigned integer for length")));
    }

    length = args[3]->Uint32Value();
    if (offset + length > buf->length()) {
      return ThrowException(Exception::Error(
        String::New("offset + length beyond buffer length")));
    }
  }

  iov.iov_base = buf->data() + offset;
  iov.iov_len = length;

  int fd_to_send = -1;
  if (args.Length() >= 5 && !args[4]->IsUndefined()) {
    if (!args[4]->IsUint32()) {
      return ThrowException(Exception::TypeError(
        String::New("Expected unsigned integer for a file descriptor")));
    }

    fd_to_send = args[4]->Uint32Value();
  }

  int flags = 0;
  if (args.Length() >= 6 && !args[5]->IsUndefined()) {
    if (!args[5]->IsUint32()) {
      return ThrowException(Exception::TypeError(
        String::New("Expected unsigned integer for a flags argument")));
    }

    flags = args[5]->Uint32Value();
  }

  struct msghdr msg;
  char scratch[64];

  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  msg.msg_flags = 0;
  msg.msg_control = NULL;
  msg.msg_controllen = 0;

  if (fd_to_send >= 0) {
    struct cmsghdr *cmsg;

    msg.msg_control = (void *) scratch;
    msg.msg_controllen = CMSG_LEN(sizeof(fd_to_send));

    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = msg.msg_controllen;
    *(int*) CMSG_DATA(cmsg) = fd_to_send;
  }

  ssize_t written = sendmsg(fd, &msg, flags);

  if (written < 0) {
    if (errno == EAGAIN || errno == EINTR) return Null();
    return ThrowException(ErrnoException(errno, "sendmsg"));
  }

  /* Note that the FD isn't explicitly closed here, this
   * happens in the JS */

  return scope.Close(Integer::New(written));
}

// var bytes = sendto(fd, buf, off, len, flags, destination port, desitnation address);
//
// Write a buffer with optional offset and length to the given file
// descriptor. Note that we refuse to send 0 bytes.
//
// The 'fd' parameter is a numerical file descriptor, or the undefined value
// to send none.
//
// The 'flags' parameter is a number representing a bitmask of MSG_* values.
// This is passed directly to sendmsg().
//
// The destination port can either be an int port, or a path.
//
// Returns null on EAGAIN or EINTR, raises an exception on all other errors
static Handle<Value> SendTo(const Arguments& args) {
  HandleScope scope;

  if (args.Length() < 5) {
    return ThrowException(Exception::TypeError(
          String::New("Takes 5 or 6 parameters")));
  }

  // The first argument should be a file descriptor
  FD_ARG(args[0])

  // Grab the actul data to be written
  if (!Buffer::HasInstance(args[1])) {
    return ThrowException(Exception::TypeError(
      String::New("Expected either a string or a buffer")));
  }

  Buffer *buf = ObjectWrap::Unwrap<Buffer>(args[1]->ToObject());

  size_t offset = 0;
  if (args.Length() >= 3 && !args[2]->IsUndefined()) {
    if (!args[2]->IsUint32()) {
      return ThrowException(Exception::TypeError(
        String::New("Expected unsigned integer for offset")));
    }

    offset = args[2]->Uint32Value();
    if (offset >= buf->length()) {
      return ThrowException(Exception::Error(
        String::New("Offset into buffer too large")));
    }
  }

  size_t length = buf->length() - offset;
  if (args.Length() >= 4 && !args[3]->IsUndefined()) {
    if (!args[3]->IsUint32()) {
      return ThrowException(Exception::TypeError(
        String::New("Expected unsigned integer for length")));
    }

    length = args[3]->Uint32Value();
    if (offset + length > buf->length()) {
      return ThrowException(Exception::Error(
        String::New("offset + length beyond buffer length")));
    }
  }

  int flags = 0;
  if (args.Length() >= 5 && !args[4]->IsUndefined()) {
    if (!args[4]->IsUint32()) {
      return ThrowException(Exception::TypeError(
        String::New("Expected unsigned integer for a flags argument")));
    }

    flags = args[4]->Uint32Value();
  }

  Handle<Value> error = ParseAddressArgs(args[5], args[6], false);
  if (!error.IsEmpty()) return ThrowException(error);

  ssize_t written = sendto(fd, buf->data() + offset, length, flags, addr, addrlen);

  if (written < 0) {
    if (errno == EAGAIN || errno == EINTR) return Null();
    return ThrowException(ErrnoException(errno, "sendmsg"));
  }

  /* Note that the FD isn't explicitly closed here, this
   * happens in the JS */

  return scope.Close(Integer::New(written));
}


// Probably only works for Linux TCP sockets?
// Returns the amount of data on the read queue.
static Handle<Value> ToRead(const Arguments& args) {
  HandleScope scope;

  FD_ARG(args[0])

  int value;
  int r = ioctl(fd, FIONREAD, &value);

  if (r < 0) {
    return ThrowException(ErrnoException(errno, "ioctl"));
  }

  return scope.Close(Integer::New(value));
}


static Handle<Value> SetNoDelay(const Arguments& args) {
  int flags, r;
  HandleScope scope;

  FD_ARG(args[0])

  flags = args[1]->IsFalse() ? 0 : 1;
  r = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));

  if (r < 0) {
    return ThrowException(ErrnoException(errno, "setsockopt"));
  }
  return Undefined();
}

static Handle<Value> SetKeepAlive(const Arguments& args) {
  int r;
  HandleScope scope;

  bool enable = false;
  int time = 0;

  FD_ARG(args[0])

  if (args.Length() > 0) enable = args[1]->IsTrue();
  if (enable == true) {
    time = args[2]->Int32Value();
  }

  int flags = enable ? 1 : 0;
  r = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
  if ((time > 0)&&(r >= 0)) {
#if defined(__APPLE__)
    r = setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, (void *)&time, sizeof(time));
#elif defined(__linux__)
    r = setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, (void *)&time, sizeof(time));
#else
    // Solaris nor FreeBSD support TCP_KEEPIDLE, so do nothing here.
#endif
  }
  if (r < 0) {
    return ThrowException(ErrnoException(errno, "setsockopt"));
  }
  return Undefined();
}

static Handle<Value> SetBroadcast(const Arguments& args) {
  int flags, r;
  HandleScope scope;

  FD_ARG(args[0])

  flags = args[1]->IsFalse() ? 0 : 1;
  r = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (void *)&flags, sizeof(flags));

  if (r < 0) {
    return ThrowException(ErrnoException(errno, "setsockopt"));
  } else {
    return scope.Close(Integer::New(flags));
  }
}

static Handle<Value> SetTTL(const Arguments& args) {
  HandleScope scope;

  if (args.Length() != 2) {
    return ThrowException(Exception::TypeError(
      String::New("Takes exactly two arguments: fd, new TTL")));
  }

  FD_ARG(args[0]);

  if (! args[1]->IsInt32()) {
    return ThrowException(Exception::TypeError(
      String::New("Argument must be a number")));
  }
  
  int newttl = args[1]->Int32Value();
  if (newttl < 1 || newttl > 255) {
    return ThrowException(Exception::TypeError(
      String::New("new TTL must be between 1 and 255")));
  }

  int r = setsockopt(fd, IPPROTO_IP, IP_TTL, (void *)&newttl, sizeof(newttl));

  if (r < 0) {
    return ThrowException(ErrnoException(errno, "setsockopt"));
  } else {
    return scope.Close(Integer::New(newttl));
  }
}


//
// G E T A D D R I N F O
//


struct resolve_request {
  Persistent<Function> cb;
  struct addrinfo *address_list;
  int ai_family; // AF_INET or AF_INET6
  char hostname[1];
};

#ifndef EAI_NODATA // EAI_NODATA is deprecated, FreeBSD already thrown it away in favor of EAI_NONAME
#define EAI_NODATA EAI_NONAME
#endif

static int AfterResolve(eio_req *req) {
  ev_unref(EV_DEFAULT_UC);

  struct resolve_request * rreq = (struct resolve_request *)(req->data);

  HandleScope scope;
  Local<Value> argv[2];

  if (req->result != 0) {
    argv[1] = Array::New();
    if (req->result == EAI_NODATA) {
      argv[0] = Local<Value>::New(Null());
    } else {
      argv[0] = ErrnoException(req->result,
                               "getaddrinfo",
                               gai_strerror(req->result));
    }
  } else {
    struct addrinfo *address;
    int n = 0;

    for (address = rreq->address_list; address; address = address->ai_next) { n++; }

    Local<Array> results = Array::New(n);

    char ip[INET6_ADDRSTRLEN];
    const char *addr;

    n = 0;
    address = rreq->address_list;
    while (address) {
      assert(address->ai_socktype == SOCK_STREAM);
      assert(address->ai_family == AF_INET || address->ai_family == AF_INET6);
      addr = ( address->ai_family == AF_INET
             ? (char *) &((struct sockaddr_in *) address->ai_addr)->sin_addr
             : (char *) &((struct sockaddr_in6 *) address->ai_addr)->sin6_addr
             );
      const char *c = inet_ntop(address->ai_family, addr, ip, INET6_ADDRSTRLEN);
      Local<String> s = String::New(c);
      results->Set(Integer::New(n), s);

      n++;
      address = address->ai_next;
    }

    argv[0] = Local<Value>::New(Null());
    argv[1] = results;
  }

  TryCatch try_catch;

  rreq->cb->Call(Context::GetCurrent()->Global(), 2, argv);

  if (try_catch.HasCaught()) {
    FatalException(try_catch);
  }

  if (rreq->address_list) freeaddrinfo(rreq->address_list);
  rreq->cb.Dispose(); // Dispose of the persistent handle
  free(rreq);

  return 0;
}


static int Resolve(eio_req *req) {
  // Note: this function is executed in the thread pool! CAREFUL
  struct resolve_request * rreq = (struct resolve_request *) req->data;

  struct addrinfo hints;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = rreq->ai_family;
  hints.ai_socktype = SOCK_STREAM;

  req->result = getaddrinfo((char*)rreq->hostname,
                            NULL,
                            &hints,
                            &(rreq->address_list));
  return 0;
}


static Handle<Value> GetAddrInfo(const Arguments& args) {
  HandleScope scope;

  String::Utf8Value hostname(args[0]->ToString());

  int type = args[1]->Int32Value();
  int fam = AF_INET;
  switch (type) {
    case 4:
      fam = AF_INET;
      break;
    case 6:
      fam = AF_INET6;
      break;
    default:
      return ThrowException(Exception::TypeError(
            String::New("Second argument must be an integer 4 or 6")));
  }

  if (!args[2]->IsFunction()) {
    return ThrowException(Exception::TypeError(
          String::New("Thrid argument must be a callback")));
  }

  Local<Function> cb = Local<Function>::Cast(args[2]);

  struct resolve_request *rreq = (struct resolve_request *)
    calloc(1, sizeof(struct resolve_request) + hostname.length() + 1);

  if (!rreq) {
    V8::LowMemoryNotification();
    return ThrowException(Exception::Error(
          String::New("Could not allocate enough memory")));
  }

  strncpy(rreq->hostname, *hostname, hostname.length() + 1);
  rreq->cb = Persistent<Function>::New(cb);
  rreq->ai_family = fam;

  // For the moment I will do DNS lookups in the eio thread pool. This is
  // sub-optimal and cannot handle massive numbers of requests.
  //
  // (One particularly annoying problem is that the pthread stack size needs
  // to be increased dramatically to handle getaddrinfo() see X_STACKSIZE in
  // wscript ).
  //
  // In the future I will move to a system using c-ares:
  // http://lists.schmorp.de/pipermail/libev/2009q1/000632.html
  eio_custom(Resolve, EIO_PRI_DEFAULT, AfterResolve, rreq);

  // There will not be any active watchers from this object on the event
  // loop while getaddrinfo() runs. If the only thing happening in the
  // script was this hostname resolution, then the event loop would drop
  // out. Thus we need to add ev_ref() until AfterResolve().
  ev_ref(EV_DEFAULT_UC);

  return Undefined();
}


static Handle<Value> IsIP(const Arguments& args) {
  HandleScope scope;

  if (!args[0]->IsString()) {
    return scope.Close(Integer::New(4));
  }

  String::Utf8Value s(args[0]->ToString());

  // avoiding buffer overflows in the following strcat
  // 2001:0db8:85a3:08d3:1319:8a2e:0370:7334
  // 39 = max ipv6 address.
  if (s.length() > INET6_ADDRSTRLEN) {
    return scope.Close(Integer::New(0));
  }

  struct sockaddr_in6 a;

  if (inet_pton(AF_INET, *s, &(a.sin6_addr)) > 0) return scope.Close(Integer::New(4));
  if (inet_pton(AF_INET6, *s, &(a.sin6_addr)) > 0) return scope.Close(Integer::New(6));

  return scope.Close(Integer::New(0));
}


static Handle<Value> CreateErrnoException(const Arguments& args) {
  HandleScope scope;

  int errorno = args[0]->Int32Value();
  String::Utf8Value syscall(args[1]->ToString());

  Local<Value> exception = ErrnoException(errorno, *syscall);

  return scope.Close(exception);
}


void InitNet(Handle<Object> target) {
  HandleScope scope;

  NODE_SET_METHOD(target, "write", Write);
  NODE_SET_METHOD(target, "read", Read);

  NODE_SET_METHOD(target, "sendMsg", SendMsg);
  NODE_SET_METHOD(target, "recvfrom", RecvFrom);
  NODE_SET_METHOD(target, "sendto", SendTo);

  recv_msg_template =
      Persistent<FunctionTemplate>::New(FunctionTemplate::New(RecvMsg));
  target->Set(String::NewSymbol("recvMsg"), recv_msg_template->GetFunction());

  NODE_SET_METHOD(target, "socket", Socket);
  NODE_SET_METHOD(target, "close", Close);
  NODE_SET_METHOD(target, "shutdown", Shutdown);
  NODE_SET_METHOD(target, "pipe", Pipe);
  NODE_SET_METHOD(target, "socketpair", SocketPair);

  NODE_SET_METHOD(target, "connect", Connect);
  NODE_SET_METHOD(target, "bind", Bind);
  NODE_SET_METHOD(target, "listen", Listen);
  NODE_SET_METHOD(target, "accept", Accept);
  NODE_SET_METHOD(target, "socketError", SocketError);
  NODE_SET_METHOD(target, "toRead", ToRead);
  NODE_SET_METHOD(target, "setNoDelay", SetNoDelay);
  NODE_SET_METHOD(target, "setBroadcast", SetBroadcast);
  NODE_SET_METHOD(target, "setTTL", SetTTL);
  NODE_SET_METHOD(target, "setKeepAlive", SetKeepAlive);
  NODE_SET_METHOD(target, "getsockname", GetSockName);
  NODE_SET_METHOD(target, "getpeername", GetPeerName);
  NODE_SET_METHOD(target, "getaddrinfo", GetAddrInfo);
  NODE_SET_METHOD(target, "isIP", IsIP);
  NODE_SET_METHOD(target, "errnoException", CreateErrnoException);

  target->Set(String::NewSymbol("ENOENT"), Integer::New(ENOENT));
  target->Set(String::NewSymbol("EMFILE"), Integer::New(EMFILE));
  target->Set(String::NewSymbol("EINPROGRESS"), Integer::New(EINPROGRESS));
  target->Set(String::NewSymbol("EINTR"), Integer::New(EINTR));
  target->Set(String::NewSymbol("EACCES"), Integer::New(EACCES));
  target->Set(String::NewSymbol("EPERM"), Integer::New(EPERM));
  target->Set(String::NewSymbol("EADDRINUSE"), Integer::New(EADDRINUSE));
  target->Set(String::NewSymbol("ECONNREFUSED"), Integer::New(ECONNREFUSED));

  errno_symbol          = NODE_PSYMBOL("errno");
  syscall_symbol        = NODE_PSYMBOL("syscall");
  fd_symbol             = NODE_PSYMBOL("fd");
  size_symbol           = NODE_PSYMBOL("size");
  address_symbol        = NODE_PSYMBOL("address");
  port_symbol           = NODE_PSYMBOL("port");
}

}  // namespace node

NODE_MODULE(node_net, node::InitNet);
