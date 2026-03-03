#include "utils/utils.hpp"
#include "utils/interfaces.hpp"
#include "neighbor/neighbors.hpp"
#include "ipc/server.hpp"
#include "common.hpp"

int main() {
    /* get machine UID */
    uint8_t machineId[MACHINE_ID_LEN];
    if (!utils::getMachineId(machineId)) {
        LOG_ERROR("Failed to get machine ID, exiting");
        return 1;
    }

    /* initialize IPC server */
    if (!ipc::initServer()) {
        LOG_ERROR("Failed to initialize IPC server");
        ipc::cleanup();
        return 1;
    }

    /* initialize neighbor storage */
    neighbor::init();

    /* initialize interface monitoring */
    if (!interfaces::initMonitor(machineId)) {
        LOG_ERROR("Failed to initialize interface monitor");
        ipc::cleanup();
        return 1;
    }

    if (interfaces::monitoredEthInterfaces.empty()) {
        LOG_ERROR("No active Ethernet interfaces found!");
        ipc::cleanup();
        return 1;
    }

    time_t last_send_time = 0;
    uint8_t recvBuf[PAYLOAD_OFFSET + sizeof(NeighborPayload)];
    LOG_INFO("Neighbor discovery service started");

    /* main loop */
    while (true) {
        time_t now = time(nullptr);

        /* check interfaces, check neighbour timeout and send */
        if (now - last_send_time >= SEND_INTERVAL_SEC) {
            interfaces::periodicResync(now);
            neighbor::checkTimeout(now);

            if (interfaces::monitoredEthInterfaces.empty()) {
                LOG_WARN("No active Ethernet interfaces found, skipping send");
                last_send_time = now;
                continue;
            }

            for (auto& [ifname, ethInterface] : interfaces::monitoredEthInterfaces) {
                ssize_t sent = sendto(ethInterface.sockfd,
                                      ethInterface.send_frame,
                                      sizeof(ethInterface.send_frame),
                                      0,
                                      (struct sockaddr*)&ethInterface.send_addr,
                                      sizeof(ethInterface.send_addr));
                if (sent < 0) LOG_ERROR("sendto() failed on " << ifname << ": " << strerror(errno));
            }
            last_send_time = now;
        }

        /* calculate timeout for select() */
        time_t time_until_send = SEND_INTERVAL_SEC - (now - last_send_time);
        if (time_until_send < 0) time_until_send = 0;

        /* build fd_set for interfaces + IPC server */
        fd_set readfds;
        FD_ZERO(&readfds);
        int max_fd = 0;
        int netlink_fd = interfaces::getNetlinkFd();

        for (const auto& [ifname, ethInterface] : interfaces::monitoredEthInterfaces) {
            FD_SET(ethInterface.sockfd, &readfds);
            if (ethInterface.sockfd > max_fd) {
                max_fd = ethInterface.sockfd;
            }
        }

        if (ipc::server_fd >= 0) {
            FD_SET(ipc::server_fd, &readfds);
            if (ipc::server_fd > max_fd) {
                max_fd = ipc::server_fd;
            }
        }

        if (netlink_fd >= 0) {
            FD_SET(netlink_fd, &readfds);
            if (netlink_fd > max_fd) {
                max_fd = netlink_fd;
            }
        }

        /* wait for incoming packets */
        struct timeval timeout{};
        timeout.tv_sec = time_until_send;

        int ret = select(max_fd + 1, &readfds, nullptr, nullptr, &timeout);
        if (ret < 0) {
            LOG_ERROR("select() failed: " << strerror(errno));
            continue;
        } else if (ret == 0) {
            continue; // no fd is set, loop back to sending
        }

        if (netlink_fd >= 0 && FD_ISSET(netlink_fd, &readfds)) {
            interfaces::handleNetlinkEvents();
        }

        if (FD_ISSET(ipc::server_fd, &readfds)) {
            ipc::checkClients();
        }

        /*
            Average packet processing time: ~1000-2000ns (recv + neighbor::store)
            Theoretically, we could process up to 5,000,000 - 10,000,000 packets per second per interface.
            However, to be safe, lets limit the number of packets processed per select() iteration.
            This prevents edge cases where incoming packets > packet processing time and we get stuck on an interface processing.
        */
        for (auto& [ifname, ethInterface] : interfaces::monitoredEthInterfaces) {
            int processed = 0;
            if (FD_ISSET(ethInterface.sockfd, &readfds)) {

                while (processed < MAX_PKTS_PER_ITER) {
                    ssize_t n = recv(ethInterface.sockfd, recvBuf, sizeof(recvBuf), MSG_DONTWAIT);
                    if (n <= 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;  // all packets drained
                        }
                        LOG_ERROR("recv() failed on " << ifname << ": " << strerror(errno));
                        break;
                    }
                    neighbor::store(recvBuf, ifname.c_str());
                    processed++;
                }
            }
        }
    }

    for (auto& [ifname, ethInterface] : interfaces::monitoredEthInterfaces) {
        close(ethInterface.sockfd);
    }
    ipc::cleanup();

    return 0;
}
