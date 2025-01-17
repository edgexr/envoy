#pragma once

#include <functional>

#include "envoy/network/address.h"
#include "envoy/network/socket.h"

#include "source/common/network/io_socket_handle_impl.h"
#include "source/common/network/socket_interface_impl.h"
#include "source/common/network/utility.h"
#include "source/common/network/win32_socket_handle_impl.h"

#include "test/test_common/network_utility.h"

#include "absl/types/optional.h"

/**
 * TestSocketInterface allows overriding the behavior of the IoHandle interface.
 */
namespace Envoy {
namespace Network {

class TestIoSocketHandle : public Test::IoSocketHandlePlatformImpl {
public:
  using WriteOverrideType = absl::optional<Api::IoCallUint64Result>(TestIoSocketHandle* io_handle,
                                                                    const Buffer::RawSlice* slices,
                                                                    uint64_t num_slice);
  using WriteOverrideProc = std::function<WriteOverrideType>;

  TestIoSocketHandle(WriteOverrideProc write_override_proc, os_fd_t fd = INVALID_SOCKET,
                     bool socket_v6only = false, absl::optional<int> domain = absl::nullopt)
      : Test::IoSocketHandlePlatformImpl(fd, socket_v6only, domain),
        write_override_(write_override_proc) {}

  void initializeFileEvent(Event::Dispatcher& dispatcher, Event::FileReadyCb cb,
                           Event::FileTriggerType trigger, uint32_t events) override {
    absl::MutexLock lock(&mutex_);
    dispatcher_ = &dispatcher;
    Test::IoSocketHandlePlatformImpl::initializeFileEvent(dispatcher, cb, trigger, events);
  }

  // Schedule resumption on the IoHandle by posting a callback to the IoHandle's dispatcher. Note
  // that this operation is inherently racy, nothing guarantees that the TestIoSocketHandle is not
  // deleted before the posted callback executes.
  void activateInDispatcherThread(uint32_t events) {
    absl::MutexLock lock(&mutex_);
    RELEASE_ASSERT(dispatcher_ != nullptr, "null dispatcher");
    dispatcher_->post([this, events]() { activateFileEvents(events); });
  }

  // HTTP/3 sockets won't have a bound peer address, but instead get peer
  // address from the argument in sendmsg. TestIoSocketHandle::sendmsg will
  // stash that in peer_address_override_.
  Address::InstanceConstSharedPtr peerAddress() override {
    if (peer_address_override_.has_value()) {
      return Network::Utility::getAddressWithPort(
          peer_address_override_.value().get(), peer_address_override_.value().get().ip()->port());
    }
    return Test::IoSocketHandlePlatformImpl::peerAddress();
  }

private:
  IoHandlePtr accept(struct sockaddr* addr, socklen_t* addrlen) override;
  Api::IoCallUint64Result writev(const Buffer::RawSlice* slices, uint64_t num_slice) override;
  Api::IoCallUint64Result sendmsg(const Buffer::RawSlice* slices, uint64_t num_slice, int flags,
                                  const Address::Ip* self_ip,
                                  const Address::Instance& peer_address,
                                  const unsigned int tos) override;

  IoHandlePtr duplicate() override;

  OptRef<const Address::Instance> peer_address_override_;
  const WriteOverrideProc write_override_;
  absl::Mutex mutex_;
  Event::Dispatcher* dispatcher_ ABSL_GUARDED_BY(mutex_) = nullptr;
};

/**
 * TestSocketInterface allows overriding of the behavior of the IoHandle interface of
 * accepted sockets.
 * Most integration tests have deterministic order in which Envoy accepts connections.
 * For example a test with one client connection will result in two accepted sockets. First
 * is for the client<->Envoy connection and the second is for the Envoy<->upstream connection.
 */

class TestSocketInterface : public SocketInterfaceImpl {
public:
  /**
   * Override the behavior of the IoSocketHandleImpl::writev() and
   * IoSocketHandleImpl::sendmsg() methods.
   * The supplied callback is invoked with the slices arguments of the write method and the index
   * of the accepted socket.
   * Returning absl::nullopt from the callback continues normal execution of the
   * write methods. Returning a Api::IoCallUint64Result from callback skips
   * the write methods with the returned result value.
   */
  TestSocketInterface(TestIoSocketHandle::WriteOverrideProc write) : write_override_proc_(write) {}

private:
  // SocketInterfaceImpl
  IoHandlePtr makeSocket(int socket_fd, bool socket_v6only,
                         absl::optional<int> domain) const override;

  const TestIoSocketHandle::WriteOverrideProc write_override_proc_;
};

} // namespace Network
} // namespace Envoy
