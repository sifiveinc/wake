/*
 * Copyright 2021 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You should have received a copy of LICENSE.Apache2 along with
 * this software. If not, you may obtain a copy at
 *
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "poll.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#if defined(__linux__)
#define USE_EPOLL 1
#elif defined(__APPLE__)
#define USE_PSELECT 1
#else
#define USE_PPOLL 1
#endif

#ifdef USE_EPOLL
#include <sys/epoll.h>
#define EVENTS 512

#include <wcl/tracing.h>

struct Poll::detail {
  int pfd;
};

EPoll::EPoll() {
  epfd = epoll_create1(EPOLL_CLOEXEC);
  if (epfd == -1) {
    wcl::log::error("epoll_create1: %s", strerror(errno)).urgent()();
    exit(1);
  }
}

Poll::Poll() : imp(std::make_unique<Poll::detail>()) {
  imp->pfd = epoll_create1(EPOLL_CLOEXEC);
  if (imp->pfd == -1) {
    wcl::log::error("epoll_create1: %s", strerror(errno)).urgent()();
    exit(1);
  }
}

EPoll::~EPoll() { close(epfd); }

Poll::~Poll() { close(imp->pfd); }

void EPoll::add(int fd, uint32_t events) {
  struct epoll_event ev;
  ev.events = events;
  ev.data.fd = fd;

  if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
    wcl::log::error("epoll_ctl(EPOLL_CTL_ADD): %s", strerror(errno)).urgent()();
    exit(1);
  }
}

void Poll::add(int fd) {
  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = fd;

  if (epoll_ctl(imp->pfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
    perror("epoll_ctl(EPOLL_CTL_ADD)");
    exit(1);
  }
}

void EPoll::remove(int fd) {
  // epoll_event is ignored on EPOLL_CTL_DEL
  struct epoll_event ev;
  if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, &ev) == -1) {
    wcl::log::error("epoll_ctl(EPOLL_CTL_DEL): %s", strerror(errno)).urgent()();
    exit(1);
  }
}

void Poll::remove(int fd) {
  // epoll_event is ignored on EPOLL_CTL_DEL
  struct epoll_event ev;

  if (epoll_ctl(imp->pfd, EPOLL_CTL_DEL, fd, &ev) == -1) {
    perror("epoll_ctl(EPOLL_CTL_DEL)");
    exit(1);
  }
}

void Poll::clear() {
  close(imp->pfd);
  imp->pfd = epoll_create1(EPOLL_CLOEXEC);
  if (imp->pfd == -1) {
    perror("epoll_create1");
    exit(1);
  }
}

std::vector<epoll_event> EPoll::wait(struct timespec *timeout, sigset_t *saved) {
  struct epoll_event events[EVENTS];
  int ptimeout;

  if (timeout) {
    ptimeout = timeout->tv_sec * 1000 + (timeout->tv_nsec + 999999) / 1000000;
  } else {
    ptimeout = -1;
  }

  int nfds = epoll_pwait(epfd, &events[0], EVENTS, ptimeout, saved);
  if (nfds == -1 && errno != EINTR) {
    wcl::log::error("epoll_pwait: %s", strerror(errno)).urgent()();
    exit(1);
  }

  std::vector<epoll_event> ready;
  if (nfds > 0) {
    for (int i = 0; i < nfds; ++i) ready.push_back(events[i]);
  }

  return ready;
}

std::vector<int> Poll::wait(struct timespec *timeout, sigset_t *saved) {
  struct epoll_event events[EVENTS];
  int ptimeout;

  if (timeout) {
    ptimeout = timeout->tv_sec * 1000 + (timeout->tv_nsec + 999999) / 1000000;
  } else {
    ptimeout = -1;
  }

  int nfds = epoll_pwait(imp->pfd, &events[0], EVENTS, ptimeout, saved);
  if (nfds == -1 && errno != EINTR) {
    perror("epoll_pwait");
    exit(1);
  }

  std::vector<int> ready;
  if (nfds > 0) {
    for (int i = 0; i < nfds; ++i) ready.push_back(events[i].data.fd);
  }

  return ready;
}

int Poll::max_fds() const {
  struct rlimit nfd;
  if (getrlimit(RLIMIT_NOFILE, &nfd) == -1) {
    perror("getrlimit(RLIMIT_NOFILE)");
    exit(1);
  }
  if (nfd.rlim_cur != nfd.rlim_max) {
    nfd.rlim_cur = nfd.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &nfd) == -1) {
      perror("setrlimit(RLIMIT_NOFILE)");
      exit(1);
    }
  }
  return nfd.rlim_max;
}

#endif

#ifdef USE_PSELECT

struct Poll::detail {
  std::vector<int> fds;
};

Poll::Poll() : imp(new Poll::detail) {}

Poll::~Poll() {}

void Poll::add(int fd) { imp->fds.push_back(fd); }

void Poll::remove(int fd) {
  imp->fds.resize(std::remove(imp->fds.begin(), imp->fds.end(), fd) - imp->fds.begin());
}

void Poll::clear() { imp->fds.clear(); }

std::vector<int> Poll::wait(struct timespec *timeout, sigset_t *saved) {
  fd_set set;
  int nfds = 0;

  FD_ZERO(&set);
  for (auto fd : imp->fds) {
    if (fd >= nfds) nfds = fd + 1;
    FD_SET(fd, &set);
  }

  int retval = pselect(nfds, &set, 0, 0, timeout, saved);
  if (retval == -1 && errno != EINTR) {
    perror("pselect");
    exit(1);
  }

  std::vector<int> ready;
  if (retval > 0) {
    for (auto fd : imp->fds) {
      if (FD_ISSET(fd, &set)) {
        ready.push_back(fd);
      }
    }
  }

  return ready;
}

int Poll::max_fds() const {
  struct rlimit nfd;
  if (getrlimit(RLIMIT_NOFILE, &nfd) == -1) {
    perror("getrlimit(RLIMIT_NOFILE)");
    exit(1);
  }
  rlim_t set = FD_SETSIZE;
  if (set > nfd.rlim_max && nfd.rlim_max != RLIM_INFINITY) set = nfd.rlim_max;
  if (nfd.rlim_cur != set) {
    nfd.rlim_cur = set;
    if (setrlimit(RLIMIT_NOFILE, &nfd) == -1) {
      perror("setrlimit(RLIMIT_NOFILE)");
      exit(1);
    }
  }
  return set;
}

#endif

#ifdef USE_PPOLL

struct Poll::detail {
  std::vector<struct pollfd> pfds;
};

Poll::Poll() : imp(new Poll::detail) {}

Poll::~Poll() {}

void Poll::add(int fd) {
  imp->pfds.resize(imp->pfds.size() + 1);
  imp->pfds.back().fd = fd;
  imp->pfds.back().events = POLLIN;
}

void Poll::remove(int fd) {
  size_t i = 0, len = imp->pfds.size();
  struct pollfd *pfds = &imp->pfds[0];
  while (i < len) {
    if (pfds[i].fd == fd) {
      pfds[i].fd = pfds[len - 1].fd;
      --len;
    } else {
      ++i;
    }
  }
  imp->pfds.resize(len);
}

void Poll::clear() { imp->pfds.clear(); }

std::vector<int> Poll::wait(struct timespec *timeout, sigset_t *saved) {
  int retval = ppoll(&imp->pfds[0], imp->pfds.size(), timeout, saved);
  if (retval == -1 && errno != EINTR) {
    perror("ppoll");
    exit(1);
  }

  std::vector<int> ready;
  if (retval > 0) {
    for (auto &pfd : imp->pfds) {
      if ((pfd.revents & (POLLIN | POLLHUP)) != 0) {
        ready.push_back(pfd.fd);
      }
    }
  }

  return ready;
}

int Poll::max_fds() const {
  struct rlimit nfd;
  if (getrlimit(RLIMIT_NOFILE, &nfd) == -1) {
    perror("getrlimit(RLIMIT_NOFILE)");
    exit(1);
  }
  if (nfd.rlim_cur != nfd.rlim_max) {
    nfd.rlim_cur = nfd.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &nfd) == -1) {
      perror("setrlimit(RLIMIT_NOFILE)");
      exit(1);
    }
  }
  return nfd.rlim_max;
}

#endif
