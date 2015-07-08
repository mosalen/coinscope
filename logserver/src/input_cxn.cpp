#include "input_cxn.hpp"

#include <cstring>
#include <unistd.h>
#include <fcntl.h>

#include <set>
#include <iostream>

#include "network.hpp"
#include "netwrap.hpp"
#include "collector.hpp"

using namespace std;

namespace input_cxn {

const uint32_t RECV_HEADER = 0x1;
const uint32_t RECV_LOG = 0x2;

set<uint32_t> taken_ids;

void handler::handle_accept_error(handlers::accept_handler<handler> *handler, const network_error &e) {
	cerr << e.what() << endl;
	handler->io.stop();
	close(handler->io.fd);

	/* TODO: call some function that continually tries to reacquire input channel
	   and destroys this handler */
}

void handler::handle_accept(handlers::accept_handler<handler> *, int fd) {
	new handler(fd); /* let him delete himself */
}


handler::handler(int fd) 
	: read_queue(4), write_queue(), state(RECV_HEADER), io() {
	io.set<handler, &handler::io_cb>(this);
	io.start(fd, ev::READ|ev::WRITE);
}

void handler::io_cb(ev::io &watcher, int revents) {
	if (revents & ev::READ) {
		ssize_t r(1);
		while(r > 0 && read_queue.hungry()) { /* do all reads we can in this event handler */
			do {
				pair<int,bool> res = read_queue.do_read(watcher.fd);
				r = res.first;
				if (r < 0 && errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) { 
					cerr << "Got unexpected error on handler : " << strerror(errno);
					suicide();
					return;
				}
				if (r == 0) { /* got disconnected! */
					suicide();
					return;
				}
			} while (r > 0 && read_queue.to_read() > 0);

			if (!read_queue.hungry()) {
				if (state == RECV_HEADER) {
					read_queue.cursor(0);
					read_queue.to_read(ntoh(*((const uint32_t*) read_queue.extract_buffer().const_ptr())));
					state = RECV_LOG;
				} else {
					/* item needs to be handled */
					wrapped_buffer<uint8_t> p = read_queue.extract_buffer();
					collector::get().append(move(p), read_queue.cursor());
					read_queue.cursor(0);
					read_queue.to_read(4);
					state = RECV_HEADER;
				}
			}
		}
	}

	if (revents & ev::WRITE) {
		ssize_t r(1);
		while(write_queue.to_write() && r > 0) {
			pair<int,bool> res = write_queue.do_write(watcher.fd);
			r = res.first;
			
			if (r < 0 && errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) { 
				/* most probably a disconnect of some sort, log error and queue object for deletion */
				cerr << "Received error on write: " << strerror(errno) << endl;
				suicide();
				return;
			} 

			if (r == 0) {
				/* disconnect */
				suicide();
				return;
			}
		}

		if (write_queue.to_write() == 0) {
			/* for logserver, only handshake ever writes out on the input channel, so we're done with that now... */
			io.set(ev::READ);
		}
	}

}


void handler::suicide() {
	io.stop();
	close(io.fd);
	io.fd = -1;
	delete this;
}


handler::~handler() { 
	if (io.fd >= 0) {
		io.stop();
		close(io.fd);
	}
}


};
