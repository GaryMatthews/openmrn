/** \copyright
 * Copyright (c) 2016, Stuart W Baker
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are  permitted provided that the following conditions are met:
 *
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * \file CC32xxSocket.cxx
 * This file intantiates and initilizes the CC32xx Wi-Fi.
 *
 * @author Stuart W. Baker
 * @date 18 March 2016
 */

#include "CC32xxSocket.hxx"
#include "CC32xxWiFi.hxx"

#include <memory>
#include <fcntl.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

// Simplelink includes
#include "socket.h"

#include "utils/format_utils.hxx"

/// @todo (Stuart Baker) since there is only a max of 16 sockets, would it be
/// more memory efficient to just allocate all the memory statically as an
/// array and perform placement construction instead of incuring the overhead
/// of a heap allocation?
/// @note(Balazs Racz) I don't think so, because most applications will only
/// ever use one socket, maybe two.
///
/// we would also #define CC32XX_SD_UNUSED -1 and #define CC32XX_SD_RESERVED -2

/// Existing (allocated) sockets.
static CC32xxSocket *cc32xxSockets[SL_MAX_SOCKETS];

/// Dummy pointer address that can be used as a reserved indicator
static volatile uint8_t reservedPtr;

/// Guard value indivating a reserved socket that is currently being allocated.
#define CC32XX_SOCKET_RESERVED ((CC32xxSocket *)(&reservedPtr))

/*
 * CC32xxSocket::socket()
 */
int CC32xxSocket::socket(int domain, int type, int protocol)
{
    int reserved = reserve_socket();
    if (reserved < 0)
    {
        return -1;
    }

    mutex.lock();
    int fd = fd_alloc();
    mutex.unlock();
    if (fd < 0)
    {
        cc32xxSockets[reserved] = nullptr;
        errno = EMFILE;
        return -1;
    }

    std::unique_ptr<CC32xxSocket> new_socket(new CC32xxSocket());
    if (new_socket.get() == nullptr)
    {
        cc32xxSockets[reserved] = nullptr;
        fd_free(fd);
        errno = ENOMEM;
        return -1;
    }

    switch (domain)
    {
        case AF_INET:
            domain = SL_AF_INET;
            break;
        case AF_INET6:
            domain = SL_AF_INET6;
            break;
        case AF_PACKET:
            domain = SL_AF_PACKET;
            break;
        default:
            cc32xxSockets[reserved] = nullptr;
            fd_free(fd);
            errno = EAFNOSUPPORT;
            return -1;
    }

    switch (type)
    {
        case SOCK_STREAM:
            type = SL_SOCK_STREAM;
            break;
        case SOCK_DGRAM:
            type = SL_SOCK_DGRAM;
            break;
        case SOCK_RAW:
            type = SL_SOCK_RAW;
            break;
        default:
            cc32xxSockets[reserved] = nullptr;
            fd_free(fd);
            errno = EINVAL;
            return -1;
    }

    switch (protocol)
    {
        case 0:
            break;
        case IPPROTO_TCP:
            protocol = SL_IPPROTO_TCP;
            break;
        case IPPROTO_UDP:
            protocol = SL_IPPROTO_UDP;
            break;
        case IPPROTO_RAW:
            protocol = SL_IPPROTO_RAW;
            break;
        default:
            cc32xxSockets[reserved] = nullptr;
            fd_free(fd);
            errno = EINVAL;
            return -1;
    }

    int result = sl_Socket(domain, type, protocol);

    if (result < 0)
    {
        switch (result)
        {
            default:
                HASSERT(0);
                break;
            case SL_EAFNOSUPPORT:
                errno = EAFNOSUPPORT;
                break;
            case SL_EPROTOTYPE:
                errno = EPROTOTYPE;
                break;
            case SL_EACCES:
                errno = EACCES;
                break;
            case SL_ENSOCK:
                errno = EMFILE;
                break;
            case SL_ENOMEM:
                errno = ENOMEM;
                break;
            case SL_EINVAL:
                errno = EINVAL;
                break;
            case SL_EPROTONOSUPPORT:
                errno = EPROTONOSUPPORT;
                break;
            case SL_EOPNOTSUPP:
                errno = EOPNOTSUPP;
                break;
        }

        cc32xxSockets[reserved] = nullptr;
        fd_free(fd);
        return -1;
    }
    

    File *file = file_lookup(fd);

    file->dev = new_socket.get();
    file->flags = O_RDWR;
    file->priv = new_socket.get();

    new_socket->sd = result;
    new_socket->references_ = 1;

    cc32xxSockets[reserved] = new_socket.release();

    return fd;
}

/*
 * CC32xxSocket::bind()
 */
int CC32xxSocket::bind(int socket, const struct sockaddr *address,
                       socklen_t address_len)
{
    CC32xxSocket *s = get_instance_from_fd(socket);
    if (s == nullptr)
    {
        return - 1;
    }

    SlSockAddr_t sl_address;
    sl_address.sa_family = address->sa_family;
    memcpy(sl_address.sa_data, address->sa_data, sizeof(sl_address.sa_data));

    int result = sl_Bind(s->sd, &sl_address, address_len);

    if (result < 0)
    {
        switch (result)
        {
            default:
                HASSERT(0);
                break;
        }
        return -1;
    }
    return result;  
}

/*
 * CC32xxSocket::listen()
 */
int CC32xxSocket::listen(int socket, int backlog)
{
    CC32xxSocket *s = get_instance_from_fd(socket);
    if (s == nullptr)
    {
        return - 1;
    }

    int result = sl_Listen(s->sd, backlog);

    if (result < 0)
    {
        switch (result)
        {
            default:
                HASSERT(0);
                break;
        }
        return -1;
    }

    s->readActive = false;
    s->listenActive = true;
    return result;  
}

/*
 * CC32xxSocket::accpet()
 */
int CC32xxSocket::accept(int socket, struct sockaddr *address,
                         socklen_t *address_len)
{
    CC32xxSocket *s = get_instance_from_fd(socket);
    if (s == nullptr)
    {
        return - 1;
    }

    if (!s->listenActive)
    {
        errno = EINVAL;
        return -1;
    }

    SlSockAddr_t sl_address;
    SlSocklen_t sl_address_len;

    int result = sl_Accept(s->sd, &sl_address, &sl_address_len);

    if (address && address_len)
    {
        memcpy(address, &sl_address, *address_len);
    }

    if (result < 0)
    {
        switch (result)
        {
            default:
                HASSERT(0);
                break;
            case SL_POOL_IS_EMPTY:
                usleep(10000);
                /* fall through */
            case SL_EAGAIN:
                errno = EAGAIN;
                break;
        }
        return -1;
    }

    int reserved = reserve_socket();
    if (reserved < 0)
    {
        return -1;
    }

    mutex.lock();
    int fd = fd_alloc();
    mutex.unlock();
    if (fd < 0)
    {
        cc32xxSockets[reserved] = nullptr;
        sl_Close(result);
        errno = EMFILE;
        return -1;
    }

    std::unique_ptr<CC32xxSocket> new_socket(new CC32xxSocket());
    if (new_socket.get() == nullptr)
    {
        cc32xxSockets[reserved] = nullptr;
        sl_Close(result);
        fd_free(fd);
        errno = ENOMEM;
        return -1;
    }

    File *file = file_lookup(fd);

    file->dev = new_socket.get();
    file->flags = O_RDWR;
    file->priv = new_socket.get();

    new_socket->sd = result;
    new_socket->references_ = 1;

    cc32xxSockets[reserved] = new_socket.release();

    return fd;  
}

/*
 * CC32xxSocket::connect()
 */
int CC32xxSocket::connect(int socket, const struct sockaddr *address,
                          socklen_t address_len)
{
    CC32xxSocket *s = get_instance_from_fd(socket);
    if (s == nullptr)
    {
        return - 1;
    }

    SlSockAddr_t sl_address;
    sl_address.sa_family = address->sa_family;
    memcpy(sl_address.sa_data, address->sa_data, sizeof(sl_address.sa_data));

    int result = sl_Connect(s->sd, &sl_address, address_len);

    if (result < 0)
    {
        switch (result)
        {
            default:
            {
                volatile int g_bad_result = 0;
                g_bad_result = g_bad_result + result;
                HASSERT(0);
                break;
            }
            case SL_ECONNREFUSED:
                errno = ECONNREFUSED;
                break;
            case SL_EALREADY:
                errno = EALREADY;
                break;
            case SL_POOL_IS_EMPTY:
                usleep(10000);
            /* fall through */
            case SL_EAGAIN:
                errno = EAGAIN;
                break;
        }
        return -1;
    }
    return result;  
}

/*
 * CC32xxSocket::recv()
 */
ssize_t CC32xxSocket::recv(int socket, void *buffer, size_t length, int flags)
{
    /* flags are not supported on the CC32xx */
    HASSERT(flags == 0);

    CC32xxSocket *s = get_instance_from_fd(socket);
    if (s == nullptr)
    {
        return - 1;
    }

    int result = sl_Recv(s->sd, buffer, length, flags);

    if (result < 0)
    {
        switch (result)
        {
            default:
                HASSERT(0);
                break;
            case SL_POOL_IS_EMPTY:
                usleep(10000);
                /* fall through */
            case SL_EAGAIN:
                errno = EAGAIN;
                s->readActive = false;
                break;
        }
        return -1;
    }
    if ((size_t)result < length)
    {
        s->readActive = false;
    }
    else
    {
        s->readActive = true;
    }
    return result;  
}

/*
 * CC32xxSocket::send()
 */
ssize_t CC32xxSocket::send(int socket, const void *buffer, size_t length, int flags)
{
    /* flags are not supported on the CC32xx */
    HASSERT(flags == 0);

    CC32xxSocket *s = get_instance_from_fd(socket);
    if (s == nullptr)
    {
        return - 1;
    }

    int result = sl_Send(s->sd, buffer, length, flags);

    if (result < 0)
    {
        switch (result)
        {
            case SL_SOC_ERROR:
                /// @todo (stbaker): handle errors via the callback.
                errno = ECONNRESET;
                break;
            case SL_EAGAIN:
                errno = EAGAIN;
                break;
            default:
                volatile int err = 0;
                err = err | result;
                /// @todo (stbaker): handle errors via the callback.
                HASSERT(0);
                break;
        }
        return -1;
    }
    if ((size_t)result < length)
    {
        s->writeActive = false;
    }
    else
    {
        s->writeActive = true;
    }
    return result;
}

/*
 * CC32xxSocket::setsockopt()
 */
int CC32xxSocket::setsockopt(int socket, int level, int option_name,
                             const void *option_value, socklen_t option_len)
{
    CC32xxSocket *s = get_instance_from_fd(socket);
    if (s == nullptr)
    {
        return - 1;
    }

    int result;

    switch (level)
    {
        default:
            errno = EINVAL;
            return -1;
        case SOL_SOCKET:
            level = SL_SOL_SOCKET;
            switch (option_name)
            {
                default:
                    errno = EINVAL;
                    return -1;
                case SO_REUSEADDR:
                    /* CC32xx does not care about SO_REUSEADDR, ignore it */
                    result = 0;
                    break;
            }
            break;
        case IPPROTO_TCP:
            switch (option_name)
            {
                default:
                    errno = EINVAL;
                    return -1;
                case TCP_NODELAY:
                    /* CC32xx does not care about Nagel algorithm, ignore it */
                    result = 0;
                    break;
            }
            break;
    }
                    
    if (result < 0)
    {
        switch (result)
        {
            default:
                HASSERT(0);
                break;
        }
        return -1;
    }

    return result;
}

/*
 * CC32xxSocket::getsockopt()
 */
int CC32xxSocket::getsockopt(int socket, int level, int option_name,
                             void *option_value, socklen_t *option_len)
{
    CC32xxSocket *s = get_instance_from_fd(socket);
    if (s == nullptr)
    {
        return - 1;
    }

    int result;

    switch (level)
    {
        default:
            errno = EINVAL;
            return -1;
        case SOL_SOCKET:
            level = SL_SOL_SOCKET;
            switch (option_name)
            {
                default:
                    errno = EINVAL;
                    return -1;
                case SO_REUSEADDR:
                {
                    /* CC32xx does not care about SO_REUSEADDR,
                     * we assume it is on by default
                     */
                    int *so_reuseaddr = static_cast<int *>(option_value);
                    *so_reuseaddr = 1;
                    *option_len = sizeof(int);
                    result = 0;
                    break;
                }
            }
            break;
        case IPPROTO_TCP:
            switch (option_name)
            {
                default:
                    errno = EINVAL;
                    return -1;
                case TCP_NODELAY:
                {
                    /* CC32xx does not care about Nagel algorithm,
                     * we assume it is off by default
                     */
                    int *tcp_nodelay = static_cast<int *>(option_value);
                    *tcp_nodelay = 1;
                    *option_len = sizeof(int);
                    result = 0;
                    break;
                }
            }
            break;
    }
                    
    if (result < 0)
    {
        switch (result)
        {
            default:
                HASSERT(0);
                break;
        }
        return -1;
    }

    return result;
}

/*
 * CC32xxSocket::close()
 */
int CC32xxSocket::close(File *file)
{
    CC32xxSocket *s = static_cast<CC32xxSocket *>(file->priv);

    mutex.lock();
    if (--references_ == 0)
    {
        mutex.unlock();
        /* request that the socket be closed */
        CC32xxWiFi::instance()->select_wakeup(s->sd);
    }
    else
    {
        mutex.unlock();
    }

    return 0;
}

/** Device select method. Default impementation returns true.
 * @param file reference to the file
 * @param mode FREAD for read active, FWRITE for write active, 0 for
 *        exceptions
 * @return true if active, false if inactive
 */
bool CC32xxSocket::select(File* file, int mode)
{
    CC32xxSocket *s = static_cast<CC32xxSocket *>(file->priv);
    bool retval = false;

    switch (mode)
    {
        case FREAD:
            portENTER_CRITICAL();
            if (readActive)
            {
                retval = true;
                portEXIT_CRITICAL();
            }
            else
            {
                select_insert(&selInfoRd);
                portEXIT_CRITICAL();
                CC32xxWiFi::instance()->fd_set_read(s->sd);
            }
            break;
        case FWRITE:
            portENTER_CRITICAL();
            HASSERT(listenActive == false);
            if (writeActive)
            {
                retval = true;
                portEXIT_CRITICAL();
            }
            else
            {
                select_insert(&selInfoWr);
                portEXIT_CRITICAL();
                CC32xxWiFi::instance()->fd_set_write(s->sd);
            }
            break;
        default:
        case 0:
            /* we don't support any exceptions */
            break;
    }
    return retval;
}

/*
 * CC32xxSocket::get_instance_from_sd()
 */
CC32xxSocket *CC32xxSocket::get_instance_from_sd(int sd)
{
    for (int i = 0; i < SL_MAX_SOCKETS; ++i)
    {
        if (cc32xxSockets[i] != nullptr &&
            cc32xxSockets[i] != CC32XX_SOCKET_RESERVED)
        {
            if (sd == cc32xxSockets[i]->sd)
            {
                return cc32xxSockets[i];
            }
        }
    }
    return nullptr;
}

/*
 * CC32xxSocket::get_instance_from_fd(int fd)
 */
CC32xxSocket *CC32xxSocket::get_instance_from_fd(int fd)
{
    File* f = file_lookup(fd);
    if (!f) 
    {
        errno = EBADF;
        return nullptr;
    }

    struct stat stat;
    ::fstat(fd, &stat);

    if (!S_ISSOCK(stat.st_mode))
    {
        errno = ENOTSOCK;
        return nullptr;
    }

    return static_cast<CC32xxSocket *>(f->priv);
}

/*
 * CC32xxSocket::remove_instance_from_sd()
 */
void CC32xxSocket::remove_instance_from_sd(int sd)
{
    for (int i = 0; i < SL_MAX_SOCKETS; ++i)
    {
        if (cc32xxSockets[i] != nullptr &&
            cc32xxSockets[i] != CC32XX_SOCKET_RESERVED)
        {
            if (sd == cc32xxSockets[i]->sd)
            {
                cc32xxSockets[i] = nullptr;
                break;
            }
        }
    }
}

/*
 * CC32xxSocket::reserve_socket()
 */
int CC32xxSocket::reserve_socket()
{
    int i = 0;
    portENTER_CRITICAL();
    for (int i; i < SL_MAX_SOCKETS; ++i)
    {
        if (cc32xxSockets[i] == nullptr)
        {
            /* mark the socket reserved */
            cc32xxSockets[i] = CC32XX_SOCKET_RESERVED;
            break;
        }
    }
    portEXIT_CRITICAL();
    if (i == SL_MAX_SOCKETS)
    {
        /* out of free sockets */
        errno = EMFILE;
        return -1;
    }

    return i;
}

/*
 * CC32xxSocket::fcntl()
 */
int CC32xxSocket::fcntl(File *file, int cmd, unsigned long data)
{
    CC32xxSocket *s = static_cast<CC32xxSocket *>(file->priv);
    if (!S_ISSOCK(s->mode_))
    {
        errno = ENOTSOCK;
        return -1;
    }

    switch (cmd)
    {
        default:
        {
            return -EINVAL;  
        }  
        case F_SETFL:
        {
            SlSockNonblocking_t sl_option_value;
            sl_option_value.NonblockingEnabled = data & O_NONBLOCK ? 1 : 0;
            int result = sl_SetSockOpt(s->sd, SL_SOL_SOCKET,
                                       SL_SO_NONBLOCKING, &sl_option_value,
                                       sizeof(sl_option_value));
            HASSERT(result == 0);
            return 0;
        }
    }
}

extern "C"
{
/** Create an unbound socket in a communications domain.
 * @param domain specifies the communications domain in which a socket is
 *               to be created
 * @param type specifies the type of socket to be created
 * @param protocol specifies a particular protocol to be used with the
 *                 socket, specifying a protocol of 0 causes socket() to
 *                 use an unspecified default protocol appropriate for the
 *                 requested socket type
 * @return a non-negative integer on success, the socket file descriptor,
 *         otherwise, a value of -1 shall be returned and errno set to
 *         indicate the error
 */
int socket(int domain, int type, int protocol)
{
    return CC32xxSocket::socket(domain, type, protocol);
}

/** Bind a name to a socket.
 * @param socket file descriptor of the socket to be bound
 * @param address points to a sockaddr structure containing the address to
 *                be bound to the socket
 * @param address_len specifies the length of the sockaddr structure pointed
 *                    to by the address argument
 * @return shall return on success, otherwise, -1 shall be returned and
 *         errno set to indicate the error
 */
int bind(int socket, const struct sockaddr *address, socklen_t address_len)
{
    return CC32xxSocket::bind(socket, address, address_len);
}

/** Mark a connection-mode socket, specified by the socket argument, as
 * accepting connections.
 * @param socket the socket file descriptor
 * @param backlog provides a hint to the implementation which the
 *                implementation shall use to limit the number of
 *                outstanding connections in the socket's listen queue
 * @return shall return 0 upon success, otherwise, -1 shall be returned and
 *         errno set to indicate the error
 */
int listen(int socket, int backlog)
{
    return CC32xxSocket::listen(socket, backlog);
}

/** Accept a new connection on a socket.
 * @param socket the socket file descriptor
 * @param address either a null pointer, or a pointer to a sockaddr
 *                structure where the address of the connecting socket
 *                shall be returned
 * @param address_len either a null pointer, if address is a null pointer,
 *                    or a pointer to a socklen_t object which on input
 *                    specifies the length of the supplied sockaddr
 *                    structure, and on output specifies the length of the
 *                    stored address
 * @return shall return the non-negative file descriptor of the accepted
 *         socket upon success, otherwise, -1 shall be returned and errno
 *         set to indicate the error
 */
int accept(int socket, struct sockaddr *address, socklen_t *address_len)
{
    return CC32xxSocket::accept(socket, address, address_len);
}

/** Connect a socket.
 * @param socket the socket file descriptor
 * @param address points to a sockaddr structure containing the peer address
 * @param address_len specifies the length of the sockaddr structure pointed
 *                    to by the address argument
 * @return shall return 0 upon success, otherwise, -1 shall be returned and
 *         errno set to indicate the error
 */
int connect(int socket, const struct sockaddr *address, socklen_t address_len)
{
    return CC32xxSocket::connect(socket, address, address_len);
}

/** Receive a message from a connection-mode or connectionless-mode socket.
 * @param socket the socket file descriptor
 * @param buffer buffer where the message should be stored
 * @param length length in bytes of the buffer pointed to by the buffer
 *               argument
 * @param flags Specifies the type of message reception
 * @return the length of the message in bytes, if no messages are available
 *         to be received and the peer has performed an orderly shutdown,
 *         recv() shall return 0. Otherwise, -1 shall be returned and errno
 *         set to indicate the error
 */
ssize_t recv(int socket, void *buffer, size_t length, int flags)
{
    return CC32xxSocket::recv(socket, buffer, length, flags);
}

/** Initiate transmission of a message from the specified socket.
 * @param socket the socket file descriptor
 * @param buffer buffer containing the message to send
 * @param length length of the message in bytes
 * @param flags the type of message transmission
 * @return the number of bytes sent, otherwise, -1 shall be returned and
 *         errno set to indicate the error
 */
ssize_t send(int socket, const void *buffer, size_t length, int flags)
{
    return CC32xxSocket::send(socket, buffer, length, flags);
}

/** Set the socket options.
 * @param socket the socket file descriptor
 * @param level specifies the protocol level at which the option resides
 * @param option_name specifies a single option to set
 * @param option_value the metadata that belongs to the option_name
 * @param option_len the length of the metadata that belongs to the
 *                   option_name
 * @return shall return 0 upon success, otherwise, -1 shall be returned and
 *         errno set to indicate the error
 */
int setsockopt(int socket, int level, int option_name,
               const void *option_value, socklen_t option_len)
{
    return CC32xxSocket::setsockopt(socket, level, option_name,
                                    option_value, option_len);
}

/** Get the socket options.
 * @param socket the socket file descriptor
 * @param level specifies the protocol level at which the option resides
 * @param option_name specifies a single option to get
 * @param option_value the metadata that belongs to the option_name
 * @param option_len the length of the metadata that belongs to the
 *                   option_name
 * @return shall return 0 upon success, otherwise, -1 shall be returned and
 *         errno set to indicate the error
 */
int getsockopt(int socket, int level, int option_name,
               void *option_value, socklen_t *option_len)
{
    return CC32xxSocket::getsockopt(socket, level, option_name,
                                    option_value, option_len);
}

/**
 * see 'man gai_strerror'
 * @param __ecode is the error code
 * @return error string (statically allocated)
 */
const char *gai_strerror (int __ecode)
{
    switch (__ecode)
    {
        default:
            return "gai_strerror unkown";
        case EAI_AGAIN:
            return "temporary failure";
        case EAI_FAIL:
            return "non-recoverable failure";
        case EAI_MEMORY:
            return "memory allocation failure";
    }
}

/** see 'man freeaddrinfo'
 * @param ai is the addrinfo structure, allocated by getaddrinfo, to be
 * released.
 */
void freeaddrinfo(struct addrinfo *ai)
{
    delete ai->ai_addr;
    delete ai;
}

/// see 'man getaddrinfo'
///
/// @param nodename what to look up (host name typically)
/// @param servname local mDNS service name to look up
/// @param hints not sure
/// @param res an addrinfo strcture will be allocated into here
///
/// @return 0 on success, -1 on error
///
int getaddrinfo(const char *nodename, const char *servname,
                const struct addrinfo *hints,
                struct addrinfo **res)
{
    uint32_t ip_addr[4];
    uint32_t port;
    uint8_t domain;
    int8_t text[120];
    uint16_t text_len = 120;

    std::unique_ptr<struct addrinfo> ai(new struct addrinfo);
    if (ai.get() == nullptr)
    {
        return EAI_MEMORY;
    }
    memset(ai.get(), 0, sizeof(struct addrinfo));

    std::unique_ptr<struct sockaddr> sa(new struct sockaddr);
    if (sa.get() == nullptr)
    {
        free(*res);
        return EAI_MEMORY;
    }
    memset(sa.get(), 0, sizeof(struct addrinfo));

    switch (hints->ai_family)
    {
        case AF_INET:
            domain = SL_AF_INET;
            break;
        case AF_INET6:
            domain = SL_AF_INET6;
            break;
        case AF_PACKET:
            domain = SL_AF_PACKET;
            break;
        default:
            errno = EAFNOSUPPORT;
            return -1;
    }

    int result;

    if (nodename)
    {
        result = sl_NetAppDnsGetHostByName((int8_t*)nodename, strlen(nodename),
                                           ip_addr, domain);
    }
    else
    {
        result = sl_NetAppDnsGetHostByService((int8_t*)servname,
                                              strlen(servname), domain, ip_addr,
                                              &port, &text_len, text);
    }

    if (result != 0)
    {
        switch (result)
        {
            default:
            case SL_POOL_IS_EMPTY:
                return EAI_AGAIN;
            case SL_NET_APP_DNS_QUERY_NO_RESPONSE:
            case SL_NET_APP_DNS_NO_SERVER:
            case SL_NET_APP_DNS_QUERY_FAILED:
            case SL_NET_APP_DNS_MALFORMED_PACKET:
            case SL_NET_APP_DNS_MISMATCHED_RESPONSE:
                return EAI_FAIL;
        }
    }

    switch (hints->ai_family)
    {
        case AF_INET:
        {
            struct sockaddr_in *sa_in = (struct sockaddr_in*)sa.get();
            ai->ai_flags = 0;
            ai->ai_family = hints->ai_family;
            ai->ai_socktype = hints->ai_socktype;
            ai->ai_protocol = hints->ai_protocol;
            ai->ai_addrlen = sizeof(struct sockaddr_in);
            sa_in->sin_family = hints->ai_family;
            long port_name = strtol(servname, nullptr, 0);
            /* the newlib implementation of strtol does not properly implement
             * errno == EINVAL. We would normally want the if statement to be:
             * if (port_name == 0 && errno == EINVAL)
             */
            if (port_name == 0 && (servname[0] < '0' || servname[0] > '9') &&
                servname[0] != '+' && servname[0] != '-')
            {
                sa_in->sin_port = htons((uint16_t)port);
            }
            else
            {
                sa_in->sin_port = htons((uint16_t)port_name);
            }
            sa_in->sin_addr.s_addr = htonl(ip_addr[0]);
            break;
        }
        case AF_INET6:
        case AF_PACKET:
        default:
            errno = EAFNOSUPPORT;
            return -1;
    }

    *res = ai.release();
    (*res)->ai_addr = sa.release();
    return 0;
}

/*
 * ::inet_ntop()
 */
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    char *org = dst;

    switch (af)
    {
        default:
            errno = EAFNOSUPPORT;
            return nullptr;
        case AF_INET6:
            HASSERT(0);
        case AF_INET:
        {
            socklen_t count = 0;
            uint8_t *ip = (uint8_t*)src;
            for (int i = 0; i < 4; ++i)
            {
                char string[5];
                char *end = integer_to_buffer(ip[i], string);
                count += (end - string) + 1;
                if (count > size)
                {
                    errno = ENOSPC;
                    return nullptr;
                }
                strcpy(dst, string);
                dst += (end - string);
                *dst++ = '.';
            }
            *--dst = '\0';
            break;
        }
    }

    return org;
}

} /* extern "C" */