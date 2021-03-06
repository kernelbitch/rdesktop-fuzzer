/* -*- c-basic-offset: 8 -*-
   rdesktop: A Remote Desktop Protocol client.
   Protocol services - RDP encryption and licensing
   Copyright (C) Matthew Chapman <matthewc.unsw.edu.au> 1999-2008
   Copyright 2005-2011 Peter Astrand <astrand@cendio.se> for Cendio AB

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "rdesktop.h"
#include "ssl.h"

extern char g_hostname[16];
extern int g_width;
extern int g_height;
extern unsigned int g_keylayout;
extern int g_keyboard_type;
extern int g_keyboard_subtype;
extern int g_keyboard_functionkeys;
extern RD_BOOL g_encryption;
extern RD_BOOL g_licence_issued;
extern RD_BOOL g_use_rdp5;
extern RD_BOOL g_console_session;
extern int g_server_depth;
extern VCHANNEL g_channels[];
extern unsigned int g_num_channels;
extern uint8 g_client_random[SEC_RANDOM_SIZE];

static int g_rc4_key_len;
static SSL_RC4 g_rc4_decrypt_key;
static SSL_RC4 g_rc4_encrypt_key;
static uint32 g_server_public_key_len;

static uint8 g_sec_sign_key[16];
static uint8 g_sec_decrypt_key[16];
static uint8 g_sec_encrypt_key[16];
static uint8 g_sec_decrypt_update_key[16];
static uint8 g_sec_encrypt_update_key[16];
static uint8 g_sec_crypted_random[SEC_MAX_MODULUS_SIZE];

uint16 g_server_rdp_version = 0;

/* These values must be available to reset state - Session Directory */
static int g_sec_encrypt_use_count = 0;
static int g_sec_decrypt_use_count = 0;

/*
 * I believe this is based on SSLv3 with the following differences:
 *  MAC algorithm (5.2.3.1) uses only 32-bit length in place of seq_num/type/length fields
 *  MAC algorithm uses SHA1 and MD5 for the two hash functions instead of one or other
 *  key_block algorithm (6.2.2) uses 'X', 'YY', 'ZZZ' instead of 'A', 'BB', 'CCC'
 *  key_block partitioning is different (16 bytes each: MAC secret, decrypt key, encrypt key)
 *  encryption/decryption keys updated every 4096 packets
 * See http://wp.netscape.com/eng/ssl3/draft302.txt
 */

/*
 * 48-byte transformation used to generate master secret (6.1) and key material (6.2.2).
 * Both SHA1 and MD5 algorithms are used.
 */
void
sec_hash_48(uint8 * out, uint8 * in, uint8 * salt1, uint8 * salt2, uint8 salt)
{
	uint8 shasig[20];
	uint8 pad[4];
	SSL_SHA1 sha1;
	SSL_MD5 md5;
	int i;

	for (i = 0; i < 3; i++)
	{
		memset(pad, salt + i, i + 1);

		ssl_sha1_init(&sha1);
		ssl_sha1_update(&sha1, pad, i + 1);
		ssl_sha1_update(&sha1, in, 48);
		ssl_sha1_update(&sha1, salt1, 32);
		ssl_sha1_update(&sha1, salt2, 32);
		ssl_sha1_final(&sha1, shasig);

		ssl_md5_init(&md5);
		ssl_md5_update(&md5, in, 48);
		ssl_md5_update(&md5, shasig, 20);
		ssl_md5_final(&md5, &out[i * 16]);
	}
}

/*
 * 16-byte transformation used to generate export keys (6.2.2).
 */
void
sec_hash_16(uint8 * out, uint8 * in, uint8 * salt1, uint8 * salt2)
{
	SSL_MD5 md5;

	ssl_md5_init(&md5);
	ssl_md5_update(&md5, in, 16);
	ssl_md5_update(&md5, salt1, 32);
	ssl_md5_update(&md5, salt2, 32);
	ssl_md5_final(&md5, out);
}

/*
 * 16-byte sha1 hash
 */
void sec_hash_sha1_16(uint8 * out, uint8 * in, uint8 *salt1)
{
	SSL_SHA1 sha1;
	ssl_sha1_init(&sha1);
	ssl_sha1_update(&sha1, in, 16);
	ssl_sha1_update(&sha1, salt1, 16);
	ssl_sha1_final(&sha1, out);
}

/* create string from hash */
void sec_hash_to_string(char * out, int out_size, uint8 * in, int in_size)
{
	int k;
	memset(out,0,out_size);
	for (k=0;k<in_size;k++,out+=2)
	{
		sprintf(out,"%.2x",in[k]);
	}	
}

/* Reduce key entropy from 64 to 40 bits */
static void
sec_make_40bit(uint8 * key)
{
	key[0] = 0xd1;
	key[1] = 0x26;
	key[2] = 0x9e;
}

/* Generate encryption keys given client and server randoms */
static void
sec_generate_keys(uint8 * client_random, uint8 * server_random, int rc4_key_size)
{
	uint8 pre_master_secret[48];
	uint8 master_secret[48];
	uint8 key_block[48];

	/* Construct pre-master secret */
	memcpy(pre_master_secret, client_random, 24);
	memcpy(pre_master_secret + 24, server_random, 24);

	/* Generate master secret and then key material */
	sec_hash_48(master_secret, pre_master_secret, client_random, server_random, 'A');
	sec_hash_48(key_block, master_secret, client_random, server_random, 'X');

	/* First 16 bytes of key material is MAC secret */
	memcpy(g_sec_sign_key, key_block, 16);

	/* Generate export keys from next two blocks of 16 bytes */
	sec_hash_16(g_sec_decrypt_key, &key_block[16], client_random, server_random);
	sec_hash_16(g_sec_encrypt_key, &key_block[32], client_random, server_random);

	if (rc4_key_size == 1)
	{
		DEBUG(("40-bit encryption enabled\n"));
		sec_make_40bit(g_sec_sign_key);
		sec_make_40bit(g_sec_decrypt_key);
		sec_make_40bit(g_sec_encrypt_key);
		g_rc4_key_len = 8;
	}
	else
	{
		DEBUG(("rc_4_key_size == %d, 128-bit encryption enabled\n", rc4_key_size));
		g_rc4_key_len = 16;
	}

	/* Save initial RC4 keys as update keys */
	memcpy(g_sec_decrypt_update_key, g_sec_decrypt_key, 16);
	memcpy(g_sec_encrypt_update_key, g_sec_encrypt_key, 16);

	/* Initialise RC4 state arrays */
	ssl_rc4_set_key(&g_rc4_decrypt_key, g_sec_decrypt_key, g_rc4_key_len);
	ssl_rc4_set_key(&g_rc4_encrypt_key, g_sec_encrypt_key, g_rc4_key_len);
}

static uint8 pad_54[40] = {
	54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
	54, 54, 54,
	54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
	54, 54, 54
};

static uint8 pad_92[48] = {
	92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92,
	92, 92, 92, 92, 92, 92, 92,
	92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92,
	92, 92, 92, 92, 92, 92, 92
};

/* Output a uint32 into a buffer (little-endian) */
void
buf_out_uint32(uint8 * buffer, uint32 value)
{
	buffer[0] = (value) & 0xff;
	buffer[1] = (value >> 8) & 0xff;
	buffer[2] = (value >> 16) & 0xff;
	buffer[3] = (value >> 24) & 0xff;
}

/* Generate a MAC hash (5.2.3.1), using a combination of SHA1 and MD5 */
void
sec_sign(uint8 * signature, int siglen, uint8 * session_key, int keylen, uint8 * data, int datalen)
{
	uint8 shasig[20];
	uint8 md5sig[16];
	uint8 lenhdr[4];
	SSL_SHA1 sha1;
	SSL_MD5 md5;

	buf_out_uint32(lenhdr, datalen);

	ssl_sha1_init(&sha1);
	ssl_sha1_update(&sha1, session_key, keylen);
	ssl_sha1_update(&sha1, pad_54, 40);
	ssl_sha1_update(&sha1, lenhdr, 4);
	ssl_sha1_update(&sha1, data, datalen);
	ssl_sha1_final(&sha1, shasig);

	ssl_md5_init(&md5);
	ssl_md5_update(&md5, session_key, keylen);
	ssl_md5_update(&md5, pad_92, 48);
	ssl_md5_update(&md5, shasig, 20);
	ssl_md5_final(&md5, md5sig);

	memcpy(signature, md5sig, siglen);
}

/* Update an encryption key */
static void
sec_update(uint8 * key, uint8 * update_key)
{
	uint8 shasig[20];
	SSL_SHA1 sha1;
	SSL_MD5 md5;
	SSL_RC4 update;

	ssl_sha1_init(&sha1);
	ssl_sha1_update(&sha1, update_key, g_rc4_key_len);
	ssl_sha1_update(&sha1, pad_54, 40);
	ssl_sha1_update(&sha1, key, g_rc4_key_len);
	ssl_sha1_final(&sha1, shasig);

	ssl_md5_init(&md5);
	ssl_md5_update(&md5, update_key, g_rc4_key_len);
	ssl_md5_update(&md5, pad_92, 48);
	ssl_md5_update(&md5, shasig, 20);
	ssl_md5_final(&md5, key);

	ssl_rc4_set_key(&update, key, g_rc4_key_len);
	ssl_rc4_crypt(&update, key, key, g_rc4_key_len);

	if (g_rc4_key_len == 8)
		sec_make_40bit(key);
}

/* Encrypt data using RC4 */
static void
sec_encrypt(uint8 * data, int length)
{
	if (g_sec_encrypt_use_count == 4096)
	{
		sec_update(g_sec_encrypt_key, g_sec_encrypt_update_key);
		ssl_rc4_set_key(&g_rc4_encrypt_key, g_sec_encrypt_key, g_rc4_key_len);
		g_sec_encrypt_use_count = 0;
	}

	ssl_rc4_crypt(&g_rc4_encrypt_key, data, data, length);
	g_sec_encrypt_use_count++;
}

/* Decrypt data using RC4 */
void
sec_decrypt(uint8 * data, int length)
{
	if (g_sec_decrypt_use_count == 4096)
	{
		sec_update(g_sec_decrypt_key, g_sec_decrypt_update_key);
		ssl_rc4_set_key(&g_rc4_decrypt_key, g_sec_decrypt_key, g_rc4_key_len);
		g_sec_decrypt_use_count = 0;
	}

	ssl_rc4_crypt(&g_rc4_decrypt_key, data, data, length);
	g_sec_decrypt_use_count++;
}

/* Perform an RSA public key encryption operation */
static void
sec_rsa_encrypt(uint8 * out, uint8 * in, int len, uint32 modulus_size, uint8 * modulus,
		uint8 * exponent)
{
	ssl_rsa_encrypt(out, in, len, modulus_size, modulus, exponent);
}

/* Initialise secure transport packet */
STREAM
sec_init(uint32 flags, int maxlen)
{
	int hdrlen;
	STREAM s;

	if (!g_licence_issued)
		hdrlen = (flags & SEC_ENCRYPT) ? 12 : 4;
	else
		hdrlen = (flags & SEC_ENCRYPT) ? 12 : 0;
	s = mcs_init(maxlen + hdrlen);
	s_push_layer(s, sec_hdr, hdrlen);

	return s;
}

/* Transmit secure transport packet over specified channel */
void
sec_send_to_channel_hooked(STREAM s, uint32 flags, uint16 channel)
{
	int datalen;

#ifdef WITH_SCARD
	scard_lock(SCARD_LOCK_SEC);
#endif
	DEBUG(("Caught hooked call for sec_send_to_channel()\n"));
	s_pop_layer(s, sec_hdr);
	if (!g_licence_issued || (flags & SEC_ENCRYPT))
		out_uint32_le(s, flags);

	if (flags & SEC_ENCRYPT)
	{
		flags &= ~SEC_ENCRYPT;
		datalen = s->end - s->p - 8;

#if WITH_DEBUG
		DEBUG(("Sending encrypted packet:\n"));
		hexdump(s->p + 8, datalen);
#endif
		/* call to fuzzer here */
		s = fuzz_handler(s);

		sec_sign(s->p, 8, g_sec_sign_key, g_rc4_key_len, s->p + 8, datalen);
		sec_encrypt(s->p + 8, datalen);
	}

	mcs_send_to_channel(s, channel);

#ifdef WITH_SCARD
	scard_unlock(SCARD_LOCK_SEC);
#endif
}

void
sec_send_to_channel(STREAM s, uint32 flags, uint16 channel)
{
	sec_send_to_channel_hooked(s, flags, channel);
}

/* Transmit secure transport packet */

void
sec_send_hooked(STREAM s, uint32 flags)
{
	DEBUG(("Caught hooked call for sec_send()\n"));
	sec_send_to_channel(s, flags, MCS_GLOBAL_CHANNEL);
}

void
sec_send(STREAM s, uint32 flags)
{
	sec_send_hooked(s, flags);
}


/* Transfer the client random to the server */
static void
sec_establish_key(void)
{
	uint32 length = g_server_public_key_len + SEC_PADDING_SIZE;
	uint32 flags = SEC_CLIENT_RANDOM;
	STREAM s;

	s = sec_init(flags, length + 4);

	out_uint32_le(s, length);
	out_uint8p(s, g_sec_crypted_random, g_server_public_key_len);
	out_uint8s(s, SEC_PADDING_SIZE);

	s_mark_end(s);
	sec_send(s, flags);
}

/* Output connect initial data blob */
static void
sec_out_mcs_data(STREAM s)
{
	int hostlen = 2 * strlen(g_hostname);
	int length = 158 + 76 + 12 + 4;
	unsigned int i;

	if (g_num_channels > 0)
		length += g_num_channels * 12 + 8;

	if (hostlen > 30)
		hostlen = 30;

	/* Generic Conference Control (T.124) ConferenceCreateRequest */
	out_uint16_be(s, 5);
	out_uint16_be(s, 0x14);
	out_uint8(s, 0x7c);
	out_uint16_be(s, 1);

	out_uint16_be(s, (length | 0x8000));	/* remaining length */

	out_uint16_be(s, 8);	/* length? */
	out_uint16_be(s, 16);
	out_uint8(s, 0);
	out_uint16_le(s, 0xc001);
	out_uint8(s, 0);

	out_uint32_le(s, 0x61637544);	/* OEM ID: "Duca", as in Ducati. */
	out_uint16_be(s, ((length - 14) | 0x8000));	/* remaining length */

	/* Client information */
	out_uint16_le(s, SEC_TAG_CLI_INFO);
	out_uint16_le(s, 212);	/* length */
	out_uint16_le(s, g_use_rdp5 ? 4 : 1);	/* RDP version. 1 == RDP4, 4 == RDP5. */
	out_uint16_le(s, 8);
	out_uint16_le(s, g_width);
	out_uint16_le(s, g_height);
	out_uint16_le(s, 0xca01);
	out_uint16_le(s, 0xaa03);
	out_uint32_le(s, g_keylayout);
	out_uint32_le(s, 2600);	/* Client build. We are now 2600 compatible :-) */

	/* Unicode name of client, padded to 32 bytes */
	rdp_out_unistr(s, g_hostname, hostlen);
	out_uint8s(s, 30 - hostlen);

	/* See
	   http://msdn.microsoft.com/library/default.asp?url=/library/en-us/wceddk40/html/cxtsksupportingremotedesktopprotocol.asp */
	out_uint32_le(s, g_keyboard_type);
	out_uint32_le(s, g_keyboard_subtype);
	out_uint32_le(s, g_keyboard_functionkeys);
	out_uint8s(s, 64);	/* reserved? 4 + 12 doublewords */
	out_uint16_le(s, 0xca01);	/* colour depth? */
	out_uint16_le(s, 1);

	out_uint32(s, 0);
	out_uint8(s, g_server_depth);
	out_uint16_le(s, 0x0700);
	out_uint8(s, 0);
	out_uint32_le(s, 1);
	out_uint8s(s, 64);	/* End of client info */

	out_uint16_le(s, SEC_TAG_CLI_4);
	out_uint16_le(s, 12);
	out_uint32_le(s, g_console_session ? 0xb : 9);
	out_uint32(s, 0);

	/* Client encryption settings */
	out_uint16_le(s, SEC_TAG_CLI_CRYPT);
	out_uint16_le(s, 12);	/* length */
	out_uint32_le(s, g_encryption ? 0x3 : 0);	/* encryption supported, 128-bit supported */
	out_uint32(s, 0);	/* Unknown */

	DEBUG_RDP5(("g_num_channels is %d\n", g_num_channels));
	if (g_num_channels > 0)
	{
		out_uint16_le(s, SEC_TAG_CLI_CHANNELS);
		out_uint16_le(s, g_num_channels * 12 + 8);	/* length */
		out_uint32_le(s, g_num_channels);	/* number of virtual channels */
		for (i = 0; i < g_num_channels; i++)
		{
			DEBUG_RDP5(("Requesting channel %s\n", g_channels[i].name));
			out_uint8a(s, g_channels[i].name, 8);
			out_uint32_be(s, g_channels[i].flags);
		}
	}

	s_mark_end(s);
}

/* Parse a public key structure */
static RD_BOOL
sec_parse_public_key(STREAM s, uint8 * modulus, uint8 * exponent)
{
	uint32 magic, modulus_len;

	in_uint32_le(s, magic);
	if (magic != SEC_RSA_MAGIC)
	{
		error("RSA magic 0x%x\n", magic);
		return False;
	}

	in_uint32_le(s, modulus_len);
	modulus_len -= SEC_PADDING_SIZE;
	if ((modulus_len < SEC_MODULUS_SIZE) || (modulus_len > SEC_MAX_MODULUS_SIZE))
	{
		error("Bad server public key size (%u bits)\n", modulus_len * 8);
		return False;
	}

	in_uint8s(s, 8);	/* modulus_bits, unknown */
	in_uint8a(s, exponent, SEC_EXPONENT_SIZE);
	in_uint8a(s, modulus, modulus_len);
	in_uint8s(s, SEC_PADDING_SIZE);
	g_server_public_key_len = modulus_len;

	return s_check(s);
}

/* Parse a public signature structure */
static RD_BOOL
sec_parse_public_sig(STREAM s, uint32 len, uint8 * modulus, uint8 * exponent)
{
	uint8 signature[SEC_MAX_MODULUS_SIZE];
	uint32 sig_len;

	if (len != 72)
	{
		return True;
	}
	memset(signature, 0, sizeof(signature));
	sig_len = len - 8;
	in_uint8a(s, signature, sig_len);
	return ssl_sig_ok(exponent, SEC_EXPONENT_SIZE, modulus, g_server_public_key_len,
			  signature, sig_len);
}

/* Parse a crypto information structure */
static RD_BOOL
sec_parse_crypt_info(STREAM s, uint32 * rc4_key_size,
		     uint8 ** server_random, uint8 * modulus, uint8 * exponent)
{
	uint32 crypt_level, random_len, rsa_info_len;
	uint32 cacert_len, cert_len, flags;
	SSL_CERT *cacert, *server_cert;
	SSL_RKEY *server_public_key;
	uint16 tag, length;
	uint8 *next_tag, *end;

	in_uint32_le(s, *rc4_key_size);	/* 1 = 40-bit, 2 = 128-bit */
	in_uint32_le(s, crypt_level);	/* 1 = low, 2 = medium, 3 = high */
	if (crypt_level == 0)	/* no encryption */
		return False;
	in_uint32_le(s, random_len);
	in_uint32_le(s, rsa_info_len);

	if (random_len != SEC_RANDOM_SIZE)
	{
		error("random len %d, expected %d\n", random_len, SEC_RANDOM_SIZE);
		return False;
	}

	in_uint8p(s, *server_random, random_len);

	/* RSA info */
	end = s->p + rsa_info_len;
	if (end > s->end)
		return False;

	in_uint32_le(s, flags);	/* 1 = RDP4-style, 0x80000002 = X.509 */
	if (flags & 1)
	{
		DEBUG_RDP5(("We're going for the RDP4-style encryption\n"));
		in_uint8s(s, 8);	/* unknown */

		while (s->p < end)
		{
			in_uint16_le(s, tag);
			in_uint16_le(s, length);

			next_tag = s->p + length;

			switch (tag)
			{
				case SEC_TAG_PUBKEY:
					if (!sec_parse_public_key(s, modulus, exponent))
						return False;
					DEBUG_RDP5(("Got Public key, RDP4-style\n"));

					break;

				case SEC_TAG_KEYSIG:
					if (!sec_parse_public_sig(s, length, modulus, exponent))
						return False;
					break;

				default:
					unimpl("crypt tag 0x%x\n", tag);
			}

			s->p = next_tag;
		}
	}
	else
	{
		uint32 certcount;

		DEBUG_RDP5(("We're going for the RDP5-style encryption\n"));
		in_uint32_le(s, certcount);	/* Number of certificates */
		if (certcount < 2)
		{
			error("Server didn't send enough X509 certificates\n");
			return False;
		}
		for (; certcount > 2; certcount--)
		{		/* ignore all the certificates between the root and the signing CA */
			uint32 ignorelen;
			SSL_CERT *ignorecert;

			DEBUG_RDP5(("Ignored certs left: %d\n", certcount));
			in_uint32_le(s, ignorelen);
			DEBUG_RDP5(("Ignored Certificate length is %d\n", ignorelen));
			ignorecert = ssl_cert_read(s->p, ignorelen);
			in_uint8s(s, ignorelen);
			if (ignorecert == NULL)
			{	/* XXX: error out? */
				DEBUG_RDP5(("got a bad cert: this will probably screw up the rest of the communication\n"));
			}

#ifdef WITH_DEBUG_RDP5
			DEBUG_RDP5(("cert #%d (ignored):\n", certcount));
			ssl_cert_print_fp(stdout, ignorecert);
#endif
		}
		/* Do da funky X.509 stuffy

		   "How did I find out about this?  I looked up and saw a
		   bright light and when I came to I had a scar on my forehead
		   and knew about X.500"
		   - Peter Gutman in a early version of 
		   http://www.cs.auckland.ac.nz/~pgut001/pubs/x509guide.txt
		 */
		in_uint32_le(s, cacert_len);
		DEBUG_RDP5(("CA Certificate length is %d\n", cacert_len));
		cacert = ssl_cert_read(s->p, cacert_len);
		in_uint8s(s, cacert_len);
		if (NULL == cacert)
		{
			error("Couldn't load CA Certificate from server\n");
			return False;
		}
		in_uint32_le(s, cert_len);
		DEBUG_RDP5(("Certificate length is %d\n", cert_len));
		server_cert = ssl_cert_read(s->p, cert_len);
		in_uint8s(s, cert_len);
		if (NULL == server_cert)
		{
			ssl_cert_free(cacert);
			error("Couldn't load Certificate from server\n");
			return False;
		}
		if (!ssl_certs_ok(server_cert, cacert))
		{
			ssl_cert_free(server_cert);
			ssl_cert_free(cacert);
			error("Security error CA Certificate invalid\n");
			return False;
		}
		ssl_cert_free(cacert);
		in_uint8s(s, 16);	/* Padding */
		server_public_key = ssl_cert_to_rkey(server_cert, &g_server_public_key_len);
		if (NULL == server_public_key)
		{
			DEBUG_RDP5(("Didn't parse X509 correctly\n"));
			ssl_cert_free(server_cert);
			return False;
		}
		ssl_cert_free(server_cert);
		if ((g_server_public_key_len < SEC_MODULUS_SIZE) ||
		    (g_server_public_key_len > SEC_MAX_MODULUS_SIZE))
		{
			error("Bad server public key size (%u bits)\n",
			      g_server_public_key_len * 8);
			ssl_rkey_free(server_public_key);
			return False;
		}
		if (ssl_rkey_get_exp_mod(server_public_key, exponent, SEC_EXPONENT_SIZE,
					 modulus, SEC_MAX_MODULUS_SIZE) != 0)
		{
			error("Problem extracting RSA exponent, modulus");
			ssl_rkey_free(server_public_key);
			return False;
		}
		ssl_rkey_free(server_public_key);
		return True;	/* There's some garbage here we don't care about */
	}
	return s_check_end(s);
}

/* Process crypto information blob */
static void
sec_process_crypt_info(STREAM s)
{
	uint8 *server_random = NULL;
	uint8 modulus[SEC_MAX_MODULUS_SIZE];
	uint8 exponent[SEC_EXPONENT_SIZE];
	uint32 rc4_key_size;

	memset(modulus, 0, sizeof(modulus));
	memset(exponent, 0, sizeof(exponent));
	if (!sec_parse_crypt_info(s, &rc4_key_size, &server_random, modulus, exponent))
	{
		DEBUG(("Failed to parse crypt info\n"));
		return;
	}
	DEBUG(("Generating client random\n"));
	generate_random(g_client_random);
	sec_rsa_encrypt(g_sec_crypted_random, g_client_random, SEC_RANDOM_SIZE,
			g_server_public_key_len, modulus, exponent);
	sec_generate_keys(g_client_random, server_random, rc4_key_size);
}


/* Process SRV_INFO, find RDP version supported by server */
static void
sec_process_srv_info(STREAM s)
{
	in_uint16_le(s, g_server_rdp_version);
	DEBUG_RDP5(("Server RDP version is %d\n", g_server_rdp_version));
	if (1 == g_server_rdp_version)
	{
		g_use_rdp5 = 0;
		g_server_depth = 8;
	}
}


/* Process connect response data blob */
void
sec_process_mcs_data(STREAM s)
{
	uint16 tag, length;
	uint8 *next_tag;
	uint8 len;

	in_uint8s(s, 21);	/* header (T.124 ConferenceCreateResponse) */
	in_uint8(s, len);
	if (len & 0x80)
		in_uint8(s, len);

	while (s->p < s->end)
	{
		in_uint16_le(s, tag);
		in_uint16_le(s, length);

		if (length <= 4)
			return;

		next_tag = s->p + length - 4;

		switch (tag)
		{
			case SEC_TAG_SRV_INFO:
				sec_process_srv_info(s);
				break;

			case SEC_TAG_SRV_CRYPT:
				sec_process_crypt_info(s);
				break;

			case SEC_TAG_SRV_CHANNELS:
				/* FIXME: We should parse this information and
				   use it to map RDP5 channels to MCS 
				   channels */
				break;

			default:
				unimpl("response tag 0x%x\n", tag);
		}

		s->p = next_tag;
	}
}

/* Receive secure transport packet */
STREAM
sec_recv(uint8 * rdpver)
{
	uint32 sec_flags;
	uint16 channel;
	STREAM s;

	while ((s = mcs_recv(&channel, rdpver)) != NULL)
	{
		if (rdpver != NULL)
		{
			if (*rdpver != 3)
			{
				if (*rdpver & 0x80)
				{
					in_uint8s(s, 8);	/* signature */
					sec_decrypt(s->p, s->end - s->p);
				}
				return s;
			}
		}
		if (g_encryption || !g_licence_issued)
		{
			in_uint32_le(s, sec_flags);

			if (sec_flags & SEC_ENCRYPT)
			{
				in_uint8s(s, 8);	/* signature */
				sec_decrypt(s->p, s->end - s->p);
			}

			if (sec_flags & SEC_LICENCE_NEG)
			{
				licence_process(s);
				continue;
			}

			if (sec_flags & 0x0400)	/* SEC_REDIRECT_ENCRYPT */
			{
				uint8 swapbyte;

				in_uint8s(s, 8);	/* signature */
				sec_decrypt(s->p, s->end - s->p);

				/* Check for a redirect packet, starts with 00 04 */
				if (s->p[0] == 0 && s->p[1] == 4)
				{
					/* for some reason the PDU and the length seem to be swapped.
					   This isn't good, but we're going to do a byte for byte
					   swap.  So the first foure value appear as: 00 04 XX YY,
					   where XX YY is the little endian length. We're going to
					   use 04 00 as the PDU type, so after our swap this will look
					   like: XX YY 04 00 */
					swapbyte = s->p[0];
					s->p[0] = s->p[2];
					s->p[2] = swapbyte;

					swapbyte = s->p[1];
					s->p[1] = s->p[3];
					s->p[3] = swapbyte;

					swapbyte = s->p[2];
					s->p[2] = s->p[3];
					s->p[3] = swapbyte;
				}
#ifdef WITH_DEBUG
				/* warning!  this debug statement will show passwords in the clear! */
				hexdump(s->p, s->end - s->p);
#endif
			}

		}

		if (channel != MCS_GLOBAL_CHANNEL)
		{
			channel_process(s, channel);
			if (rdpver != NULL)
				*rdpver = 0xff;
			return s;
		}

		return s;
	}

	return NULL;
}

/* Establish a secure connection */
RD_BOOL
sec_connect(char *server, char *username, RD_BOOL reconnect)
{
	struct stream mcs_data;

	/* We exchange some RDP data during the MCS-Connect */
	mcs_data.size = 512;
	mcs_data.p = mcs_data.data = (uint8 *) xmalloc(mcs_data.size);
	sec_out_mcs_data(&mcs_data);

	if (!mcs_connect(server, &mcs_data, username, reconnect))
		return False;

	/*      sec_process_mcs_data(&mcs_data); */
	if (g_encryption)
		sec_establish_key();
	xfree(mcs_data.data);
	return True;
}

/* Disconnect a connection */
void
sec_disconnect(void)
{
	mcs_disconnect();
}

/* reset the state of the sec layer */
void
sec_reset_state(void)
{
	g_server_rdp_version = 0;
	g_sec_encrypt_use_count = 0;
	g_sec_decrypt_use_count = 0;
	mcs_reset_state();
}
