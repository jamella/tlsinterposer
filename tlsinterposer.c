/*
    TLS Interposer: An OpenSSL library interposer to get
    software to use more secure TLS protocol variants.

    Copyright (C) 2013,2014 Marcel Waldvogel

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
    USA
*/
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <openssl/ssl.h>
#include <openssl/dh.h>
#include <dlfcn.h>
#include "ssl-version.h"

#ifdef __APPLE__
#define SSLCONST
#else
#define SSLCONST const
#endif

/* Environment variables used
 * ==========================
 * LD_PRELOAD		    used by ld.so, should be set to /full/path/to/tlsinterposer.so
 * TLS_INTERPOSER_CIPHERS   defaults to DEFAULT_CIPHERS below
 * TLS_INTERPOSER_OPTIONS   comma-separated list of options
 * - debug                  be verbose
 * - ssllib=                full name of libssl.so.X.Y.Z
 * - -comp                  disable compression
 * - +sslv2		    enable SSLv2 (strongly advised against)
 * - +sslv3		    enable SSLv3 (advised against)
 * - -tlsv1                 disable TLSv1, leaving TLSv1.1 and TLSv1.2, if supported
 * - -ecdhe                 disable forward secrecy
 * - -ccert         disable client certificate requests on the server side
 * TLS_INTERPOSER_NO_COMPRESSION (DEPRECATED, please use "-comp" above instead;
 *			    disables TLS compression when set to any value, even an empty value)
*/

// Qualys recommendation (I know the RC4 part could be simplified)
// - https://community.qualys.com/blogs/securitylabs/2013/08/05/configuring-apache-nginx-and-openssl-for-forward-secrecy
#define DEFAULT_CIPHERS "EECDH+ECDSA+AESGCM EECDH+aRSA+AESGCM EECDH+ECDSA+SHA384 EECDH+ECDSA+SHA256 EECDH+aRSA+SHA384 EECDH+aRSA+SHA256 EECDH+aRSA+RC4 EECDH EDH+aRSA RC4 !aNULL !eNULL !LOW !3DES !MD5 !EXP !PSK !SRP !DSS +RC4 RC4"

#define LOGPREFIX "libtlsinterposer.so: "
#define ERRORLOG(...) fprintf(stderr, LOGPREFIX __VA_ARGS__)
#ifdef NDEBUG
#define DEBUGLOG(...)
#else
#define DEBUGLOG(...) do {if (interposer_debug != 0) ERRORLOG(__VA_ARGS__);} while (0)
#endif

// interposer_debug, used by DEBUGLOG(), is only valid after the first call to interposer_dlsym()
#define ORIG_FUNC(func, rettype, args, fail)					\
	static rettype (*orig_ ## func) args;					\
	if (orig_ ## func == NULL) orig_ ## func = interposer_dlsym( #func );	\
	DEBUGLOG("Intercepted function %s\n", __func__);			\
	if (orig_ ## func == NULL) return fail;


static int   interposer_inited     = 0;
static int   interposer_opt_set    = (SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_SINGLE_DH_USE),
	         interposer_opt_clr    = 0,
	         interposer_debug      = 0,
             interposer_no_ccert   = 0;
static char *interposer_ssllib     = DEFAULT_SSLLIB,
	        *interposer_ciphers    = DEFAULT_CIPHERS;

static void interposer_parse_opts(void)
{
	char *opts, *optend;
	size_t optlen;
	char *ciphers;

	// This is only needed to improve efficiency, not correctness,
	// so an at-least-once semantic is used (increment at the end of the function)
	if (interposer_inited > 0) return;

	ciphers = getenv("TLS_INTERPOSER_CIPHERS");
	if (ciphers != NULL) interposer_ciphers = ciphers;

	opts = getenv("TLS_INTERPOSER_OPTIONS");
	if (opts == NULL) return;
	/* Non-destructive strtok() clone */
	while (*opts != '\0') {
		optend = index(opts, ',');
		if (optend == NULL) {
			optlen = strlen(opts); // Until EOS
			optend = opts + optlen;
		} else {
			optlen = optend - opts;
			optend++;
		}
		if (strncasecmp(opts, "debug", optlen) == 0) {
			interposer_debug++;
		} else if (strncasecmp(opts, "+sslv2", optlen) == 0) {
			interposer_opt_set &= ~SSL_OP_NO_SSLv2;
			interposer_opt_clr |= SSL_OP_NO_SSLv2;
		} else if (strncasecmp(opts, "+sslv3", optlen) == 0) {
			interposer_opt_set &= ~SSL_OP_NO_SSLv3;
			interposer_opt_clr |= SSL_OP_NO_SSLv3;
		} else if (strncasecmp(opts, "-tlsv1", optlen) == 0) {
			interposer_opt_clr |= SSL_OP_NO_TLSv1;
#ifdef SSL_OP_NO_COMPRESSION
		} else if (strncasecmp(opts, "-comp", optlen) == 0) {
			interposer_opt_set |= SSL_OP_NO_COMPRESSION;
#endif
		} else if (strncasecmp(opts, "-ecdhe", optlen) == 0) {
			interposer_opt_set &= ~SSL_OP_SINGLE_DH_USE;
			interposer_opt_clr |= SSL_OP_SINGLE_DH_USE;
		} else if (strncasecmp(opts, "-ccert", optlen) == 0) {
			interposer_no_ccert++;
		} else if (optlen > 7 && strncasecmp(opts, "libssl=", 7) == 0) {
			interposer_ssllib = opts+7;
		} else if (interposer_debug) {
			fprintf(stderr, "tlsinterposer.so: WARNING: Unknown option '%.*s' found in TLS_INTERPOSER_OPTIONS\n", (int)optlen, opts);
		}
		opts = optend;
	}
	// For backward compatibility, DEPRECATED
#ifdef SSL_OP_NO_COMPRESSION
	if (getenv("TLS_INTERPOSER_NO_COMPRESSION") != NULL)
		 interposer_opt_set |= SSL_OP_NO_COMPRESSION;
#endif
	interposer_inited++;
}

// Get a symbol from libssl
static void *interposer_dlsym(const char *name)
{
	if (interposer_inited == 0) interposer_parse_opts();
	void *addr = dlsym(RTLD_NEXT, name);
	if (addr == NULL) {
		// Try again with a more specific name
		// Needed for ejabberd
		void *file = dlopen(interposer_ssllib, RTLD_LAZY | RTLD_GLOBAL | RTLD_NOLOAD);
		if (file != NULL) {
			addr = dlsym(file, name);
			if (addr == NULL) {
				ERRORLOG("Cannot find symbol %s in libssl %s\n", name, interposer_ssllib);
			}
			// Can be dlclose()d here. As it wasn't loaded (RTLD_NOLOAD), it won't be unloaded here
			dlclose(file);
		} else {
			ERRORLOG("Cannot find mapped libssl %s looking for symbol %s\n", interposer_ssllib, name);
		}
	}
	return addr;
}

// Make ciphers in interposer_ciphers sticky
int SSL_CTX_set_cipher_list(SSL_CTX *ctx, const char *str)
{
	ORIG_FUNC(SSL_CTX_set_cipher_list, int, (SSL_CTX *ctx, const char *str), 0);
	return (*orig_SSL_CTX_set_cipher_list)(ctx, interposer_ciphers);
}
int SSL_set_cipher_list(SSL *ssl, const char *str)
{
	ORIG_FUNC(SSL_set_cipher_list, int, (SSL *ssl, const char *str), 0);
	return (*orig_SSL_set_cipher_list)(ssl, interposer_ciphers);
}

#if 0
// Make options in interposer_opt_{set,clr} sticky
long SSL_CTX_set_options(SSL_CTX *ctx, long options)
{
	ORIG_FUNC(SSL_CTX_set_options, long, (SSL_CTX *, long), 0);
	return (*orig_SSL_CTX_set_options)(ctx, (options | interposer_opt_set) & ~interposer_opt_clr);
}
long SSL_set_options(SSL *ssl, long options)
{
	ORIG_FUNC(SSL_set_options, long, (SSL *, long), 0);
	return (*orig_SSL_set_options)(ssl, (options | interposer_opt_set) & ~interposer_opt_clr);
}
long SSL_CTX_clear_options(SSL_CTX *ctx, long options)
{
	ORIG_FUNC(SSL_CTX_set_options, long, (SSL_CTX *, long), 0);
	return (*orig_SSL_CTX_set_options)(ctx, (options | interposer_opt_clr) & ~interposer_opt_set);
}
long SSL_clear_options(SSL *ssl, long options)
{
	ORIG_FUNC(SSL_set_options, long, (SSL *, long), 0);
	return (*orig_SSL_set_options)(ssl, (options | interposer_opt_clr) & ~interposer_opt_set);
}
#endif

// Based on Apache's bug #49559 by Kaspar Brand
// - https://issues.apache.org/bugzilla/show_bug.cgi?id=49559#c13
/*
 * Grab well-defined DH parameters from OpenSSL, see <openssl/bn.h>
 * (get_rfc*) for all available primes.
 */
#define make_get_dh(rfc,size,gen) \
static DH *get_dh##size(void) \
{ \
    DH *dh; \
    if (!(dh = DH_new())) { \
        return NULL; \
    } \
    dh->p = get_##rfc##_prime_##size(NULL); \
    BN_dec2bn(&dh->g, #gen); \
    if (!dh->p || !dh->g) { \
        DH_free(dh); \
        return NULL; \
    } \
    return dh; \
}

/*
 * Prepare DH parameters from 1024 to 4096 bits, in 1024-bit increments
 */
make_get_dh(rfc2409, 1024, 2)
make_get_dh(rfc3526, 2048, 2)
make_get_dh(rfc3526, 3072, 2)
make_get_dh(rfc3526, 4096, 2)

static DH *ssl_callback_TmpDH(SSL *ssl, int export, int keylen)
{
	EVP_PKEY *pkey = SSL_get_privatekey(ssl);
	int type = pkey ? EVP_PKEY_type(pkey->type) : EVP_PKEY_NONE;
	if ((type == EVP_PKEY_RSA) || (type == EVP_PKEY_DSA)) {
		keylen = EVP_PKEY_bits(pkey);
	}
	if (keylen >= 4096)
		return get_dh4096();
	else if (keylen >= 3072)
		return get_dh3072();
	else if (keylen >= 2048)
		return get_dh2048();
	else
		return get_dh1024();
}

void SSL_CTX_set_tmp_dh_callback(SSL_CTX *ctx, DH *(*tmp_dh_callback)(SSL *ssl, int is_export, int keylength))
{
	ORIG_FUNC(SSL_CTX_set_tmp_dh_callback, void, (SSL_CTX *, DH *(*)(SSL *, int, int)), /*void*/);
	(*orig_SSL_CTX_set_tmp_dh_callback)(ctx, ssl_callback_TmpDH);
}

void SSL_set_tmp_dh_callback(SSL *ssl, DH *(*tmp_dh_callback)(SSL *ssl, int is_export, int keylength))
{
	ORIG_FUNC(SSL_set_tmp_dh_callback, void, (SSL *, DH *(*)(SSL *, int, int)), /*void*/);
	(*orig_SSL_set_tmp_dh_callback)(ssl, ssl_callback_TmpDH);
}
// END Apache-bug derived code

/* ========================== Handling most options and ciphers */

SSL_CTX *SSL_CTX_new(SSLCONST SSL_METHOD *method)
{
	ORIG_FUNC(SSL_CTX_new, SSL_CTX *, (SSLCONST SSL_METHOD *), NULL);
	SSL_CTX *ctx = (*orig_SSL_CTX_new)(method);
	if (ctx != NULL) {
		SSL_CTX_set_options(ctx, interposer_opt_set);
		SSL_CTX_clear_options(ctx, interposer_opt_clr);
		SSL_CTX_set_cipher_list(ctx, interposer_ciphers);
		SSL_CTX_set_tmp_dh_callback(ctx, ssl_callback_TmpDH);
		// Based on code by Vincent Bernat
		// - http://vincent.bernat.im/en/blog/2011-ssl-perfect-forward-secrecy.html
		// - https://github.com/bumptech/stud/pull/61
#ifdef NID_X9_62_prime256v1
		if ((interposer_opt_clr & SSL_OP_SINGLE_DH_USE) != 0) {
			EC_KEY *ecdh;
			ecdh = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
			SSL_CTX_set_tmp_ecdh(ctx, ecdh);
			EC_KEY_free(ecdh);
			DEBUGLOG("ECDH Initialized with NIST P-256\n");
		}
#endif
	}
	DEBUGLOG("SSL_CTX_new returning %p\n", ctx);
	return ctx;
}

/* ========================== Handling "-ccert" */

/* When intercepting SSL_CTX_set_verify(), two modes of server operations possible:
 * - CTX does not have accept/connect information, it is set only after SSL_new():
 *   SSL_set_accept_state() and SSL_set_verify() need to be intercepted anyway
 * - CTX has accept/connect, SSL_new() "clones" from CTX:
 *   SSL_set_accept_state() and SSL_set_verify() may not be called,
 *   everything is handled in SSL_new()
 * So there is no need to intercept SSL_CTX_verify(), especially as
 * SSL_is_server() might be changed later, which would result in data loss
 */
void SSL_set_verify(SSL *s, int mode,
                    int (*verify_callback)(int, X509_STORE_CTX *))
{
	ORIG_FUNC(SSL_set_verify, void, (SSL *, int, int (*)(int, X509_STORE_CTX *)), /*void*/);
    if (interposer_no_ccert != 0 && SSL_is_server(s)) {
        // Disable requesting the client certificate
        mode = SSL_VERIFY_NONE;
    }
	(*orig_SSL_set_verify)(s, mode, verify_callback);
}

void SSL_set_accept_state(SSL *s)
{
	ORIG_FUNC(SSL_set_accept_state, void, (SSL *), /*void*/);
    (*orig_SSL_set_accept_state)(ssl);
    if (interposer_no_cert) {
        SSL_set_verify(SSL_VERIFY_NONE, NULL); // NULL indicates no change, phew!
    }
}

SSL *SSL_new(CTX *ctx)
{
    ORIG_FUNC(SSL_new, SSL *, (CTX *), NULL);
    SSL *s = (*orig_SSL_new)(ctx);
    if (s != NULL && interposer_no_ccert != 0 && SSL_is_server(s)) {
        SSL_set_verify(SSL_VERIFY_NONE, NULL); // NULL indicates no change, phew!
    }
    return s;
}
