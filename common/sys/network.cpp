// ======================================================================== //
// Copyright 2009-2017 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "network.h"
#include "string.h"
#include "mutex.h"

////////////////////////////////////////////////////////////////////////////////
/// Platforms supporting Socket interface
////////////////////////////////////////////////////////////////////////////////

#if defined(__WIN32__)
#define _WINSOCK_DEPRECATED_NO_WARNINGS
//#include <winsock2.h>
//#include <io.h>
typedef int socklen_t;
#define SHUT_RDWR 0x2
#else 
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h> 
#define SOCKET int
#define INVALID_SOCKET -1
#define closesocket ::close
#endif

/*! ignore if not supported */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 
#endif

#define BUFFERING 1

namespace embree
{
  namespace network 
  {
    __forceinline void initialize() {
#ifdef __WIN32__
      static bool initialized = false;
      static MutexSys initMutex;
      Lock<MutexSys> lock(initMutex);
      WSADATA wsaData;
      short version = MAKEWORD(1,1);
      if (WSAStartup(version,&wsaData) != 0)
        THROW_RUNTIME_ERROR("Winsock initialization failed");
      initialized = true;
#endif
    }

    struct buffered_socket_t 
    {
      buffered_socket_t (SOCKET fd, size_t isize = 64*1024, size_t osize = 64*1024)
        : fd(fd), 
          ibuf(new char[isize]), isize(isize), istart(0), iend(0),
          obuf(new char[osize]), osize(osize), oend(0) {
      }

      ~buffered_socket_t () {
        delete[] ibuf; ibuf = nullptr;
        delete[] obuf; obuf = nullptr;
      }

    private:
      buffered_socket_t (const buffered_socket_t& other) DELETED; // do not implement
      buffered_socket_t& operator= (const buffered_socket_t& other) DELETED; // do not implement

    public:
      SOCKET fd;               //!< file descriptor of the socket
      char* ibuf;
      size_t isize;
      size_t istart,iend;
      char* obuf;
      size_t osize;
      size_t oend;
    };

    struct AutoCloseSocket
    {
      SOCKET sock;
      AutoCloseSocket (SOCKET sock) : sock(sock) {}
      ~AutoCloseSocket () {
        if (sock != INVALID_SOCKET) {
          closesocket(sock);
        }
      }
    };

    socket_t connect(const char* host, unsigned short port) 
    {
      initialize();

      /*! create a new socket */
      SOCKET sockfd = ::socket(AF_INET, SOCK_STREAM, 0);
      if (sockfd == INVALID_SOCKET) THROW_RUNTIME_ERROR("cannot create socket");
      AutoCloseSocket auto_close(sockfd);
      
      /*! perform DNS lookup */
      struct hostent* server = ::gethostbyname(host);
      if (!server) 
        THROW_RUNTIME_ERROR("server "+std::string(host)+" not found");
      if (server->h_length != sizeof(in_addr))
        THROW_RUNTIME_ERROR("gethostbyname error");
     
      /*! perform connection */
      struct sockaddr_in serv_addr;
      memset((char*)&serv_addr, 0, sizeof(serv_addr));
      serv_addr.sin_family = AF_INET;
      serv_addr.sin_port = (unsigned short) htons(port);
      serv_addr.sin_addr = *(in_addr*) server->h_addr;
      
      if (::connect(sockfd,(struct sockaddr*) &serv_addr,sizeof(serv_addr)) < 0)
        THROW_RUNTIME_ERROR("connection to "+std::string(host)+":"+toString(port)+" failed");
      
      /*! enable TCP_NODELAY */
#ifdef TCP_NODELAY
      { int flag = 1; ::setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(int)); }
#endif
      
      /*! we do not want SIGPIPE to be thrown */
#ifdef SO_NOSIGPIPE
      { int flag = 1; setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, (const char*) &flag, sizeof(int)); }
#endif
      
      auto_close.sock = INVALID_SOCKET;
      return (socket_t) new buffered_socket_t(sockfd);
    }
    
    socket_t bind(unsigned short port)
    {
      initialize();

      /*! create a new socket */
      SOCKET sockfd = ::socket(AF_INET, SOCK_STREAM, 0);
      if (sockfd == INVALID_SOCKET) THROW_RUNTIME_ERROR("cannot create socket");
      AutoCloseSocket auto_close(sockfd);

      /* When the server completes, the server socket enters a time-wait state during which the local
      address and port used by the socket are believed to be in use by the OS. The wait state may
      last several minutes. This socket option allows bind() to reuse the port immediately. */
#ifdef SO_REUSEADDR
      { int flag = true; ::setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&flag, sizeof(int)); }
#endif
      
      /*! bind socket to port */
      struct sockaddr_in serv_addr;
      memset((char *) &serv_addr, 0, sizeof(serv_addr));
      serv_addr.sin_family = AF_INET;
      serv_addr.sin_port = (unsigned short) htons(port);
      serv_addr.sin_addr.s_addr = INADDR_ANY;

      if (::bind(sockfd, (struct sockaddr*) &serv_addr, sizeof(serv_addr)) < 0)
        THROW_RUNTIME_ERROR("binding to port "+toString(port)+" failed");
      
      /*! listen to port, up to 5 pending connections */
      if (::listen(sockfd,5) < 0)
        THROW_RUNTIME_ERROR("listening on socket failed");

      auto_close.sock = INVALID_SOCKET;
      return (socket_t) new buffered_socket_t(sockfd);
    }
    
    socket_t listen(socket_t hsock)
    {
      SOCKET sockfd = ((buffered_socket_t*) hsock)->fd;
            
      /*! accept incoming connection */
      struct sockaddr_in addr;
      socklen_t len = sizeof(addr);
      SOCKET fd = ::accept(sockfd, (struct sockaddr *) &addr, &len);
      if (fd == INVALID_SOCKET) THROW_RUNTIME_ERROR("cannot accept connection");

      /*! enable TCP_NODELAY */
#ifdef TCP_NODELAY
      { int flag = 1; ::setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,(char*)&flag,sizeof(int)); }
#endif

      /*! we do not want SIGPIPE to be thrown */
#ifdef SO_NOSIGPIPE
      { int flag = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (void*)&flag, sizeof(int)); }
#endif

      return (socket_t) new buffered_socket_t(fd); 
    }
    
    void read(socket_t hsock_i, void* data_i, size_t bytes)
    {
#if BUFFERING
      char* data = (char*)data_i;
      buffered_socket_t* hsock = (buffered_socket_t*) hsock_i;
      while (bytes) {
        if (hsock->istart == hsock->iend) {
          ssize_t n = ::recv(hsock->fd,hsock->ibuf,int(hsock->isize),MSG_NOSIGNAL);
          if      (n == 0) throw Disconnect();
          else if (n  < 0) THROW_RUNTIME_ERROR("error reading from socket");
          hsock->istart = 0;
          hsock->iend = n;
        }
        size_t bsize = hsock->iend-hsock->istart;
        if (bytes < bsize) bsize = bytes;
        memcpy(data,hsock->ibuf+hsock->istart,bsize);
        data += bsize;
        hsock->istart += bsize;
        bytes -= bsize;
      }
#else
      char* data = (char*) data_i;
      buffered_socket_t* hsock = (buffered_socket_t*) hsock_i;
      while (bytes) {
        ssize_t n = ::read(hsock->fd,data,bytes);
        if      (n == 0) throw Disconnect();
        else if (n  < 0) THROW_RUNTIME_ERROR("error reading from socket");
        data+=n;
        bytes-=n;
      }
#endif
    }

    void write(socket_t hsock_i, const void* data_i, size_t bytes)
    {
#if BUFFERING
      const char* data = (const char*) data_i;
      buffered_socket_t* hsock = (buffered_socket_t*) hsock_i;
      while (bytes) {
        if (hsock->oend == hsock->osize) flush(hsock_i);
        size_t bsize = hsock->osize-hsock->oend;
        if (bytes < bsize) bsize = bytes;
        memcpy(hsock->obuf+hsock->oend,data,bsize);
        data += bsize;
        hsock->oend += bsize;
        bytes -= bsize;
      }
#else
      const char* data = (const char*) data_i;
      buffered_socket_t* hsock = (buffered_socket_t*) hsock_i;
      while (bytes) {
        ssize_t n = ::write(hsock->fd,data,bytes);
        if (n  < 0) THROW_RUNTIME_ERROR("error writing to socket");
        data+=n;
        bytes-=n;
      }
#endif
    }

    void flush(socket_t hsock_i)
    {
#if BUFFERING
      buffered_socket_t* hsock = (buffered_socket_t*) hsock_i;
      char* data = hsock->obuf;
      size_t bytes = hsock->oend;
      while (bytes > 0) {
        ssize_t n = ::send(hsock->fd,data,(int)bytes,MSG_NOSIGNAL);
        if (n < 0) THROW_RUNTIME_ERROR("error writing to socket");
        bytes -= n;
        data += n;
      } 
      hsock->oend = 0;
#endif
    }
    
    void close(socket_t hsock_i) {
      buffered_socket_t* hsock = (buffered_socket_t*) hsock_i;
      ::shutdown(hsock->fd,SHUT_RDWR);
      closesocket(hsock->fd);
      delete hsock;
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// All Platforms
////////////////////////////////////////////////////////////////////////////////

namespace embree
{
  namespace network 
  {
    bool read_bool(socket_t socket) 
    {
      bool value = 0;
      read(socket,&value,sizeof(bool));
      return value;
    }

    char read_char(socket_t socket) 
    {
      char value = 0;
      read(socket,&value,sizeof(char));
      return value;
    }
    
    int read_int(socket_t socket) 
    {
      int value = 0;
      read(socket,&value,sizeof(int));
      return value;
    }
    
    float read_float(socket_t socket) 
    {
      float value = 0.0f;
      read(socket,&value,sizeof(float));
      return value;
    }
    
    std::string read_string(socket_t socket) 
    {
      int bytes = read_int(socket);
      char* str = new char[bytes+1];
      read(socket,str,bytes);
      str[bytes] = 0x00;
      std::string s(str);
      delete[] str;
      return s;
    }

    void write(socket_t socket, bool value) {
      write(socket,&value,sizeof(bool));
    }

    void write(socket_t socket, char value) {
      write(socket,&value,sizeof(char));
    }
    
    void write(socket_t socket, int value) {
      write(socket,&value,sizeof(int));
    }
    
    void write(socket_t socket, float value) {
      write(socket,&value,sizeof(float));
    }
    
    void write(socket_t socket, const std::string& str) {
      write(socket,(int)str.size());
      write(socket,str.c_str(),str.size());
    }
  }
}
