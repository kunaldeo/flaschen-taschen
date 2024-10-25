// -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation version 2.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://gnu.org/licenses/gpl-2.0.txt>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "composite-flaschen-taschen.h"
#include "ft-thread.h"
#include "ppm-reader.h"
#include "servers.h"

volatile bool interrupt_received = false;
static void InterruptHandler(int signo) { interrupt_received = true; }

static int server_socket = -1;

bool udp_server_init(int port) {
  if ((server_socket = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
    perror("Failed to create IPv6 socket");
    return false;
  }

  // Force IPv6 only for optimal performance
  int ipv6_only = 1;
  if (setsockopt(server_socket, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6_only,
                 sizeof(ipv6_only)) < 0) {
    perror("Failed to set IPv6-only mode");
    close(server_socket);
    return false;
  }

  // Optimize receive buffer for aarch64
  int rcvbuf = 8 * 1024 * 1024; // 8MB buffer, optimized for RPi memory
  setsockopt(server_socket, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  struct sockaddr_in6 addr = {0};
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_any;
  addr.sin6_port = htons(port);

  if (bind(server_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind failed");
    close(server_socket);
    return false;
  }

  fprintf(stderr, "IPv6 UDP server ready on port %d\n", port);
  return true;
}

void udp_server_run_blocking(CompositeFlaschenTaschen *display,
                             ft::Mutex *mutex) {
  // Aligned static buffer for better aarch64 NEON performance
  static char packet_buffer[65535] __attribute__((aligned(16)));

  struct msghdr msg = {0};
  struct iovec iov = {0};
  struct sockaddr_in6 src_addr;

  // Set up message structure
  iov.iov_base = packet_buffer;
  iov.iov_len = sizeof(packet_buffer);
  msg.msg_name = &src_addr;
  msg.msg_namelen = sizeof(src_addr);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  struct sigaction sa = {{0}};
  sa.sa_handler = InterruptHandler;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  while (!interrupt_received) {
    ssize_t received_bytes = recvmsg(server_socket, &msg, 0);

    if (received_bytes < 0) {
      if (errno == EINTR)
        continue;
      perror("recvmsg failed");
      break;
    }

    ImageMetaInfo img_info = {0};
    img_info.width = display->width();
    img_info.height = display->height();

    const char *pixel_pos =
        ReadImageData(packet_buffer, received_bytes, &img_info);
    if (pixel_pos) {
      mutex->Lock();
      display->SetLayer(img_info.layer);

      // Process pixels in bulk for better cache usage
      for (int y = 0; y < img_info.height; ++y) {
        for (int x = 0; x < img_info.width; ++x) {
          Color c;
          c.r = *pixel_pos++;
          c.g = *pixel_pos++;
          c.b = *pixel_pos++;
          display->SetPixel(x + img_info.offset_x, y + img_info.offset_y, c);
        }
      }

      display->Send();
      display->SetLayer(0);
      mutex->Unlock();
    }
  }
}
