#include "ws.h"
#include <pthread.h>
#define SHA1_HASH_SIZE 20
struct globals_s globals;

#ifndef WSS_STANDALONE

void init_ssl(void) 
{
	SSL_library_init();
}
void deinit_ssl(void) 
{
	return;
}

#else
static unsigned long pthreads_thread_id(void);
static void pthreads_locking_callback(int mode, int type, const char *file, int line);

static pthread_mutex_t *lock_cs;
static long *lock_count;



static void thread_setup(void)
{
	int i;

	lock_cs = OPENSSL_malloc(CRYPTO_num_locks() * sizeof(pthread_mutex_t));
	lock_count = OPENSSL_malloc(CRYPTO_num_locks() * sizeof(long));

	for (i = 0; i < CRYPTO_num_locks(); i++) {
		lock_count[i] = 0;
		pthread_mutex_init(&(lock_cs[i]), NULL);
	}

	CRYPTO_set_id_callback(pthreads_thread_id);
	CRYPTO_set_locking_callback(pthreads_locking_callback);
}

static void thread_cleanup(void)
{
	int i;

	CRYPTO_set_locking_callback(NULL);

	for (i=0; i<CRYPTO_num_locks(); i++) {
		pthread_mutex_destroy(&(lock_cs[i]));
	}
	OPENSSL_free(lock_cs);
	OPENSSL_free(lock_count);

}

static void pthreads_locking_callback(int mode, int type, const char *file, int line)
{

	if (mode & CRYPTO_LOCK) {
		pthread_mutex_lock(&(lock_cs[type]));
		lock_count[type]++;
	} else {
		pthread_mutex_unlock(&(lock_cs[type]));
	}
}



static unsigned long pthreads_thread_id(void)
{
	return (unsigned long) pthread_self();
}


void init_ssl(void) {
	SSL_library_init();


	OpenSSL_add_all_algorithms();   /* load & register cryptos */
	SSL_load_error_strings();     /* load all error messages */
	globals.ssl_method = TLSv1_server_method();   /* create server instance */
	globals.ssl_ctx = SSL_CTX_new(globals.ssl_method);         /* create context */
	assert(globals.ssl_ctx);
	
	/* set the local certificate from CertFile */
	SSL_CTX_use_certificate_file(globals.ssl_ctx, globals.cert, SSL_FILETYPE_PEM);
	/* set the private key from KeyFile */
	SSL_CTX_use_PrivateKey_file(globals.ssl_ctx, globals.key, SSL_FILETYPE_PEM);
	/* verify private key */
	if ( !SSL_CTX_check_private_key(globals.ssl_ctx) ) {
		abort();
    }

	SSL_CTX_set_cipher_list(globals.ssl_ctx, "HIGH:!DSS:!aNULL@STRENGTH");

	thread_setup();
}


void deinit_ssl(void) {
	thread_cleanup();
}

#endif

static const char c64[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";


static int cheezy_get_var(char *data, char *name, char *buf, size_t buflen)
{
  char *p=data;

  /* the old way didnt make sure that variable values were used for the name hunt
   * and didnt ensure that only a full match of the variable name was used
   */

  do {
    if(!strncmp(p,name,strlen(name)) && *(p+strlen(name))==':') break;
  } while((p = (strstr(p,"\n")+1))!=(char *)1);


  if (p != (char *)1 && *p!='\0') {
    char *v, *e;

    if ((v = strchr(p, ':'))) {
      v++;
      while(v && *v == ' ') {
	v++;
      }
      if (v)  {
	if (!(e = strchr(v, '\r'))) {
	  e = strchr(v, '\n');
	}
      }
			
      if (v && e) {
	int cplen;
	int len = e - v;
	
	if (len > buflen - 1) {
	  cplen = buflen -1;
	} else {
	  cplen = len;
	}
	
	strncpy(buf, v, cplen);
	*(buf+cplen) = '\0';
	return 1;
      }
      
    }
  }
  return 0;
}

static int b64encode(unsigned char *in, size_t ilen, unsigned char *out, size_t olen) 
{
	int y=0,bytes=0;
	size_t x=0;
	unsigned int b=0,l=0;

	for(x=0;x<ilen;x++) {
		b = (b<<8) + in[x];
		l += 8;
		while (l >= 6) {
			out[bytes++] = c64[(b>>(l-=6))%64];
			if(++y!=72) {
				continue;
			}
			//out[bytes++] = '\n';
			y=0;
		}
	}

	if (l > 0) {
		out[bytes++] = c64[((b%16)<<(6-l))%64];
	}
	if (l != 0) while (l < 6) {
		out[bytes++] = '=', l += 2;
	}

	return 0;
}

#ifdef NO_OPENSSL
static void sha1_digest(char *digest, unsigned char *in)
{
	SHA1Context sha;
	char *p;
	int x;


	SHA1Init(&sha);
	SHA1Update(&sha, in, strlen(in));
	SHA1Final(&sha, digest);
}
#else 

static void sha1_digest(unsigned char *digest, char *in)
{
	SHA_CTX sha;

	SHA1_Init(&sha);
	SHA1_Update(&sha, in, strlen(in));
	SHA1_Final(digest, &sha);

}

#endif

int ws_handshake(wsh_t *wsh)
{
	char key[256] = "";
	char version[5] = "";
	char proto[256] = "";
	char uri[256] = "";
	char input[256] = "";
	unsigned char output[SHA1_HASH_SIZE] = "";
	char b64[256] = "";
	char respond[512] = "";
	ssize_t bytes;
	char *p, *e;

	if (wsh->sock == ws_sock_invalid) {
		return -3;
	}

	while((bytes = ws_raw_read(wsh, wsh->buffer + wsh->datalen, wsh->buflen - wsh->datalen)) > 0) {
		wsh->datalen += bytes;
		if (strstr(wsh->buffer, "\r\n\r\n") || strstr(wsh->buffer, "\n\n")) {
			break;
		}
	}

	*(wsh->buffer+bytes) = '\0';
	
	if (strncasecmp(wsh->buffer, "GET ", 4)) {
		goto err;
	}
	
	p = wsh->buffer + 4;
	
	if (!(e = strchr(p, ' '))) {
		goto err;
	}
	
	strncpy(uri, p, e-p);
	
	cheezy_get_var(wsh->buffer, "Sec-WebSocket-Key", key, sizeof(key));
	cheezy_get_var(wsh->buffer, "Sec-WebSocket-Version", version, sizeof(version));
	cheezy_get_var(wsh->buffer, "Sec-WebSocket-Protocol", proto, sizeof(proto));
	
	if (!*key) {
		goto err;
	}
		
	snprintf(input, sizeof(input), "%s%s", key, WEBSOCKET_GUID);
	sha1_digest(output, input);
	b64encode((unsigned char *)output, SHA1_HASH_SIZE, (unsigned char *)b64, sizeof(b64));

	snprintf(respond, sizeof(respond), 
			 "HTTP/1.1 101 Switching Protocols\r\n"
			 "Upgrade: websocket\r\n"
			 "Connection: Upgrade\r\n"
			 "Sec-WebSocket-Accept: %s\r\n"
			 "Sec-WebSocket-Protocol: %s\r\n\r\n",
			 b64,
			 proto);


	ws_raw_write(wsh, respond, strlen(respond));
	wsh->handshake = 1;
	
	return 0;

 err:

	snprintf(respond, sizeof(respond), "HTTP/1.1 400 Bad Request\r\n"
			 "Sec-WebSocket-Version: 13\r\n\r\n");

	//printf("ERR:\n%s\n", respond);


	ws_raw_write(wsh, respond, strlen(respond));

	ws_close(wsh, WS_NONE);

	return -1;

}

ssize_t ws_raw_read(wsh_t *wsh, void *data, size_t bytes)
{
	ssize_t r;
	int x = 0;

	if (wsh->ssl) {
		do {
			r = SSL_read(wsh->ssl, data, bytes);
			if (x++) usleep(10000);
		} while (r == -1 && SSL_get_error(wsh->ssl, r) == SSL_ERROR_WANT_READ && x < 100);

		return r;
	}

	do {
		r = recv(wsh->sock, data, bytes, 0);
		if (x++) usleep(10000);
	} while (r == -1 && (errno == EAGAIN || errno == EINTR) && x < 100);

	//if (r<0) {
	//	printf("READ FAIL: %s\n", strerror(errno));
	//}

	return r;
}

ssize_t ws_raw_write(wsh_t *wsh, void *data, size_t bytes)
{
	size_t r;

	if (wsh->ssl) {
		do {
			r = SSL_write(wsh->ssl, data, bytes);
		} while (r == -1 && SSL_get_error(wsh->ssl, r) == SSL_ERROR_WANT_WRITE);

		return r;
	}

	do {
		r = send(wsh->sock, data, bytes, 0);
	} while (r == -1 && (errno == EAGAIN || errno == EINTR));

	//if (r<0) {
		//printf("wRITE FAIL: %s\n", strerror(errno));
	//}

	return r;
}

int ws_init(wsh_t *wsh, ws_socket_t sock, size_t buflen, SSL_CTX *ssl_ctx, int close_sock)
{
	memset(wsh, 0, sizeof(*wsh));
	wsh->sock = sock;

	if (!ssl_ctx) {
		ssl_ctx = globals.ssl_ctx;
	}

	if (close_sock) {
		wsh->close_sock = 1;
	}

	if (buflen > MAXLEN) {
		buflen = MAXLEN;
	}

	wsh->buflen = buflen;
	wsh->secure = ssl_ctx ? 1 : 0;

	if (!wsh->buffer) {
		wsh->buffer = malloc(wsh->buflen);
		assert(wsh->buffer);
	}

	if (wsh->secure) {
		int code;

		wsh->ssl = SSL_new(ssl_ctx);
		assert(wsh->ssl);

		SSL_set_fd(wsh->ssl, wsh->sock);

		do {
			code = SSL_accept(wsh->ssl);
		} while (code == -1 && SSL_get_error(wsh->ssl, code) == SSL_ERROR_WANT_READ);

	}

	while (!wsh->down && !wsh->handshake) {
		ws_handshake(wsh);
	}

	if (wsh->down) {
		return -1;
	}

	return 0;
}

ssize_t ws_close(wsh_t *wsh, int16_t reason) 
{
	
	if (wsh->down) {
		return -1;
	}
	wsh->down++;

	if (reason) {
		uint16_t *u16;
		uint8_t fr[4] = {WSOC_CLOSE | 0x80, 2, 0};

		u16 = (uint16_t *) &fr[2];
		*u16 = htons((int16_t)reason);
		ws_raw_write(wsh, fr, 4);
	}


	if (wsh->ssl) {
		int code;
		do {
			code = SSL_shutdown(wsh->ssl);
		} while (code == -1 && SSL_get_error(wsh->ssl, code) == SSL_ERROR_WANT_READ);

		SSL_free(wsh->ssl);
		wsh->ssl = NULL;
	}

	if (wsh->close_sock) {
		close(wsh->sock);
	}

	wsh->sock = ws_sock_invalid;

	if (wsh->buffer) {
		free(wsh->buffer);
		wsh->buffer = NULL;
	}

	if (wsh->wbuffer) {
		free(wsh->wbuffer);
		wsh->wbuffer = NULL;
	}


	return reason * -1;
	
}

ssize_t ws_read_frame(wsh_t *wsh, ws_opcode_t *oc, uint8_t **data)
{
	
	ssize_t need = 2;
	char *maskp;

 again:
	need = 2;
	maskp = NULL;
	*data = NULL;

	if (wsh->down) {
		return -1;
	}

	if (!wsh->handshake) {
		return ws_close(wsh, WS_PROTO_ERR);
	}

	if ((wsh->datalen = ws_raw_read(wsh, wsh->buffer, 14)) < need) {
		if ((wsh->datalen += ws_raw_read(wsh, wsh->buffer + wsh->datalen, 14 - wsh->datalen)) < need) {
			/* too small - protocol err */
			return ws_close(wsh, WS_PROTO_ERR);
		}
	}

	*oc = *wsh->buffer & 0xf;

	switch(*oc) {
	case WSOC_CLOSE:
		{
			wsh->plen = wsh->buffer[1] & 0x7f;
			*data = (uint8_t *) &wsh->buffer[2];
			return ws_close(wsh, 1000);
		}
		break;
	case WSOC_CONTINUATION:
	case WSOC_TEXT:
	case WSOC_BINARY:
	case WSOC_PING:
	case WSOC_PONG:
		{
			//int fin = (wsh->buffer[0] >> 7) & 1;
			int mask = (wsh->buffer[1] >> 7) & 1;

			if (mask) {
				need += 4;
				
				if (need > wsh->datalen) {
					/* too small - protocol err */
					*oc = WSOC_CLOSE;
					return ws_close(wsh, WS_PROTO_ERR);
				}
			}

			wsh->plen = wsh->buffer[1] & 0x7f;
			wsh->payload = &wsh->buffer[2];
			
			if (wsh->plen == 127) {
				uint64_t *u64;

				need += 8;

				if (need > wsh->datalen) {
					/* too small - protocol err */
					*oc = WSOC_CLOSE;
					return ws_close(wsh, WS_PROTO_ERR);
				}

				u64 = (uint64_t *) wsh->payload;
				wsh->payload += 8;

				wsh->plen = ntohl(*u64);

			} else if (wsh->plen == 126) {
				uint16_t *u16;

				need += 2;

				if (need > wsh->datalen) {
					/* too small - protocol err */
					*oc = WSOC_CLOSE;
					return ws_close(wsh, WS_PROTO_ERR);
				}

				u16 = (uint16_t *) wsh->payload;
				wsh->payload += 2;
				wsh->plen = ntohs(*u16);
			}

			if (mask) {
				maskp = (char *)wsh->payload;
				wsh->payload += 4;
			}

			need = (wsh->plen - (wsh->datalen - need));

			if ((need + wsh->datalen) > wsh->buflen) {
				/* too big - Ain't nobody got time fo' dat */
				*oc = WSOC_CLOSE;
				return ws_close(wsh, WS_DATA_TOO_BIG);				
			}

			wsh->rplen = wsh->plen - need;

			while(need) {
				ssize_t r = ws_raw_read(wsh, wsh->payload + wsh->rplen, need);

				if (r < 1) {
					/* invalid read - protocol err .. */
					*oc = WSOC_CLOSE;
					return ws_close(wsh, WS_PROTO_ERR);
				}

				wsh->datalen += r;
				wsh->rplen += r;
				need -= r;
			}
			
			if (mask && maskp) {
				uint32_t i;

				for (i = 0; i < wsh->datalen; i++) {
					wsh->payload[i] ^= maskp[i % 4];
				}
			}
			

			if (*oc == WSOC_PING) {
				ws_write_frame(wsh, WSOC_PONG, wsh->payload, wsh->rplen);
				goto again;
			}
			

			*(wsh->payload+wsh->rplen) = '\0';
			*data = (uint8_t *)wsh->payload;

			//printf("READ[%ld][%d]-----------------------------:\n[%s]\n-------------------------------\n", wsh->rplen, *oc, (char *)*data);


			return wsh->rplen;
		}
		break;
	default:
		{
			/* invalid op code - protocol err .. */
			*oc = WSOC_CLOSE;
			return ws_close(wsh, WS_PROTO_ERR);
		}
		break;
	}
}

ssize_t ws_feed_buf(wsh_t *wsh, void *data, size_t bytes)
{

	if (bytes + wsh->wdatalen > wsh->buflen) {
		return -1;
	}


	if (!wsh->wbuffer) {
		wsh->wbuffer = malloc(wsh->buflen);
		assert(wsh->wbuffer);
	}
	

	memcpy(wsh->wbuffer + wsh->wdatalen, data, bytes);
	
	wsh->wdatalen += bytes;

	return bytes;
}

ssize_t ws_send_buf(wsh_t *wsh, ws_opcode_t oc)
{
	ssize_t r = 0;

	if (!wsh->wdatalen) {
		return -1;
	}
	
	r = ws_write_frame(wsh, oc, wsh->wbuffer, wsh->wdatalen);
	
	wsh->wdatalen = 0;

	return r;
}


ssize_t ws_write_frame(wsh_t *wsh, ws_opcode_t oc, void *data, size_t bytes)
{
	uint8_t hdr[14] = { 0 };
	size_t hlen = 2;

	if (wsh->down) {
		return -1;
	}

	//printf("WRITE[%ld]-----------------------------:\n[%s]\n-----------------------------------\n", bytes, (char *) data);

	hdr[0] = oc | 0x80;

	if (bytes < 126) {
		hdr[1] = bytes;
	} else if (bytes < 0x10000) {
		uint16_t *u16;

		hdr[1] = 126;
		hlen += 2;

		u16 = (uint16_t *) &hdr[2];
		*u16 = htons((uint16_t) bytes);

	} else {
		uint64_t *u64;

		hdr[1] = 127;
		hlen += 8;
		
		u64 = (uint64_t *) &hdr[2];
		*u64 = htonl(bytes);
	}

	if (ws_raw_write(wsh, (void *) &hdr[0], hlen) != hlen) {
		return -1;
	}

	if (ws_raw_write(wsh, data, bytes) != bytes) {
		return -2;
	}
	
	return bytes;
}


