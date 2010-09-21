/*
 * $File: _snc.c
 * $Author: Jiakai <jia.kai66@gmail.com>
 * $Date: Tue Sep 21 18:39:26 2010 +0800
 */
/*
This file is part of orzoj

Copyright (C) <2010>  Jiakai <jia.kai66@gmail.com>

Orzoj is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Orzoj is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with orzoj.  If not, see <http://www.gnu.org/licenses/>.
*/

// Snc: secure network connection

// I need to re-implement socket module because Python does not
// ensure the version passed to WSAStartup on Windows
//
// SSL module needs re-implementing because SSL socket wrapper in Python
// will hang if remove closes while reding (see Issue 1 on Google Code)

#include <Python.h>

#ifdef WITH_THREAD
// serves as a flag to see whether we've initialized the SSL thread support
// 0 means no, greater than 0 means yes 

static unsigned int ssl_locks_count = 0;

#include <pythread.h>
#define SNC_BEGIN_ALLOW_THREADS { \
            PyThreadState *_save = NULL;  \
            if (ssl_locks_count > 0) {_save = PyEval_SaveThread();}

#define SNC_END_ALLOW_THREADS if (ssl_locks_count > 0) {PyEval_RestoreThread(_save);} \
         }


#else // no WITH_THREAD

#define SNC_BEGIN_ALLOW_THREADS 
#define SNC_END_ALLOW_THREADS

#endif // WITH_THREAD


#if defined(_linux_) || defined(__linux__) || defined(linux) || defined(__unix__)
#	define PLATFORM_UNIX 1
#endif

#if defined(_WIN32) || defined(_WIN64)
#	define PLATFORM_WINDOWS 1
#endif

#include <string.h>
#include <math.h>
#include <stdlib.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#ifdef PLATFORM_UNIX
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
typedef int Socket_t;

#define CLOSE_SOCKET close
#define SOCKFD_ERROR(_s_) (_s_ < 0)

#endif // PLATFORM_UNIX


#ifdef PLATFORM_WINDOWS
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "wsock32.lib")
#pragma comment(lib, "libeay32.lib")
#pragma comment(lib, "ssleay32.lib")

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

typedef SOCKET Socket_t;

#define CLOSE_SOCKET closesocket
#define SOCKFD_ERROR(_s_) (_s_ == INVALID_SOCKET)

#endif // PLATFORM_WINDOWS

typedef struct {
	PyObject_HEAD
	Socket_t sockfd;
} Snc_obj_socket;

typedef struct {
	PyObject_HEAD
	Snc_obj_socket *socket;
	SSL_CTX *ssl_ctx;
	SSL *ssl;
	double timeout_default;
} Snc_obj_snc;


static PyObject *snc_error_obj;


// if @host is NULL, it will be regarded as server and socket is automatically binded,
// otherwise it will be connected to the host
//
// @use_ipv6 is available only when @host is NULL
static Snc_obj_socket* socket_new(const char *host, int port, int use_ipv6);

// parse args and call new_socket
// args: (host:str, port:int, use_ipv6:int)
// host = None for server mode
// module function
static PyObject* socket_new_ex(PyObject *self, PyObject *args);

// return a tuple (conn, addr), where conn is an Snc_obj_socket instance and addr is
// a string indicating the peer's address
// should be called only in server mode
// object method
static PyObject* socket_accept(Snc_obj_socket *self, void*);

static PyObject* socket_close(Snc_obj_socket *self, void*);
static void socket_close_do(Snc_obj_socket *self);

static void socket_dealloc(Snc_obj_socket *self);

// set SO_RCVTIMEO and SO_SNDTIMEO for @sockfd to @val seconds
// return whether succeeds, and set snc_error_obj if necessary
static int set_timeout(Socket_t sockfd, double val);

static Snc_obj_snc* snc_new(Snc_obj_socket *sock, int is_server, double timeout_default,
		const char *fname_cert, const char *fname_priv_key, const char *fname_ca);

// args: is the same as snc_new
// module function
static PyObject* snc_new_ex(PyObject *self, PyObject *args);

// read exactly @len bytes
// args: (len:int, timeout:float)
// object method
static PyObject* snc_read(Snc_obj_snc *self, PyObject *args);

// write exactly @len bytes
// args: (data:str, timeout:float)
// object method
static PyObject* snc_write(Snc_obj_snc *self, PyObject *args);

// object method
static PyObject* snc_shutdown(Snc_obj_snc *self, void *);

static void snc_dealloc(Snc_obj_snc *self);

static int os_init(void);
static int ssl_init(void);


// Convenience function to raise an error according to errno
// and return a NULL pointer from a function.
static PyObject* socket_set_error(void);
// @func: SSL function name 
static PyObject* snc_set_error(const char *func, int err_code);

static PyMethodDef
	methods_module[] = 
	{
		{"socket", (PyCFunction)socket_new_ex, METH_VARARGS, NULL},
		{"snc", (PyCFunction)snc_new_ex, METH_VARARGS, NULL},
		{NULL, NULL, 0, NULL}
	},
	methods_socket[] = 
	{
		{"accept", (PyCFunction)socket_accept, METH_NOARGS, NULL},
		{"close", (PyCFunction)socket_close, METH_NOARGS, NULL},
		{NULL, NULL, 0, NULL}
	},
	methods_snc[] = 
	{
		{"read", (PyCFunction)snc_read, METH_VARARGS, NULL},
		{"write", (PyCFunction)snc_write, METH_VARARGS, NULL},
		{"shutdown", (PyCFunction)snc_shutdown, METH_NOARGS, NULL},
		{NULL, NULL, 0, NULL}
	};

static PyTypeObject
	type_socket = 
	{
		PyVarObject_HEAD_INIT(0, 0)
		"snc.socket",                               /*tp_name*/
		sizeof(Snc_obj_socket),                     /*tp_basicsize*/
	},
	type_snc = 
	{
		PyVarObject_HEAD_INIT(0, 0)
		"snc.snc",                                  /*tp_name*/
		sizeof(Snc_obj_snc),                        /*tp_basicsize*/
	};

static void set_type_attr(void)
{
	type_socket.tp_dealloc = (destructor)(socket_dealloc);
	type_socket.tp_getattro = PyObject_GenericGetAttr;
	type_socket.tp_methods = methods_socket;

	type_snc.tp_dealloc = (destructor)(snc_dealloc);
	type_snc.tp_getattro = PyObject_GenericGetAttr;
	type_snc.tp_methods = methods_snc;
}


Snc_obj_socket* socket_new(const char *host, int port, int use_ipv6)
{
	Snc_obj_socket *self = PyObject_New(Snc_obj_socket, &type_socket);
	if (!self)
		return NULL;
	self->sockfd = 0;
	Socket_t sockfd = 0;

	int getaddrinfo_ret = 0, ok, ret;
	struct addrinfo hints, *result, *ptr;
	char port_str[20];
	if (!host)
	{
		if (use_ipv6)
		{
			Py_BEGIN_ALLOW_THREADS
			sockfd = socket(AF_INET6, SOCK_STREAM, 0);
			Py_END_ALLOW_THREADS

			if (SOCKFD_ERROR(sockfd))
				goto FAIL;

			Py_BEGIN_ALLOW_THREADS
			struct sockaddr_in6 srv_addr;
			memset(&srv_addr, 0, sizeof(srv_addr));
			srv_addr.sin6_family = AF_INET6;
			srv_addr.sin6_addr = in6addr_any;
			srv_addr.sin6_port = htons(port);
			ret = bind(sockfd, (struct sockaddr*)&srv_addr, sizeof(srv_addr));
			Py_END_ALLOW_THREADS

			if (ret)
				goto FAIL;

			Py_BEGIN_ALLOW_THREADS
			ret = listen(sockfd, 5);
			Py_END_ALLOW_THREADS

			if (ret)
				goto FAIL;
		} else
		{
			Py_BEGIN_ALLOW_THREADS
			sockfd = socket(AF_INET, SOCK_STREAM, 0);
			Py_END_ALLOW_THREADS

			if (SOCKFD_ERROR(sockfd))
				goto FAIL;

			Py_BEGIN_ALLOW_THREADS
			struct sockaddr_in srv_addr;
			memset(&srv_addr, 0, sizeof(srv_addr));
			srv_addr.sin_family = AF_INET;
			srv_addr.sin_addr.s_addr = INADDR_ANY;
			srv_addr.sin_port = htons(port);
			ret = bind(sockfd, (struct sockaddr*)&srv_addr, sizeof(srv_addr));
			Py_END_ALLOW_THREADS

			if (ret)
				goto FAIL;

			Py_BEGIN_ALLOW_THREADS
			ret = listen(sockfd, 5);
			Py_END_ALLOW_THREADS

			if (ret)
				goto FAIL;
		}
	} else
	{
		Py_BEGIN_ALLOW_THREADS
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;

		sprintf(port_str, "%d", port);

		getaddrinfo_ret = getaddrinfo(host, port_str, &hints, &result);
		Py_END_ALLOW_THREADS

		if (getaddrinfo_ret)
			goto FAIL;
		
		ok = 0;
		for (ptr = result; ptr; ptr = ptr->ai_next)
		{
			Py_BEGIN_ALLOW_THREADS
			sockfd = socket(ptr->ai_family, ptr->ai_socktype,
					ptr->ai_protocol);
			Py_END_ALLOW_THREADS

			if (SOCKFD_ERROR(sockfd))
				continue;

			Py_BEGIN_ALLOW_THREADS
			ret = connect(sockfd, ptr->ai_addr, ptr->ai_addrlen);
			Py_END_ALLOW_THREADS

			if (ret)
			{
				Py_BEGIN_ALLOW_THREADS
				CLOSE_SOCKET(sockfd);
				Py_END_ALLOW_THREADS

				sockfd = 0;
				continue;
			}
			ok = 1;
			break;
		}
		freeaddrinfo(result);
		if (!ok)
			goto FAIL;
	}
	self->sockfd = sockfd;
	return self;

FAIL:
	if (!host)
		socket_set_error();
	else
	{
		if (getaddrinfo_ret)
		{
#ifdef PLATFORM_WINDOWS
			socket_set_error();
#else
			PyErr_Format(snc_error_obj, "getaddrinfo: %s", gai_strerror(getaddrinfo_ret));
#endif
		} else PyErr_SetString(snc_error_obj, "failed to connect");
	}
	if (!SOCKFD_ERROR(sockfd))
	{
		Py_BEGIN_ALLOW_THREADS
		CLOSE_SOCKET(sockfd);
		Py_END_ALLOW_THREADS
	}
	Py_XDECREF(self);
	return NULL;
}

PyObject* socket_new_ex(PyObject *self, PyObject *args)
{
	const char *host;
	int port, use_ipv6;
	if (!PyArg_ParseTuple(args, "zii:socket", &host, &port, &use_ipv6))
		return NULL;

	return (PyObject*) socket_new(host, port, use_ipv6);
}

PyObject* socket_accept(Snc_obj_socket *self, void *___)
{
	Socket_t newfd;
	struct sockaddr addr;
	socklen_t addrlen = sizeof(addr);
	memset(&addr, 0, addrlen);

	Py_BEGIN_ALLOW_THREADS
	newfd = accept(self->sockfd, &addr, &addrlen);
	Py_END_ALLOW_THREADS
	
	if (SOCKFD_ERROR(newfd))
	{
		socket_set_error();
		return NULL;
	}

	Snc_obj_socket *conn = PyObject_New(Snc_obj_socket, &type_socket);
	if (!conn)
	{
		Py_BEGIN_ALLOW_THREADS
		CLOSE_SOCKET(newfd);
		Py_END_ALLOW_THREADS

		return NULL;
	}

	conn->sockfd = newfd;

	char hostbuf[NI_MAXHOST], servbuf[NI_MAXSERV];
	int ret;
	Py_BEGIN_ALLOW_THREADS
	ret = getnameinfo(&addr, addrlen, hostbuf, NI_MAXHOST,
			servbuf, NI_MAXSERV, NI_NUMERICHOST | NI_NUMERICSERV);
	Py_END_ALLOW_THREADS

	if (ret)
	{
		strcpy(hostbuf, "unknown");
		strcpy(servbuf, "unknown");
	}

	PyObject *py_addr = PyString_FromFormat("%s:%s", hostbuf, servbuf),
			 *res = NULL;
	if (py_addr)
		res = PyTuple_Pack(2, conn, py_addr);

	Py_XDECREF(conn);
	Py_XDECREF(py_addr);
	return res;
}

void socket_close_do(Snc_obj_socket *self)
{
	Py_BEGIN_ALLOW_THREADS
	if (self->sockfd)
	{
		CLOSE_SOCKET(self->sockfd);
		self->sockfd = 0;
	}
	Py_END_ALLOW_THREADS
}

PyObject* socket_close(Snc_obj_socket *self, void *___)
{
	socket_close_do(self);
	Py_INCREF(Py_None);
	return Py_None;
}

void socket_dealloc(Snc_obj_socket *self)
{
	socket_close_do(self);
	PyObject_Del(self);
}

#ifdef PLATFORM_WINDOWS
int set_timeout(Socket_t sockfd, double val)
{
	int tv = floor(val * 1000);
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(tv)))
	{
		socket_set_error();
		return 0;
	}
	if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(tv)))
	{
		socket_set_error();
		return 0;
	}
	return 1;
}
#else
// non-windows

int set_timeout(Socket_t sockfd, double val)
{
	struct timeval tv;
	tv.tv_sec = floor(val);
	tv.tv_usec = (val - floor(val)) * 1e6;
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)))
	{
		socket_set_error();
		return 0;
	}
	if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)))
	{
		socket_set_error();
		return 0;
	}
	return 1;
}

#endif

Snc_obj_snc* snc_new(Snc_obj_socket *sock, int is_server, double timeout_default,
		const char *fname_cert, const char *fname_priv_key, const char *fname_ca)
{
	if (!set_timeout(sock->sockfd, timeout_default))
		return NULL;

	Snc_obj_snc *self = PyObject_New(Snc_obj_snc, &type_snc);
	if (!self)
		return NULL;

	self->ssl = NULL;
	self->ssl_ctx = NULL;
	self->socket = NULL;
	self->timeout_default = timeout_default;

	ERR_clear_error();

	SNC_BEGIN_ALLOW_THREADS
	if (is_server)
		self->ssl_ctx = SSL_CTX_new(TLSv1_server_method());
	else 
		self->ssl_ctx = SSL_CTX_new(TLSv1_client_method());
	SNC_END_ALLOW_THREADS

	if (self->ssl_ctx == NULL)
	{
		snc_set_error("SSL_CTX_new", ERR_get_error());
		goto FAIL;
	}

	SSL_CTX_set_mode(self->ssl_ctx, SSL_MODE_AUTO_RETRY);

	if (!SSL_CTX_load_verify_locations(self->ssl_ctx, fname_ca, NULL))
	{
		snc_set_error("SSL_CTX_load_verify_locations", ERR_get_error());
		goto FAIL;
	}

	if (!SSL_CTX_use_certificate_file(self->ssl_ctx, fname_cert, SSL_FILETYPE_PEM))
	{
		snc_set_error("SSL_CTX_use_certificate_file", ERR_get_error());
		goto FAIL;
	}

	if (!SSL_CTX_use_PrivateKey_file(self->ssl_ctx, fname_priv_key, SSL_FILETYPE_PEM))
	{
		snc_set_error("SSL_CTX_use_PrivateKey_file", ERR_get_error());
		goto FAIL;
	}

	if (!SSL_CTX_check_private_key(self->ssl_ctx))
	{
		snc_set_error("SSL_CTX_check_private_key", ERR_get_error());
		goto FAIL;
	}

	// use bilateral authentication
	SSL_CTX_set_verify(self->ssl_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);

	SNC_BEGIN_ALLOW_THREADS
	self->ssl = SSL_new(self->ssl_ctx);
	SNC_END_ALLOW_THREADS

	if (self->ssl == NULL)
	{
		snc_set_error("SSL_new", ERR_get_error());
		goto FAIL;
	}

	if (!SSL_set_fd(self->ssl, sock->sockfd))
	{
		snc_set_error("SSL_set_fd", ERR_get_error());
		goto FAIL;
	}

	int ret;
	SNC_BEGIN_ALLOW_THREADS
	if  (is_server)
		ret = SSL_accept(self->ssl);
	else ret = SSL_connect(self->ssl);
	SNC_END_ALLOW_THREADS

	if (ret != 1)
	{
		snc_set_error(is_server ? "SSL_accept" : "SSL_connect",
				SSL_get_error(self->ssl, ret));
		goto FAIL;
	}

	if ((ret = SSL_get_verify_result(self->ssl)) != X509_V_OK)
	{
		PyErr_Format(snc_error_obj, "Certificate doesn't verify. Verify result: %d.", ret);
		goto FAIL;
	}

	self->socket = sock;
	Py_INCREF(sock);
	return self;

FAIL:
	Py_XDECREF(self);
	return NULL;
}

PyObject* snc_new_ex(PyObject *self, PyObject *args)
{
	Snc_obj_socket *sock;
	int is_server;
	double timeout;
	const char *fname_cert, *fname_priv_key, *fname_ca;

	if (!PyArg_ParseTuple(args, "O!idsss:snc", &type_socket,
				&sock, &is_server, &timeout,
				&fname_cert, &fname_priv_key, &fname_ca))
		return NULL;

	return (PyObject*)snc_new(sock, is_server, timeout,
			fname_cert, fname_priv_key, fname_ca);
}

PyObject* snc_read(Snc_obj_snc *self, PyObject *args)
{
	if (self->socket == NULL)
	{
		PyErr_SetString(snc_error_obj, "attempt to read from a closed socket");
		return NULL;
	}
	int len;
	double timeout;
	if (!PyArg_ParseTuple(args, "id:read", &len, &timeout))
		return NULL;

	if (!set_timeout(self->socket->sockfd, timeout))
		return NULL;

	PyObject *buf = PyString_FromStringAndSize(NULL, len);
	if (!buf)
		return NULL;

	int tot = 0, ret;
	while (tot < len)
	{
		SNC_BEGIN_ALLOW_THREADS
		ret = SSL_read(self->ssl, PyString_AS_STRING(buf) + tot, len - tot);
		SNC_END_ALLOW_THREADS

		if (ret <= 0)
		{
			Py_XDECREF(buf);
			snc_set_error("SSL_read", SSL_get_error(self->ssl, ret));
			return NULL;
		}

		tot += ret;
	}

	return buf;
}

PyObject* snc_write(Snc_obj_snc *self, PyObject *args)
{
	if (self->socket == NULL)
	{
		PyErr_SetString(snc_error_obj, "attempt to write to a closed socket");
		return NULL;
	}
	Py_buffer buf;
	double timeout;
	if (!PyArg_ParseTuple(args, "s*d:write", &buf, &timeout))
		return NULL;

	int tot = 0, ret;
	while (tot < buf.len)
	{
		SNC_BEGIN_ALLOW_THREADS
		ret = SSL_write(self->ssl, buf.buf + tot, buf.len - tot);
		SNC_END_ALLOW_THREADS

		if (ret <= 0)
		{
			PyBuffer_Release(&buf);
			snc_set_error("SSL_write", SSL_get_error(self->ssl, ret));
			return NULL;
		}

		tot += ret;
	}

	PyBuffer_Release(&buf);

	Py_INCREF(Py_None);
	return Py_None;
}

void snc_shutdown_do(Snc_obj_snc *self)
{
	if (self->ssl)
	{
		if (self->socket)
		{
			set_timeout(self->socket->sockfd, self->timeout_default);
			SNC_BEGIN_ALLOW_THREADS
			int ret = SSL_shutdown(self->ssl);
			if (ret == 0) // according to the manual page, should call SSL_shutdown again
				ret = SSL_shutdown(self->ssl);
			// ignore errors here
			SNC_END_ALLOW_THREADS
		}
		SNC_BEGIN_ALLOW_THREADS
		SSL_free(self->ssl);
		SNC_END_ALLOW_THREADS
	}
	if (self->ssl_ctx)
	{
		SNC_BEGIN_ALLOW_THREADS
		SSL_CTX_free(self->ssl_ctx);
		SNC_END_ALLOW_THREADS
	}
	self->ssl = NULL;
	self->ssl_ctx = NULL;
}

PyObject* snc_shutdown(Snc_obj_snc *self, void *___)
{
	snc_shutdown_do(self);
	Py_INCREF(Py_None);
	return Py_None;
}

void snc_dealloc(Snc_obj_snc *self)
{
	snc_shutdown_do(self);
	Py_XDECREF(self->socket);
	PyObject_Del(self);
}

#ifdef PLATFORM_UNIX
int os_init(void)
{
	return 1;
}
#endif // PLATFORM_UNIX

#ifdef PLATFORM_WINDOWS
static void os_cleanup(void);

int os_init()
{
    WSADATA WSAData;
    int ret = WSAStartup(MAKEWORD(2, 2), &WSAData);
	if (ret)
	{
		PyErr_SetExcFromWindowsErr(snc_error_obj, ret);
		return 0;
	}
	return 1;
}

void os_cleanup()
{
	WSACleanup();
}
#endif // PLATFORM_WINDOWS

PyObject* socket_set_error(void)
{
#ifdef PLATFORM_WINDOWS
    int err_no = WSAGetLastError();
    /* PyErr_SetExcFromWindowsErr() invokes FormatMessage() which
       recognizes the error codes used by both GetLastError() and
       WSAGetLastError */
    if (err_no)
        return PyErr_SetExcFromWindowsErr(snc_error_obj, err_no);
	return NULL;
#else
    return PyErr_SetFromErrno(snc_error_obj);
#endif
}

PyObject* snc_set_error(const char *func, int err_code)
{
	return PyErr_Format(snc_error_obj, "SSL error [func %s] [errno: %s]: %s",
			func, strerror(errno), ERR_error_string(err_code, NULL));
}


#ifdef WITH_THREAD

// an implementation of OpenSSL threading operations in terms
// of the Python C thread library

static PyThread_type_lock *ssl_locks = NULL;

static unsigned long ssl_thread_id_function (void)
{
    return PyThread_get_thread_ident();
}

static void ssl_thread_locking_function (int mode, int n, const char *file, int line)
{
    /* this function is needed to perform locking on shared data
       structures. (Note that OpenSSL uses a number of global data
       structures that will be implicitly shared whenever multiple threads
       use OpenSSL.) Multi-threaded applications will crash at random if
       it is not set.

       locking_function() must be able to handle up to CRYPTO_num_locks()
       different mutex locks. It sets the n-th lock if mode & CRYPTO_LOCK, and
       releases it otherwise.

       file and line are the file number of the function setting the
       lock. They can be useful for debugging.
    */

    if ((ssl_locks == NULL) ||
        (n < 0) || ((unsigned)n >= ssl_locks_count))
        return;

    if (mode & CRYPTO_LOCK)
        PyThread_acquire_lock(ssl_locks[n], 1);
    else
        PyThread_release_lock(ssl_locks[n]);
}

static void free_ssl_locks_mem(void)
{
	if (ssl_locks)
	{
		free(ssl_locks);
		ssl_locks = NULL;
	}
}

static int setup_ssl_threads(void)
{
    if (ssl_locks == NULL)
	{
        ssl_locks_count = CRYPTO_num_locks();
        ssl_locks = (PyThread_type_lock *)
            malloc(sizeof(PyThread_type_lock) * ssl_locks_count);
        if (ssl_locks == NULL)
            return 0;
        memset(ssl_locks, 0, sizeof(PyThread_type_lock) * ssl_locks_count);
		unsigned int i;
		for (i = 0; i < ssl_locks_count; i ++)
		{
            ssl_locks[i] = PyThread_allocate_lock();
            if (ssl_locks[i] == NULL)
			{
                unsigned int j;
                for (j = 0; j < i; j++)
                    PyThread_free_lock(ssl_locks[j]);
                free(ssl_locks);
				PyErr_SetString(snc_error_obj, "failed to initialize SSL locks");
                return 0;
            }
        }
        CRYPTO_set_locking_callback(ssl_thread_locking_function);
        CRYPTO_set_id_callback(ssl_thread_id_function);
		Py_AtExit(free_ssl_locks_mem);
    }
    return 1;
}

#endif  // WITH_THREAD

int ssl_init()
{
	SSL_load_error_strings();
	SSL_library_init();
#ifdef WITH_THREAD
    /* note that this will start threading if not already started */
    if (!setup_ssl_threads())
        return 0;
#endif
	OpenSSL_add_all_algorithms();
	return 1;
}

#ifndef PyMODINIT_FUNC	/* declarations for DLL import/export */
#define PyMODINIT_FUNC extern void
#endif

// initialize the module
// may raise _snc.error on os-specified initialization failure

PyMODINIT_FUNC
init_snc(void)
{
	set_type_attr();

	static int os_init_done = 0;
	if (!os_init_done)
	{
		if (!os_init())
			return;
		os_init_done = 1;
	}

	if (PyType_Ready(&type_socket) < 0 ||
			PyType_Ready(&type_snc) < 0)
		return;

	PyObject *m = Py_InitModule3("_snc", methods_module, NULL);
	if (!m)
		return;

	if (!ssl_init())
		return;

	snc_error_obj = PyErr_NewException("_snc.error", NULL, NULL);
	if (!snc_error_obj)
		return;

	Py_INCREF(snc_error_obj);
	if (PyModule_AddObject(m, "error", snc_error_obj) < 0)
		return;
}
