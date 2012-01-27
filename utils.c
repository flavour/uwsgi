#include "uwsgi.h"


extern struct uwsgi_server uwsgi;
extern char **environ;

/* statistically ordered */
struct http_status_codes hsc[] = {
        {"200", "OK"},
        {"302", "Found"},
        {"404", "Not Found"},
        {"500", "Internal Server Error"},
        {"301", "Moved Permanently"},
        {"304", "Not Modified"},
        {"303", "See Other"},
        {"403", "Forbidden"},
        {"307", "Temporary Redirect"},
        {"401", "Unauthorized"},
        {"400", "Bad Request"},
        {"405", "Method Not Allowed"},
        {"408", "Request Timeout"},

        {"100", "Continue"},
        {"101", "Switching Protocols"},
        {"201", "Created"},
        {"202", "Accepted"},
        {"203", "Non-Authoritative Information"},
        {"204", "No Content"},
        {"205", "Reset Content"},
        {"206", "Partial Content"},
        {"300", "Multiple Choices"},
        {"305", "Use Proxy"},
        {"402", "Payment Required"},
        {"406", "Not Acceptable"},
        {"407", "Proxy Authentication Required"},
        {"409", "Conflict"},
        {"410", "Gone"},
        {"411", "Length Required"},
        {"412", "Precondition Failed"},
        {"413", "Request Entity Too Large"},
        {"414", "Request-URI Too Long"},
        {"415", "Unsupported Media Type"},
        {"416", "Requested Range Not Satisfiable"},
        {"417", "Expectation Failed"},
        {"501", "Not Implemented"},
        {"502", "Bad Gateway"},
        {"503", "Service Unavailable"},
        {"504", "Gateway Timeout"},
        {"505", "HTTP Version Not Supported"},
        { "", NULL },
};



#ifdef __BIG_ENDIAN__
uint16_t uwsgi_swap16(uint16_t x) {
	return (uint16_t) ((x & 0xff) << 8 | (x & 0xff00) >> 8);
}

uint32_t uwsgi_swap32(uint32_t x) {
	x = ((x << 8) & 0xFF00FF00) | ((x >> 8) & 0x00FF00FF);
	return (x >> 16) | (x << 16);
}

// thanks to ffmpeg project for this idea :P
uint64_t uwsgi_swap64(uint64_t x) {
	union {
		uint64_t ll;
		uint32_t l[2];
	} w, r;
	w.ll = x;
	r.l[0] = uwsgi_swap32(w.l[1]);
	r.l[1] = uwsgi_swap32(w.l[0]);
	return r.ll;
}

#endif

int check_hex(char *str, int len) {
	int i;
	for (i = 0; i < len; i++) {
		if ((str[i] < '0' && str[i] > '9') && (str[i] < 'a' && str[i] > 'f') && (str[i] < 'A' && str[i] > 'F')
			) {
			return 0;
		}
	}

	return 1;

}

void inc_harakiri(int sec) {
	if (uwsgi.master_process) {
		uwsgi.workers[uwsgi.mywid].harakiri += sec;
	}
	else {
		alarm(uwsgi.shared->options[UWSGI_OPTION_HARAKIRI] + sec);
	}
}

void set_harakiri(int sec) {
	if (sec == 0) {
		uwsgi.workers[uwsgi.mywid].harakiri = 0;
	}
	else {
		uwsgi.workers[uwsgi.mywid].harakiri = time(NULL) + sec;
	}
	if (!uwsgi.master_process) {
		alarm(sec);
	}
}

void set_mule_harakiri(int sec) {
        if (sec == 0) {
                uwsgi.mules[uwsgi.muleid-1].harakiri = 0;
        }
        else {
                uwsgi.mules[uwsgi.muleid-1].harakiri = time(NULL) + sec;
        }
        if (!uwsgi.master_process) {
                alarm(sec);
        }
}

#ifdef UWSGI_SPOOLER
void set_spooler_harakiri(int sec) {
        if (sec == 0) {
                uwsgi.i_am_a_spooler->harakiri = 0;
        }
        else {
                uwsgi.i_am_a_spooler->harakiri = time(NULL) + sec;
        }
        if (!uwsgi.master_process) {
                alarm(sec);
        }
}
#endif


void daemonize(char *logfile) {
	pid_t pid;
	int fdin;

	// do not daemonize under emperor
	if (uwsgi.has_emperor) {
		logto(logfile);
		return;
	}

	pid = fork();
	if (pid < 0) {
		uwsgi_error("fork()");
		exit(1);
	}
	if (pid != 0) {
		exit(0);
	}

	if (setsid() < 0) {
		uwsgi_error("setsid()");
		exit(1);
	}

	/* refork... */
	pid = fork();
	if (pid < 0) {
		uwsgi_error("fork()");
		exit(1);
	}
	if (pid != 0) {
		exit(0);
	}

	umask(0);

	/*if (chdir("/") != 0) {
	   uwsgi_error("chdir()");
	   exit(1);
	   } */


	fdin = open("/dev/null", O_RDWR);
	if (fdin < 0) {
		uwsgi_error_open("/dev/null");
		exit(1);
	}

	/* stdin */
	if (dup2(fdin, 0) < 0) {
		uwsgi_error("dup2()");
		exit(1);
	}


	logto(logfile);
}

void logto(char *logfile) {

	int fd;

#ifdef UWSGI_UDP
	char *udp_port;
	struct sockaddr_in udp_addr;

	udp_port = strchr(logfile, ':');
	if (udp_port) {
		udp_port[0] = 0;
		if (!udp_port[1] || !logfile[0]) {
			uwsgi_log("invalid udp address\n");
			exit(1);
		}

		fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd < 0) {
			uwsgi_error("socket()");
			exit(1);
		}

		memset(&udp_addr, 0, sizeof(struct sockaddr_in));

		udp_addr.sin_family = AF_INET;
		udp_addr.sin_port = htons(atoi(udp_port + 1));
		char *resolved = uwsgi_resolve_ip(logfile);
		if (resolved) {
			udp_addr.sin_addr.s_addr = inet_addr(resolved);
		}
		else {
			udp_addr.sin_addr.s_addr = inet_addr(logfile);
		}

		if (connect(fd, (const struct sockaddr *) &udp_addr, sizeof(struct sockaddr_in)) < 0) {
			uwsgi_error("connect()");
			exit(1);
		}
	}
	else {
#endif
		if (uwsgi.log_truncate) {
			fd = open(logfile, O_RDWR | O_CREAT | O_TRUNC , S_IRUSR | S_IWUSR | S_IRGRP);
		}
		else {
			fd = open(logfile, O_RDWR | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR | S_IRGRP);
		}
		if (fd < 0) {
			uwsgi_error_open(logfile);
			exit(1);
		}
		uwsgi.logfile = logfile;

		if (uwsgi.chmod_logfile_value) {
			if (chmod(uwsgi.logfile, uwsgi.chmod_logfile_value)) {
				uwsgi_error("chmod()");
			}
		}
#ifdef UWSGI_UDP
	}
#endif


	/* stdout */
	if (fd != 1) {
		if (dup2(fd, 1) < 0) {
			uwsgi_error("dup2()");
			exit(1);
		}
		close(fd);
	}

	/* stderr */
	if (dup2(1, 2) < 0) {
		uwsgi_error("dup2()");
		exit(1);
	}
}

#ifdef UWSGI_ZEROMQ
ssize_t uwsgi_zeromq_logger(struct uwsgi_logger *ul, char *message, size_t len) {

	if (!ul->configured) {

		if (!uwsgi.choosen_logger_arg) {
			uwsgi_log_safe("invalid zeromq syntax\n");
			exit(1);
		}

        	void *ctx = zmq_init(1);
        	if (ctx == NULL) {
                	uwsgi_error_safe("zmq_init()");
                	exit(1);
        	}

        	ul->data = zmq_socket(ctx, ZMQ_PUSH);
        	if (ul->data == NULL) {
                	uwsgi_error_safe("zmq_socket()");
                	exit(1);
        	}

        	if (zmq_connect(ul->data, uwsgi.choosen_logger_arg) < 0) {
                	uwsgi_error_safe("zmq_connect()");
                	exit(1);
        	}

		ul->configured = 1;
	}

	zmq_msg_t msg;
        if (zmq_msg_init_size (&msg, len) == 0) {
                memcpy(zmq_msg_data(&msg), message, len);
                zmq_send(ul->data, &msg, 0);
        	zmq_msg_close(&msg);
        }

	return 0;
}
#endif

void create_logpipe(void) {

#if defined(SOCK_SEQPACKET) && defined(__linux__)
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, uwsgi.shared->worker_log_pipe)) {
#else
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, uwsgi.shared->worker_log_pipe)) {
#endif
                uwsgi_error("socketpair()\n");
                exit(1);
        }

        uwsgi_socket_nb(uwsgi.shared->worker_log_pipe[0]);
        uwsgi_socket_nb(uwsgi.shared->worker_log_pipe[1]);

        if (uwsgi.shared->worker_log_pipe[1] != 1) {
                if (dup2(uwsgi.shared->worker_log_pipe[1], 1) < 0) {
                        uwsgi_error("dup2()");
                        exit(1);
                }
        }

        if (dup2(1, 2) < 0) {
                uwsgi_error("dup2()");
                exit(1);
        }

}

char *uwsgi_get_cwd() {

	// set this to static to avoid useless reallocations in stats mode
	static size_t newsize = 256;

	char *cwd = uwsgi_malloc(newsize);

	if (getcwd(cwd, newsize) == NULL && errno == ERANGE) {
		newsize += 256;
		uwsgi_log("need a bigger buffer (%d bytes) for getcwd(). doing reallocation.\n", newsize);
		free(cwd);
		cwd = uwsgi_malloc(newsize);
		if (getcwd(cwd, newsize) == NULL) {
			uwsgi_error("getcwd()");
			exit(1);
		}
	}

	return cwd;

}

void internal_server_error(struct wsgi_request *wsgi_req, char *message) {

	if (uwsgi.wsgi_req->headers_size == 0) {
		if (uwsgi.shared->options[UWSGI_OPTION_CGI_MODE] == 0) {
			uwsgi.wsgi_req->headers_size = wsgi_req->socket->proto_write_header(wsgi_req, "HTTP/1.1 500 Internal Server Error\r\nContent-type: text/html\r\n\r\n", 63);
		}
		else {
			uwsgi.wsgi_req->headers_size = wsgi_req->socket->proto_write_header(wsgi_req, "Status: 500 Internal Server Error\r\nContent-type: text/html\r\n\r\n", 62);
		}
		uwsgi.wsgi_req->header_cnt = 2;
	}

	uwsgi.wsgi_req->response_size = wsgi_req->socket->proto_write(wsgi_req, "<h1>uWSGI Error</h1>", 20);
	uwsgi.wsgi_req->response_size += wsgi_req->socket->proto_write(wsgi_req, message, strlen(message));
}

#ifdef __linux__
void uwsgi_set_cgroup() {

	char *cgroup_taskfile;
	FILE *cgroup;
	char *cgroup_opt;
	struct uwsgi_string_list *usl, *uslo;

	if (!uwsgi.cgroup) return;

	usl = uwsgi.cgroup;

	while(usl) {
		if (mkdir(usl->value, 0700)) {
                	uwsgi_log("using Linux cgroup %s\n", usl->value);
			if (errno != EEXIST) {
				uwsgi_error("mkdir()");
			}
                }
                else {
                	uwsgi_log("created Linux cgroup %s\n", usl->value);
                }

                cgroup_taskfile = uwsgi_concat2(usl->value, "/tasks");
                cgroup = fopen(cgroup_taskfile, "w");
                if (!cgroup) {
                	uwsgi_error_open(cgroup_taskfile);
                        exit(1);
                }
                if (fprintf(cgroup, "%d", (int) getpid()) <= 0) {
                	uwsgi_log("could not set cgroup\n");
                        exit(1);
                }
		uwsgi_log("assigned process %d to cgroup %s\n", (int) getpid(), cgroup_taskfile);
                fclose(cgroup);
                free(cgroup_taskfile);


		uslo = uwsgi.cgroup_opt;
		while(uslo) {
                                cgroup_opt = strchr(uslo->value, '=');
                                if (!cgroup_opt) {
                                        cgroup_opt = strchr(uslo->value, ':');
                                        if (!cgroup_opt) {
                                                uwsgi_log("invalid cgroup-opt syntax\n");
                                                exit(1);
                                        }
                                }

                                cgroup_opt[0] = 0;
                                cgroup_opt++;

                                cgroup_taskfile = uwsgi_concat3(usl->value, "/", uslo->value);
                                cgroup = fopen(cgroup_taskfile, "w");
                                if (cgroup) {
                                	if (fprintf(cgroup, "%s\n", cgroup_opt) < 0) {
                                        	uwsgi_log("could not set cgroup option %s to %s\n", uslo->value, cgroup_opt);
                                        	exit(1);
					}
                                	fclose(cgroup);
					uwsgi_log("set %s to %s\n", cgroup_opt, cgroup_taskfile);
                                }
                                free(cgroup_taskfile);

				cgroup_opt[-1] = '=';

				uslo = uslo->next;
                        }

		usl = usl->next;
	}

}
#endif

void uwsgi_as_root() {


	if (!getuid()) {
		if (!uwsgi.master_as_root && !uwsgi.uidname) {
			uwsgi_log_initial("uWSGI running as root, you can use --uid/--gid/--chroot options\n");
		}

#ifdef UWSGI_CAP
		if (uwsgi.cap && uwsgi.cap_count > 0 && !uwsgi.reloads) {

			cap_value_t minimal_cap_values[] = { CAP_SYS_CHROOT, CAP_SETUID, CAP_SETGID, CAP_SETPCAP };

			cap_t caps = cap_init();
			
			if (!caps) {
				uwsgi_error("cap_init()");
				exit(1);
			}
			cap_clear(caps);

    			cap_set_flag(caps, CAP_EFFECTIVE, 4, minimal_cap_values, CAP_SET);

    			cap_set_flag(caps, CAP_PERMITTED, 4, minimal_cap_values, CAP_SET);
    			cap_set_flag(caps, CAP_PERMITTED, uwsgi.cap_count, uwsgi.cap, CAP_SET);

    			cap_set_flag(caps, CAP_INHERITABLE, uwsgi.cap_count, uwsgi.cap, CAP_SET);
    
    			if (cap_set_proc(caps) < 0) {
				uwsgi_error("cap_set_proc()");
				exit(1);
    			}
    			cap_free(caps);

#ifdef __linux__
    			if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) < 0) {
				uwsgi_error("prctl()");
				exit(1);
    			}
#endif
		}
#endif

#if defined(__linux__) && !defined(OBSOLETE_LINUX_KERNEL)
		if (uwsgi.unshare && !uwsgi.reloads) {

			if (unshare(uwsgi.unshare)) {
				uwsgi_error("unshare()");
				exit(1);
			}
			else {
				uwsgi_log("[linux-namespace] applied unshare() mask: %d\n", uwsgi.unshare);
			}
		}
#endif


		if (uwsgi.chroot && !uwsgi.reloads) {
			if (!uwsgi.master_as_root)
				uwsgi_log("chroot() to %s\n", uwsgi.chroot);
			if (chroot(uwsgi.chroot)) {
				uwsgi_error("chroot()");
				exit(1);
			}
#ifdef __linux__
			if (uwsgi.shared->options[UWSGI_OPTION_MEMORY_DEBUG]) {
				uwsgi_log("*** Warning, on linux system you have to bind-mount the /proc fs in your chroot to get memory debug/report.\n");
			}
#endif
		}

		// now run the scripts needed by root
		struct uwsgi_string_list *usl = uwsgi.exec_as_root;
		while(usl) {
			uwsgi_log("running \"%s\" (as root)...\n", usl->value);
			int ret = uwsgi_run_command_and_wait(NULL, usl->value);
			if (ret != 0) {
				uwsgi_log("command \"%s\" exited with non-zero code: %d\n", usl->value, ret);
				exit(1);
			}
			usl = usl->next;
		}

		if (uwsgi.gidname) {
			struct group *ugroup = getgrnam(uwsgi.gidname);
			if (ugroup) {
				uwsgi.gid = ugroup->gr_gid;
			}
			else {
				uwsgi_log("group %s not found.\n", uwsgi.gidname);
				exit(1);
			}
		}
		if (uwsgi.uidname) {
			struct passwd *upasswd = getpwnam(uwsgi.uidname);
			if (upasswd) {
				uwsgi.uid = upasswd->pw_uid;
			}
			else {
				uwsgi_log("user %s not found.\n", uwsgi.uidname);
				exit(1);
			}
		}

		if (uwsgi.logfile_chown) {
			if (fchown(2, uwsgi.uid, uwsgi.gid)) {
				uwsgi_error("fchown()");
				exit(1);
			}
		}
		if (uwsgi.gid) {
			if (!uwsgi.master_as_root)
				uwsgi_log("setgid() to %d\n", uwsgi.gid);
			if (setgid(uwsgi.gid)) {
				uwsgi_error("setgid()");
				exit(1);
			}
			if (setgroups(0, NULL)) {
				uwsgi_error("setgroups()");
				exit(1);
			}
		}
		if (uwsgi.uid) {
			if (!uwsgi.master_as_root)
				uwsgi_log("setuid() to %d\n", uwsgi.uid);
			if (setuid(uwsgi.uid)) {
				uwsgi_error("setuid()");
				exit(1);
			}
		}

		if (!getuid()) {
			uwsgi_log_initial("*** WARNING: you are running uWSGI as root !!! (use the --uid flag) *** \n");
		}

#ifdef UWSGI_CAP

		if (uwsgi.cap && uwsgi.cap_count > 0 && !uwsgi.reloads) {

			cap_t caps = cap_init();

                        if (!caps) {
                                uwsgi_error("cap_init()");
                                exit(1);
                        }
                        cap_clear(caps);

                        cap_set_flag(caps, CAP_EFFECTIVE, uwsgi.cap_count, uwsgi.cap, CAP_SET);
                        cap_set_flag(caps, CAP_PERMITTED, uwsgi.cap_count, uwsgi.cap, CAP_SET);
                        cap_set_flag(caps, CAP_INHERITABLE, uwsgi.cap_count, uwsgi.cap, CAP_SET);

                        if (cap_set_proc(caps) < 0) {
                                uwsgi_error("cap_set_proc()");
                                exit(1);
                        }
                        cap_free(caps);
		}
#endif

		// now run the scripts needed by the user
		usl = uwsgi.exec_as_user;
		while(usl) {
			uwsgi_log("running \"%s\" (as uid: %d gid: %d) ...\n", usl->value, (int) getuid(), (int) getgid());
			int ret = uwsgi_run_command_and_wait(NULL, usl->value);
			if (ret != 0) {
				uwsgi_log("command \"%s\" exited with non-zero code: %d\n", usl->value, ret);
				exit(1);
			}
			usl = usl->next;
		}
	}
	else {
		if (uwsgi.chroot && !uwsgi.is_a_reload) {
			uwsgi_log("cannot chroot() as non-root user\n");
			exit(1);
		}
		if (uwsgi.gid && getgid() != uwsgi.gid) {
			uwsgi_log("cannot setgid() as non-root user\n");
			exit(1);
		}
		if (uwsgi.uid && getuid() != uwsgi.uid) {
			uwsgi_log("cannot setuid() as non-root user\n");
			exit(1);
		}
	}
}

void uwsgi_destroy_request(struct wsgi_request *wsgi_req) {

	wsgi_req->socket->proto_close(wsgi_req);

#ifdef UWSGI_THREADING
	int foo;
        if (uwsgi.threads > 1) {
                // now the thread can die...
                pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &foo);
        }
#endif

	memset(wsgi_req, 0, sizeof(struct wsgi_request));

	
}

void uwsgi_close_request(struct wsgi_request *wsgi_req) {

	int waitpid_status;
	int tmp_id;
	uint64_t tmp_rt, rss = 0, vsz = 0;

	gettimeofday(&wsgi_req->end_of_request, NULL);
	
	tmp_rt = (wsgi_req->end_of_request.tv_sec * 1000000 + wsgi_req->end_of_request.tv_usec) - (wsgi_req->start_of_request.tv_sec * 1000000 + wsgi_req->start_of_request.tv_usec);

	uwsgi.workers[uwsgi.mywid].running_time += tmp_rt;
	uwsgi.workers[uwsgi.mywid].avg_response_time = (uwsgi.workers[uwsgi.mywid].avg_response_time+tmp_rt)/2;

	// get memory usage
	if (uwsgi.shared->options[UWSGI_OPTION_MEMORY_DEBUG] == 1 || uwsgi.force_get_memusage ) {
		get_memusage(&rss, &vsz);
		uwsgi.workers[uwsgi.mywid].vsz_size = vsz;
		uwsgi.workers[uwsgi.mywid].rss_size = rss;
	}


	// close the connection with the webserver
	if (!wsgi_req->fd_closed || wsgi_req->body_as_file) {
		// NOTE, if we close the socket before receiving eventually sent data, socket layer will send a RST
		wsgi_req->socket->proto_close(wsgi_req);
	}
	uwsgi.workers[0].requests++;
	uwsgi.workers[uwsgi.mywid].requests++;
	// this is used for MAX_REQUESTS
	uwsgi.workers[uwsgi.mywid].delta_requests++;

	// after_request hook
	if (uwsgi.p[wsgi_req->uh.modifier1]->after_request)
		uwsgi.p[wsgi_req->uh.modifier1]->after_request(wsgi_req);

#ifdef UWSGI_THREADING
	if (uwsgi.threads > 1) {
		// now the thread can die...
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &tmp_id);
	}
#endif

	// leave harakiri mode
	if (uwsgi.workers[uwsgi.mywid].harakiri > 0) {
		set_harakiri(0);
	}

	// this is racy in multithread mode
	if (wsgi_req->response_size > 0) {
		uwsgi.workers[uwsgi.mywid].tx += wsgi_req->response_size;
	}
	if (wsgi_req->headers_size > 0) {
		uwsgi.workers[uwsgi.mywid].tx += wsgi_req->headers_size;
	}

	// defunct process reaper
	if (uwsgi.shared->options[UWSGI_OPTION_REAPER] == 1 || uwsgi.grunt) {
		while (waitpid(WAIT_ANY, &waitpid_status, WNOHANG) > 0);
	}

	// reset request
	tmp_id = wsgi_req->async_id;
	memset(wsgi_req, 0, sizeof(struct wsgi_request));
	wsgi_req->async_id = tmp_id;

	if (uwsgi.shared->options[UWSGI_OPTION_MAX_REQUESTS] > 0 && uwsgi.workers[uwsgi.mywid].delta_requests >= uwsgi.shared->options[UWSGI_OPTION_MAX_REQUESTS]) {
		goodbye_cruel_world();
	}

	if (uwsgi.reload_on_as && (rlim_t) vsz >= uwsgi.reload_on_as) {
		goodbye_cruel_world();
	}

	if (uwsgi.reload_on_rss && (rlim_t) rss >= uwsgi.reload_on_rss) {
		goodbye_cruel_world();
	}


	// ready to accept request, if i am a vassal signal Emperor about my loyalty
	if (uwsgi.has_emperor && !uwsgi.loyal) {
		uwsgi_log("announcing my loyalty to the Emperor...\n");
		char byte = 17;
		if (write(uwsgi.emperor_fd, &byte, 1) != 1) {
			uwsgi_error("write()");
		}
		uwsgi.loyal = 1;
	}

#ifdef __linux__
#ifdef MADV_MERGEABLE
	// run the ksm mapper
	if (uwsgi.linux_ksm > 0 && (uwsgi.workers[uwsgi.mywid].requests % uwsgi.linux_ksm) == 0) {
		uwsgi_linux_ksm_map();
	}
#endif
#endif

}

#ifdef __linux__
#ifdef MADV_MERGEABLE

void uwsgi_linux_ksm_map(void) {

	int dirty = 0;
	size_t i;
	unsigned long long start = 0, end = 0;
	int errors = 0; int lines = 0;

        int fd = open("/proc/self/maps", O_RDONLY);
	if (fd < 0) {
		uwsgi_error_open("[uwsgi-KSM] /proc/self/maps");
		return ;
	}

	// allocate memory if not available;
	if (uwsgi.ksm_mappings_current == NULL) {
		if (!uwsgi.ksm_buffer_size) uwsgi.ksm_buffer_size = 32768;
		uwsgi.ksm_mappings_current = uwsgi_malloc(uwsgi.ksm_buffer_size);
		uwsgi.ksm_mappings_current_size = 0;
	}
	if (uwsgi.ksm_mappings_last == NULL) {
		if (!uwsgi.ksm_buffer_size) uwsgi.ksm_buffer_size = 32768;
		uwsgi.ksm_mappings_last = uwsgi_malloc(uwsgi.ksm_buffer_size);
		uwsgi.ksm_mappings_last_size = 0;
	}

	uwsgi.ksm_mappings_current_size = read(fd, uwsgi.ksm_mappings_current, uwsgi.ksm_buffer_size);
	close(fd);
	if (uwsgi.ksm_mappings_current_size <= 0) {
		uwsgi_log("[uwsgi-KSM] unable to read /proc/self/maps data\n");
		return;
	}

	// we now have areas
	if (uwsgi.ksm_mappings_last_size == 0 || uwsgi.ksm_mappings_current_size == 0 || uwsgi.ksm_mappings_current_size != uwsgi.ksm_mappings_last_size) {
		dirty = 1;
	}
	else {
		if (memcmp(uwsgi.ksm_mappings_current, uwsgi.ksm_mappings_last, uwsgi.ksm_mappings_current_size) != 0) {
			dirty = 1;
		}
	}

	// it is dirty, swap addresses and parse it
	if (dirty) {
		char *tmp = uwsgi.ksm_mappings_last;
		uwsgi.ksm_mappings_last = uwsgi.ksm_mappings_current;
		uwsgi.ksm_mappings_current = tmp;

		size_t tmp_size = uwsgi.ksm_mappings_last_size;
		uwsgi.ksm_mappings_last_size = uwsgi.ksm_mappings_current_size;
		uwsgi.ksm_mappings_current_size = tmp_size;

		// scan each line and call madvise on it
		char *ptr = uwsgi.ksm_mappings_last;
		for(i=0;i<uwsgi.ksm_mappings_last_size;i++) {
			if (uwsgi.ksm_mappings_last[i] == '\n') {
				lines++;
				uwsgi.ksm_mappings_last[i] = 0;
				if (sscanf(ptr, "%llx-%llx %*s", &start, &end) == 2) {
					if (madvise((void *) (long) start, (size_t) (end-start), MADV_MERGEABLE)) {
						errors ++;
					}
				}
				uwsgi.ksm_mappings_last[i] = '\n';
				ptr = uwsgi.ksm_mappings_last+i+1;
			}
		}

		if (errors >= lines) {
			uwsgi_error("[uwsgi-KSM] unable to share pages");
		}
	}
}
#endif
#endif

void wsgi_req_setup(struct wsgi_request *wsgi_req, int async_id, struct uwsgi_socket *uwsgi_sock) {

	wsgi_req->poll.events = POLLIN;

	wsgi_req->app_id = uwsgi.default_app;

	wsgi_req->async_id = async_id;
#ifdef UWSGI_SENDFILE
	wsgi_req->sendfile_fd = -1;
#endif

	wsgi_req->hvec = uwsgi.async_hvec[wsgi_req->async_id];
	wsgi_req->buffer = uwsgi.async_buf[wsgi_req->async_id];

#ifdef UWSGI_ROUTING
	wsgi_req->ovector = uwsgi.async_ovector[wsgi_req->async_id];
#endif

	if (uwsgi.post_buffering > 0) {
		wsgi_req->post_buffering_buf = uwsgi.async_post_buf[wsgi_req->async_id];
	}

	if (uwsgi_sock) {
		wsgi_req->socket = uwsgi_sock;
	}

	uwsgi.core[wsgi_req->async_id]->in_request = 0;
	uwsgi.workers[uwsgi.mywid].busy = 0;

	// now check for suspend request
	if (uwsgi.workers[uwsgi.mywid].suspended == 1) {
		uwsgi_log_verbose("*** worker %d suspended ***\n", uwsgi.mywid);
cycle:
		// wait for some signal (normally SIGTSTP) or 10 seconds (as fallback)
		(void) poll(NULL, 0, 10*1000);
		if (uwsgi.workers[uwsgi.mywid].suspended == 1) goto cycle;
		uwsgi_log_verbose("*** worker %d resumed ***\n", uwsgi.mywid);
	}
}

#ifdef UWSGI_ASYNC
int wsgi_req_async_recv(struct wsgi_request *wsgi_req) {

	uwsgi.core[wsgi_req->async_id]->in_request = 1;
	uwsgi.workers[uwsgi.mywid].busy = 1;

	gettimeofday(&wsgi_req->start_of_request, NULL);

	if (!wsgi_req->do_not_add_to_async_queue) {
		if (event_queue_add_fd_read(uwsgi.async_queue, wsgi_req->poll.fd) < 0)
			return -1;

		async_add_timeout(wsgi_req, uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT]);
		uwsgi.async_proto_fd_table[wsgi_req->poll.fd] = wsgi_req;
	}



	// enter harakiri mode
	if (uwsgi.shared->options[UWSGI_OPTION_HARAKIRI] > 0) {
		set_harakiri(uwsgi.shared->options[UWSGI_OPTION_HARAKIRI]);
	}

	return 0;
}
#endif

int wsgi_req_recv(struct wsgi_request *wsgi_req) {

	uwsgi.core[wsgi_req->async_id]->in_request = 1;
	uwsgi.workers[uwsgi.mywid].busy = 1;

	gettimeofday(&wsgi_req->start_of_request, NULL);

	if (!wsgi_req->socket->edge_trigger) {
		if (!uwsgi_parse_packet(wsgi_req, uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT])) {
			return -1;
		}
	}

	// enter harakiri mode
	if (uwsgi.shared->options[UWSGI_OPTION_HARAKIRI] > 0) {
		set_harakiri(uwsgi.shared->options[UWSGI_OPTION_HARAKIRI]);
	}

	wsgi_req->async_status = uwsgi.p[wsgi_req->uh.modifier1]->request(wsgi_req);

	return 0;
}


int wsgi_req_simple_accept(struct wsgi_request *wsgi_req, int fd) {

	wsgi_req->poll.fd = wsgi_req->socket->proto_accept(wsgi_req, fd);

	if (wsgi_req->poll.fd < 0) {
		return -1;
	}

	if (wsgi_req->socket->edge_trigger && uwsgi.close_on_exec) {
		fcntl(wsgi_req->poll.fd, F_SETFD, FD_CLOEXEC);
	}

	return 0;
}

int wsgi_req_accept(int queue, struct wsgi_request *wsgi_req) {

	int ret;
	int interesting_fd;
	char uwsgi_signal;
	struct uwsgi_socket *uwsgi_sock = uwsgi.sockets;

	thunder_lock;
	ret = event_queue_wait(queue, uwsgi.edge_triggered - 1, &interesting_fd);
	if (ret < 0) {
		thunder_unlock;
		return -1;
	}

#ifdef UWSGI_THREADING
	// kill the thread after the request completion
	if (uwsgi.threads > 1) pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &ret);
#endif

	if (uwsgi.signal_socket > -1 && (interesting_fd == uwsgi.signal_socket || interesting_fd == uwsgi.my_signal_socket)) {

		thunder_unlock;

		if (read(interesting_fd, &uwsgi_signal, 1) <= 0) {
			if (uwsgi.no_orphans) {
				uwsgi_log_verbose("uWSGI worker %d screams: UAAAAAAH my master died, i will follow him...\n", uwsgi.mywid);
				end_me(0);
			}
			else {
				close(interesting_fd);
			}
		}
		else {
#ifdef UWSGI_DEBUG
			uwsgi_log_verbose("master sent signal %d to worker %d\n", uwsgi_signal, uwsgi.mywid);
#endif
			if (uwsgi_signal_handler(uwsgi_signal)) {
				uwsgi_log_verbose("error managing signal %d on worker %d\n", uwsgi_signal, uwsgi.mywid);
			}
		}

#ifdef UWSGI_THREADING
        	if (uwsgi.threads > 1) pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &ret);
#endif
		return -1;
	}


	while(uwsgi_sock) {
		if (interesting_fd == uwsgi_sock->fd || (uwsgi.edge_triggered && uwsgi_sock->edge_trigger)) {
			wsgi_req->socket = uwsgi_sock;
			wsgi_req->poll.fd = wsgi_req->socket->proto_accept(wsgi_req, interesting_fd);
			thunder_unlock;
			if (wsgi_req->poll.fd < 0) {
#ifdef UWSGI_THREADING
        			if (uwsgi.threads > 1) pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &ret);
#endif
				return -1;
			}

			if (!uwsgi_sock->edge_trigger) {
// in Linux, new sockets do not inherit attributes
#ifndef __linux__
				/* re-set blocking socket */
				int arg = uwsgi_sock->arg;
				arg &= (~O_NONBLOCK);
				if (fcntl(wsgi_req->poll.fd, F_SETFL, arg) < 0) {
					uwsgi_error("fcntl()");
#ifdef UWSGI_THREADING
        				if (uwsgi.threads > 1) pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &ret);
#endif
					return -1;
				}

#endif

				if (uwsgi.close_on_exec) {
					fcntl(wsgi_req->poll.fd, F_SETFD, FD_CLOEXEC);
				}

			}
			return 0;
		}

		uwsgi_sock = uwsgi_sock->next;
	}

	thunder_unlock;
#ifdef UWSGI_THREADING
        if (uwsgi.threads > 1) pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &ret);
#endif
	return -1;
}

void sanitize_args() {

	if (uwsgi.async > 1) {
		uwsgi.cores = uwsgi.async;
	}

	if (uwsgi.threads > 1) {
		uwsgi.has_threads = 1;
		uwsgi.cores = uwsgi.threads;
	}

	if (uwsgi.shared->options[UWSGI_OPTION_HARAKIRI] > 0) {
		if (!uwsgi.post_buffering) {
			uwsgi_log(" *** WARNING: you have enabled harakiri without post buffering. Slow upload could be rejected on post-unbuffered webservers *** \n");
		}
	}

#ifdef UWSGI_HTTP
	if (uwsgi.http && !uwsgi.http_only) {
		uwsgi.vacuum = 1;
	}
#endif
}

void env_to_arg(char *src, char *dst) {
	int i;
	int val = 0;

	for (i = 0; i < (int) strlen(src); i++) {
		if (src[i] == '=') {
			val = 1;
		}
		if (val) {
			dst[i] = src[i];
		}
		else {
			dst[i] = tolower((int) src[i]);
			if (dst[i] == '_') {
				dst[i] = '-';
			}
		}
	}

	dst[strlen(src)] = 0;
}

char *uwsgi_lower(char *str, size_t size) {
	size_t i;
	for(i=0;i<size;i++) {
		str[i] = tolower((int) str[i]);
	}

	return str;
}

void parse_sys_envs(char **envs) {

	char **uenvs = envs;
	char *earg, *eq_pos;

	while (*uenvs) {
		if (!strncmp(*uenvs, "UWSGI_", 6) && strncmp(*uenvs, "UWSGI_RELOADS=", 14) && strncmp(*uenvs, "UWSGI_ORIGINAL_PROC_NAME=", 25)) {
			earg = uwsgi_malloc(strlen(*uenvs + 6) + 1);
			env_to_arg(*uenvs + 6, earg);
			eq_pos = strchr(earg, '=');
			if (!eq_pos) {
				break;
			}
			eq_pos[0] = 0;

			add_exported_option(earg, eq_pos + 1, 0);
		}
		uenvs++;
	}

}

//use this instead of fprintf to avoid buffering mess with udp logging
void uwsgi_log(const char *fmt, ...) {
	va_list ap;
	char logpkt[4096];
	int rlen = 0;
	int ret;

	struct timeval tv;
	char sftime[64];
	time_t now;

	if (uwsgi.logdate) {
		if (uwsgi.log_strftime) {
			now = time(NULL);
			rlen = strftime(sftime, 64, uwsgi.log_strftime, localtime(&now));
			memcpy(logpkt, sftime, rlen);
			memcpy(logpkt + rlen, " - ", 3);
			rlen += 3;
		}
		else {
			gettimeofday(&tv, NULL);

			memcpy(logpkt, ctime((const time_t *) &tv.tv_sec), 24);
			memcpy(logpkt + 24, " - ", 3);

			rlen = 24 + 3;
		}
	}

	va_start(ap, fmt);
	ret = vsnprintf(logpkt + rlen, 4096 - rlen, fmt, ap);
	va_end(ap);

	if (ret >= 4096) {
		char *tmp_buf = uwsgi_malloc(rlen + ret + 1);
		memcpy(tmp_buf, logpkt, rlen);
		va_start(ap, fmt);
		ret = vsnprintf(tmp_buf + rlen,  ret+1, fmt, ap);
		va_end(ap);
		rlen = write(2, tmp_buf, rlen+ret);
		free(tmp_buf);
		return;
	}

	rlen+=ret;
	// do not check for errors
	rlen = write(2, logpkt, rlen);
}

void uwsgi_log_verbose(const char *fmt, ...) {

	va_list ap;
	char logpkt[4096];
	int rlen = 0;

	struct timeval tv;
	char sftime[64];
        time_t now;

		if (uwsgi.log_strftime) {
                        now = time(NULL);
                        rlen = strftime(sftime, 64, uwsgi.log_strftime, localtime(&now));
                        memcpy(logpkt, sftime, rlen);
                        memcpy(logpkt + rlen, " - ", 3);
                        rlen += 3;
                }
                else {
                        gettimeofday(&tv, NULL);

                        memcpy(logpkt, ctime((const time_t *) &tv.tv_sec), 24);
                        memcpy(logpkt + 24, " - ", 3);

                        rlen = 24 + 3;
                }
	


	va_start(ap, fmt);
	rlen += vsnprintf(logpkt + rlen, 4096 - rlen, fmt, ap);
	va_end(ap);

	// do not check for errors
	rlen = write(2, logpkt, rlen);
}

char *uwsgi_str_contains(char *str, int slen, char what) {

	int i;
	for(i=0;i<slen;i++) {
		if (str[i] == what) {
			return str+i;
		}
	}
	return NULL;
}

inline int uwsgi_strncmp(char *src, int slen, char *dst, int dlen) {

	if (slen != dlen)
		return 1;

	return memcmp(src, dst, dlen);

}

inline int uwsgi_starts_with(char *src, int slen, char *dst, int dlen) {

	if (slen < dlen)
		return -1;

	return memcmp(src, dst, dlen);
}

inline int uwsgi_startswith(char *src, char *what, int wlen) {

	int i;

	for (i = 0; i < wlen; i++) {
		if (src[i] != what[i])
			return -1;
	}

	return 0;
}

char *uwsgi_concatn(int c, ...) {

	va_list s;
	char *item;
	int j = c;
	char *buf;
	size_t len = 1;
	size_t tlen = 1;

	va_start(s, c);
	while (j > 0) {
		item = va_arg(s, char *);
		if (item == NULL) {
			break;
		}
		len += va_arg(s, int);
		j--;
	}
	va_end(s);


	buf = uwsgi_malloc(len);
	memset(buf, 0, len);

	j = c;

	len = 0;

	va_start(s, c);
	while (j > 0) {
		item = va_arg(s, char *);
		if (item == NULL) {
			break;
		}
		tlen = va_arg(s, int);
		memcpy(buf + len, item, tlen);
		len += tlen;
		j--;
	}
	va_end(s);


	return buf;

}

char *uwsgi_concat2(char *one, char *two) {

	char *buf;
	size_t len = strlen(one) + strlen(two) + 1;


	buf = uwsgi_malloc(len);
	buf[len - 1] = 0;

	memcpy(buf, one, strlen(one));
	memcpy(buf + strlen(one), two, strlen(two));

	return buf;

}

char *uwsgi_concat4(char *one, char *two, char *three, char *four) {

	char *buf;
	size_t len = strlen(one) + strlen(two) + strlen(three) + strlen(four) + 1;


	buf = uwsgi_malloc(len);
	buf[len - 1] = 0;

	memcpy(buf, one, strlen(one));
	memcpy(buf + strlen(one), two, strlen(two));
	memcpy(buf + strlen(one) + strlen(two), three, strlen(three));
	memcpy(buf + strlen(one) + strlen(two) + strlen(three), four, strlen(four));

	return buf;

}


char *uwsgi_concat3(char *one, char *two, char *three) {

	char *buf;
	size_t len = strlen(one) + strlen(two) + strlen(three) + 1;


	buf = uwsgi_malloc(len);
	buf[len - 1] = 0;

	memcpy(buf, one, strlen(one));
	memcpy(buf + strlen(one), two, strlen(two));
	memcpy(buf + strlen(one) + strlen(two), three, strlen(three));

	return buf;

}

char *uwsgi_concat2n(char *one, int s1, char *two, int s2) {

	char *buf;
	size_t len = s1 + s2 + 1;


	buf = uwsgi_malloc(len);
	buf[len - 1] = 0;

	memcpy(buf, one, s1);
	memcpy(buf + s1, two, s2);

	return buf;

}

char *uwsgi_concat2nn(char *one, int s1, char *two, int s2, int *len) {

	char *buf;
	*len = s1 + s2 + 1;


	buf = uwsgi_malloc(*len);
	buf[*len - 1] = 0;

	memcpy(buf, one, s1);
	memcpy(buf + s1, two, s2);

	return buf;

}


char *uwsgi_concat3n(char *one, int s1, char *two, int s2, char *three, int s3) {

	char *buf;
	size_t len = s1 + s2 + s3 + 1;


	buf = uwsgi_malloc(len);
	buf[len - 1] = 0;

	memcpy(buf, one, s1);
	memcpy(buf + s1, two, s2);
	memcpy(buf + s1 + s2, three, s3);

	return buf;

}

char *uwsgi_concat4n(char *one, int s1, char *two, int s2, char *three, int s3, char *four, int s4) {

	char *buf;
	size_t len = s1 + s2 + s3 + s4 + 1;


	buf = uwsgi_malloc(len);
	buf[len - 1] = 0;

	memcpy(buf, one, s1);
	memcpy(buf + s1, two, s2);
	memcpy(buf + s1 + s2, three, s3);
	memcpy(buf + s1 + s2 + s3, four, s4);

	return buf;

}



char *uwsgi_concat(int c, ...) {

	va_list s;
	char *item;
	size_t len = 1;
	int j = c;
	char *buf;

	va_start(s, c);
	while (j > 0) {
		item = va_arg(s, char *);
		if (item == NULL) {
			break;
		}
		len += (int) strlen(item);
		j--;
	}
	va_end(s);


	buf = uwsgi_malloc(len);
	memset(buf, 0, len);

	j = c;

	len = 0;

	va_start(s, c);
	while (j > 0) {
		item = va_arg(s, char *);
		if (item == NULL) {
			break;
		}
		memcpy(buf + len, item, strlen(item));
		len += strlen(item);
		j--;
	}
	va_end(s);


	return buf;

}

char *uwsgi_strncopy(char *s, int len) {

	char *buf;

	buf = uwsgi_malloc(len + 1);
	buf[len] = 0;

	memcpy(buf, s, len);

	return buf;

}


int uwsgi_get_app_id(char *app_name, int app_name_len, int modifier1) {

	int i;
	struct stat st;
	int found;

	for (i = 0; i < uwsgi_apps_cnt; i++) {
		// reset check
		found = 0;
#ifdef UWSGI_DEBUG
		uwsgi_log("searching for %.*s in %.*s %p\n", app_name_len, app_name, uwsgi_apps[i].mountpoint_len, uwsgi_apps[i].mountpoint, uwsgi_apps[i].callable);
#endif
		if (!uwsgi_apps[i].callable) {
			continue;
		}
	
#ifdef UWSGI_PCRE
		if (uwsgi_apps[i].pattern) {
			if (uwsgi_regexp_match(uwsgi_apps[i].pattern, uwsgi_apps[i].pattern_extra, app_name, app_name_len) >= 0) {
				found = 1;
			}
		}
		else
#endif
		if (!uwsgi_strncmp(uwsgi_apps[i].mountpoint, uwsgi_apps[i].mountpoint_len, app_name, app_name_len)) {
			found = 1;
		}

		if (found) {
			if (uwsgi_apps[i].touch_reload) {
				if (!stat(uwsgi_apps[i].touch_reload, &st)) {
					if (st.st_mtime != uwsgi_apps[i].touch_reload_mtime) {
						// serve the new request and reload
						uwsgi.workers[uwsgi.mywid].manage_next_request = 0;
						if (uwsgi.threads > 1) {
							uwsgi.workers[uwsgi.mywid].destroy = 1;
						}

#ifdef UWSGI_DEBUG
						uwsgi_log("mtime %d %d\n", st.st_mtime, uwsgi_apps[i].touch_reload_mtime);
#endif
					}
				}
			}
			if (modifier1 == -1)
				return i;
			if (modifier1 == uwsgi_apps[i].modifier1)
				return i;
		}
	}

	return -1;
}

int uwsgi_count_options(struct uwsgi_option *uopt) {

	struct uwsgi_option *aopt;
	int count = 0;

	while ((aopt = uopt)) {
		if (!aopt->name)
			break;
		count++;
		uopt++;
	}

	return count;
}

int uwsgi_read_whole_body_in_mem(struct wsgi_request *wsgi_req, char *buf) {

	size_t post_remains = wsgi_req->post_cl;
	int ret;
	ssize_t len;
	char *ptr = buf;

	while (post_remains > 0) {
		if (uwsgi.shared->options[UWSGI_OPTION_HARAKIRI] > 0) {
			inc_harakiri(uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT]);
		}

		ret = uwsgi_waitfd(wsgi_req->poll.fd, uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT]);
		if (ret < 0) {
			return 0;
		}

		if (!ret) {
			uwsgi_log("buffering POST data timed-out !!!\n");
			return 0;
		}

		len = read(wsgi_req->poll.fd, ptr, post_remains);

		if (len <= 0) {
			uwsgi_error("read()");
			return 0;
		}
		ptr += len;
		post_remains -= len;
	}

	return 1;

}

int uwsgi_read_whole_body(struct wsgi_request *wsgi_req, char *buf, size_t len) {

	size_t post_remains = wsgi_req->post_cl;
	ssize_t post_chunk;
	int ret, i;
	int upload_progress_fd = -1;
	char *upload_progress_filename = NULL;
	const char *x_progress_id = "X-Progress-ID=";
	char *xpi_ptr = (char *) x_progress_id;


	wsgi_req->async_post = tmpfile();
	if (!wsgi_req->async_post) {
		uwsgi_error("tmpfile()");
		return 0;
	}

	if (uwsgi.upload_progress) {
		// first check for X-Progress-ID size
		// separator + 'X-Progress-ID' + '=' + uuid     
		if (wsgi_req->uri_len > 51) {
			for (i = 0; i < wsgi_req->uri_len; i++) {
				if (wsgi_req->uri[i] == xpi_ptr[0]) {
					if (xpi_ptr[0] == '=') {
						if (wsgi_req->uri + i + 36 <= wsgi_req->uri + wsgi_req->uri_len) {
							upload_progress_filename = wsgi_req->uri + i + 1;
						}
						break;
					}
					xpi_ptr++;
				}
				else {
					xpi_ptr = (char *) x_progress_id;
				}
			}

			// now check for valid uuid (from spec available at http://en.wikipedia.org/wiki/Universally_unique_identifier)
			if (upload_progress_filename) {

				uwsgi_log("upload progress uuid = %.*s\n", 36, upload_progress_filename);
				if (!check_hex(upload_progress_filename, 8))
					goto cycle;
				if (upload_progress_filename[8] != '-')
					goto cycle;

				if (!check_hex(upload_progress_filename + 9, 4))
					goto cycle;
				if (upload_progress_filename[13] != '-')
					goto cycle;

				if (!check_hex(upload_progress_filename + 14, 4))
					goto cycle;
				if (upload_progress_filename[18] != '-')
					goto cycle;

				if (!check_hex(upload_progress_filename + 19, 4))
					goto cycle;
				if (upload_progress_filename[23] != '-')
					goto cycle;

				if (!check_hex(upload_progress_filename + 24, 12))
					goto cycle;

				upload_progress_filename = uwsgi_concat4n(uwsgi.upload_progress, strlen(uwsgi.upload_progress), "/", 1, upload_progress_filename, 36, ".js", 3);
				// here we use O_EXCL to avoid eventual application bug in uuid generation/using
				upload_progress_fd = open(upload_progress_filename, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP);
				if (upload_progress_fd < 0) {
					uwsgi_error_open(upload_progress_filename);
					free(upload_progress_filename);
				}
			}
		}
	}

      cycle:
	if (upload_progress_filename && upload_progress_fd == -1) {
		uwsgi_log("invalid X-Progress-ID value: must be a UUID\n");
	}
	// manage buffered data and upload progress
	while (post_remains > 0) {

		if (uwsgi.shared->options[UWSGI_OPTION_HARAKIRI] > 0) {
			inc_harakiri(uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT]);
		}

		ret = uwsgi_waitfd(wsgi_req->poll.fd, uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT]);
		if (ret < 0) {
			return 0;
		}

		if (!ret) {
			uwsgi_log("buffering POST data timed-out !!!\n");
			goto end;
		}

		if (post_remains > len) {
			post_chunk = read(wsgi_req->poll.fd, buf, len);
		}
		else {
			post_chunk = read(wsgi_req->poll.fd, buf, post_remains);
		}

		if (post_chunk < 0) {
			uwsgi_error("read()");
			goto end;
		}

		if (post_chunk == 0) {
			uwsgi_log("client did not send the whole body: %s\n", strerror(errno));
			goto end;
		}

		if (fwrite(buf, post_chunk, 1, wsgi_req->async_post) != 1) {
			uwsgi_error("fwrite()");
			goto end;
		}
		if (upload_progress_fd > -1) {
			//write json data to the upload progress file
			if (lseek(upload_progress_fd, 0, SEEK_SET)) {
				uwsgi_error("lseek()");
				goto end;
			}

			// reuse buf for json buffer
			ret = snprintf(buf, len, "{ \"state\" : \"uploading\", \"received\" : %d, \"size\" : %d }\r\n", (int) (wsgi_req->post_cl - post_remains), (int) wsgi_req->post_cl);
			if (ret < 0) {
				uwsgi_log("unable to write JSON data in upload progress file %s\n", upload_progress_filename);
				goto end;
			}
			if (write(upload_progress_fd, buf, ret) < 0) {
				uwsgi_error("write()");
				goto end;
			}

			if (fsync(upload_progress_fd)) {
				uwsgi_error("fsync()");
			}
		}
		post_remains -= post_chunk;
	}
	rewind(wsgi_req->async_post);

	if (upload_progress_fd > -1) {
		close(upload_progress_fd);
		if (unlink(upload_progress_filename)) {
			uwsgi_error("unlink()");
		}
		free(upload_progress_filename);
	}

	return 1;

      end:
	if (upload_progress_fd > -1) {
		close(upload_progress_fd);
		if (unlink(upload_progress_filename)) {
			uwsgi_error("unlink()");
		}
		free(upload_progress_filename);
	}
	return 0;
}

struct uwsgi_option *uwsgi_opt_get(char *name) {
	struct uwsgi_option *op = uwsgi.options;

	while(op->name) {
		if (!strcmp(name, op->name)) {
			return op;
		}
		op++;
	}

	return NULL;
}

void add_exported_option(char *key, char *value, int configured) {

	if (!uwsgi.exported_opts) {
                        uwsgi.exported_opts = uwsgi_malloc(sizeof(struct uwsgi_opt *));
                }
                else {
                        uwsgi.exported_opts = realloc(uwsgi.exported_opts, sizeof(struct uwsgi_opt *) * (uwsgi.exported_opts_cnt + 1));
                        if (!uwsgi.exported_opts) {
                                uwsgi_error("realloc()");
                                exit(1);
                        }
                }

		int id = uwsgi.exported_opts_cnt;
                uwsgi.exported_opts[id] = uwsgi_malloc(sizeof(struct uwsgi_opt));
                uwsgi.exported_opts[id]->key = key;
                uwsgi.exported_opts[id]->value = value;
                uwsgi.exported_opts[id]->configured = configured;
                uwsgi.exported_opts_cnt++;
		uwsgi.dirty_config = 1;

		struct uwsgi_option *op = uwsgi_opt_get(key);
		// immediate ?
		if (op && op->prio == UWSGI_OPT_IMMEDIATE) {
			op->func(key, value, 0, op->data);
			uwsgi.exported_opts[id]->configured = 1;
		}

}

int uwsgi_waitfd(int fd, int timeout) {

	int ret;
	struct pollfd upoll[1];
	char oob;
	ssize_t rlen;


	if (!timeout)
		timeout = uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT];

	timeout = timeout * 1000;
	if (timeout < 0)
		timeout = -1;

	upoll[0].fd = fd;
	upoll[0].events = POLLIN | POLLPRI;
	upoll[0].revents = 0;
	ret = poll(upoll, 1, timeout);

	if (ret < 0) {
		uwsgi_error("poll()");
	}
	else if (ret > 0) {
		if (upoll[0].revents & POLLIN) {
			return ret;
		}

		if (upoll[0].revents & POLLPRI) {
			uwsgi_log("DETECTED PRI DATA\n");
			rlen = recv(fd, &oob, 1, MSG_OOB);
			uwsgi_log("RECEIVE OOB DATA %d !!!\n", rlen);
			if (rlen < 0) {
				return -1;
			}
			return 0;
		}
	}

	return ret;
}


inline void *uwsgi_malloc(size_t size) {

	char *ptr = malloc(size);
	if (ptr == NULL) {
		uwsgi_error("malloc()");
		exit(1);
	}

	return ptr;
}

inline void *uwsgi_calloc(size_t size) {

	char *ptr = uwsgi_malloc(size);
	memset(ptr, 0, size);
	return ptr;
}


char *uwsgi_cheap_string(char *buf, int len) {

	int i;
	char *cheap_buf = buf - 1;


	for (i = 0; i < len; i++) {
		*cheap_buf++ = buf[i];
	}


	buf[len - 1] = 0;

	return buf - 1;
}

char *uwsgi_resolve_ip(char *domain) {

	struct hostent *he;

	he = gethostbyname(domain);
	if (!he || !*he->h_addr_list || he->h_addrtype != AF_INET) {
		return NULL;
	}

	return inet_ntoa(*(struct in_addr *) he->h_addr_list[0]);
}

int uwsgi_file_exists(char *filename) {
	// TODO check for http url or stdin
	return !access(filename, R_OK);
}

char *uwsgi_read_fd(int fd, int *size, int add_zero) {

	char stack_buf[4096];
	ssize_t len;
	char *buffer = NULL;

	len = 1;
        while(len > 0) {
        	len = read(fd, stack_buf, 4096);
                if (len > 0) {
                	*size += len;
                        buffer = realloc(buffer, *size);
                        memcpy(buffer+(*size-len), stack_buf, len);
                }
	}

        if (add_zero) {
        	*size = *size+1;
                buffer = realloc(buffer, *size);
                buffer[*size-1] = 0;
	}

	return buffer;

}

char *uwsgi_simple_file_read(char *filename) {

	struct stat sb;
	char *buffer;
	ssize_t len;
	int fd = open(filename, O_RDONLY);
        if (fd < 0) {
        	uwsgi_error_open(filename);
		goto end;		
        }

        if (fstat(fd, &sb)) {
        	uwsgi_error("fstat()");
		close(fd);
		goto end;
        }

        buffer = uwsgi_malloc(sb.st_size+1);

        len = read(fd, buffer, sb.st_size);
        if (len != sb.st_size) {
        	uwsgi_error("read()");
		free(buffer);
		close(fd);
		goto end;
        }

        close(fd);
	if (buffer[sb.st_size-1] == '\n' || buffer[sb.st_size-1] == '\r') {
		buffer[sb.st_size-1] = 0;
	}
	buffer[sb.st_size] = 0;
	return buffer;
end:
	return (char *) "";

}

char *uwsgi_open_and_read(char *url, int *size, int add_zero, char *magic_table[]) {

	int fd;
	struct stat sb;
	char *buffer = NULL;
	char byte;
	ssize_t len;
	char *uri, *colon;
	char *domain;
	char *ip;
	int body = 0;
	char *magic_buf;

	// stdin ?
	if (!strcmp(url, "-")) {
		buffer = uwsgi_read_fd(0, size, add_zero);
	}
	// fd ?
	else if (!strncmp("fd://", url, 5)) {
		fd = atoi(url+5);
		buffer = uwsgi_read_fd(fd, size, add_zero);
	}
	// http url ?
	else if (!strncmp("http://", url, 7)) {
		domain = url + 7;
		uri = strchr(domain, '/');
		if (!uri) {
			uwsgi_log("invalid http url\n");
			exit(1);
		}
		uri[0] = 0;
		uwsgi_log("domain: %s\n", domain);

		colon = uwsgi_get_last_char(domain, ':');

		if (colon) {
			colon[0] = 0;
		}


		ip = uwsgi_resolve_ip(domain);
		if (!ip) {
			uwsgi_log("unable to resolve address %s\n", domain);
			exit(1);
		}

		if (colon) {
			colon[0] = ':';
			ip = uwsgi_concat2(ip, colon);
		}
		else {
			ip = uwsgi_concat2(ip, ":80");
		}

		fd = uwsgi_connect(ip, 0, 0);

		if (fd < 0) {
			exit(1);
		}

		uri[0] = '/';

		len = write(fd, "GET ", 4);
		len = write(fd, uri, strlen(uri));
		len = write(fd, " HTTP/1.0\r\n", 11);
		len = write(fd, "Host: ", 6);

		uri[0] = 0;
		len = write(fd, domain, strlen(domain));
		uri[0] = '/';

		len = write(fd, "\r\nUser-Agent: uWSGI on ", 23);
		len = write(fd, uwsgi.hostname, uwsgi.hostname_len);
		len = write(fd, "\r\n\r\n", 4);

		int http_status_code_ptr = 0;

		while (read(fd, &byte, 1) == 1) {
			if (byte == '\r' && body == 0) {
				body = 1;
			}
			else if (byte == '\n' && body == 1) {
				body = 2;
			}
			else if (byte == '\r' && body == 2) {
				body = 3;
			}
			else if (byte == '\n' && body == 3) {
				body = 4;
			}
			else if (body == 4) {
				*size = *size + 1;
				buffer = realloc(buffer, *size);
				if (!buffer) {
					uwsgi_error("realloc()");
					exit(1);
				}
				buffer[*size - 1] = byte;
			}
			else {
				body = 0;
				http_status_code_ptr++;
				if (http_status_code_ptr == 10) {
					if (byte != '2') {
						uwsgi_log("Not usable HTTP response: %cxx\n", byte);
						if (uwsgi.has_emperor) {
							exit(UWSGI_EXILE_CODE);
						}
						else {
							exit(1);
						}
					}
				}
			}
		}

		close(fd);

		if (add_zero) {
			*size = *size + 1;
			buffer = realloc(buffer, *size);
			buffer[*size - 1] = 0;
		}

	}
	else if (!strncmp("emperor://", url, 10)) {
		if (uwsgi.emperor_fd_config < 0) {
			uwsgi_log("this is not a vassal instance\n");
			exit(1);
		}
		char *tmp_buffer[4096];
		ssize_t rlen = 1;
		*size = 0;
		while (rlen > 0) {
			rlen = read(uwsgi.emperor_fd_config, tmp_buffer, 4096);
			if (rlen > 0) {
				*size += rlen;
				buffer = realloc(buffer, *size);
				if (!buffer) {
					uwsgi_error("realloc()");
					exit(1);
				}
				memcpy(buffer + (*size - rlen), tmp_buffer, rlen);
			}
		}
		close(uwsgi.emperor_fd_config);
		uwsgi.emperor_fd_config = -1;

		if (add_zero) {
			*size = *size + 1;
			buffer = realloc(buffer, *size);
			buffer[*size - 1] = 0;
		}
	}
#ifdef UWSGI_EMBED_CONFIG
	else if (url[0] == 0) {
		*size = &UWSGI_EMBED_CONFIG_END-&UWSGI_EMBED_CONFIG;
		if (add_zero) {
			*size+=1;
		}
		buffer = uwsgi_malloc(*size);
		memset(buffer, 0, *size);
		memcpy(buffer, &UWSGI_EMBED_CONFIG, &UWSGI_EMBED_CONFIG_END-&UWSGI_EMBED_CONFIG);
	}
#endif
	else if (!strncmp("data://", url, 7)) {
		fd = open(uwsgi.binary_path, O_RDONLY);
		if (fd < 0) {
			uwsgi_error_open(uwsgi.binary_path);
			exit(1);
		}
		int slot = atoi(url+7);
		if (slot < 0) {
			uwsgi_log("invalid binary data slot requested\n");
			exit(1);
		}
		uwsgi_log("requesting binary data slot %d\n", slot);
		off_t fo = lseek(fd, 0, SEEK_END);
		if (fo < 0) {
			uwsgi_error("lseek()");
			uwsgi_log("invalid binary data slot requested\n");
			exit(1);
		}
		int i = 0;
		uint64_t datasize = 0;
		for(i=0;i<=slot;i++) {
			fo = lseek(fd, -9, SEEK_CUR);
			if (fo < 0) {
				uwsgi_error("lseek()");
				uwsgi_log("invalid binary data slot requested\n");
				exit(1);
			}
			ssize_t len = read(fd, &datasize, 8);
			if (len != 8) {
				uwsgi_error("read()");
				uwsgi_log("invalid binary data slot requested\n");
				exit(1);
			}
			if (datasize == 0) {
				uwsgi_log("0 size binary data !!!\n");
				exit(1);
			}
			fo = lseek(fd, -(datasize+9), SEEK_CUR);	
			if (fo < 0) {
				uwsgi_error("lseek()");
				uwsgi_log("invalid binary data slot requested\n");
				exit(1);
			}

			if (i == slot) {
				*size = datasize;
				if (add_zero) {
                        		*size+=1;
                		}
				buffer = uwsgi_malloc(*size);
                		memset(buffer, 0, *size);
				len = read(fd, buffer, datasize);
				if (len != (ssize_t) datasize) {
					uwsgi_error("read()");
					uwsgi_log("invalid binary data slot requested\n");
					exit(1);
				}
			}
		}
	}
	else if (!strncmp("sym://", url, 6)) {
		char *symbol = uwsgi_concat3("_binary_", url+6, "_start") ;
		void *sym_start_ptr = dlsym(RTLD_DEFAULT, symbol);
		if (!sym_start_ptr) {
			uwsgi_log("unable to find symbol %s\n", symbol);	
			exit(1);
		}
		free(symbol);
		symbol = uwsgi_concat3("_binary_", url+6, "_end");
		void *sym_end_ptr = dlsym(RTLD_DEFAULT, symbol);
                if (!sym_end_ptr) {
                        uwsgi_log("unable to find symbol %s\n", symbol);
                        exit(1);
                }
                free(symbol);

		*size = sym_end_ptr - sym_start_ptr;
		if (add_zero) {
                        *size+=1;
                }
                buffer = uwsgi_malloc(*size);
                memset(buffer, 0, *size);
                memcpy(buffer, sym_start_ptr, sym_end_ptr - sym_start_ptr);

	}
	// fallback to file
	else {
		fd = open(url, O_RDONLY);
		if (fd < 0) {
			uwsgi_error_open(url);
			exit(1);
		}

		if (fstat(fd, &sb)) {
			uwsgi_error("fstat()");
			exit(1);
		}

		if (S_ISFIFO(sb.st_mode)) {
			buffer = uwsgi_read_fd(fd, size, add_zero);
			close(fd);
			goto end;
		}

		buffer = malloc(sb.st_size + add_zero);

		if (!buffer) {
			uwsgi_error("malloc()");
			exit(1);
		}


		len = read(fd, buffer, sb.st_size);
		if (len != sb.st_size) {
			uwsgi_error("read()");
			exit(1);
		}

		close(fd);

		*size = sb.st_size + add_zero;

		if (add_zero)
			buffer[sb.st_size] = 0;
	}

end:

	if (magic_table) {

		magic_buf = magic_sub(buffer, *size, size, magic_table);
		free(buffer);
		return magic_buf;
	}

	return buffer;
}

char *magic_sub(char *buffer, int len, int *size, char *magic_table[]) {

	int i;
	size_t magic_len = 0;
	char *magic_buf = uwsgi_malloc(len);
	char *magic_ptr = magic_buf;
	char *old_magic_buf;

	for (i = 0; i < len; i++) {
		if (buffer[i] == '%' && (i + 1) < len && magic_table[(int) buffer[i + 1]]) {
			old_magic_buf = magic_buf;
			magic_buf = uwsgi_concat3n(old_magic_buf, magic_len, magic_table[(int) buffer[i + 1]], strlen(magic_table[(int) buffer[i + 1]]), buffer + i + 2, len - i);
			free(old_magic_buf);
			magic_len += strlen(magic_table[(int) buffer[i + 1]]);
			magic_ptr = magic_buf + magic_len;
			i++;
		}
		else {
			*magic_ptr = buffer[i];
			magic_ptr++;
			magic_len++;
		}
	}

	*size = magic_len;

	return magic_buf;

}

void init_magic_table(char *magic_table[]) {

	int i;
	for (i = 0; i <= 0xff; i++) {
		magic_table[i] = "";
	}

	magic_table['%'] = "%";
	magic_table['('] = "%(";
}

char *uwsgi_get_last_char(char *what, char c) {
	int i, j = 0;
	char *ptr = NULL;

	if (!strncmp("http://", what, 7))
		j = 7;
	if (!strncmp("emperor://", what, 10))
		j = 10;

	for (i = j; i < (int) strlen(what); i++) {
		if (what[i] == c) {
			ptr = what + i;
		}
	}

	return ptr;
}

void spawn_daemon(struct uwsgi_daemon *ud) {

	char *argv[64];
	char *a;
	int cnt = 1;
	int devnull = -1;
	int throttle = 0;

	if (uwsgi.current_time - ud->last_spawn <= 3) {
		throttle = ud->respawns - (uwsgi.current_time-ud->last_spawn);
	}

	pid_t pid = uwsgi_fork("uWSGI external daemon");
	if (pid < 0) {
		uwsgi_error("fork()");
		return;
	}

	if (pid > 0) {
		ud->pid = pid;
		ud->status = 1;
		if (ud->respawns == 0) {
			ud->born = time(NULL);
		}

		ud->respawns++;
		ud->last_spawn = time(NULL);

	}
	else {
		// close uwsgi sockets
		uwsgi_close_all_sockets();

		// /dev/null will became stdin
		devnull = open("/dev/null", O_RDONLY);	
		if (devnull < 0) {
			uwsgi_error("/dev/null open()");
			exit(1);
		}
		if (devnull != 0) {
			if (dup2(devnull, 0)) {
				uwsgi_error("dup2()");
				exit(1);
			}
		}

		if (setsid() < 0) {
			uwsgi_error("setsid()");
			exit(1);
		}

#ifdef __linux__
		if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0)) {
			uwsgi_error("prctl()");
		}
#endif

		// free the old area
		if (ud->tmp_command) free(ud->tmp_command);

		ud->tmp_command = uwsgi_str(ud->command);

		a = strtok(ud->tmp_command, " ");
		if (a) {
			argv[0] = a;
			while (a != NULL) {
				a = strtok(NULL, " ");
				if (a) {
					argv[cnt] = a;
					cnt++;
				}
			}
		}
		else {
			argv[0] = ud->tmp_command;
		}

		argv[cnt] = NULL;

		if (throttle) {
			uwsgi_log("[uwsgi-daemons] throttling \"%s\" (%s) for %d seconds\n", ud->command, argv[0], throttle);
			sleep(throttle);
		}

		uwsgi_log("[uwsgi-daemons] spawning \"%s\" (%s)\n", ud->command, argv[0]);
		if (execvp(argv[0], argv)) {
			uwsgi_error("execvp()");
		}
		uwsgi_log("[uwsgi-daemons] unable to spawn \"%s\" (%s)\n", ud->command, argv[0]);

		// never here;
		exit(1);
	}

	return;
}

char *uwsgi_num2str(int num) {

	char *str = uwsgi_malloc(11);

	snprintf(str, 11, "%d", num);
	return str;
}

int uwsgi_num2str2(int num, char *ptr) {

	return snprintf(ptr, 11, "%d", num);
}

int uwsgi_num2str2n(int num, char *ptr, int size) {
	return snprintf(ptr, size, "%d", num);
}

int uwsgi_long2str2n(unsigned long long num, char *ptr, int size) {
	int ret = snprintf(ptr, size, "%llu", num);
	if (ret < 0) return 0;
	return ret;
}

int is_unix(char *socket_name, int len) {
	int i;
	for (i = 0; i < len; i++) {
		if (socket_name[i] == ':')
			return 0;
	}

	return 1;
}

int is_a_number(char *what) {
	int i;

	for (i = 0; i < (int) strlen(what); i++) {
		if (!isdigit((int) what[i]))
			return 0;
	}

	return 1;
}

void uwsgi_unix_signal(int signum, void (*func) (int)) {

	struct sigaction sa;

	memset(&sa, 0, sizeof(struct sigaction));

	sa.sa_handler = func;

	sigemptyset(&sa.sa_mask);

	if (sigaction(signum, &sa, NULL) < 0) {
		uwsgi_error("sigaction()");
	}
}

char *uwsgi_get_exported_opt(char *key) {

	int i;

	for (i = 0; i < uwsgi.exported_opts_cnt; i++) {
		if (!strcmp(uwsgi.exported_opts[i]->key, key)) {
			return uwsgi.exported_opts[i]->value;
		}
	}

	return NULL;
}

char *uwsgi_get_optname_by_index(int index) {

	struct uwsgi_option *op = uwsgi.options;

	while (op->name) {
		if (op->shortcut == index) {
			return op->name;
		}
		op++;
	}

	return NULL;
}

int uwsgi_list_has_num(char *list, int num) {

	char *list2 = uwsgi_concat2(list, "");

	char *p = strtok(list2, ",");
	while (p != NULL) {
		if (atoi(p) == num) {
			free(list2);
			return 1;
		}
		p = strtok(NULL, ",");
	}

	free(list2);
	return 0;
}

int uwsgi_list_has_str(char *list, char *str) {

	char *list2 = uwsgi_concat2(list + 1, "");

	char *p = strtok(list2, " ");
	while (p != NULL) {
		if (!strcasecmp(p, str)) {
			free(list2);
			return 1;
		}
		p = strtok(NULL, " ");
	}

	free(list2);
	return 0;
}

char hex2num(char *str) {
	
	char val = 0;
	
	val <<= 4;

	if (str[0] >= '0' && str[0] <= '9') {
		val += str[0] & 0x0F;
	}
	else if ( str[0] >= 'A' && str[0] <= 'F') {
		val += (str[0] & 0x0F) + 9;
	}
	else {
		return 0;
	}

	val <<= 4;

	if (str[1] >= '0' && str[1] <= '9') {
		val += str[1] & 0x0F;
	}
	else if ( str[1] >= 'A' && str[1] <= 'F') {
		val += (str[1] & 0x0F) + 9;
	}
	else {
		return 0;
	}

	return val;
}

int uwsgi_str2_num(char *str) {

	int num = 0;

	num = 10 * (str[0] - 48);
	num += str[1] - 48;

	return num;
}

int uwsgi_str3_num(char *str) {

	int num = 0;

	num = 100 * (str[0] - 48);
	num += 10 * (str[1] - 48);
	num += str[2] - 48;

	return num;
}


int uwsgi_str4_num(char *str) {

	int num = 0;

	num = 1000 * (str[0] - 48);
	num += 100 * (str[1] - 48);
	num += 10 * (str[2] - 48);
	num += str[3] - 48;

	return num;
}

size_t uwsgi_str_num(char *str, int len) {

	int i;
	size_t num = 0;

	size_t delta = pow(10, len);

	for (i = 0; i < len; i++) {
		delta = delta / 10;
		num += delta * (str[i] - 48);
	}

	return num;
}

char *uwsgi_split3(char *buf, size_t len, char sep, char **part1, size_t *part1_len, char **part2, size_t *part2_len, char **part3, size_t *part3_len) {

	size_t i;
	int status = 0;

	*part1 = NULL;
	*part2 = NULL;
	*part3 = NULL;

	for (i = 0; i < len; i++) {
		if (buf[i] == sep) {
			// get part1
			if (status == 0) {
				*part1 = buf;
				*part1_len = i;
				status = 1;
			}
			// get part2
			else if (status == 1) {
				*part2 = *part1 + *part1_len + 1;
				*part2_len = (buf + i) - *part2;
				break;
			}
		}
	}

	if (*part1 && *part2) {
		if (*part2+*part2_len+1 > buf+len) {
			return NULL;
		}
		*part3 = *part2+*part2_len+1;
		*part3_len = (buf + len) - *part3;
		return buf + len;
	}

	return NULL;
}

char *uwsgi_split4(char *buf, size_t len, char sep, char **part1, size_t *part1_len, char **part2, size_t *part2_len, char **part3, size_t *part3_len, char **part4, size_t *part4_len) {

        size_t i;
        int status = 0;

        *part1 = NULL;
        *part2 = NULL;
        *part3 = NULL;
        *part4 = NULL;

        for (i = 0; i < len; i++) {
                if (buf[i] == sep) {
                        // get part1
                        if (status == 0) {
                                *part1 = buf;
                                *part1_len = i;
                                status = 1;
                        }
                        // get part2
                        else if (status == 1) {
                                *part2 = *part1 + *part1_len + 1;
                                *part2_len = (buf + i) - *part2;
                                status = 2;
                        }
                        // get part3
                        else if (status == 2) {
                                *part3 = *part2 + *part2_len + 1;
                                *part3_len = (buf + i) - *part3;
				break;
                        }
                }
        }

        if (*part1 && *part2 && *part3) {
		if (*part3+*part3_len+1 > buf+len) {
			return NULL;
		}
		*part4 = *part3+*part3_len+1;
		*part4_len = (buf + len) - *part4;
                return buf + len;
        }

        return NULL;
}


char *uwsgi_netstring(char *buf, size_t len, char **netstring, size_t *netstring_len) {

	char *ptr = buf;
	char *watermark = buf+len;
	*netstring_len = 0;

	while(ptr < watermark) {
		// end of string size ?
		if (*ptr == ':') {
			*netstring_len = uwsgi_str_num(buf, ptr-buf);

			if (ptr+*netstring_len+2 > watermark) {
				return NULL;
			}
			*netstring = ptr+1;
			return ptr+*netstring_len+2;
		}
		ptr++;
	}

	return NULL;
}

struct uwsgi_daemon *uwsgi_daemon_new(struct uwsgi_daemon **ud, char *command) {

        struct uwsgi_daemon *uwsgi_ud = *ud, *old_ud;

        if (!uwsgi_ud) {
                *ud = uwsgi_malloc(sizeof(struct uwsgi_daemon));
                uwsgi_ud = *ud;
        }
        else {
                while(uwsgi_ud) {
                        old_ud = uwsgi_ud;
                        uwsgi_ud = uwsgi_ud->next;
                }

                uwsgi_ud = uwsgi_malloc(sizeof(struct uwsgi_daemon));
                old_ud->next = uwsgi_ud;
        }

        uwsgi_ud->command = command;
        uwsgi_ud->tmp_command = NULL;
        uwsgi_ud->pid = 0;
        uwsgi_ud->status = 0;
        uwsgi_ud->registered = 0;
        uwsgi_ud->next = NULL;
	uwsgi_ud->respawns = 0;
	uwsgi_ud->last_spawn = 0;

	uwsgi.daemons_cnt++;

        return uwsgi_ud;
}


struct uwsgi_dyn_dict *uwsgi_dyn_dict_new(struct uwsgi_dyn_dict **dd, char *key, int keylen, char *val, int vallen) {

        struct uwsgi_dyn_dict *uwsgi_dd = *dd, *old_dd;

        if (!uwsgi_dd) {
                *dd = uwsgi_malloc(sizeof(struct uwsgi_dyn_dict));
                uwsgi_dd = *dd;
		uwsgi_dd->prev = NULL;
        }
        else {
                while(uwsgi_dd) {
                        old_dd = uwsgi_dd;
                        uwsgi_dd = uwsgi_dd->next;
                }

                uwsgi_dd = uwsgi_malloc(sizeof(struct uwsgi_dyn_dict));
                old_dd->next = uwsgi_dd;
		uwsgi_dd->prev = old_dd;
        }

        uwsgi_dd->key = key;
        uwsgi_dd->keylen = keylen;
        uwsgi_dd->value = val;
        uwsgi_dd->vallen = vallen;
	uwsgi_dd->hits = 0;
	uwsgi_dd->status = 0;
        uwsgi_dd->next = NULL;

        return uwsgi_dd;
}

void uwsgi_dyn_dict_del(struct uwsgi_dyn_dict *item) {

	struct uwsgi_dyn_dict *prev = item->prev;
	struct uwsgi_dyn_dict *next = item->next;

	if (prev) {
		prev->next = next;
	}

	if (next) {
		next->prev = prev;
	}

	free(item);
}

void *uwsgi_malloc_shared(size_t size) {

        void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);

        if (addr == NULL) {
                uwsgi_error("mmap()");
                exit(1);
        }

        return addr;
}


#ifdef UWSGI_SPOOLER
struct uwsgi_spooler *uwsgi_new_spooler(char *dir) {

        struct uwsgi_spooler *uspool = uwsgi.spoolers;

        if (!uspool) {
                uwsgi.spoolers = uwsgi_malloc_shared(sizeof(struct uwsgi_spooler));
                uspool = uwsgi.spoolers;
        }
        else {
                while(uspool) {
			if (uspool->next == NULL) {
				uspool->next = uwsgi_malloc_shared(sizeof(struct uwsgi_spooler));
				uspool = uspool->next;
				break;
			}
			uspool = uspool->next;
                }
        }

	if (!realpath(dir, uspool->dir)) {
		uwsgi_error("[spooler] realpath()");
		exit(1);
	}

	uspool->lock = uwsgi_mmap_shared_lock();
        uwsgi_lock_init(uspool->lock);

        uspool->next = NULL;

        return uspool;
}
#endif


struct uwsgi_string_list *uwsgi_string_new_list(struct uwsgi_string_list **list, char *value) {

	struct uwsgi_string_list *uwsgi_string = *list, *old_uwsgi_string;

        if (!uwsgi_string) {
                *list = uwsgi_malloc(sizeof(struct uwsgi_string_list));
                uwsgi_string = *list;
        }
        else {
                while(uwsgi_string) {
                        old_uwsgi_string = uwsgi_string;
                        uwsgi_string = uwsgi_string->next;
                }

                uwsgi_string = uwsgi_malloc(sizeof(struct uwsgi_string_list));
                old_uwsgi_string->next = uwsgi_string;
        }

        uwsgi_string->value = value;
	uwsgi_string->len = 0;
	if (value) {
        	uwsgi_string->len = strlen(value);
	}
	uwsgi_string->next = NULL;

        return uwsgi_string;
}

char *uwsgi_string_get_list(struct uwsgi_string_list **list, int pos, size_t *len) {

	struct uwsgi_string_list *uwsgi_string = *list;
	int counter = 0;

	while(uwsgi_string) {
		if (counter == pos) {
			*len = uwsgi_string->len;
			return uwsgi_string->value;
		}
		uwsgi_string = uwsgi_string->next;
		counter++;
	}

	*len = 0;
	return NULL;
	
}


void uwsgi_string_del_list(struct uwsgi_string_list **list, struct uwsgi_string_list *item) {

	struct uwsgi_string_list *uwsgi_string = *list, *old_uwsgi_string = NULL;

	while(uwsgi_string) {
		if (uwsgi_string == item) {
			// parent instance ?
			if (old_uwsgi_string == NULL) {
				*list = uwsgi_string->next;
			}
			else {
				old_uwsgi_string->next = uwsgi_string->next;
			}

			free(uwsgi_string);
			return;
		}

		old_uwsgi_string = uwsgi_string;
		uwsgi_string = uwsgi_string->next;
	}
	
}

void uwsgi_sig_pause() {

	sigset_t mask;
        sigemptyset(&mask);
	sigsuspend(&mask);
}

int uwsgi_run_command_and_wait(char *command, char *arg) {

	char *argv[4];
	int waitpid_status = 0;
	pid_t pid = fork();
	if (pid < 0) {
		return -1;
	}
	
	if (pid > 0) {
		if (waitpid(pid, &waitpid_status, 0) < 0) {
			uwsgi_error("waitpid()");
			return -1;
		}

		return WEXITSTATUS(waitpid_status);
	}

#ifdef __linux__
	if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0)) {
        	uwsgi_error("prctl()");
        }
#endif

	if (command == NULL) {
		argv[0] = "/bin/sh";
		argv[1] = "-c";
		argv[2] = arg;
		argv[3] = NULL;
		execvp(argv[0], argv);
	}
	else {
		argv[0] = command;
		argv[1] = arg;
		argv[2] = NULL;
		execvp(command, argv);
	}


	uwsgi_error("execvp()");
	//never here
	exit(1);
}

int uwsgi_run_command(char *command) {

	char *argv[4];

        int waitpid_status = 0;
        pid_t pid = fork();
        if (pid < 0) {
                return -1;
        }

        if (pid > 0) {
                if (waitpid(pid, &waitpid_status, WNOHANG) < 0) {
                        uwsgi_error("waitpid()");
                        return -1;
                }

                return WEXITSTATUS(waitpid_status);
        }

	uwsgi_close_all_sockets();

	if (setsid() < 0) {
		uwsgi_error("setsid()");
		exit(1);
	}

	argv[0] = "/bin/sh";
	argv[1] = "-c";
	argv[2] = command;
	argv[3] = NULL;

        execvp("/bin/sh", argv);

        uwsgi_error("execvp()");
        //never here
        exit(1);
}

int *uwsgi_attach_fd(int fd, int count, char *code, size_t code_len) {

	struct msghdr   msg;
	ssize_t len;
	char *id = NULL;

	struct iovec iov;
	struct cmsghdr *cmsg;
	int *ret;
	int i;

	void *msg_control = uwsgi_malloc(CMSG_SPACE(sizeof(int) * count));
	
	memset( msg_control, 0, CMSG_SPACE(sizeof(int) * count));

	if (code && code_len > 0) {
		id = uwsgi_malloc(code_len);
		memset(id, 0, code_len);
	}

	iov.iov_base = id;
        iov.iov_len = code_len;
        memset(&msg, 0, sizeof(msg));

	msg.msg_name = NULL;
        msg.msg_namelen = 0;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = msg_control;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * count);
        msg.msg_flags = 0;

        len = recvmsg(fd, &msg, 0);
	if (len <= 0) {
		uwsgi_error("recvmsg()");
		return NULL;
	}
	
	if (code && code_len > 0) {
		if (strcmp(id, code)) {
			return NULL;
		}
	}
	
	cmsg = CMSG_FIRSTHDR(&msg);
	if (!cmsg) return NULL;

	if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
		return NULL;
	}

	if ((size_t) (cmsg->cmsg_len - ((char *)CMSG_DATA(cmsg)- (char *)cmsg)) > (size_t) (sizeof(int) * (count + 1))) {
        	uwsgi_log("not enough space for sockets data, consider increasing it\n");
        	return NULL;
	}

	ret = uwsgi_malloc(sizeof(int) * (count + 1));
	for(i=0;i<count+1;i++) {
		ret[i] = -1;
	}

	memcpy(ret, CMSG_DATA(cmsg), cmsg->cmsg_len - ((char *)CMSG_DATA(cmsg)- (char *)cmsg));

	free(msg_control);
	if (code && code_len > 0) {
		free(id);
	}

	return ret;
}

int uwsgi_endswith(char *str1, char *str2) {

	size_t i;
	size_t str1len = strlen(str1);
	size_t str2len = strlen(str2);
	char *ptr;

	if (str2len > str1len) return 0;
	
	ptr = (str1 + str1len) - str2len;

	for(i=0;i<str2len;i++) {
		if (*ptr != str2[i]) return 0;
		ptr++;
	}

	return 1;
}

void uwsgi_chown(char *filename, char *owner) {

	uid_t new_uid = -1;
	uid_t new_gid = -1;
	struct group *new_group = NULL;
	struct passwd *new_user = NULL;

	char *colon = strchr(owner, ':');
	if (colon) {
		colon[0] = 0;
	}

	
	if (is_a_number(owner)) {
		new_uid = atoi(owner);
	}
	else {
		new_user = getpwnam(owner);
		if (!new_user) {
			uwsgi_log("unable to find user %s\n", owner);
			exit(1);
		}
		new_uid = new_user->pw_uid;
	}

	if (colon) {
		colon[0] = ':';
		if (is_a_number(colon+1)) {
			new_gid = atoi(colon+1);
		}
		else {
			new_group = getgrnam(colon+1);
			if (!new_group) {
				uwsgi_log("unable to find group %s\n", colon+1);
				exit(1);
			}
			new_gid = new_group->gr_gid;
		}
	}

	if (chown(filename, new_uid, new_gid)) {
		uwsgi_error("chown()");
		exit(1);
	}

}

char *uwsgi_get_binary_path(char *argvzero) {

#if defined(__linux__)
	char *buf = uwsgi_malloc(uwsgi.page_size);
	ssize_t len = readlink("/proc/self/exe", buf, uwsgi.page_size);
	if (len > 0) {
		return buf;
	}
	free(buf);
#elif defined(__NetBSD__)
	char *buf = uwsgi_malloc(PATH_MAX+1);
        ssize_t len = readlink("/proc/curproc/exe", buf, PATH_MAX);
        if (len > 0) {
                return buf;
        }               

	if (realpath(argvzero, buf)) {
		return buf;
	}
        free(buf);
#elif defined(__APPLE__)
	char *buf = uwsgi_malloc(uwsgi.page_size);
	uint32_t len = uwsgi.page_size;
	if (_NSGetExecutablePath(buf, &len) == 0) {
		// return only absolute path
		char *newbuf = realpath(buf, NULL);
		if (newbuf) {
			free(buf);
			return newbuf;
		}
	}
	free(buf);
#elif defined(__sun__)
	// do not free this value !!!
	char *buf = (char *)getexecname();
	if (buf) {
		// return only absolute path
		if (buf[0] == '/') {
			return buf;
		}

		char *newbuf = uwsgi_malloc(PATH_MAX+1);
		if (realpath(buf, newbuf)) {
			return newbuf;	
		}
	}	
#elif defined(__FreeBSD__)
	char *buf = uwsgi_malloc(uwsgi.page_size);
	size_t len = uwsgi.page_size;
	int mib[4];
	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_PATHNAME;
	mib[3] = -1;
	if (sysctl(mib, 4, buf, &len, NULL, 0) == 0) {
		return buf;
	}
	free(buf);
#endif


	return argvzero;

}

char *uwsgi_get_line(char *ptr, char *watermark, int *size) {
	char *p = ptr;
	int count = 0;

	while(p < watermark) {
		if (*p == '\n') {
			*size = count;
			return ptr+count;
		}
		count++;
		p++;
	}

	return NULL;
}

void uwsgi_build_mime_dict(char *filename) {

	int size = 0;
	char *buf = uwsgi_open_and_read(filename, &size, 1, NULL);
	char *watermark = buf+size;

	int linesize = 0;
	char *line = buf;
	int i;
	int type_size = 0;
	int ext_start = 0;
	int found ;
	int entries = 0;

	uwsgi_log("building mime-types dictionary from file %s...", filename);

	while(uwsgi_get_line(line, watermark, &linesize) != NULL) {
		found = 0;
		if (isalnum((int)line[0])) { 
			// get the type size
			for(i=0;i<linesize;i++) {
				if (isblank((int)line[i])) {
					type_size = i;
					found = 1;
					break;
				}
			}
			if (!found) { line += linesize+1; continue;}
			found = 0;
			for(i=type_size;i<linesize;i++) {
				if (!isblank((int)line[i])) {
					ext_start = i;
					found = 1;
					break;
				}
			}
			if (!found) { line += linesize+1; continue;}

			char *current = line+ext_start;
			int ext_size = 0;
			for(i=ext_start;i<linesize;i++) {
				if (isblank((int)line[i])) {
#ifdef UWSGI_DEBUG
					uwsgi_log("%.*s %.*s\n", ext_size, current, type_size, line);
#endif
					uwsgi_dyn_dict_new(&uwsgi.mimetypes, current, ext_size, line, type_size);
					entries++;
					ext_size = 0;
					current = NULL;
					continue;
				}
				else if (current == NULL) {
					current = line+i;
				}
				ext_size++;
			}
			if (current && ext_size > 1) {
#ifdef UWSGI_DEBUG
				uwsgi_log("%.*s %.*s\n", ext_size, current, type_size, line);
#endif
				uwsgi_dyn_dict_new(&uwsgi.mimetypes, current, ext_size, line, type_size);
				entries++;
			}
		
		}
		line += linesize+1;
	}

	uwsgi_log("%d entry found\n", entries);

}

#ifdef __linux__
struct uwsgi_unshare_id {
	char *name;
	int value;	
};

static struct uwsgi_unshare_id uwsgi_unshare_list[] = {
#ifdef CLONE_FILES
	{"files", CLONE_FILES},
#endif
#ifdef CLONE_FS
	{"fs", CLONE_FS},
#endif
#ifdef CLONE_NEWIPC
	{"ipc", CLONE_NEWIPC},
#endif
#ifdef CLONE_NEWNET
	{"net", CLONE_NEWNET},
#endif
#ifdef CLONE_NEWPID
	{"pid", CLONE_NEWPID},
#endif
#ifdef CLONE_NEWNS
	{"ns", CLONE_NEWNS},
	{"mount", CLONE_NEWNS},
#endif
#ifdef CLONE_SYSVSEM
	{"sysvsem", CLONE_SYSVSEM},
#endif
#ifdef CLONE_NEWUTS
	{"uts", CLONE_NEWUTS},
#endif
	{ NULL, -1}
};

static int uwsgi_get_unshare_id(char *name) {

        struct uwsgi_unshare_id *uui = uwsgi_unshare_list;
        while(uui->name) {
                if (!strcmp(uui->name, name)) return uui->value;
                uui++;
        }

        return -1;
}

void uwsgi_build_unshare(char *what) {

        char *list = uwsgi_str(what);

        char *p = strtok(list, ",");
        while (p != NULL) {
        	int u_id = uwsgi_get_unshare_id(p);
		if (u_id != -1) {
			uwsgi.unshare |= u_id;
		}
                p = strtok(NULL, ",");
        }
        free(list);
}


#endif

#ifdef UWSGI_CAP
struct uwsgi_cap {
	char *name;
	cap_value_t value;
};

static struct uwsgi_cap uwsgi_cap_list[] = {
	{ "chown", CAP_CHOWN},
	{ "dac_override", CAP_DAC_OVERRIDE},
	{ "dac_read_search", CAP_DAC_READ_SEARCH},
	{ "fowner", CAP_FOWNER},
	{ "fsetid", CAP_FSETID},
	{ "kill", CAP_KILL},
	{ "setgid", CAP_SETGID},
	{ "setuid", CAP_SETUID},
	{ "setpcap", CAP_SETPCAP},
	{ "linux_immutable", CAP_LINUX_IMMUTABLE},
	{ "net_bind_service", CAP_NET_BIND_SERVICE},
	{ "net_broadcast", CAP_NET_BROADCAST},
	{ "net_admin", CAP_NET_ADMIN},
	{ "net_raw", CAP_NET_RAW},
	{ "ipc_lock", CAP_IPC_LOCK},
	{ "ipc_owner", CAP_IPC_OWNER},
	{ "sys_module", CAP_SYS_MODULE},
	{ "sys_rawio", CAP_SYS_RAWIO},
	{ "sys_chroot", CAP_SYS_CHROOT},
	{ "sys_ptrace", CAP_SYS_PTRACE},
	{ "sys_pacct", CAP_SYS_PACCT},
	{ "sys_admin", CAP_SYS_ADMIN},
	{ "sys_boot", CAP_SYS_BOOT},
	{ "sys_nice", CAP_SYS_NICE},
	{ "sys_resource", CAP_SYS_RESOURCE},
	{ "sys_time", CAP_SYS_TIME},
	{ "sys_tty_config", CAP_SYS_TTY_CONFIG},
	{ "mknod", CAP_MKNOD},
#ifdef CAP_LEASE
	{ "lease", CAP_LEASE},
#endif
#ifdef CAP_AUDIT_WRITE
	{ "audit_write", CAP_AUDIT_WRITE},
#endif
#ifdef CAP_AUDIT_CONTROL
	{ "audit_control", CAP_AUDIT_CONTROL},
#endif
#ifdef CAP_SETFCAP
	{ "setfcap", CAP_SETFCAP},
#endif
#ifdef CAP_MAC_OVERRIDE
	{ "mac_override", CAP_MAC_OVERRIDE},
#endif
#ifdef CAP_MAC_ADMIN
	{ "mac_admin", CAP_MAC_ADMIN},
#endif
#ifdef CAP_SYSLOG
	{ "syslog", CAP_SYSLOG},
#endif
#ifdef CAP_WAKE_ALARM
	{ "wake_alarm", CAP_WAKE_ALARM},
#endif
	{ NULL, -1}
};

static int uwsgi_get_cap_id(char *name) {

	struct uwsgi_cap *ucl = uwsgi_cap_list;
	while(ucl->name) {
		if (!strcmp(ucl->name, name)) return ucl->value;
		ucl++;
	}

	return -1;
}

void uwsgi_build_cap(char *what) {

	int cap_id;
	char *caps = uwsgi_str(what);
	int pos = 0;
	uwsgi.cap_count = 0;

	char *p = strtok(caps, ",");
        while (p != NULL) {
		if (is_a_number(p)) {
			uwsgi.cap_count++;
		}
		else {
			cap_id = uwsgi_get_cap_id(p);
			if (cap_id != -1) {
				uwsgi.cap_count++;
			}
		}
                p = strtok(NULL, ",");
	}
	free(caps);

	uwsgi.cap = uwsgi_malloc( sizeof(cap_value_t) * uwsgi.cap_count);

	caps = uwsgi_str(what);
	p = strtok(caps, ",");
        while (p != NULL) {
                if (is_a_number(p)) {
			cap_id = atoi(p);
                }
                else {
			cap_id = uwsgi_get_cap_id(p);	
		}
                if (cap_id != -1) {
			uwsgi.cap[pos] = cap_id;
			uwsgi_log("setting capability %s [%d]\n", p, cap_id);
			pos++;
                }
                p = strtok(NULL, ",");
        }
        free(caps);

}

#endif

void uwsgi_apply_config_pass(char symbol, char*(*hook)(char *) ) {

	int i, j;

	for (i = 0; i < uwsgi.exported_opts_cnt; i++) {
                int has_symbol = 0;
                char *magic_key = NULL;
                char *magic_val = NULL;
                if (uwsgi.exported_opts[i]->value && !uwsgi.exported_opts[i]->configured) {
                        for (j = 0; j < (int) strlen(uwsgi.exported_opts[i]->value); j++) {
                                if (uwsgi.exported_opts[i]->value[j] == symbol) {
                                        has_symbol = 1;
                                }
                                else if (uwsgi.exported_opts[i]->value[j] == '(' && has_symbol == 1) {
                                        has_symbol = 2;
                                        magic_key = uwsgi.exported_opts[i]->value + j + 1;
                                }
                                else if (has_symbol > 1) {
                                        if (uwsgi.exported_opts[i]->value[j] == ')') {
                                                if (has_symbol <= 2) {
                                                        magic_key = NULL;
                                                        has_symbol = 0;
                                                        continue;
                                                }
#ifdef UWSGI_DEBUG
                                                uwsgi_log("need to interpret the %.*s tag\n", has_symbol - 2, magic_key);
#endif
                                                char *tmp_magic_key = uwsgi_concat2n(magic_key, has_symbol - 2, "", 0);
                                                magic_val = hook(tmp_magic_key);
                                                free(tmp_magic_key);
                                                if (!magic_val) {
                                                        magic_key = NULL;
                                                        has_symbol = 0;
                                                        continue;
                                                }
                                                uwsgi.exported_opts[i]->value = uwsgi_concat4n(uwsgi.exported_opts[i]->value, (magic_key - 2) - uwsgi.exported_opts[i]->value, magic_val, strlen(magic_val), magic_key + (has_symbol - 1), strlen(magic_key + (has_symbol - 1)), "", 0);
#ifdef UWSGI_DEBUG
                                                uwsgi_log("computed new value = %s\n", uwsgi.exported_opts[i]->value);
#endif
                                                magic_key = NULL;
                                                has_symbol = 0;
                                                j = 0;
                                        }
                                        else {
                                                has_symbol++;
                                        }
                                }
                                else {
                                        has_symbol = 0;
                                }
                        }
                }
        }

}

void uwsgi_set_processname(char *name) {

#if defined(__linux__) || defined(__sun__)
	size_t amount = 0;

	// prepare for strncat
	*uwsgi.orig_argv[0] = 0;

	if (uwsgi.procname_prefix) {
		amount += strlen(uwsgi.procname_prefix);
		if ((int)amount > uwsgi.max_procname-1) return;
		strncat(uwsgi.orig_argv[0], uwsgi.procname_prefix, uwsgi.max_procname-(amount+1));
	}
	
	amount += strlen(name);
	if ((int)amount > uwsgi.max_procname-1) return;
	strncat(uwsgi.orig_argv[0], name, (uwsgi.max_procname-amount+1));

	if (uwsgi.procname_append) {
		amount += strlen(uwsgi.procname_append);
		if ((int)amount > uwsgi.max_procname-1) return;
		strncat(uwsgi.orig_argv[0], uwsgi.procname_append, uwsgi.max_procname-(amount+1));
	}

	memset(uwsgi.orig_argv[0]+amount+1, ' ', uwsgi.max_procname-(amount-1));
#elif defined(__FreeBSD__)
	if (uwsgi.procname_prefix) {
		if (!uwsgi.procname_append) {
			setproctitle("-%s%s", uwsgi.procname_prefix, name);
		}
		else {
			setproctitle("-%s%s%s", uwsgi.procname_prefix, name, uwsgi.procname_append);
		}
	}
	else if (uwsgi.procname_append) {
		if (!uwsgi.procname_prefix) {
			setproctitle("-%s%s", name, uwsgi.procname_append);
		}
		else {
			setproctitle("-%s%s%s", uwsgi.procname_prefix, name, uwsgi.procname_append);
		}
	}
	else {
		setproctitle("-%s", name);
	}
#endif
}

// this is a wrapper for fork restoring original argv
pid_t uwsgi_fork(char *name) {


	pid_t pid = fork();
	if (pid == 0) {
#if defined(__linux__) || defined(__sun__)
		int i;
		for(i=0;i<uwsgi.argc;i++) {
			strcpy(uwsgi.orig_argv[i],uwsgi.argv[i]);
		}
#endif
		
		if (uwsgi.auto_procname && name) {
			if (uwsgi.procname) {
				uwsgi_set_processname(uwsgi.procname);
			}
			else {
				uwsgi_set_processname(name);
			}
		}
	}

	return pid;
}

void escape_shell_arg(char *src, size_t len, char *dst) {

	size_t i;
	char *ptr = dst;

	for(i=0;i<len;i++) {
		if (strchr("&;`'\"|*?~<>^()[]{}$\\\n", src[i])) {
			*ptr++= '\\';
		}
		*ptr++= src[i];
	}
	
	*ptr++= 0;
}

void http_url_decode(char *buf, uint16_t *len, char *dst) {

	uint16_t i;
	int percent = 0;
	char value[2];
	size_t new_len = 0;

	char *ptr = dst;

	value[0] = '0';
	value[1] = '0';

	for(i=0;i<*len;i++) {
		if (buf[i] == '%') {
			if (percent == 0) {
				percent = 1;
			}
			else {
				*ptr++= '%';
				new_len++;
				percent = 0;
			}
		}
		else {
			if (percent == 1) {
                                value[0] = buf[i];
                                percent = 2;
                        }
                        else if (percent == 2) {
                                value[1] = buf[i];
                                *ptr++= hex2num(value);
                                percent = 0;
                                new_len++;
                        }
			else {
				*ptr++= buf[i];
				new_len++;
			}
		}
	}

	*len = new_len;

}

char *uwsgi_get_var(struct wsgi_request *wsgi_req, char *key, uint16_t keylen, uint16_t *len) {

	int i;

	for (i = 0; i < wsgi_req->var_cnt; i += 2) {
                if (!uwsgi_strncmp(key, keylen, wsgi_req->hvec[i].iov_base, wsgi_req->hvec[i].iov_len)) {
			*len = wsgi_req->hvec[i + 1].iov_len;
                        return wsgi_req->hvec[i + 1].iov_base;
                }
        }

	return NULL;
}

void uwsgi_add_app(int id, uint8_t modifier1, char *mountpoint, int mountpoint_len) {

	struct uwsgi_app *wi = &uwsgi_apps[id];
        memset(wi, 0, sizeof(struct uwsgi_app));

        wi->modifier1 = modifier1;
        wi->mountpoint = mountpoint;
        wi->mountpoint_len = mountpoint_len;
        
        uwsgi_apps_cnt++;
        // check if we need to emulate fork() COW
        int i;
        if (uwsgi.mywid == 0) {
                for(i=1;i<=uwsgi.numproc;i++) {
                        memcpy(&uwsgi.workers[i].apps[id], &uwsgi.workers[0].apps[id], sizeof(struct uwsgi_app));
                        uwsgi.workers[i].apps_cnt = uwsgi_apps_cnt;
                }
        }
}

