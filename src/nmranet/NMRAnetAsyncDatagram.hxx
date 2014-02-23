/** \copyright
 * Copyright (c) 2014, Balazs Racz
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
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
 * \file NMRAnetAsyncDatagram.hxx
 *
 * Interface for datagram functionality in Async NMRAnet implementation.
 *
 * @author Balazs Racz
 * @date 25 Jan 2013
 */

#ifndef _NMRAnetAsyncDatagram_hxx_
#define _NMRAnetAsyncDatagram_hxx_

#include "utils/NodeHandlerMap.hxx"
#include "executor/allocator.hxx"
#include "nmranet/AsyncIf.hxx"

namespace NMRAnet
{

struct IncomingDatagram;

/// Allocator for getting IncomingDatagram objects.
extern InitializedAllocator<IncomingDatagram> g_incoming_datagram_allocator;

/// Defines how long to wait for a Datagram_OK / Datagram_Rejected message.
extern long long DATAGRAM_RESPONSE_TIMEOUT_NSEC;

struct IncomingDatagram : public QueueMember
{
    NodeHandle src;
    AsyncNode* dst;
    // Owned by the current IncomingDatagram object. Includes the datagram ID
    // as the first byte.
    Buffer* payload;

    void free()
    {
        if (payload)
        {
            payload->free();
            payload = nullptr;
        }
        g_incoming_datagram_allocator.Release(this);
    }
};

/** Base class for datagram handlers.
 *
 * The datagram handler needs to listen to the incoming queue for arriving
 * datagrams. */
class DatagramHandler
{
public:
    /** Sends a datagram to this handler. Takes ownership of datagram. */
    void datagram_arrived(IncomingDatagram* datagram)
    {
        queue_.ReleaseBack(datagram);
    }

protected:
    TypedAllocator<IncomingDatagram> queue_;
};

/** Use this class to send datagrams */
class DatagramClient : public QueueMember
{
public:
    virtual ~DatagramClient()
    {
    }

    /** Triggers sending a datagram.
     *
     * @param src is the sending node.
     * @param dst is the destination node.
     * @param payload is the datagram content buffer. The first byte has to be
     * the datagram ID.
     * @param done will be notified when the datagram send is successful or
     * failed.
     *
     * After `done' is notified, the caller must ensure that the datagram
     * client is released back to the allocator.
     */
    virtual void write_datagram(NodeID src, NodeHandle dst, Buffer* payload,
                                Notifiable* done) = 0;

    /** Requests cancelling the datagram send operation. Will notify the done
     * callback when the canceling is completed. */
    virtual void cancel() = 0;

    /** Returns a bitmask of ResultCodes for the transmission operation. */
    uint32_t result()
    {
        return result_;
    }

    enum ResultCodes
    {
        PERMANENT_ERROR = 0x1000,
        RESEND_OK = 0x2000,
        // Resend OK errors
        TRANSPORT_ERROR = 0x4000,
        BUFFER_UNAVAILABLE = 0x0020,
        OUT_OF_ORDER = 0x0040,
        // Permanent error error bits
        SOURCE_NOT_PERMITTED = 0x0020,
        DATAGRAMS_NOT_ACCEPTED = 0x0040,

        // Internal error codes generated by the send flow
        OPERATION_SUCCESS = 0x10000, //< set when the Datagram OK arrives
        OPERATION_PENDING = 0x20000, //< cleared when done is called.
        DST_NOT_FOUND = 0x40000,     //< on CAN. Permanent error code.
        TIMEOUT = 0x80000,           //< Timeout waiting for ack/nack.

        // The top byte of result_ is the response flags from Datagram_OK
        // response.
        RESPONSE_FLAGS_SHIFT = 24,
        OK_REPLY_PENDING = (1 << 31),
    };

    // These values are okay in the respond_ok flags byte.
    enum ResponseFlag {
        REPLY_PENDING = 0x80,
        REPLY_TIMEOUT_SEC = 0x1,
        REPLY_TIMEOUT_MASK = 0xf,
    };

protected:
    uint32_t result_;
};

/** Transport-agnostic dispatcher of datagrams.
 *
 * There will be typically one instance of this for each interface with virtual
 * nodes. This class is responsible for maintaining the registered datagram
 * handlers, and taking the datagram MTI from the incoming messages and routing
 * them to the datagram handlers. */
class DatagramSupport
{
public:
    typedef TypedNodeHandlerMap<AsyncNode, DatagramHandler> Registry;

    /** Creates a datagram dispatcher.
     *
     * @param interface is the async interface to which to bind.
     * @param num_registry_entries is the size of the registry map (how
     * many datagram handlers can be registered)
     */
    DatagramSupport(AsyncIf* interface, size_t num_registry_entries);
    ~DatagramSupport();

    /// @returns the registry of datagram handlers.
    Registry* registry()
    {
        return dispatcher_.registry();
    }

    /** Datagram clients.
     *
     * Use control flows from this allocator to send datagrams to remote nodes.
     * When the client flow completes, it is the caller's responsibility to
     * return it to this allocator, once the client is done examining the
     * result codes. */
    TypedAllocator<DatagramClient>* client_allocator()
    {
        return &clients_;
    }

    AsyncIf* interface()
    {
        return interface_;
    }

private:
    class DatagramDispatcher : public IncomingMessageHandler,
                               private AllocationResult
    {
    public:
        /// @TODO(balazs.racz) we have two QueueMember base classes in here.
        DatagramDispatcher(size_t num_registry_entries) : m_(nullptr), done_(nullptr), registry_(num_registry_entries)
        {
            lock_.TypedRelease(this);
        }

        ~DatagramDispatcher() {}

        /// @returns the registry of datagram handlers.
        Registry* registry()
        {
            return &registry_;
        }

    private:
        /// Lock used for incoming messages.
        virtual AllocatorBase* get_allocator()
        {
            return &lock_;
        }

        /// Callback from the message dispatcher.
        virtual void handle_message(IncomingMessage* m, Notifiable* done);
        /// Callback from the allocator.
        virtual void AllocationCallback(QueueMember* entry);

        virtual void Run()
        {
            HASSERT(0);
        }

        /// Message called from the dispatcher.
        IncomingMessage* m_;
        Notifiable* done_;

        /// Maintains the registered datagram handlers.
        Registry registry_;

        TypedAllocator<IncomingMessageHandler> lock_;
    };

    /// Interface on which we are registered.
    AsyncIf* interface_;

    /// Datagram clients.
    TypedAllocator<DatagramClient> clients_;

    /// Datagram dispatch handler.
    DatagramDispatcher dispatcher_;
};

} // namespace NMRAnet

#endif // _NMRAnetAsyncDatagram_hxx_