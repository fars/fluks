/* Copyright (c) 2009, Markus Peloquin <markus@cs.wisc.edu>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED 'AS IS' AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
 * IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#include "hmac.hpp"
#include "util.hpp"

std::shared_ptr<fluks::Hmac_function>
fluks::Hmac_function::create(enum hash_type type)
{
	switch (type) {
	case HT_MD5:
		return std::shared_ptr<Hmac_function>(new Hmac_md5);
	case HT_RMD160:
		return std::shared_ptr<Hmac_function>(new Hmac_rmd160);
	case HT_SHA1:
		return std::shared_ptr<Hmac_function>(new Hmac_sha1);
	case HT_SHA224:
		return std::shared_ptr<Hmac_function>(new Hmac_sha224);
	case HT_SHA256:
		return std::shared_ptr<Hmac_function>(new Hmac_sha256);
	case HT_SHA384:
		return std::shared_ptr<Hmac_function>(new Hmac_sha384);
	case HT_SHA512:
		return std::shared_ptr<Hmac_function>(new Hmac_sha512);
	default:
		return std::shared_ptr<Hmac_function>(new Hmac_impl(
		    Hash_function::create(type)));
	}
}

void
fluks::Hmac_impl::init(const uint8_t *key, size_t sz) throw ()
{
	size_t sz_block = traits()->block_size;
	if (sz > sz_block) {
		// key too long, so
		// K := H(K)
		_hashfn->init();
		_hashfn->add(key, sz);
		_hashfn->end(_key.get());
		sz = traits()->digest_size;
	} else {
		std::copy(key, key + sz, _key.get());
	}

	if (sz < sz_block)
		// (1) fill remainder with zeros
		std::fill(_key.get() + sz, _key.get() + sz_block, 0);

	// (2) XOR result of (1) with ipad
	uint8_t key_ipad[sz_block];
	xor_buf_byte(_key.get(), sz_block, IPAD, key_ipad);

	// done below as well as successive calls to add():
	// (3) append text to result of (2)
	// (4) apply H to result of (3)

	_hashfn->init();
	_hashfn->add(key_ipad, sz_block);
}

void
fluks::Hmac_impl::end(uint8_t *out) throw ()
{
	size_t sz_block = traits()->block_size;
	uint8_t key_opad[sz_block];
	uint8_t mid_digest[traits()->digest_size];

	// (5) XOR result of (1) with opad
	xor_buf_byte(_key.get(), sz_block, OPAD, key_opad);

	// (6) append result of (4) to result of (5)
	// (7) apply H to result of (6) and output result

	// get H1 = H( K^ipad . data )
	_hashfn->end(mid_digest);
	_hashfn->init();
	_hashfn->add(key_opad, sz_block);
	_hashfn->add(mid_digest, traits()->digest_size);
	// get H( K^opad . H1 )
	_hashfn->end(out);
}
