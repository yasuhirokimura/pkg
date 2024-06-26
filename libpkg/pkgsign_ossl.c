/*-
 * Copyright (c) 2011-2013 Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2011-2012 Julien Laffaye <jlaffaye@FreeBSD.org>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/stat.h>
#include <sys/param.h>

#include <fcntl.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "pkg.h"
#include "private/event.h"
#include "private/pkg.h"
#include "private/pkgsign.h"

#if OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(LIBRESSL_VERSION_NUMBER)
/*
 * This matches the historical usage for pkg.  Older versions sign the hex
 * encoding of the SHA256 checksum.  If we ever deprecated RSA, this can go
 * away.
 */
static EVP_MD *md_pkg_sha1;

static EVP_MD *
EVP_md_pkg_sha1(void)
{

	if (md_pkg_sha1 != NULL)
		return (md_pkg_sha1);

	md_pkg_sha1 = EVP_MD_meth_dup(EVP_sha1());
	if (md_pkg_sha1 == NULL)
		return (NULL);

	EVP_MD_meth_set_result_size(md_pkg_sha1,
	    pkg_checksum_type_size(PKG_HASH_TYPE_SHA256_HEX));
	return (md_pkg_sha1);
}
#endif	/* OPENSSL_VERSION_NUMBER >= 0x10100000L */

struct ossl_sign_ctx {
	struct pkgsign_ctx sctx;
	EVP_PKEY *key;
};

/* Grab the ossl context from a pkgsign_ctx. */
#define	OSSL_CTX(c)	((struct ossl_sign_ctx *)(c))

static int
_load_private_key(struct ossl_sign_ctx *keyinfo)
{
	FILE *fp;

	if ((fp = fopen(keyinfo->sctx.path, "re")) == NULL)
		return (EPKG_FATAL);

	keyinfo->key = PEM_read_PrivateKey(fp, 0, keyinfo->sctx.pw_cb,
	    keyinfo->sctx.path);
	if (keyinfo->key == NULL) {
		fclose(fp);
		return (EPKG_FATAL);
	}

	fclose(fp);
	return (EPKG_OK);
}

static EVP_PKEY *
_load_public_key_buf(unsigned char *cert, int certlen)
{
	EVP_PKEY *pkey;
	BIO *bp;
	char errbuf[1024];

	bp = BIO_new_mem_buf((void *)cert, certlen);
	if (bp == NULL) {
		pkg_emit_error("error allocating public key bio: %s",
		    ERR_error_string(ERR_get_error(), errbuf));
		return (NULL);
	}

	pkey = PEM_read_bio_PUBKEY(bp, NULL, NULL, NULL);
	if (pkey == NULL) {
		pkg_emit_error("error reading public key: %s",
		    ERR_error_string(ERR_get_error(), errbuf));
		BIO_free(bp);
		return (NULL);
	}

	BIO_free(bp);
	return (pkey);
}

struct ossl_verify_cbdata {
	unsigned char *key;
	size_t keylen;
	unsigned char *sig;
	size_t siglen;
	bool verbose;
};

static int
ossl_verify_cert_cb(int fd, void *ud)
{
	struct ossl_verify_cbdata *cbdata = ud;
	char *sha256;
	char *hash;
	char errbuf[1024];
	EVP_PKEY *pkey = NULL;
	EVP_PKEY_CTX *ctx;
	int ret;

	sha256 = pkg_checksum_fd(fd, PKG_HASH_TYPE_SHA256_HEX);
	if (sha256 == NULL)
		return (EPKG_FATAL);

	hash = pkg_checksum_data(sha256, strlen(sha256),
	    PKG_HASH_TYPE_SHA256_RAW);
	free(sha256);

	pkey = _load_public_key_buf(cbdata->key, cbdata->keylen);
	if (pkey == NULL) {
		free(hash);
		return (EPKG_FATAL);
	}

	if (EVP_PKEY_id(pkey) != EVP_PKEY_RSA) {
		EVP_PKEY_free(pkey);
		free(hash);
		return (EPKG_FATAL);
	}

	ctx = EVP_PKEY_CTX_new(pkey, NULL);
	if (ctx == NULL) {
		EVP_PKEY_free(pkey);
		free(hash);
		return (EPKG_FATAL);
	}

	if (EVP_PKEY_verify_init(ctx) <= 0) {
		EVP_PKEY_CTX_free(ctx);
		EVP_PKEY_free(pkey);
		free(hash);
		return (EPKG_FATAL);
	}

	if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) <= 0) {
		EVP_PKEY_CTX_free(ctx);
		EVP_PKEY_free(pkey);
		free(hash);
		return (EPKG_FATAL);
	}

	if (EVP_PKEY_CTX_set_signature_md(ctx, EVP_sha256()) <= 0) {
		EVP_PKEY_CTX_free(ctx);
		EVP_PKEY_free(pkey);
		free(hash);
		return (EPKG_FATAL);
	}

	ret = EVP_PKEY_verify(ctx, cbdata->sig, cbdata->siglen, hash,
	    pkg_checksum_type_size(PKG_HASH_TYPE_SHA256_RAW));
	free(hash);
	if (ret <= 0 && cbdata->verbose) {
		if (ret < 0)
			pkg_emit_error("rsa verify failed: %s",
					ERR_error_string(ERR_get_error(), errbuf));
		else
			pkg_emit_error("rsa signature verification failure");
	}
	if (ret <= 0) {
		EVP_PKEY_CTX_free(ctx);
		EVP_PKEY_free(pkey);
		return (EPKG_FATAL);
	}

	EVP_PKEY_CTX_free(ctx);
	EVP_PKEY_free(pkey);

	return (EPKG_OK);
}

static int
ossl_verify_cert(const struct pkgsign_ctx *sctx __unused, unsigned char *key,
    size_t keylen, unsigned char *sig, size_t siglen, int fd)
{
	int ret;
	bool need_close = false;
	struct ossl_verify_cbdata cbdata;

	(void)lseek(fd, 0, SEEK_SET);

	cbdata.key = key;
	cbdata.keylen = keylen;
	cbdata.sig = sig;
	cbdata.siglen = siglen;
	cbdata.verbose = true;

	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();
	OpenSSL_add_all_ciphers();

	ret = pkg_emit_sandbox_call(ossl_verify_cert_cb, fd, &cbdata);
	if (need_close)
		close(fd);

	return (ret);
}

static int
ossl_verify_cb(int fd, void *ud)
{
	struct ossl_verify_cbdata *cbdata = ud;
	char *sha256;
	char errbuf[1024];
	EVP_PKEY *pkey = NULL;
#if OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(LIBRESSL_VERSION_NUMBER)
	EVP_PKEY_CTX *ctx;
#else
	RSA *rsa;
#endif
	int ret;

	sha256 = pkg_checksum_fd(fd, PKG_HASH_TYPE_SHA256_HEX);
	if (sha256 == NULL)
		return (EPKG_FATAL);

	pkey = _load_public_key_buf(cbdata->key, cbdata->keylen);
	if (pkey == NULL) {
		free(sha256);
		return (EPKG_FATAL);
	}

	if (EVP_PKEY_id(pkey) != EVP_PKEY_RSA) {
		EVP_PKEY_free(pkey);
		free(sha256);
		return (EPKG_FATAL);
	}

#if OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(LIBRESSL_VERSION_NUMBER)
	ctx = EVP_PKEY_CTX_new(pkey, NULL);
	if (ctx == NULL) {
		EVP_PKEY_free(pkey);
		free(sha256);
		return (EPKG_FATAL);
	}

	if (EVP_PKEY_verify_init(ctx) <= 0) {
		EVP_PKEY_CTX_free(ctx);
		EVP_PKEY_free(pkey);
		free(sha256);
		return (EPKG_FATAL);
	}

	if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) <= 0) {
		EVP_PKEY_CTX_free(ctx);
		EVP_PKEY_free(pkey);
		free(sha256);
		return (EPKG_FATAL);
	}

	if (EVP_PKEY_CTX_set_signature_md(ctx, EVP_md_pkg_sha1()) <= 0) {
		EVP_PKEY_CTX_free(ctx);
		EVP_PKEY_free(pkey);
		free(sha256);
		return (EPKG_FATAL);
	}

	ret = EVP_PKEY_verify(ctx, cbdata->sig, cbdata->siglen, sha256,
	    pkg_checksum_type_size(PKG_HASH_TYPE_SHA256_HEX));
#else
	rsa = EVP_PKEY_get1_RSA(pkey);

	ret = RSA_verify(NID_sha1, sha256,
	    pkg_checksum_type_size(PKG_HASH_TYPE_SHA256_HEX), cbdata->sig,
	    cbdata->siglen, rsa);
#endif
	free(sha256);
	if (ret <= 0) {
		if (ret < 0)
			pkg_emit_error("%s: %s", cbdata->key,
				ERR_error_string(ERR_get_error(), errbuf));
		else
			pkg_emit_error("%s: rsa signature verification failure",
			    cbdata->key);
#if OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(LIBRESSL_VERSION_NUMBER)
		EVP_PKEY_CTX_free(ctx);
#else
		RSA_free(rsa);
#endif
		EVP_PKEY_free(pkey);
		return (EPKG_FATAL);
	}

#if OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(LIBRESSL_VERSION_NUMBER)
	EVP_PKEY_CTX_free(ctx);
#else
	RSA_free(rsa);
#endif
	EVP_PKEY_free(pkey);

	return (EPKG_OK);
}

static int
ossl_verify(const struct pkgsign_ctx *sctx __unused, const char *keypath,
    unsigned char *sig, size_t sig_len, int fd)
{
	int ret;
	bool need_close = false;
	struct ossl_verify_cbdata cbdata;
	char *key_buf;
	off_t key_len;

	if (file_to_buffer(keypath, (char**)&key_buf, &key_len) != EPKG_OK) {
		pkg_emit_errno("ossl_verify", "cannot read key");
		return (EPKG_FATAL);
	}

	(void)lseek(fd, 0, SEEK_SET);

	/*
	 * XXX Older versions of pkg write out the NUL terminator of the
	 * signature, so we shim it out here to avoid breaking compatibility.
	 * We can't do it at a lower level in the caller, because other signers
	 * may use a binary format that could legitimately contain a nul byte.
	 */
	if (sig[sig_len - 1] == '\0')
		sig_len--;

	cbdata.key = key_buf;
	cbdata.keylen = key_len;
	cbdata.sig = sig;
	cbdata.siglen = sig_len;
	cbdata.verbose = false;

	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();
	OpenSSL_add_all_ciphers();

	ret = pkg_emit_sandbox_call(ossl_verify_cert_cb, fd, &cbdata);
	if (need_close)
		close(fd);
	if (ret != EPKG_OK) {
		cbdata.verbose = true;
		(void)lseek(fd, 0, SEEK_SET);
		ret = pkg_emit_sandbox_call(ossl_verify_cb, fd, &cbdata);
	}

	free(key_buf);

	return (ret);
}

int
ossl_sign_data(struct pkgsign_ctx *sctx, const unsigned char *msg, size_t msgsz,
    unsigned char **sigret, size_t *siglen)
{
	char errbuf[1024];
	struct ossl_sign_ctx *keyinfo = OSSL_CTX(sctx);
	int max_len = 0, ret;
#if OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(LIBRESSL_VERSION_NUMBER)
	EVP_PKEY_CTX *ctx;
	const EVP_MD *md;

#if OPENSSL_VERSION_NUMBER < 0x30000000L
	md = EVP_md_pkg_sha1();
#else
	md = EVP_sha256();
	char *hash;
#endif
#else
	RSA *rsa;
#endif

	if (keyinfo->key == NULL && _load_private_key(keyinfo) != EPKG_OK) {
		pkg_emit_error("can't load key from %s", keyinfo->sctx.path);
		return (EPKG_FATAL);
	}

	max_len = EVP_PKEY_size(keyinfo->key);
	*sigret = xcalloc(1, max_len + 1);

#if OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(LIBRESSL_VERSION_NUMBER)
	ctx = EVP_PKEY_CTX_new(keyinfo->key, NULL);
	if (ctx == NULL)
		return (EPKG_FATAL);

	if (EVP_PKEY_sign_init(ctx) <= 0) {
		EVP_PKEY_CTX_free(ctx);
		return (EPKG_FATAL);
	}

	if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) <= 0) {
		EVP_PKEY_CTX_free(ctx);
		return (EPKG_FATAL);
	}

	if (EVP_PKEY_CTX_set_signature_md(ctx, md) <= 0) {
		EVP_PKEY_CTX_free(ctx);
		return (EPKG_FATAL);
	}

	*siglen = max_len;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
	hash = pkg_checksum_data(msg, msgsz, PKG_HASH_TYPE_SHA256_RAW);
	ret = EVP_PKEY_sign(ctx, *sigret, siglen, hash,
	    EVP_MD_size(md));
	free(hash);
#else
	ret = EVP_PKEY_sign(ctx, *sigret, siglen, msg, msgsz);
#endif

#else	/* OPENSSL_VERSION_NUMBER < 0x10100000L || LIBRESSL_VERSION_NUMBER */
	rsa = EVP_PKEY_get1_RSA(keyinfo->key);

	ret = RSA_sign(NID_sha1, msg, msgsz, *sigret, siglen, rsa);
#endif
	if (ret <= 0) {
		pkg_emit_error("%s: %s", keyinfo->sctx.path,
		   ERR_error_string(ERR_get_error(), errbuf));
#if OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(LIBRESSL_VERSION_NUMBER)
		EVP_PKEY_CTX_free(ctx);
#else
		RSA_free(rsa);
#endif
		return (EPKG_FATAL);
	}

#if OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(LIBRESSL_VERSION_NUMBER)
	assert(*siglen < INT_MAX);
	EVP_PKEY_CTX_free(ctx);
#else
	RSA_free(rsa);
#endif
	*siglen += 1;
	return (EPKG_OK);
}

int
ossl_sign(struct pkgsign_ctx *sctx, const char *path, unsigned char **sigret,
    size_t *siglen)
{
	struct ossl_sign_ctx *keyinfo = OSSL_CTX(sctx);
	char *sha256;
	int ret;

	if (access(keyinfo->sctx.path, R_OK) == -1) {
		pkg_emit_errno("access", keyinfo->sctx.path);
		return (EPKG_FATAL);
	}

	sha256 = pkg_checksum_file(path, PKG_HASH_TYPE_SHA256_HEX);
	if (sha256 == NULL)
		return (EPKG_FATAL);

#if OPENSSL_VERSION_NUMBER >= 0x30000000L && !defined(LIBRESSL_VERSION_NUMBER)
	ret = ossl_sign_data(sctx, sha256, strlen(sha256), sigret, siglen);
#else
	ret = ossl_sign_data(sctx, sha256,
	    pkg_checksum_type_size(PKG_HASH_TYPE_SHA256_HEX), sigret, siglen);
#endif

	free(sha256);

	return (ret);
}

static int
ossl_generate(struct pkgsign_ctx *sctx, const struct iovec *iov __unused,
    int niov __unused)
{
	char errbuf[1024];
	struct ossl_sign_ctx *keyinfo = OSSL_CTX(sctx);
	const char *path = sctx->path;
	EVP_PKEY_CTX *ctx;
	EVP_PKEY *pkey;
	FILE *fp;
	int rc;

	if (niov != 0)
		return (EPKG_FATAL);

	fp = fopen(path, "w");
	if (fp == NULL) {
		pkg_emit_errno("fopen write", path);
		return (EPKG_FATAL);
	}

	if (fchmod(fileno(fp), 0400) != 0) {
		pkg_emit_errno("fchmod", path);
		fclose(fp);
		return (EPKG_FATAL);
	}

	pkey = NULL;
	rc = EPKG_FATAL;
	ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
	if (ctx == NULL)
		goto out;

	if (EVP_PKEY_keygen_init(ctx) <= 0)
		goto out;

	if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) <= 0)
		goto out;

	if (EVP_PKEY_keygen(ctx, &pkey) <= 0)
		goto out;

	if (PEM_write_PrivateKey(fp, pkey, NULL, NULL, 0, 0, NULL) <= 0)
		goto out;

	rc = EPKG_OK;
	if (keyinfo->key != NULL)
		EVP_PKEY_free(keyinfo->key);
	keyinfo->key = pkey;
out:
	if (rc != EPKG_OK) {
		pkg_emit_error("%s: %s", path,
		    ERR_error_string(ERR_get_error(), errbuf));

		/* keyinfo claims the pkey on success for any future operations. */
		EVP_PKEY_free(pkey);
	}

	fclose(fp);
	EVP_PKEY_CTX_free(ctx);
	return (rc);
}

static int
ossl_pubkey(struct pkgsign_ctx *sctx, char **pubkey, size_t *pubkeylen)
{
	char errbuf[1024];
	struct ossl_sign_ctx *keyinfo = OSSL_CTX(sctx);
	BIO *bp;

	if (keyinfo->key == NULL && _load_private_key(keyinfo) != EPKG_OK) {
		pkg_emit_error("can't load key from %s", sctx->path);
		return (EPKG_FATAL);
	}

	bp = BIO_new(BIO_s_mem());
	if (bp == NULL) {
		pkg_emit_error("error allocating public key bio: %s",
		    ERR_error_string(ERR_get_error(), errbuf));
		return (EPKG_FATAL);
	}

	BIO_set_close(bp, BIO_NOCLOSE);

	if (PEM_write_bio_PUBKEY(bp, keyinfo->key) <= 0) {
		pkg_emit_error("error writing public key: %s",
		    ERR_error_string(ERR_get_error(), errbuf));
		BIO_free(bp);
		return (EPKG_FATAL);
	}

	*pubkeylen = BIO_get_mem_data(bp, pubkey);
	BIO_free(bp);
	return (EPKG_OK);
}

static int
ossl_new(const char *name __unused, struct pkgsign_ctx *sctx __unused)
{

	SSL_load_error_strings();

	OpenSSL_add_all_algorithms();
	OpenSSL_add_all_ciphers();

	return (0);
}

static void
ossl_free(struct pkgsign_ctx *sctx)
{
	struct ossl_sign_ctx *keyinfo = OSSL_CTX(sctx);

	if (keyinfo->key != NULL)
		EVP_PKEY_free(keyinfo->key);

	ERR_free_strings();
}

const struct pkgsign_ops pkgsign_ossl = {
	.pkgsign_ctx_size = sizeof(struct ossl_sign_ctx),
	.pkgsign_new = ossl_new,
	.pkgsign_free = ossl_free,

	.pkgsign_sign = ossl_sign,
	.pkgsign_verify = ossl_verify,
	.pkgsign_verify_cert = ossl_verify_cert,

	.pkgsign_generate = ossl_generate,
	.pkgsign_pubkey = ossl_pubkey,
	.pkgsign_sign_data = ossl_sign_data,
};
