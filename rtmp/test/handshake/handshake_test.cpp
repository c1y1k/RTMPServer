#include "rtmp.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kIoTimeoutMs = 2000;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool wait_for(int fd, short events) {
    pollfd descriptor{fd, events, 0};
    for (;;) {
        const int result = poll(&descriptor, 1, kIoTimeoutMs);
        if (result > 0) {
            return (descriptor.revents & events) != 0;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
}

bool send_all(int fd, const uint8_t* data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        const ssize_t result = send(fd, data + sent, length - sent, MSG_NOSIGNAL);
        if (result > 0) {
            sent += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_for(fd, POLLOUT)) {
                continue;
            }
        }
        return false;
    }
    return true;
}

bool receive_exact(int fd, uint8_t* data, size_t length) {
    size_t received = 0;
    while (received < length) {
        const ssize_t result = recv(fd, data + received, length - received, 0);
        if (result > 0) {
            received += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_for(fd, POLLIN)) {
                continue;
            }
        }
        return false;
    }
    return true;
}

void receive_available(int fd, std::vector<uint8_t>& output) {
    std::array<uint8_t, 4096> buffer{};
    for (;;) {
        const ssize_t result = recv(fd, buffer.data(), buffer.size(), 0);
        if (result > 0) {
            output.insert(output.end(), buffer.begin(), buffer.begin() + result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

std::vector<uint8_t> make_c0c1(uint8_t seed = 17) {
    std::vector<uint8_t> c0c1(1537, 0);
    c0c1[0] = 3;
    c0c1[1] = 0x01;
    c0c1[2] = 0x23;
    c0c1[3] = 0x45;
    c0c1[4] = 0x67;
    for (size_t index = 9; index < c0c1.size(); ++index) {
        c0c1[index] = static_cast<uint8_t>((index * seed) & 0xffU);
    }
    return c0c1;
}

std::vector<uint8_t> make_c2(const std::vector<uint8_t>& response) {
    std::vector<uint8_t> c2(1536, 0);
    std::copy(response.begin() + 1, response.begin() + 5, c2.begin());
    c2[4] = 0xaa;
    c2[5] = 0xbb;
    c2[6] = 0xcc;
    c2[7] = 0xdd;
    std::copy(response.begin() + 9, response.begin() + 1537, c2.begin() + 8);
    return c2;
}

bool validate_response(const std::vector<uint8_t>& c0c1,
                       const std::vector<uint8_t>& response) {
    bool ok = true;
    ok &= expect(response.size() == 3073, "S0S1S2 total length");
    ok &= expect(response[0] == 3, "S0 version must be 3");
    ok &= expect(std::all_of(response.begin() + 5, response.begin() + 9,
                            [](uint8_t value) { return value == 0; }),
                 "S1 Zero must contain four zero bytes");
    ok &= expect(std::equal(c0c1.begin() + 1, c0c1.begin() + 5,
                           response.begin() + 1537),
                 "S2 Time must echo C1 Time");
    ok &= expect(std::equal(c0c1.begin() + 9, c0c1.end(),
                           response.begin() + 1545),
                 "S2 Random must echo C1 Random");
    return ok;
}

bool test_fragmented_handshake(Rtmp::TriggerMode mode) {
    int sockets[2] = {-1, -1};
    if (!expect(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
                "create socketpair for fragmented handshake")) {
        return false;
    }
    bool ok = expect(set_nonblocking(sockets[0]) && set_nonblocking(sockets[1]),
                     "set fragmented handshake sockets nonblocking");

    {
        Rtmp server(sockets[0], mode);
        const std::vector<uint8_t> c0c1 = make_c0c1();

        ok &= expect(send_all(sockets[1], c0c1.data(), 500),
                     "send first C0C1 fragment");
        auto result = server.process();
        ok &= expect(result.status == Rtmp::StepStatus::NeedRead &&
                         result.stage == Rtmp::ProcessStage::Handshake,
                     "partial C0C1 must wait in handshake stage");

        ok &= expect(send_all(sockets[1], c0c1.data() + 500, c0c1.size() - 500),
                     "send remaining C0C1 bytes");
        result = server.process();
        ok &= expect(result.status == Rtmp::StepStatus::NeedRead &&
                         result.stage == Rtmp::ProcessStage::Handshake,
                     "server must wait for C2 after sending response");

        std::vector<uint8_t> response(3073);
        ok &= expect(receive_exact(sockets[1], response.data(), response.size()),
                     "receive complete S0S1S2");
        ok &= validate_response(c0c1, response);

        const std::vector<uint8_t> c2 = make_c2(response);
        ok &= expect(send_all(sockets[1], c2.data(), 600),
                     "send first C2 fragment");
        result = server.process();
        ok &= expect(result.status == Rtmp::StepStatus::NeedRead &&
                         result.stage == Rtmp::ProcessStage::Handshake,
                     "partial C2 must wait in handshake stage");

        ok &= expect(send_all(sockets[1], c2.data() + 600, c2.size() - 600),
                     "send remaining C2 bytes");
        result = server.process();
        ok &= expect(result.status == Rtmp::StepStatus::NeedRead &&
                         result.stage == Rtmp::ProcessStage::ChunkRead,
                     "complete handshake must advance to chunk reading");
    }

    close(sockets[1]);
    return ok;
}

bool test_invalid_c0_version() {
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        return expect(false, "create socketpair for invalid C0");
    }
    set_nonblocking(sockets[0]);
    set_nonblocking(sockets[1]);

    bool ok = true;
    {
        Rtmp server(sockets[0], Rtmp::TriggerMode::LT);
        std::vector<uint8_t> c0c1 = make_c0c1();
        c0c1[0] = 2;
        ok &= expect(send_all(sockets[1], c0c1.data(), c0c1.size()),
                     "send invalid C0 version");
        const auto result = server.process();
        ok &= expect(result.status == Rtmp::StepStatus::Error &&
                         result.stage == Rtmp::ProcessStage::Handshake,
                     "invalid C0 version must fail in handshake stage");
    }

    close(sockets[1]);
    return ok;
}

bool test_invalid_c1_zero() {
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        return expect(false, "create socketpair for invalid C1 Zero");
    }
    set_nonblocking(sockets[0]);
    set_nonblocking(sockets[1]);

    bool ok = true;
    {
        Rtmp server(sockets[0], Rtmp::TriggerMode::ET);
        std::vector<uint8_t> c0c1 = make_c0c1();
        c0c1[5] = 1;
        ok &= expect(send_all(sockets[1], c0c1.data(), c0c1.size()),
                     "send invalid C1 Zero");
        const auto result = server.process();
        ok &= expect(result.status == Rtmp::StepStatus::Error &&
                         result.stage == Rtmp::ProcessStage::Handshake,
                     "nonzero C1 Zero must fail in handshake stage");
    }

    close(sockets[1]);
    return ok;
}

bool test_invalid_c2_echo() {
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        return expect(false, "create socketpair for invalid C2");
    }
    set_nonblocking(sockets[0]);
    set_nonblocking(sockets[1]);

    bool ok = true;
    {
        Rtmp server(sockets[0], Rtmp::TriggerMode::ET);
        const std::vector<uint8_t> c0c1 = make_c0c1();
        ok &= expect(send_all(sockets[1], c0c1.data(), c0c1.size()),
                     "send C0C1 before invalid C2");
        auto result = server.process();
        ok &= expect(result.status == Rtmp::StepStatus::NeedRead,
                     "wait for C2 before invalid echo test");

        std::vector<uint8_t> response(3073);
        ok &= expect(receive_exact(sockets[1], response.data(), response.size()),
                     "receive response before invalid C2");
        std::vector<uint8_t> c2 = make_c2(response);
        c2.back() ^= 0xffU;
        ok &= expect(send_all(sockets[1], c2.data(), c2.size()),
                     "send invalid C2 echo");
        result = server.process();
        ok &= expect(result.status == Rtmp::StepStatus::Error &&
                         result.stage == Rtmp::ProcessStage::Handshake,
                     "invalid C2 Random must fail in handshake stage");
    }

    close(sockets[1]);
    return ok;
}

bool test_send_resume_after_eagain() {
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        return expect(false, "create socketpair for send resume");
    }
    set_nonblocking(sockets[0]);
    set_nonblocking(sockets[1]);

    int send_buffer_size = 4096;
    setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF,
               &send_buffer_size, sizeof(send_buffer_size));

    std::array<uint8_t, 4096> filler{};
    size_t filler_bytes = 0;
    for (;;) {
        const ssize_t result = send(sockets[0], filler.data(), filler.size(), MSG_NOSIGNAL);
        if (result > 0) {
            filler_bytes += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        close(sockets[0]);
        close(sockets[1]);
        return expect(false, "fill server send buffer");
    }

    const size_t initial_drain = std::min<size_t>(1024, filler_bytes);
    std::vector<uint8_t> discarded(initial_drain);
    bool ok = expect(receive_exact(sockets[1], discarded.data(), discarded.size()),
                     "partially drain prefilled send buffer");

    {
        Rtmp server(sockets[0], Rtmp::TriggerMode::ET);
        const std::vector<uint8_t> c0c1 = make_c0c1(29);
        ok &= expect(send_all(sockets[1], c0c1.data(), c0c1.size()),
                     "send C0C1 for send resume");

        auto result = server.process();
        ok &= expect(result.status == Rtmp::StepStatus::NeedWrite &&
                         result.stage == Rtmp::ProcessStage::Handshake,
                     "full send buffer must return NeedWrite");

        discarded.resize(filler_bytes - initial_drain);
        ok &= expect(receive_exact(sockets[1], discarded.data(), discarded.size()),
                     "drain remaining prefilled bytes");

        std::vector<uint8_t> response;
        receive_available(sockets[1], response);
        for (int attempts = 0;
             attempts < 16 && result.status == Rtmp::StepStatus::NeedWrite;
             ++attempts) {
            result = server.process();
            receive_available(sockets[1], response);
        }

        ok &= expect(result.status == Rtmp::StepStatus::NeedRead &&
                         result.stage == Rtmp::ProcessStage::Handshake,
                     "resumed response must advance to waiting C2");

        if (response.size() < 3073) {
            const size_t old_size = response.size();
            response.resize(3073);
            ok &= expect(receive_exact(sockets[1], response.data() + old_size,
                                       response.size() - old_size),
                         "receive remaining resumed response bytes");
        }
        ok &= validate_response(c0c1, response);
    }

    close(sockets[1]);
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= test_fragmented_handshake(Rtmp::TriggerMode::LT);
    ok &= test_fragmented_handshake(Rtmp::TriggerMode::ET);
    ok &= test_invalid_c0_version();
    ok &= test_invalid_c1_zero();
    ok &= test_invalid_c2_echo();
    ok &= test_send_resume_after_eagain();

    if (!ok) {
        return 1;
    }
    std::cout << "6/6 handshake tests passed\n";
    return 0;
}
